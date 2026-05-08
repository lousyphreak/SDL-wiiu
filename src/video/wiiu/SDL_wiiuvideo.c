/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_WIIU

#include "SDL_wiiuvideo.h"

#include "../../core/wiiu/SDL_wiiudrc.h"
#include "../../core/wiiu/SDL_wiiulifecycle.h"
#include "../../events/SDL_keyboard_c.h"

#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2r/mem.h>
#include <vpad/input.h>

#define WIIUVID_DRIVER_NAME "wiiu"
#define WIIU_TV_DISPLAY_NAME "Wii U TV"
#define WIIU_DRC_DISPLAY_NAME "Wii U GamePad"
#define WIIU_GX2_COMMAND_BUFFER_POOL_SIZE 0x400000
#define WIIU_MEM1_FRAME_HEAP_TAG 0x53444C31u
static SDL_VideoDevice *SDL_WIIU_video_device;

static void SDL_WIIU_DeleteDevice(SDL_VideoDevice *device);
static SDL_VideoDevice *SDL_WIIU_CreateDevice(void);
static bool SDL_WIIU_VideoInit(SDL_VideoDevice *_this);
static void SDL_WIIU_VideoQuit(SDL_VideoDevice *_this);
static bool SDL_WIIU_GetDisplayBounds(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_Rect *rect);
static bool SDL_WIIU_GetDisplayUsableBounds(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_Rect *rect);
static bool SDL_WIIU_GetDisplayModes(SDL_VideoDevice *_this, SDL_VideoDisplay *display);
static bool SDL_WIIU_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static bool SDL_WIIU_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props);
static void SDL_WIIU_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window);
static SDL_DisplayID SDL_WIIU_GetDisplayForWindow(SDL_VideoDevice *_this, SDL_Window *window);
static void SDL_WIIU_GetWindowSizeInPixels(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h);
static float SDL_WIIU_GetWindowContentScale(SDL_VideoDevice *_this, SDL_Window *window);
static bool SDL_WIIU_StartTextInput(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props);
static bool SDL_WIIU_StopTextInput(SDL_VideoDevice *_this, SDL_Window *window);
static bool SDL_WIIU_HasScreenKeyboardSupport(SDL_VideoDevice *_this);

static void SDL_WIIU_ForegroundAcquired(void *userdata);
static void SDL_WIIU_ForegroundReleased(void *userdata);
static bool SDL_WIIU_InitForegroundHeap(SDL_VideoData *driverdata);
static void SDL_WIIU_DestroyForegroundHeap(SDL_VideoData *driverdata);
static bool SDL_WIIU_InitMEM1Heap(SDL_VideoData *driverdata);
static void SDL_WIIU_DestroyMEM1Heap(SDL_VideoData *driverdata);
static bool SDL_WIIU_AllocateScanTarget(SDL_VideoData *driverdata, SDL_WiiUScanTarget *target);
static void SDL_WIIU_ClearScanTarget(SDL_WiiUScanTarget *target);
static bool SDL_WIIU_GetScanTargetConfiguration(SDL_WiiUScanTarget *target, SDL_Window *window, uint32_t *render_width, uint32_t *render_height, uint32_t *scan_width, uint32_t *scan_height, GX2TVRenderMode *tv_render_mode, GX2DrcRenderMode *drc_render_mode);
static bool SDL_WIIU_AddDisplay(SDL_VideoData *driverdata, SDL_WiiUDisplayKind kind, const char *name, int x, int y, int w, int h, SDL_DisplayID *out_display);
static void SDL_WIIU_DRCConnectCallback(uint32_t drc_slot, BOOL attached);
static bool SDL_WIIU_IsActiveWindow(SDL_Window *window);
static int SDL_WIIU_GetActiveWindowCount(SDL_VideoDevice *_this);

static void *SDL_WIIU_GX2RAlloc(GX2RResourceFlags flags, uint32_t size, uint32_t alignment)
{
    (void)flags;

    if (alignment < 4) {
        alignment = 4;
    }
    return MEMAllocFromDefaultHeapEx(size, alignment);
}

static void SDL_WIIU_GX2RFree(GX2RResourceFlags flags, void *block)
{
    (void)flags;

    MEMFreeToDefaultHeap(block);
}

static bool SDL_WIIU_IsActiveWindow(SDL_Window *window)
{
    return window && !window->is_destroying && window->internal;
}

static int SDL_WIIU_GetActiveWindowCount(SDL_VideoDevice *_this)
{
    int count = 0;

    for (SDL_Window *window = _this->windows; window; window = window->next) {
        if (SDL_WIIU_IsActiveWindow(window)) {
            ++count;
        }
    }

    return count;
}

SDL_Window *SDL_WIIU_GetOutputWindow(SDL_VideoDevice *_this, SDL_WiiUDisplayKind kind)
{
    SDL_Window *shared_window = NULL;

    for (SDL_Window *window = _this->windows; window; window = window->next) {
        SDL_WindowData *windowdata;

        if (!SDL_WIIU_IsActiveWindow(window)) {
            continue;
        }

        windowdata = window->internal;
        if (windowdata->display_kind == kind) {
            return window;
        }
        if (windowdata->role == SDL_WIIU_WINDOW_ROLE_SHARED) {
            shared_window = window;
        }
    }

    return shared_window;
}

void SDL_WIIU_GetPresentationRect(int src_w, int src_h, int dst_w, int dst_h, SDL_Rect *rect)
{
    SDL_zero(*rect);

    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    if (((Sint64)dst_w * src_h) <= ((Sint64)dst_h * src_w)) {
        rect->w = dst_w;
        rect->h = SDL_max(1, (int)(((Sint64)src_h * dst_w) / src_w));
    } else {
        rect->h = dst_h;
        rect->w = SDL_max(1, (int)(((Sint64)src_w * dst_h) / src_h));
    }

    rect->x = (dst_w - rect->w) / 2;
    rect->y = (dst_h - rect->h) / 2;
}

void SDL_WIIU_GetWindowPresentationSize(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h)
{
    (void)_this;

    if (w) {
        *w = window ? window->w : 0;
    }
    if (h) {
        *h = window ? window->h : 0;
    }
}

static bool SDL_WIIU_GetTVScanDimensions(GX2TVRenderMode tv_render_mode, uint32_t *w, uint32_t *h)
{
    uint32_t width;
    uint32_t height;

    switch (tv_render_mode) {
    case GX2_TV_RENDER_MODE_STANDARD_480P:
        width = 640;
        height = 480;
        break;
    case GX2_TV_RENDER_MODE_WIDE_480P:
        width = 854;
        height = 480;
        break;
    case GX2_TV_RENDER_MODE_WIDE_720P:
        switch (GX2GetSystemTVScanMode()) {
        case GX2_TV_SCAN_MODE_576I:
            width = 640;
            height = 360;
            break;
        case GX2_TV_SCAN_MODE_480I:
        case GX2_TV_SCAN_MODE_480P:
        case GX2_TV_SCAN_MODE_720P:
        case GX2_TV_SCAN_MODE_1080I:
        case GX2_TV_SCAN_MODE_1080P:
        default:
            width = 1280;
            height = 720;
            break;
        }
        break;
    case GX2_TV_RENDER_MODE_WIDE_1080P:
        switch (GX2GetSystemTVScanMode()) {
        case GX2_TV_SCAN_MODE_576I:
        case GX2_TV_SCAN_MODE_480I:
            width = 960;
            height = 540;
            break;
        case GX2_TV_SCAN_MODE_480P:
        case GX2_TV_SCAN_MODE_720P:
        case GX2_TV_SCAN_MODE_1080I:
        case GX2_TV_SCAN_MODE_1080P:
        default:
            width = 1920;
            height = 1080;
            break;
        }
        break;
    default:
        return false;
    }

    if (w) {
        *w = width;
    }
    if (h) {
        *h = height;
    }
    return true;
}

static bool SDL_WIIU_GetTVRenderTargetLimit(GX2TVRenderMode tv_render_mode, uint32_t *w, uint32_t *h)
{
    switch (tv_render_mode) {
    case GX2_TV_RENDER_MODE_STANDARD_480P:
        if (w) {
            *w = 640;
        }
        if (h) {
            *h = 480;
        }
        return true;
    case GX2_TV_RENDER_MODE_WIDE_480P:
        if (w) {
            *w = 854;
        }
        if (h) {
            *h = 480;
        }
        return true;
    case GX2_TV_RENDER_MODE_WIDE_720P:
        if (w) {
            *w = 1280;
        }
        if (h) {
            *h = 720;
        }
        return true;
    case GX2_TV_RENDER_MODE_WIDE_1080P:
        if (w) {
            *w = 1920;
        }
        if (h) {
            *h = 1080;
        }
        return true;
    default:
        return false;
    }
}

static bool SDL_WIIU_GetScanTargetConfiguration(SDL_WiiUScanTarget *target, SDL_Window *window, uint32_t *render_width, uint32_t *render_height, uint32_t *scan_width, uint32_t *scan_height, GX2TVRenderMode *tv_render_mode, GX2DrcRenderMode *drc_render_mode)
{
    const uint32_t target_render_width = window ? (uint32_t)SDL_max(window->w, 1) : target->width;
    const uint32_t target_render_height = window ? (uint32_t)SDL_max(window->h, 1) : target->height;

    if (render_width) {
        *render_width = target_render_width;
    }
    if (render_height) {
        *render_height = target_render_height;
    }

    if (target->kind == SDL_WIIU_DISPLAY_TV) {
        const GX2TVRenderMode candidates[] = {
            GX2_TV_RENDER_MODE_STANDARD_480P,
            GX2_TV_RENDER_MODE_WIDE_480P,
            GX2_TV_RENDER_MODE_WIDE_720P,
            GX2_TV_RENDER_MODE_WIDE_1080P
        };
        GX2TVRenderMode best_mode = GX2_TV_RENDER_MODE_DISABLED;
        uint32_t best_scan_width = 0;
        uint32_t best_scan_height = 0;
        Uint64 best_area = 0;
        bool found = false;
        size_t i;

        for (i = 0; i < SDL_arraysize(candidates); ++i) {
            uint32_t render_limit_width;
            uint32_t render_limit_height;
            uint32_t candidate_width;
            uint32_t candidate_height;
            Uint64 candidate_area;

            if (!SDL_WIIU_GetTVRenderTargetLimit(candidates[i], &render_limit_width, &render_limit_height)) {
                continue;
            }
            if (target_render_width > render_limit_width || target_render_height > render_limit_height) {
                continue;
            }
            if (!SDL_WIIU_GetTVScanDimensions(candidates[i], &candidate_width, &candidate_height)) {
                continue;
            }

            candidate_area = (Uint64)candidate_width * (Uint64)candidate_height;
            if (!found ||
                candidate_area < best_area ||
                (candidate_area == best_area &&
                 (candidate_width < best_scan_width ||
                  (candidate_width == best_scan_width && candidate_height < best_scan_height)))) {
                found = true;
                best_area = candidate_area;
                best_mode = candidates[i];
                best_scan_width = candidate_width;
                best_scan_height = candidate_height;
            }
        }

        if (!found) {
            return SDL_SetError("Wii U TV render target size %ux%u exceeds the supported TV render target modes", target_render_width, target_render_height);
        }

        if (scan_width) {
            *scan_width = best_scan_width;
        }
        if (scan_height) {
            *scan_height = best_scan_height;
        }
        if (tv_render_mode) {
            *tv_render_mode = best_mode;
        }
        if (drc_render_mode) {
            *drc_render_mode = target->drc_render_mode;
        }
    } else {
        GX2DrcRenderMode mode = GX2GetSystemDRCMode();

        if (mode == GX2_DRC_RENDER_MODE_DISABLED) {
            mode = GX2_DRC_RENDER_MODE_SINGLE;
        }
        if (scan_width) {
            *scan_width = target->width;
        }
        if (scan_height) {
            *scan_height = target->height;
        }
        if (tv_render_mode) {
            *tv_render_mode = target->tv_render_mode;
        }
        if (drc_render_mode) {
            *drc_render_mode = mode;
        }
    }

    return true;
}

bool SDL_WIIU_UpdateOutputWindows(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    SDL_Window *tv_window;
    SDL_Window *drc_window;
    SDL_VideoDisplay *tv_display;
    SDL_VideoDisplay *drc_display;
    const int active_window_count = SDL_WIIU_GetActiveWindowCount(_this);
    uint32_t tv_render_width;
    uint32_t tv_render_height;
    uint32_t tv_scan_width;
    uint32_t tv_scan_height;
    uint32_t drc_render_width;
    uint32_t drc_render_height;
    uint32_t drc_scan_width;
    uint32_t drc_scan_height;
    GX2TVRenderMode tv_render_mode;
    GX2DrcRenderMode drc_render_mode;
    bool need_rebuild;

    for (SDL_Window *window = _this->windows; window; window = window->next) {
        SDL_WindowData *windowdata;

        if (!SDL_WIIU_IsActiveWindow(window)) {
            continue;
        }

        windowdata = window->internal;
        windowdata->role = (active_window_count == 1) ? SDL_WIIU_WINDOW_ROLE_SHARED : SDL_WIIU_WINDOW_ROLE_DISPLAY;
    }

    tv_window = SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_TV);
    drc_window = SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_DRC);
    tv_display = SDL_GetVideoDisplay(driverdata->tv_display);
    drc_display = SDL_GetVideoDisplay(driverdata->drc_display);

    if (tv_display) {
        tv_display->fullscreen_window = tv_window;
    }
    if (drc_display) {
        drc_display->fullscreen_window = drc_window;
    }

    if (!SDL_WIIU_GetScanTargetConfiguration(&driverdata->tv, tv_window, &tv_render_width, &tv_render_height, &tv_scan_width, &tv_scan_height, &tv_render_mode, NULL) ||
        !SDL_WIIU_GetScanTargetConfiguration(&driverdata->drc, drc_window, &drc_render_width, &drc_render_height, &drc_scan_width, &drc_scan_height, NULL, &drc_render_mode)) {
        return false;
    }

    need_rebuild =
        driverdata->tv.render_width != tv_render_width ||
        driverdata->tv.render_height != tv_render_height ||
        driverdata->tv.scan_width != tv_scan_width ||
        driverdata->tv.scan_height != tv_scan_height ||
        driverdata->tv.tv_render_mode != tv_render_mode ||
        driverdata->drc.render_width != drc_render_width ||
        driverdata->drc.render_height != drc_render_height ||
        driverdata->drc.scan_width != drc_scan_width ||
        driverdata->drc.scan_height != drc_scan_height ||
        driverdata->drc.drc_render_mode != drc_render_mode;

    driverdata->tv.render_width = tv_render_width;
    driverdata->tv.render_height = tv_render_height;
    driverdata->tv.scan_width = tv_scan_width;
    driverdata->tv.scan_height = tv_scan_height;
    driverdata->tv.tv_render_mode = tv_render_mode;

    driverdata->drc.render_width = drc_render_width;
    driverdata->drc.render_height = drc_render_height;
    driverdata->drc.scan_width = drc_scan_width;
    driverdata->drc.scan_height = drc_scan_height;
    driverdata->drc.drc_render_mode = drc_render_mode;

    if (need_rebuild && driverdata->foreground_ready) {
        SDL_WIIU_ForegroundReleased(_this);
        SDL_WIIU_ForegroundAcquired(_this);
        if (!driverdata->foreground_ready) {
            return false;
        }
    }

    return true;
}

static void SDL_WIIU_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device->internal);
    SDL_free(device);
}

static SDL_VideoDevice *SDL_WIIU_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(*device));
    SDL_VideoData *driverdata;

    if (!device) {
        return NULL;
    }

    driverdata = (SDL_VideoData *)SDL_calloc(1, sizeof(*driverdata));
    if (!driverdata) {
        SDL_free(device);
        return NULL;
    }

    driverdata->tv.kind = SDL_WIIU_DISPLAY_TV;
    driverdata->drc.kind = SDL_WIIU_DISPLAY_DRC;
    driverdata->drc_connected = true;

    device->internal = driverdata;
    device->name = WIIUVID_DRIVER_NAME;
    device->VideoInit = SDL_WIIU_VideoInit;
    device->VideoQuit = SDL_WIIU_VideoQuit;
    device->GetDisplayBounds = SDL_WIIU_GetDisplayBounds;
    device->GetDisplayUsableBounds = SDL_WIIU_GetDisplayUsableBounds;
    device->GetDisplayModes = SDL_WIIU_GetDisplayModes;
    device->SetDisplayMode = SDL_WIIU_SetDisplayMode;
    device->CreateSDLWindow = SDL_WIIU_CreateWindow;
    device->DestroyWindow = SDL_WIIU_DestroyWindow;
    device->GetDisplayForWindow = SDL_WIIU_GetDisplayForWindow;
    device->GetWindowSizeInPixels = SDL_WIIU_GetWindowSizeInPixels;
    device->GetWindowContentScale = SDL_WIIU_GetWindowContentScale;
    device->PumpEvents = SDL_WIIU_PumpEvents;
    device->StartTextInput = SDL_WIIU_StartTextInput;
    device->StopTextInput = SDL_WIIU_StopTextInput;
    device->HasScreenKeyboardSupport = SDL_WIIU_HasScreenKeyboardSupport;
    device->CreateWindowFramebuffer = SDL_WIIU_CreateWindowFramebuffer;
    device->SetWindowFramebufferVSync = SDL_WIIU_SetWindowFramebufferVSync;
    device->GetWindowFramebufferVSync = SDL_WIIU_GetWindowFramebufferVSync;
    device->UpdateWindowFramebuffer = SDL_WIIU_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SDL_WIIU_DestroyWindowFramebuffer;
    device->free = SDL_WIIU_DeleteDevice;
    device->device_caps = VIDEO_DEVICE_CAPS_FULLSCREEN_ONLY;

    return device;
}

VideoBootStrap WIIU_bootstrap = {
    WIIUVID_DRIVER_NAME,
    "Wii U Video Driver",
    SDL_WIIU_CreateDevice,
    NULL,
    false
};

static void SDL_WIIU_ConfigureTVMode(SDL_WiiUScanTarget *target)
{
    switch (GX2GetSystemTVScanMode()) {
    case GX2_TV_SCAN_MODE_480I:
    case GX2_TV_SCAN_MODE_576I:
    case GX2_TV_SCAN_MODE_480P:
        if (GX2GetSystemTVAspectRatio() == GX2_ASPECT_RATIO_16_9) {
            target->tv_render_mode = GX2_TV_RENDER_MODE_WIDE_480P;
            target->width = 854;
            target->height = 480;
        } else {
            target->tv_render_mode = GX2_TV_RENDER_MODE_STANDARD_480P;
            target->width = 640;
            target->height = 480;
        }
        break;
    case GX2_TV_SCAN_MODE_1080I:
    case GX2_TV_SCAN_MODE_1080P:
        target->tv_render_mode = GX2_TV_RENDER_MODE_WIDE_1080P;
        target->width = 1920;
        target->height = 1080;
        break;
    case GX2_TV_SCAN_MODE_720P:
    default:
        target->tv_render_mode = GX2_TV_RENDER_MODE_WIDE_720P;
        target->width = 1280;
        target->height = 720;
        break;
    }

    target->render_width = target->width;
    target->render_height = target->height;
    target->scan_width = target->width;
    target->scan_height = target->height;
}

static void SDL_WIIU_ConfigureDRCMode(SDL_WiiUScanTarget *target)
{
    target->drc_render_mode = GX2GetSystemDRCMode();
    if (target->drc_render_mode == GX2_DRC_RENDER_MODE_DISABLED) {
        target->drc_render_mode = GX2_DRC_RENDER_MODE_SINGLE;
    }
    target->width = 854;
    target->height = 480;
    target->render_width = target->width;
    target->render_height = target->height;
    target->scan_width = target->width;
    target->scan_height = target->height;
}

static bool SDL_WIIU_AddDisplay(SDL_VideoData *driverdata, SDL_WiiUDisplayKind kind, const char *name, int x, int y, int w, int h, SDL_DisplayID *out_display)
{
    SDL_DisplayMode mode;
    SDL_VideoDisplay display;
    SDL_DisplayData *displaydata = (SDL_DisplayData *)SDL_calloc(1, sizeof(*displaydata));

    if (!displaydata) {
        return false;
    }

    SDL_zero(mode);
    SDL_zero(display);

    mode.w = w;
    mode.h = h;
    mode.refresh_rate = 60.0f;
    mode.format = SDL_PIXELFORMAT_RGBA8888;

    displaydata->kind = kind;
    displaydata->x = x;
    displaydata->y = y;

    display.name = (char *)name;
    display.desktop_mode = mode;
    display.content_scale = 1.0f;
    display.internal = displaydata;

    *out_display = SDL_AddVideoDisplay(&display, false);
    return *out_display != 0;
}

static bool SDL_WIIU_InitForegroundHeap(SDL_VideoData *driverdata)
{
    MEMHeapHandle base_heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG);
    uint32_t size;
    void *base;

    if (driverdata->foreground_heap) {
        return true;
    }

    size = MEMGetAllocatableSizeForFrmHeapEx(base_heap, 4);
    if (!size) {
        return SDL_SetError("Foreground heap is unavailable");
    }

    base = MEMAllocFromFrmHeapEx(base_heap, size, 4);
    if (!base) {
        return SDL_SetError("MEMAllocFromFrmHeapEx() failed for foreground heap");
    }

    driverdata->foreground_heap = MEMCreateExpHeapEx(base, size, 0);
    if (!driverdata->foreground_heap) {
        MEMFreeToFrmHeap(base_heap, MEM_FRM_HEAP_FREE_ALL);
        return SDL_SetError("MEMCreateExpHeapEx() failed for foreground heap");
    }

    return true;
}

static void SDL_WIIU_DestroyForegroundHeap(SDL_VideoData *driverdata)
{
    MEMHeapHandle base_heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG);

    if (driverdata->foreground_heap) {
        MEMDestroyExpHeap(driverdata->foreground_heap);
        driverdata->foreground_heap = NULL;
    }

    MEMFreeToFrmHeap(base_heap, MEM_FRM_HEAP_FREE_ALL);
}

static bool SDL_WIIU_InitMEM1Heap(SDL_VideoData *driverdata)
{
    MEMHeapHandle base_heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
    uint32_t size;
    void *base;

    if (driverdata->mem1_heap) {
        return true;
    }

    if (!MEMRecordStateForFrmHeap(base_heap, WIIU_MEM1_FRAME_HEAP_TAG)) {
        return SDL_SetError("MEMRecordStateForFrmHeap() failed for MEM1 heap");
    }

    size = MEMGetAllocatableSizeForFrmHeapEx(base_heap, 4);
    if (!size) {
        MEMFreeByStateToFrmHeap(base_heap, WIIU_MEM1_FRAME_HEAP_TAG);
        return SDL_SetError("MEM1 heap is unavailable");
    }

    base = MEMAllocFromFrmHeapEx(base_heap, size, 4);
    if (!base) {
        MEMFreeByStateToFrmHeap(base_heap, WIIU_MEM1_FRAME_HEAP_TAG);
        return SDL_SetError("MEMAllocFromFrmHeapEx() failed for MEM1 heap");
    }

    driverdata->mem1_heap = MEMCreateExpHeapEx(base, size, 0);
    if (!driverdata->mem1_heap) {
        MEMFreeByStateToFrmHeap(base_heap, WIIU_MEM1_FRAME_HEAP_TAG);
        return SDL_SetError("MEMCreateExpHeapEx() failed for MEM1 heap");
    }

    return true;
}

static void SDL_WIIU_DestroyMEM1Heap(SDL_VideoData *driverdata)
{
    MEMHeapHandle base_heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);

    if (driverdata->mem1_heap) {
        MEMDestroyExpHeap(driverdata->mem1_heap);
        driverdata->mem1_heap = NULL;
    }

    MEMFreeByStateToFrmHeap(base_heap, WIIU_MEM1_FRAME_HEAP_TAG);
}

static void SDL_WIIU_ClearScanTarget(SDL_WiiUScanTarget *target)
{
    if (target->context_state) {
        MEMFreeToDefaultHeap(target->context_state);
        target->context_state = NULL;
    }

    SDL_zero(target->color_buffer);
    target->scan_buffer = NULL;
}

static bool SDL_WIIU_AllocateScanTarget(SDL_VideoData *driverdata, SDL_WiiUScanTarget *target)
{
    uint32_t unused_size;
    GX2Surface *color_surface = &target->color_buffer.surface;

    if (target->kind == SDL_WIIU_DISPLAY_TV) {
        GX2CalcTVSize(target->tv_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE, &target->total_size, &unused_size);
    } else {
        GX2CalcDRCSize(target->drc_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE, &target->total_size, &unused_size);
    }

    target->scan_buffer = MEMAllocFromExpHeapEx(driverdata->foreground_heap, target->total_size, GX2_SCAN_BUFFER_ALIGNMENT);
    if (!target->scan_buffer) {
        SDL_SetError("Unable to allocate Wii U scan buffer");
        return false;
    }

    SDL_memset(target->scan_buffer, 0, target->total_size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, target->scan_buffer, target->total_size);

    SDL_zero(target->color_buffer);
    color_surface->use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    color_surface->dim = GX2_SURFACE_DIM_TEXTURE_2D;
    color_surface->width = target->render_width;
    color_surface->height = target->render_height;
    color_surface->depth = 1;
    color_surface->mipLevels = 1;
    color_surface->format = target->surface_format;
    color_surface->aa = GX2_AA_MODE1X;
    color_surface->tileMode = GX2_TILE_MODE_DEFAULT;
    target->color_buffer.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(color_surface);
    GX2InitColorBufferRegs(&target->color_buffer);

    color_surface->image = MEMAllocFromExpHeapEx(driverdata->mem1_heap, color_surface->imageSize, color_surface->alignment);
    if (!color_surface->image) {
        SDL_SetError("Unable to allocate Wii U color buffer");
        return false;
    }

    SDL_memset(color_surface->image, 0, color_surface->imageSize);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_COLOR_BUFFER), color_surface->image, color_surface->imageSize);

    target->context_state = MEMAllocFromDefaultHeapEx(sizeof(*target->context_state), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!target->context_state) {
        return SDL_SetError("Unable to allocate Wii U GX2 context state");
    }
    GX2SetupContextStateEx(target->context_state, TRUE);
    GX2SetContextState(target->context_state);
    GX2SetColorBuffer(&target->color_buffer, GX2_RENDER_TARGET_0);
    GX2SetViewport(0.0f, 0.0f, (float)target->render_width, (float)target->render_height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, target->render_width, target->render_height);

    if (target->kind == SDL_WIIU_DISPLAY_TV) {
        GX2SetTVBuffer(target->scan_buffer, target->total_size, target->tv_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE);
        GX2SetTVScale(target->scan_width, target->scan_height);
    } else {
        GX2SetDRCBuffer(target->scan_buffer, target->total_size, target->drc_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE);
        GX2SetDRCScale(target->scan_width, target->scan_height);
    }

    return true;
}

static void SDL_WIIU_ForegroundAcquired(void *userdata)
{
    SDL_VideoDevice *_this = (SDL_VideoDevice *)userdata;
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;

    if (driverdata->foreground_ready) {
        return;
    }

    if (!SDL_WIIU_InitForegroundHeap(driverdata)) {
        return;
    }

    if (!SDL_WIIU_InitMEM1Heap(driverdata)) {
        SDL_WIIU_DestroyForegroundHeap(driverdata);
        return;
    }

    if (!SDL_WIIU_AllocateScanTarget(driverdata, &driverdata->tv) ||
        !SDL_WIIU_AllocateScanTarget(driverdata, &driverdata->drc)) {
        SDL_WIIU_ForegroundReleased(userdata);
        return;
    }

    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(driverdata->drc_connected ? TRUE : FALSE);
    driverdata->foreground_ready = true;
}

static void SDL_WIIU_ForegroundReleased(void *userdata)
{
    SDL_VideoDevice *_this = (SDL_VideoDevice *)userdata;
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;

    if (!driverdata->foreground_ready && !driverdata->foreground_heap && !driverdata->mem1_heap) {
        return;
    }

    GX2DrawDone();
    GX2SetTVEnable(FALSE);
    GX2SetDRCEnable(FALSE);
    SDL_WIIU_ClearScanTarget(&driverdata->tv);
    SDL_WIIU_ClearScanTarget(&driverdata->drc);
    SDL_WIIU_DestroyMEM1Heap(driverdata);
    SDL_WIIU_DestroyForegroundHeap(driverdata);
    driverdata->foreground_ready = false;
}

static void SDL_WIIU_DRCConnectCallback(uint32_t drc_slot, BOOL attached)
{
    SDL_VideoDevice *_this = SDL_WIIU_video_device;
    SDL_VideoData *driverdata;

    (void)drc_slot;

    if (!_this) {
        return;
    }

    driverdata = (SDL_VideoData *)_this->internal;
    driverdata->drc_connected = attached ? true : false;
    if (driverdata->foreground_ready) {
        GX2SetDRCEnable(attached ? TRUE : FALSE);
    }
}

static bool SDL_WIIU_VideoInit(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    uint32_t init_attribs[] = {
        GX2_INIT_CMD_BUF_BASE, 0,
        GX2_INIT_CMD_BUF_POOL_SIZE, WIIU_GX2_COMMAND_BUFFER_POOL_SIZE,
        GX2_INIT_ARGC, 0,
        GX2_INIT_ARGV, 0,
        GX2_INIT_END
    };

    driverdata->command_buffer_pool = MEMAllocFromDefaultHeapEx(WIIU_GX2_COMMAND_BUFFER_POOL_SIZE, GX2_COMMAND_BUFFER_ALIGNMENT);
    if (!driverdata->command_buffer_pool) {
        return SDL_SetError("Unable to allocate GX2 command buffer pool");
    }

    init_attribs[1] = (uint32_t)(uintptr_t)driverdata->command_buffer_pool;
    GX2Init(init_attribs);
    driverdata->gx2_initialized = true;
    GX2RSetAllocator(SDL_WIIU_GX2RAlloc, SDL_WIIU_GX2RFree);
    driverdata->tv.surface_format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    driverdata->drc.surface_format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;

    SDL_WIIU_ConfigureTVMode(&driverdata->tv);
    SDL_WIIU_ConfigureDRCMode(&driverdata->drc);

    if (!SDL_WIIU_AddDisplay(driverdata, SDL_WIIU_DISPLAY_TV, WIIU_TV_DISPLAY_NAME, 0, 0,
                             (int)driverdata->tv.width, (int)driverdata->tv.height, &driverdata->tv_display) ||
        !SDL_WIIU_AddDisplay(driverdata, SDL_WIIU_DISPLAY_DRC, WIIU_DRC_DISPLAY_NAME, 0, (int)driverdata->tv.height,
                             (int)driverdata->drc.width, (int)driverdata->drc.height, &driverdata->drc_display)) {
        return SDL_SetError("Unable to add Wii U video displays");
    }

    GX2SetSwapInterval(1);
    GX2SetDRCConnectCallback(0, SDL_WIIU_DRCConnectCallback);
    SDL_WIIU_SetLifecycleCallbacks(SDL_WIIU_ForegroundAcquired, SDL_WIIU_ForegroundReleased, _this);
    SDL_WIIU_ForegroundAcquired(_this);

    driverdata->vpad_initialized = SDL_WIIU_InitDRC();
    SDL_WIIU_InitTouch();
    driverdata->keyboard_initialized = SDL_WIIU_InitKeyboard(_this);

    SDL_WIIU_video_device = _this;
    return true;
}

static void SDL_WIIU_VideoQuit(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;

    SDL_WIIU_video_device = NULL;
    SDL_WIIU_SetLifecycleCallbacks(NULL, NULL, NULL);
    SDL_WIIU_QuitFramebufferRenderer(_this);
    SDL_WIIU_QuitKeyboard();
    SDL_WIIU_QuitTouch();

    if (driverdata->vpad_initialized) {
        SDL_WIIU_QuitDRC();
        driverdata->vpad_initialized = false;
    }

    GX2SetDRCConnectCallback(0, NULL);
    SDL_WIIU_ForegroundReleased(_this);

    if (driverdata->gx2_initialized) {
        GX2RSetAllocator(NULL, NULL);
        GX2Shutdown();
        driverdata->gx2_initialized = false;
    }

    if (driverdata->command_buffer_pool) {
        MEMFreeToDefaultHeap(driverdata->command_buffer_pool);
        driverdata->command_buffer_pool = NULL;
    }
}

static bool SDL_WIIU_GetDisplayBounds(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_Rect *rect)
{
    SDL_DisplayData *displaydata = display->internal;
    (void)_this;

    rect->x = displaydata->x;
    rect->y = displaydata->y;
    rect->w = display->current_mode->w;
    rect->h = display->current_mode->h;
    return true;
}

static bool SDL_WIIU_GetDisplayUsableBounds(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_Rect *rect)
{
    return SDL_WIIU_GetDisplayBounds(_this, display, rect);
}

static bool SDL_WIIU_GetDisplayModes(SDL_VideoDevice *_this, SDL_VideoDisplay *display)
{
    SDL_DisplayMode mode;
    (void)_this;

    SDL_zero(mode);
    mode.w = display->desktop_mode.w;
    mode.h = display->desktop_mode.h;
    mode.refresh_rate = display->desktop_mode.refresh_rate;
    mode.format = display->desktop_mode.format;
    return SDL_AddFullscreenDisplayMode(display, &mode);
}

static bool SDL_WIIU_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    (void)_this;

    if (!mode) {
        return true;
    }

    if (mode->w == display->desktop_mode.w &&
        mode->h == display->desktop_mode.h &&
        mode->format == display->desktop_mode.format) {
        return true;
    }

    return SDL_SetError("Wii U only supports the native display mode");
}

static bool SDL_WIIU_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_Window *other = NULL;
    SDL_DisplayData *displaydata;
    SDL_WindowData *windowdata;
    int active_window_count = 0;

    (void)create_props;

    for (SDL_Window *candidate = _this->windows; candidate; candidate = candidate->next) {
        if (candidate != window && SDL_WIIU_IsActiveWindow(candidate)) {
            other = candidate;
            ++active_window_count;
        }
    }

    if (active_window_count >= 2) {
        return SDL_SetError("Wii U video supports at most two top-level windows");
    }

    displaydata = SDL_GetDisplayDriverDataForWindow(window);
    if (!displaydata) {
        return SDL_SetError("Unable to resolve the window display");
    }

    windowdata = (SDL_WindowData *)SDL_calloc(1, sizeof(*windowdata));
    if (!windowdata) {
        return false;
    }

    if (other) {
        SDL_WindowData *otherdata = other->internal;

        if (otherdata->display_kind == displaydata->kind) {
            SDL_free(windowdata);
            return SDL_SetError("Wii U video only supports one window per display");
        }
    }

    windowdata->display_id = window->displayID;
    windowdata->display_kind = displaydata->kind;
    windowdata->role = SDL_WIIU_WINDOW_ROLE_SHARED;
    windowdata->framebuffer_vsync = 1;
    window->internal = windowdata;

    if (!SDL_WIIU_UpdateOutputWindows(_this)) {
        window->internal = NULL;
        SDL_free(windowdata);
        SDL_WIIU_UpdateOutputWindows(_this);
        return false;
    }
    SDL_SetKeyboardFocus(window);
    return true;
}

static void SDL_WIIU_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    if (window && window->internal) {
        SDL_free(window->internal);
        window->internal = NULL;
    }

    SDL_WIIU_UpdateOutputWindows(_this);
}

static SDL_DisplayID SDL_WIIU_GetDisplayForWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *windowdata = window->internal;
    (void)_this;

    if (!windowdata) {
        return SDL_GetPrimaryDisplay();
    }
    return windowdata->display_id;
}

static void SDL_WIIU_GetWindowSizeInPixels(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h)
{
    (void)_this;

    if (w) {
        *w = window ? window->w : 0;
    }
    if (h) {
        *h = window ? window->h : 0;
    }
}

static float SDL_WIIU_GetWindowContentScale(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    (void)window;
    return 1.0f;
}

static bool SDL_WIIU_StartTextInput(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props)
{
    (void)_this;
    (void)window;
    (void)props;
    return true;
}

static bool SDL_WIIU_StopTextInput(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    (void)window;
    return true;
}

static bool SDL_WIIU_HasScreenKeyboardSupport(SDL_VideoDevice *_this)
{
    (void)_this;
    return false;
}

#endif // SDL_VIDEO_DRIVER_WIIU
