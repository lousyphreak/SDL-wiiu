#include "SDL_internal.h"

#ifdef SDL_CAMERA_DRIVER_WIIU

#include "../SDL_syscamera.h"
#include "../../core/wiiu/SDL_wiiulifecycle.h"

#include <camera/camera.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>

#define WIIU_CAMERA_DEVICE_TOKEN ((void *)(uintptr_t)1)
#define WIIU_CAMERA_SURFACE_COUNT 4
#define WIIU_CAMERA_COMPLETION_CAPACITY (WIIU_CAMERA_SURFACE_COUNT + 1)
#define WIIU_CAMERA_SURFACE_ALIGNMENT 1024

typedef enum WIIU_CameraSurfaceState
{
    WIIU_CAMERA_SURFACE_IDLE,
    WIIU_CAMERA_SURFACE_SUBMITTED,
    WIIU_CAMERA_SURFACE_READY,
    WIIU_CAMERA_SURFACE_APP_HELD
} WIIU_CameraSurfaceState;

typedef struct WIIU_CameraSurface
{
    CAMSurface surface;
    Uint8 *buffer;
} WIIU_CameraSurface;

typedef struct WIIU_CameraCompletion
{
    Uint8 surface_index;
    bool failed;
} WIIU_CameraCompletion;

struct SDL_PrivateCameraData
{
    int instance;
    CAMHandle handle;
    void *workmem;
    uint32_t workmem_size;
    bool camera_initialized;
    bool camera_open;
    WIIU_CameraSurface surfaces[WIIU_CAMERA_SURFACE_COUNT];
    int surface_states[WIIU_CAMERA_SURFACE_COUNT];
    WIIU_CameraCompletion completions[WIIU_CAMERA_COMPLETION_CAPACITY];
    SDL_AtomicInt queue_read;
    SDL_AtomicInt queue_write;
    SDL_AtomicInt callbacks_enabled;
    SDL_AtomicInt detached;
};

static const char *WIIUCAMERA_GetErrorString(CAMError err)
{
    switch (err) {
    case CAMERA_ERROR_OK:
        return "ok";
    case CAMERA_ERROR_INVALID_ARG:
        return "invalid argument";
    case CAMERA_ERROR_INVALID_HANDLE:
        return "invalid handle";
    case CAMERA_ERROR_TOO_MANY_SURFACES:
        return "too many queued surfaces";
    case CAMERA_ERROR_INSUFFICIENT_MEMORY:
        return "insufficient memory";
    case CAMERA_ERROR_NOT_READY:
        return "camera not ready";
    case CAMERA_ERROR_UNINITIALIZED:
        return "camera library uninitialized";
    case CAMERA_ERROR_UVC:
        return "uvc failure";
    case CAMERA_ERROR_UVD_CONTEXT:
        return "uvd context failure";
    case CAMERA_ERROR_DEVICE_IN_USE:
        return "device already in use";
    case CAMERA_ERROR_UVD_SESSION:
        return "uvd session failure";
    case CAMERA_ERROR_SEGMENT_VIOLATION:
        return "memory segmentation rejected";
    default:
        return "unknown camera error";
    }
}

static bool WIIUCAMERA_SetError(const char *operation, CAMError err)
{
    return SDL_SetError("%s failed: %s (%d)", operation, WIIUCAMERA_GetErrorString(err), err);
}

static bool WIIUCAMERA_FindDeviceByToken(SDL_Camera *device, void *userdata)
{
    return device->handle == userdata;
}

static bool WIIUCAMERA_FindDeviceByHandle(SDL_Camera *device, void *userdata)
{
    const struct SDL_PrivateCameraData *hidden = device->hidden;
    return hidden && hidden->camera_initialized && ((uintptr_t)hidden->handle == (uintptr_t)userdata);
}

static int WIIUCAMERA_FindSurfaceIndexByBuffer(const struct SDL_PrivateCameraData *hidden, const void *buffer)
{
    for (int i = 0; i < SDL_arraysize(hidden->surfaces); ++i) {
        if (hidden->surfaces[i].buffer == buffer) {
            return i;
        }
    }

    return -1;
}

static void WIIUCAMERA_ResetCompletionQueue(struct SDL_PrivateCameraData *hidden)
{
    SDL_SetAtomicInt(&hidden->queue_read, 0);
    SDL_SetAtomicInt(&hidden->queue_write, 0);
}

static void WIIUCAMERA_ResetSurfaceStates(struct SDL_PrivateCameraData *hidden, bool preserve_app_held)
{
    for (int i = 0; i < SDL_arraysize(hidden->surfaces); ++i) {
        if (preserve_app_held && hidden->surface_states[i] == WIIU_CAMERA_SURFACE_APP_HELD) {
            continue;
        }

        hidden->surface_states[i] = WIIU_CAMERA_SURFACE_IDLE;
    }
}

static bool WIIUCAMERA_HasQueuedCompletion(const struct SDL_PrivateCameraData *hidden)
{
    return SDL_GetAtomicInt(&hidden->queue_read) != SDL_GetAtomicInt(&hidden->queue_write);
}

static bool WIIUCAMERA_EnqueueCompletion(struct SDL_PrivateCameraData *hidden, int surface_index, bool failed)
{
    const int write = SDL_GetAtomicInt(&hidden->queue_write);
    const int next = (write + 1) % WIIU_CAMERA_COMPLETION_CAPACITY;
    const int read = SDL_GetAtomicInt(&hidden->queue_read);

    if (next == read) {
        hidden->surface_states[surface_index] = WIIU_CAMERA_SURFACE_IDLE;
        return false;
    }

    hidden->completions[write].surface_index = (Uint8)surface_index;
    hidden->completions[write].failed = failed;
    SDL_SetAtomicInt(&hidden->queue_write, next);
    return true;
}

static bool WIIUCAMERA_DequeueCompletion(struct SDL_PrivateCameraData *hidden, int *surface_index, bool *failed)
{
    const int read = SDL_GetAtomicInt(&hidden->queue_read);
    const int write = SDL_GetAtomicInt(&hidden->queue_write);

    if (read == write) {
        return false;
    }

    const WIIU_CameraCompletion completion = hidden->completions[read];
    SDL_SetAtomicInt(&hidden->queue_read, (read + 1) % WIIU_CAMERA_COMPLETION_CAPACITY);
    *surface_index = completion.surface_index;
    *failed = completion.failed;
    return true;
}

static bool WIIUCAMERA_SubmitSurface(struct SDL_PrivateCameraData *hidden, int surface_index, bool set_error)
{
    CAMError err;

    if (!hidden->camera_open || hidden->surface_states[surface_index] != WIIU_CAMERA_SURFACE_IDLE) {
        return true;
    }

    err = CAMSubmitTargetSurface(hidden->handle, &hidden->surfaces[surface_index].surface);
    if (err != CAMERA_ERROR_OK) {
        if (set_error) {
            return WIIUCAMERA_SetError("CAMSubmitTargetSurface", err);
        }
        return false;
    }

    hidden->surface_states[surface_index] = WIIU_CAMERA_SURFACE_SUBMITTED;
    return true;
}

static void WIIUCAMERA_StopCapture(struct SDL_PrivateCameraData *hidden)
{
    SDL_SetAtomicInt(&hidden->callbacks_enabled, 0);

    if (hidden->camera_open) {
        const CAMError err = CAMClose(hidden->handle);
        if (err != CAMERA_ERROR_OK && err != CAMERA_ERROR_NOT_READY && err != CAMERA_ERROR_INVALID_HANDLE) {
            SDL_LogWarn(SDL_LOG_CATEGORY_SYSTEM,
                        "Wii U camera: CAMClose returned %s (%d)",
                        WIIUCAMERA_GetErrorString(err), err);
        }
        hidden->camera_open = false;
    }

    WIIUCAMERA_ResetCompletionQueue(hidden);
    WIIUCAMERA_ResetSurfaceStates(hidden, true);
}

static bool WIIUCAMERA_StartCapture(struct SDL_PrivateCameraData *hidden)
{
    if (hidden->camera_open) {
        return true;
    }

    CAMError err = CAMOpen(hidden->handle);
    if (err != CAMERA_ERROR_OK) {
        return WIIUCAMERA_SetError("CAMOpen", err);
    }

    hidden->camera_open = true;
    WIIUCAMERA_ResetCompletionQueue(hidden);
    WIIUCAMERA_ResetSurfaceStates(hidden, true);
    SDL_SetAtomicInt(&hidden->callbacks_enabled, 1);

    for (int i = 0; i < SDL_arraysize(hidden->surfaces); ++i) {
        if (hidden->surface_states[i] == WIIU_CAMERA_SURFACE_IDLE &&
            !WIIUCAMERA_SubmitSurface(hidden, i, true)) {
            WIIUCAMERA_StopCapture(hidden);
            return false;
        }
    }

    return true;
}

static void WIIUCAMERA_EventHandler(CAMEventData *cam_event)
{
    SDL_Camera *device;
    struct SDL_PrivateCameraData *hidden;
    int surface_index;

    if (!cam_event) {
        return;
    }

    switch (cam_event->eventType) {
    case CAMERA_DECODE_DONE:
        device = SDL_FindPhysicalCameraByCallback(WIIUCAMERA_FindDeviceByHandle, (void *)(uintptr_t)cam_event->decode.handle);
        if (!device || !device->hidden) {
            return;
        }

        hidden = device->hidden;
        if (!SDL_GetAtomicInt(&hidden->callbacks_enabled)) {
            return;
        }

        surface_index = WIIUCAMERA_FindSurfaceIndexByBuffer(hidden, cam_event->decode.surfaceBuffer);
        if (surface_index < 0) {
            return;
        }

        hidden->surface_states[surface_index] = WIIU_CAMERA_SURFACE_READY;
        (void)WIIUCAMERA_EnqueueCompletion(hidden, surface_index, cam_event->decode.failed ? true : false);
        break;

    case CAMERA_DRC_DETACH:
        device = SDL_FindPhysicalCameraByCallback(WIIUCAMERA_FindDeviceByHandle, (void *)(uintptr_t)cam_event->detach.handle);
        if (device && device->hidden) {
            hidden = device->hidden;
            SDL_SetAtomicInt(&hidden->detached, 1);
        }
        break;

    default:
        break;
    }
}

static void WIIUCAMERA_DetectDevices(void)
{
    static const SDL_CameraSpec specs[] = {
        { SDL_PIXELFORMAT_NV12, SDL_COLORSPACE_BT601_LIMITED, CAMERA_WIDTH, CAMERA_HEIGHT, 30, 1 },
        { SDL_PIXELFORMAT_NV12, SDL_COLORSPACE_BT601_LIMITED, CAMERA_WIDTH, CAMERA_HEIGHT, 15, 1 },
    };

    if (SDL_FindPhysicalCameraByCallback(WIIUCAMERA_FindDeviceByToken, WIIU_CAMERA_DEVICE_TOKEN)) {
        return;
    }

    SDL_AddCamera("Wii U GamePad camera",
                  SDL_CAMERA_POSITION_FRONT_FACING,
                  SDL_arraysize(specs), specs,
                  WIIU_CAMERA_DEVICE_TOKEN);
}

static bool WIIUCAMERA_OpenDevice(SDL_Camera *device, const SDL_CameraSpec *spec)
{
    CAMStreamInfo stream_info;
    CAMSetupInfo setup_info;
    CAMError err = CAMERA_ERROR_OK;
    struct SDL_PrivateCameraData *hidden = SDL_calloc(1, sizeof(*hidden));

    if (!hidden) {
        return false;
    }

    hidden->instance = 0;
    device->hidden = hidden;

    SDL_zero(stream_info);
    stream_info.type = CAMERA_STREAM_TYPE_1;
    stream_info.width = CAMERA_WIDTH;
    stream_info.height = CAMERA_HEIGHT;

    const int32_t workmem_size = CAMGetMemReq(&stream_info);
    if (workmem_size < 0) {
        WIIUCAMERA_SetError("CAMGetMemReq", (CAMError)workmem_size);
        goto failed;
    }
    hidden->workmem_size = (uint32_t)workmem_size;

    hidden->workmem = MEMAllocFromDefaultHeapEx(hidden->workmem_size, 64);
    if (!hidden->workmem) {
        SDL_SetError("Failed to allocate Wii U camera work memory");
        goto failed;
    }

    err = CAMCheckMemSegmentation(hidden->workmem, hidden->workmem_size);
    if (err != CAMERA_ERROR_OK) {
        WIIUCAMERA_SetError("CAMCheckMemSegmentation(workmem)", err);
        goto failed;
    }

    for (int i = 0; i < SDL_arraysize(hidden->surfaces); ++i) {
        hidden->surfaces[i].buffer = (Uint8 *)MEMAllocFromDefaultHeapEx(CAMERA_YUV_BUFFER_SIZE, WIIU_CAMERA_SURFACE_ALIGNMENT);
        if (!hidden->surfaces[i].buffer) {
            SDL_SetError("Failed to allocate Wii U camera surface %d", i);
            goto failed;
        }

        err = CAMCheckMemSegmentation(hidden->surfaces[i].buffer, CAMERA_YUV_BUFFER_SIZE);
        if (err != CAMERA_ERROR_OK) {
            WIIUCAMERA_SetError("CAMCheckMemSegmentation(surface)", err);
            goto failed;
        }

        SDL_zero(hidden->surfaces[i].surface);
        hidden->surfaces[i].surface.surfaceSize = CAMERA_YUV_BUFFER_SIZE;
        hidden->surfaces[i].surface.surfaceBuffer = hidden->surfaces[i].buffer;
        hidden->surfaces[i].surface.height = CAMERA_HEIGHT;
        hidden->surfaces[i].surface.width = CAMERA_WIDTH;
        hidden->surfaces[i].surface.pitch = CAMERA_PITCH;
        hidden->surfaces[i].surface.alignment = WIIU_CAMERA_SURFACE_ALIGNMENT;
        hidden->surfaces[i].surface.tileMode = 0;
        hidden->surfaces[i].surface.pixelFormat = 0;
        hidden->surface_states[i] = WIIU_CAMERA_SURFACE_IDLE;
    }

    SDL_zero(setup_info);
    setup_info.streamInfo = stream_info;
    setup_info.workMem.size = hidden->workmem_size;
    setup_info.workMem.pMem = hidden->workmem;
    setup_info.eventHandler = WIIUCAMERA_EventHandler;
    setup_info.mode.forceDrc = FALSE;
    setup_info.mode.fps = ((spec->framerate_numerator / SDL_max(spec->framerate_denominator, 1)) >= 30) ? CAMERA_FPS_30 : CAMERA_FPS_15;
    setup_info.threadAffinity = OS_THREAD_ATTRIB_AFFINITY_ANY;

    hidden->handle = CAMInit(hidden->instance, &setup_info, &err);
    if (hidden->handle < 0) {
        WIIUCAMERA_SetError("CAMInit", err);
        goto failed;
    }

    hidden->camera_initialized = true;
    if (SDL_WIIU_IsInForeground() && !WIIUCAMERA_StartCapture(hidden)) {
        goto failed;
    }

    SDL_CameraPermissionOutcome(device, true);
    return true;

failed:
    WIIUCAMERA_StopCapture(hidden);
    if (hidden->camera_initialized) {
        CAMExit(hidden->handle);
    }
    for (int i = 0; i < SDL_arraysize(hidden->surfaces); ++i) {
        if (hidden->surfaces[i].buffer) {
            MEMFreeToDefaultHeap(hidden->surfaces[i].buffer);
        }
    }
    if (hidden->workmem) {
        MEMFreeToDefaultHeap(hidden->workmem);
    }
    SDL_free(hidden);
    device->hidden = NULL;
    return false;
}

static void WIIUCAMERA_CloseDevice(SDL_Camera *device)
{
    struct SDL_PrivateCameraData *hidden = device->hidden;

    if (!hidden) {
        return;
    }

    WIIUCAMERA_StopCapture(hidden);
    if (hidden->camera_initialized) {
        CAMExit(hidden->handle);
    }

    for (int i = 0; i < SDL_arraysize(hidden->surfaces); ++i) {
        if (hidden->surfaces[i].buffer) {
            MEMFreeToDefaultHeap(hidden->surfaces[i].buffer);
        }
    }

    if (hidden->workmem) {
        MEMFreeToDefaultHeap(hidden->workmem);
    }

    SDL_free(hidden);
    device->hidden = NULL;
}

static bool WIIUCAMERA_WaitDevice(SDL_Camera *device)
{
    struct SDL_PrivateCameraData *hidden = device->hidden;

    while (!SDL_GetAtomicInt(&device->shutdown)) {
        if (SDL_GetAtomicInt(&hidden->detached)) {
            WIIUCAMERA_StopCapture(hidden);
            SDL_SetError("Wii U camera disconnected");
            return false;
        }

        if (!SDL_WIIU_IsInForeground()) {
            WIIUCAMERA_StopCapture(hidden);
            SDL_Delay(16);
            continue;
        }

        if (!hidden->camera_open && !WIIUCAMERA_StartCapture(hidden)) {
            return false;
        }

        if (WIIUCAMERA_HasQueuedCompletion(hidden)) {
            return true;
        }

        SDL_Delay(1);
    }

    return true;
}

static SDL_CameraFrameResult WIIUCAMERA_AcquireFrame(SDL_Camera *device, SDL_Surface *frame, Uint64 *timestampNS, float *rotation)
{
    struct SDL_PrivateCameraData *hidden = device->hidden;
    int surface_index;
    bool failed;

    if (!WIIUCAMERA_DequeueCompletion(hidden, &surface_index, &failed)) {
        return SDL_CAMERA_FRAME_SKIP;
    }

    if (surface_index < 0 || surface_index >= SDL_arraysize(hidden->surfaces)) {
        SDL_SetError("Wii U camera returned an invalid surface index");
        return SDL_CAMERA_FRAME_ERROR;
    }

    if (failed) {
        hidden->surface_states[surface_index] = WIIU_CAMERA_SURFACE_IDLE;
        if (hidden->camera_open && SDL_WIIU_IsInForeground()) {
            (void)WIIUCAMERA_SubmitSurface(hidden, surface_index, false);
        }
        return SDL_CAMERA_FRAME_SKIP;
    }

    hidden->surface_states[surface_index] = WIIU_CAMERA_SURFACE_APP_HELD;
    DCInvalidateRange(hidden->surfaces[surface_index].buffer, CAMERA_YUV_BUFFER_SIZE);
    frame->pixels = hidden->surfaces[surface_index].buffer;
    frame->pitch = hidden->surfaces[surface_index].surface.pitch;
    *timestampNS = SDL_GetTicksNS();
    *rotation = 0.0f;
    return SDL_CAMERA_FRAME_READY;
}

static void WIIUCAMERA_ReleaseFrame(SDL_Camera *device, SDL_Surface *frame)
{
    struct SDL_PrivateCameraData *hidden = device->hidden;
    const int surface_index = WIIUCAMERA_FindSurfaceIndexByBuffer(hidden, frame->pixels);

    if (surface_index < 0) {
        return;
    }

    hidden->surface_states[surface_index] = WIIU_CAMERA_SURFACE_IDLE;
    if (hidden->camera_open && SDL_WIIU_IsInForeground() && !SDL_GetAtomicInt(&hidden->detached)) {
        (void)WIIUCAMERA_SubmitSurface(hidden, surface_index, false);
    }
}

static void WIIUCAMERA_FreeDeviceHandle(SDL_Camera *device)
{
    (void)device;
}

static void WIIUCAMERA_Deinitialize(void)
{
}

static bool WIIUCAMERA_Init(SDL_CameraDriverImpl *impl)
{
    impl->DetectDevices = WIIUCAMERA_DetectDevices;
    impl->OpenDevice = WIIUCAMERA_OpenDevice;
    impl->CloseDevice = WIIUCAMERA_CloseDevice;
    impl->WaitDevice = WIIUCAMERA_WaitDevice;
    impl->AcquireFrame = WIIUCAMERA_AcquireFrame;
    impl->ReleaseFrame = WIIUCAMERA_ReleaseFrame;
    impl->FreeDeviceHandle = WIIUCAMERA_FreeDeviceHandle;
    impl->Deinitialize = WIIUCAMERA_Deinitialize;
    impl->ProvidesOwnCallbackThread = false;

    return true;
}

CameraBootStrap WIIUCAMERA_bootstrap = {
    "wiiu", "SDL Wii U camera driver", WIIUCAMERA_Init, false
};

#endif // SDL_CAMERA_DRIVER_WIIU
