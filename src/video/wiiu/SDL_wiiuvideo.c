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
#include "../../core/wiiu/SDL_wiiukpad.h"
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
#include <proc_ui/procui.h>
#include <vpad/input.h>

#define WIIUVID_DRIVER_NAME "wiiu"
#define WIIU_TV_DISPLAY_NAME "Wii U TV"
#define WIIU_DRC_DISPLAY_NAME "Wii U GamePad"
#define WIIU_GX2_COMMAND_BUFFER_POOL_SIZE 0x400000
#define WIIU_MEM1_FRAME_HEAP_TAG 0x53444C31u
#define WIIU_DRC_NATIVE_WIDTH 854
#define WIIU_DRC_NATIVE_HEIGHT 480

typedef struct SDL_WiiUTVMode
{
    int w;
    int h;
    GX2TVRenderMode render_mode;
} SDL_WiiUTVMode;

extern void SDL_WIIU_JoystickForegroundReleased(void);
extern void SDL_WIIU_JoystickForegroundAcquired(void);

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
static void SDL_WIIU_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window);
static bool SDL_WIIU_SyncWindow(SDL_VideoDevice *_this, SDL_Window *window);
static void SDL_WIIU_GetWindowSizeInPixels(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h);
static float SDL_WIIU_GetWindowContentScale(SDL_VideoDevice *_this, SDL_Window *window);
static bool SDL_WIIU_StartTextInput(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props);
static bool SDL_WIIU_StopTextInput(SDL_VideoDevice *_this, SDL_Window *window);

static void SDL_WIIU_ForegroundAcquired(void *userdata);
static void SDL_WIIU_ForegroundReleased(void *userdata);
static bool SDL_WIIU_InitForegroundHeap(SDL_VideoData *driverdata);
static void SDL_WIIU_DestroyForegroundHeap(SDL_VideoData *driverdata);
static bool SDL_WIIU_InitMEM1Heap(SDL_VideoData *driverdata);
static void SDL_WIIU_DestroyMEM1Heap(SDL_VideoData *driverdata);
static void *SDL_WIIU_AllocForegroundMemory(SDL_VideoData *driverdata, uint32_t size, uint32_t alignment, bool prefer_mem1, bool fallback_to_foreground);
static void SDL_WIIU_BindScanTarget(SDL_WiiUScanTarget *target);
static bool SDL_WIIU_AllocateScanBuffer(SDL_VideoData *driverdata, SDL_WiiUScanTarget *target);
static bool SDL_WIIU_AllocateScanTargetRenderBuffer(SDL_VideoData *driverdata, SDL_WiiUScanTarget *target);
static void SDL_WIIU_ReleaseForegroundRenderers(SDL_VideoDevice *_this, SDL_VideoData *driverdata);
static void SDL_WIIU_ClearScanTarget(SDL_WiiUScanTarget *target);
static bool SDL_WIIU_GetScanTargetConfiguration(SDL_WiiUScanTarget *target, SDL_Window *window, uint32_t *render_width, uint32_t *render_height, uint32_t *scan_width, uint32_t *scan_height, GX2TVRenderMode *tv_render_mode, GX2DrcRenderMode *drc_render_mode);
static bool SDL_WIIU_AddDisplay(SDL_VideoData *driverdata, SDL_WiiUDisplayKind kind, const char *name, int x, int y, int w, int h, SDL_DisplayID *out_display);
static void SDL_WIIU_DRCConnectCallback(uint32_t drc_slot, BOOL attached);
static bool SDL_WIIU_IsActiveWindow(SDL_Window *window);
static int SDL_WIIU_GetActiveWindowCount(SDL_VideoDevice *_this);
static Uint32 SDL_WIIU_GetWindowPresentOutputMask(SDL_VideoDevice *_this, SDL_Window *window);
static void SDL_WIIU_UpdateWindowGX2Properties(SDL_VideoDevice *_this, SDL_Window *window);
static void SDL_WIIU_UpdateAllWindowGX2Properties(SDL_VideoDevice *_this);
static void SDL_WIIU_InitDisplayMode(SDL_DisplayMode *mode, SDL_DisplayID display_id, int w, int h);
static bool SDL_WIIU_GetTVDisplaySize(GX2TVRenderMode render_mode, int *w, int *h);
static int SDL_WIIU_GetMaxTVModeCount(void);
static int SDL_WIIU_GetSupportedTVModes(SDL_WiiUTVMode *modes, int max_modes);
static const SDL_WiiUTVMode *SDL_WIIU_GetDefaultTVMode(void);
static const SDL_WiiUTVMode *SDL_WIIU_GetTVModeForSize(int w, int h);
static const SDL_WiiUTVMode *SDL_WIIU_GetTVModeForRequest(int w, int h);
static void SDL_WIIU_GetSupportedWindowSize(SDL_WiiUDisplayKind kind, int requested_w, int requested_h, int *out_w, int *out_h);
static void SDL_WIIU_SetWindowSizeInternal(SDL_Window *window, int w, int h);
static void SDL_WIIU_ApplyWindowSize(SDL_VideoDevice *_this, SDL_Window *window, int requested_w, int requested_h);
static SDL_Window *SDL_WIIU_GetWindowForDisplay(SDL_VideoDevice *_this, SDL_DisplayID display_id);
static const SDL_DisplayMode *SDL_WIIU_GetExactFullscreenDisplayMode(SDL_VideoDisplay *display, int w, int h);
static void SDL_WIIU_UpdateDisplayLayout(SDL_VideoData *driverdata, int tv_height);

static void *SDL_WIIU_GX2RAlloc(GX2RResourceFlags flags, uint32_t size, uint32_t alignment)
{
    SDL_VideoDevice *_this = SDL_GetVideoDevice();
    SDL_VideoData *driverdata = _this ? (SDL_VideoData *)_this->internal : NULL;
    void *block = NULL;

    if (alignment < 4) {
        alignment = 4;
    }

    if ((flags & GX2R_RESOURCE_USAGE_FORCE_MEM1) && driverdata && driverdata->mem1_heap) {
        block = MEMAllocFromExpHeapEx(driverdata->mem1_heap, size, (int)alignment);
        if (block) {
            return block;
        }
    }

    return MEMAllocFromDefaultHeapEx(size, alignment);
}

static void SDL_WIIU_GX2RFree(GX2RResourceFlags flags, void *block)
{
    SDL_VideoDevice *_this = SDL_GetVideoDevice();
    SDL_VideoData *driverdata = _this ? (SDL_VideoData *)_this->internal : NULL;
    MEMHeapHandle heap;

    (void)flags;

    if (!block) {
        return;
    }

    heap = MEMFindContainHeap(block);
    if (driverdata && heap && (heap == driverdata->mem1_heap || heap == driverdata->foreground_heap)) {
        MEMFreeToExpHeap(heap, block);
        return;
    }

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

Uint32 SDL_WIIU_GetPresentOutputMask(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata;
    Uint32 mask = 0;

    if (!_this || !_this->internal) {
        return 0;
    }

    driverdata = (SDL_VideoData *)_this->internal;
    if (SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_TV) && driverdata->tv.scan_buffer) {
        mask |= SDL_WIIU_PRESENT_OUTPUT_TV;
    }
    if (driverdata->drc_connected &&
        SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_DRC) &&
        driverdata->drc.scan_buffer) {
        mask |= SDL_WIIU_PRESENT_OUTPUT_DRC;
    }

    return mask;
}

static Uint32 SDL_WIIU_GetWindowPresentOutputMask(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_VideoData *driverdata;
    Uint32 mask = 0;

    if (!_this || !_this->internal || !window || !window->internal) {
        return 0;
    }

    driverdata = (SDL_VideoData *)_this->internal;
    if (!driverdata->foreground_ready) {
        return 0;
    }

    if (SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_TV) == window &&
        driverdata->tv.scan_buffer && driverdata->tv.context_state) {
        mask |= SDL_WIIU_PRESENT_OUTPUT_TV;
    }

    if (driverdata->drc_connected &&
        SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_DRC) == window &&
        driverdata->drc.scan_buffer) {
        mask |= SDL_WIIU_PRESENT_OUTPUT_DRC;
    }

    return mask;
}

static void SDL_WIIU_UpdateWindowGX2Properties(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_VideoData *driverdata = (_this && _this->internal) ? (SDL_VideoData *)_this->internal : NULL;
    SDL_PropertiesID props;
    GX2ColorBuffer *tv_color_buffer = NULL;
    GX2ContextState *tv_context_state = NULL;
    GX2ColorBuffer *drc_color_buffer = NULL;
    GX2ContextState *drc_context_state = NULL;
    Uint32 output_mask = 0;
    Sint64 foreground_generation = 0;
    bool owns_tv_output = false;
    bool owns_drc_output = false;

    if (!window) {
        return;
    }

    props = SDL_GetWindowProperties(window);
    if (driverdata) {
        foreground_generation = (Sint64)driverdata->foreground_generation;
    }

    if (driverdata && window->internal) {
        owns_tv_output = (SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_TV) == window);
        owns_drc_output = driverdata->drc_connected && (SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_DRC) == window);
        output_mask = SDL_WIIU_GetWindowPresentOutputMask(_this, window);
        if (owns_tv_output &&
            driverdata->tv.context_state) {
            tv_color_buffer = &driverdata->tv.color_buffer;
            tv_context_state = driverdata->tv.context_state;
        }
        if (owns_drc_output &&
            !driverdata->drc_uses_tv_render_buffer &&
            driverdata->drc.context_state) {
            drc_color_buffer = &driverdata->drc.color_buffer;
            drc_context_state = driverdata->drc.context_state;
        }
    }

    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WIIU_MEM1_HEAP_POINTER,
                           driverdata && driverdata->foreground_ready ? driverdata->mem1_heap : NULL);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WIIU_TV_COLOR_BUFFER_POINTER, tv_color_buffer);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WIIU_TV_CONTEXT_STATE_POINTER, tv_context_state);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WIIU_DRC_COLOR_BUFFER_POINTER, drc_color_buffer);
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_WIIU_DRC_CONTEXT_STATE_POINTER, drc_context_state);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_WIIU_PRESENT_OUTPUT_MASK_NUMBER, (Sint64)output_mask);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_WIIU_FOREGROUND_GENERATION_NUMBER, foreground_generation);
}

static void SDL_WIIU_UpdateAllWindowGX2Properties(SDL_VideoDevice *_this)
{
    if (!_this) {
        return;
    }

    for (SDL_Window *window = _this->windows; window; window = window->next) {
        SDL_WIIU_UpdateWindowGX2Properties(_this, window);
    }
}

bool SDL_WIIU_ShouldSwapPresent(SDL_VideoDevice *_this, Uint32 rendered_outputs)
{
    SDL_VideoData *driverdata;
    const Uint32 expected_outputs = SDL_WIIU_GetPresentOutputMask(_this);

    if (!_this || !_this->internal || !expected_outputs) {
        return false;
    }

    driverdata = (SDL_VideoData *)_this->internal;
    rendered_outputs &= expected_outputs;
    if (!rendered_outputs) {
        return false;
    }

    driverdata->pending_present_mask &= expected_outputs;
    driverdata->pending_present_mask |= rendered_outputs;
    if ((driverdata->pending_present_mask & expected_outputs) != expected_outputs) {
        return false;
    }

    driverdata->pending_present_mask = 0;
    return true;
}

void SDL_WIIU_ResetPresentState(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata;

    if (!_this || !_this->internal) {
        return;
    }

    driverdata = (SDL_VideoData *)_this->internal;
    driverdata->pending_present_mask = 0;
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

static void SDL_WIIU_InitDisplayMode(SDL_DisplayMode *mode, SDL_DisplayID display_id, int w, int h)
{
    SDL_zero(*mode);
    mode->displayID = display_id;
    mode->w = w;
    mode->h = h;
    mode->pixel_density = 1.0f;
    mode->refresh_rate = 60.0f;
    mode->format = SDL_PIXELFORMAT_RGBA8888;
}

static bool SDL_WIIU_GetTVDisplaySize(GX2TVRenderMode render_mode, int *w, int *h)
{
    int width;
    int height;

    switch (render_mode) {
    case GX2_TV_RENDER_MODE_WIDE_480P:
        width = 854;
        height = 480;
        break;
    case GX2_TV_RENDER_MODE_WIDE_720P:
        width = 1280;
        height = 720;
        break;
    case GX2_TV_RENDER_MODE_WIDE_1080P:
        width = 1920;
        height = 1080;
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

static int SDL_WIIU_GetMaxTVModeCount(void)
{
    switch (GX2GetSystemTVScanMode()) {
    case GX2_TV_SCAN_MODE_1080I:
    case GX2_TV_SCAN_MODE_1080P:
        return 3;
    case GX2_TV_SCAN_MODE_720P:
        return 2;
    case GX2_TV_SCAN_MODE_576I:
    case GX2_TV_SCAN_MODE_480I:
    case GX2_TV_SCAN_MODE_480P:
    default:
        return 1;
    }
}

static int SDL_WIIU_GetSupportedTVModes(SDL_WiiUTVMode *modes, int max_modes)
{
    static const GX2TVRenderMode render_modes[] = {
        GX2_TV_RENDER_MODE_WIDE_480P,
        GX2_TV_RENDER_MODE_WIDE_720P,
        GX2_TV_RENDER_MODE_WIDE_1080P
    };
    const int capped_mode_count = SDL_min(SDL_WIIU_GetMaxTVModeCount(), (int)SDL_arraysize(render_modes));
    int count = 0;
    int i;
    int j;

    for (i = 0; i < capped_mode_count; ++i) {
        int width;
        int height;

        if (!SDL_WIIU_GetTVDisplaySize(render_modes[i], &width, &height)) {
            continue;
        }

        if (count < max_modes) {
            modes[count].w = width;
            modes[count].h = height;
            modes[count].render_mode = render_modes[i];
        }
        ++count;
    }

    if (count > max_modes) {
        count = max_modes;
    }

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            const Sint64 area_i = (Sint64)modes[i].w * (Sint64)modes[i].h;
            const Sint64 area_j = (Sint64)modes[j].w * (Sint64)modes[j].h;

            if (area_j < area_i ||
                (area_j == area_i &&
                 (modes[j].w < modes[i].w ||
                  (modes[j].w == modes[i].w && modes[j].h < modes[i].h)))) {
                SDL_WiiUTVMode tmp = modes[i];
                modes[i] = modes[j];
                modes[j] = tmp;
            }
        }
    }

    return count;
}

static const SDL_WiiUTVMode *SDL_WIIU_GetDefaultTVMode(void)
{
    static SDL_WiiUTVMode mode;
    const int max_tv_mode_count = SDL_WIIU_GetMaxTVModeCount();

    mode.render_mode = (max_tv_mode_count >= 3) ? GX2_TV_RENDER_MODE_WIDE_1080P :
                       (max_tv_mode_count >= 2) ? GX2_TV_RENDER_MODE_WIDE_720P :
                                                  GX2_TV_RENDER_MODE_WIDE_480P;

    if (!SDL_WIIU_GetTVDisplaySize(mode.render_mode, &mode.w, &mode.h)) {
        mode.w = 854;
        mode.h = 480;
        mode.render_mode = GX2_TV_RENDER_MODE_WIDE_480P;
    }

    return &mode;
}

static const SDL_WiiUTVMode *SDL_WIIU_GetTVModeForSize(int w, int h)
{
    static SDL_WiiUTVMode modes[3];
    const int count = SDL_WIIU_GetSupportedTVModes(modes, (int)SDL_arraysize(modes));
    int i;

    for (i = 0; i < count; ++i) {
        if (modes[i].w == w && modes[i].h == h) {
            return &modes[i];
        }
    }

    return NULL;
}

static const SDL_WiiUTVMode *SDL_WIIU_GetTVModeForRequest(int w, int h)
{
    static SDL_WiiUTVMode modes[3];
    const int count = SDL_WIIU_GetSupportedTVModes(modes, (int)SDL_arraysize(modes));
    int i;

    w = SDL_max(w, 1);
    h = SDL_max(h, 1);

    for (i = 0; i < count; ++i) {
        if (w <= modes[i].w && h <= modes[i].h) {
            return &modes[i];
        }
    }

    return (count > 0) ? &modes[count - 1] : NULL;
}

static void SDL_WIIU_GetSupportedWindowSize(SDL_WiiUDisplayKind kind, int requested_w, int requested_h, int *out_w, int *out_h)
{
    const SDL_WiiUTVMode *mode;

    if (kind == SDL_WIIU_DISPLAY_DRC) {
        if (out_w) {
            *out_w = WIIU_DRC_NATIVE_WIDTH;
        }
        if (out_h) {
            *out_h = WIIU_DRC_NATIVE_HEIGHT;
        }
        return;
    }

    mode = SDL_WIIU_GetTVModeForRequest(requested_w, requested_h);
    if (!mode) {
        if (out_w) {
            *out_w = 854;
        }
        if (out_h) {
            *out_h = 480;
        }
        return;
    }
    if (out_w) {
        *out_w = mode->w;
    }
    if (out_h) {
        *out_h = mode->h;
    }
}

static void SDL_WIIU_SetWindowSizeInternal(SDL_Window *window, int w, int h)
{
    SDL_DisplayMode mode;
    SDL_DisplayID display_id = 0;

    if (!window) {
        return;
    }

    display_id = window->displayID ? window->displayID : window->pending_displayID;
    window->floating.w = window->windowed.w = window->pending.w = window->w = w;
    window->floating.h = window->windowed.h = window->pending.h = window->h = h;
    window->last_size_pending = false;

    SDL_WIIU_InitDisplayMode(&mode, display_id, w, h);
    SDL_copyp(&window->requested_fullscreen_mode, &mode);
    SDL_copyp(&window->current_fullscreen_mode, &mode);
}

static void SDL_WIIU_ApplyWindowSize(SDL_VideoDevice *_this, SDL_Window *window, int requested_w, int requested_h)
{
    SDL_WindowData *windowdata;
    SDL_VideoDisplay *display;
    const SDL_DisplayMode *target_mode;
    int supported_w;
    int supported_h;

    if (!_this || !window || !window->internal) {
        return;
    }

    windowdata = window->internal;
    SDL_WIIU_GetSupportedWindowSize(windowdata->display_kind, requested_w, requested_h, &supported_w, &supported_h);
    display = SDL_GetVideoDisplay(windowdata->display_id);
    target_mode = display ? SDL_WIIU_GetExactFullscreenDisplayMode(display, supported_w, supported_h) : NULL;

    if (target_mode && display && target_mode != display->current_mode) {
        if (SDL_SetDisplayModeForDisplay(display, (SDL_DisplayMode *)target_mode)) {
            window->surface_valid = false;
            return;
        }
        SDL_ClearError();
    }

    SDL_WIIU_SetWindowSizeInternal(window, supported_w, supported_h);
    window->surface_valid = false;
    SDL_WIIU_UpdateOutputWindows(_this);
}

static SDL_Window *SDL_WIIU_GetWindowForDisplay(SDL_VideoDevice *_this, SDL_DisplayID display_id)
{
    for (SDL_Window *window = _this->windows; window; window = window->next) {
        SDL_WindowData *windowdata;

        if (!SDL_WIIU_IsActiveWindow(window)) {
            continue;
        }

        windowdata = window->internal;
        if (windowdata && windowdata->display_id == display_id) {
            return window;
        }
    }

    return NULL;
}

static const SDL_DisplayMode *SDL_WIIU_GetExactFullscreenDisplayMode(SDL_VideoDisplay *display, int w, int h)
{
    SDL_DisplayMode closest;
    int i;

    if (!display) {
        return NULL;
    }

    if (display->desktop_mode.w == w && display->desktop_mode.h == h) {
        return &display->desktop_mode;
    }

    if (!SDL_GetClosestFullscreenDisplayMode(display->id, w, h, display->desktop_mode.refresh_rate, false, &closest)) {
        SDL_ClearError();
        return NULL;
    }
    if (closest.w != w || closest.h != h) {
        return NULL;
    }

    for (i = 0; i < display->num_fullscreen_modes; ++i) {
        const SDL_DisplayMode *mode = &display->fullscreen_modes[i];
        if (mode->w == w && mode->h == h) {
            return mode;
        }
    }

    return NULL;
}

static void SDL_WIIU_UpdateDisplayLayout(SDL_VideoData *driverdata, int tv_height)
{
    SDL_DisplayData *tv_displaydata;
    SDL_DisplayData *drc_displaydata;

    if (!driverdata) {
        return;
    }

    tv_displaydata = SDL_GetDisplayDriverData(driverdata->tv_display);
    drc_displaydata = SDL_GetDisplayDriverData(driverdata->drc_display);
    if (tv_displaydata) {
        tv_displaydata->x = 0;
        tv_displaydata->y = 0;
    }
    if (drc_displaydata) {
        drc_displaydata->x = 0;
        drc_displaydata->y = tv_height;
    }
}

static bool SDL_WIIU_GetScanTargetConfiguration(SDL_WiiUScanTarget *target, SDL_Window *window, uint32_t *render_width, uint32_t *render_height, uint32_t *scan_width, uint32_t *scan_height, GX2TVRenderMode *tv_render_mode, GX2DrcRenderMode *drc_render_mode)
{
    if (target->kind == SDL_WIIU_DISPLAY_TV) {
        const int requested_width = window ? SDL_max(window->w, 1) : (int)target->width;
        const int requested_height = window ? SDL_max(window->h, 1) : (int)target->height;
        const SDL_WiiUTVMode *mode = SDL_WIIU_GetTVModeForSize(requested_width, requested_height);

        if (!mode) {
            mode = SDL_WIIU_GetTVModeForRequest(requested_width, requested_height);
        }
        if (!mode) {
            return SDL_SetError("Unable to resolve a supported Wii U TV display size");
        }

        if (render_width) {
            *render_width = (uint32_t)mode->w;
        }
        if (render_height) {
            *render_height = (uint32_t)mode->h;
        }
        if (scan_width) {
            *scan_width = (uint32_t)mode->w;
        }
        if (scan_height) {
            *scan_height = (uint32_t)mode->h;
        }
        if (tv_render_mode) {
            *tv_render_mode = mode->render_mode;
        }
        if (drc_render_mode) {
            *drc_render_mode = target->drc_render_mode;
        }
    } else {
        GX2DrcRenderMode mode = GX2GetSystemDRCMode();

        if (mode == GX2_DRC_RENDER_MODE_DISABLED) {
            mode = GX2_DRC_RENDER_MODE_SINGLE;
        }
        if (render_width) {
            *render_width = target->width;
        }
        if (render_height) {
            *render_height = target->height;
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
    uint32_t tv_render_width = 0;
    uint32_t tv_render_height = 0;
    uint32_t tv_scan_width = 0;
    uint32_t tv_scan_height = 0;
    uint32_t drc_render_width = 0;
    uint32_t drc_render_height = 0;
    uint32_t drc_scan_width = 0;
    uint32_t drc_scan_height = 0;
    GX2TVRenderMode tv_render_mode = GX2_TV_RENDER_MODE_DISABLED;
    GX2DrcRenderMode drc_render_mode = GX2_DRC_RENDER_MODE_DISABLED;
    bool drc_uses_tv_render_buffer = false;
    bool need_rebuild = false;

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
    drc_uses_tv_render_buffer = (active_window_count == 1) && tv_window && (tv_window == drc_window);
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
        SDL_WIIU_UpdateAllWindowGX2Properties(_this);
        return false;
    }

    const bool external_tv = tv_window && ((SDL_WindowData *)tv_window->internal)->external_graphics;
    const bool external_drc = drc_window && ((SDL_WindowData *)drc_window->internal)->external_graphics;
    need_rebuild =
        (driverdata->tv.context_state && (!!driverdata->tv.color_buffer.surface.image == external_tv)) ||
        (!drc_uses_tv_render_buffer && driverdata->drc.context_state && (!!driverdata->drc.color_buffer.surface.image == external_drc)) ||
        driverdata->tv.render_width != tv_render_width ||
        driverdata->tv.render_height != tv_render_height ||
        driverdata->tv.scan_width != tv_scan_width ||
        driverdata->tv.scan_height != tv_scan_height ||
        driverdata->tv.tv_render_mode != tv_render_mode ||
        driverdata->drc_uses_tv_render_buffer != drc_uses_tv_render_buffer ||
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
    driverdata->drc_uses_tv_render_buffer = drc_uses_tv_render_buffer;

    driverdata->drc.render_width = drc_render_width;
    driverdata->drc.render_height = drc_render_height;
    driverdata->drc.scan_width = drc_scan_width;
    driverdata->drc.scan_height = drc_scan_height;
    driverdata->drc.drc_render_mode = drc_render_mode;
    driverdata->pending_present_mask = 0;

    if (need_rebuild && driverdata->foreground_ready) {
        SDL_WIIU_ForegroundReleased(_this);
        SDL_WIIU_ForegroundAcquired(_this);
        if (!driverdata->foreground_ready) {
            SDL_WIIU_UpdateAllWindowGX2Properties(_this);
            return false;
        }
    }

    SDL_WIIU_UpdateAllWindowGX2Properties(_this);
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
    device->SetWindowSize = SDL_WIIU_SetWindowSize;
    device->SyncWindow = SDL_WIIU_SyncWindow;
    device->GetWindowSizeInPixels = SDL_WIIU_GetWindowSizeInPixels;
    device->GetWindowContentScale = SDL_WIIU_GetWindowContentScale;
    device->PumpEvents = SDL_WIIU_PumpEvents;
    device->StartTextInput = SDL_WIIU_StartTextInput;
    device->StopTextInput = SDL_WIIU_StopTextInput;
    device->HasScreenKeyboardSupport = SDL_WIIU_HasScreenKeyboardSupport;
    device->ShowMessageBox = SDL_WIIU_ShowMessageBox;
    device->CreateWindowFramebuffer = SDL_WIIU_CreateWindowFramebuffer;
    device->SetWindowFramebufferVSync = SDL_WIIU_SetWindowFramebufferVSync;
    device->GetWindowFramebufferVSync = SDL_WIIU_GetWindowFramebufferVSync;
    device->UpdateWindowFramebuffer = SDL_WIIU_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SDL_WIIU_DestroyWindowFramebuffer;
    device->WIIU_SwapWindow = SDL_WIIU_GX2SwapWindow;
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
    const SDL_WiiUTVMode *mode = SDL_WIIU_GetDefaultTVMode();

    target->tv_render_mode = mode->render_mode;
    target->width = (uint32_t)mode->w;
    target->height = (uint32_t)mode->h;

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
    target->width = WIIU_DRC_NATIVE_WIDTH;
    target->height = WIIU_DRC_NATIVE_HEIGHT;
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

    SDL_WIIU_InitDisplayMode(&mode, 0, w, h);

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

static void *SDL_WIIU_AllocForegroundMemory(SDL_VideoData *driverdata, uint32_t size, uint32_t alignment, bool prefer_mem1, bool fallback_to_foreground)
{
    void *block = NULL;

    if (alignment < 4) {
        alignment = 4;
    }

    if (prefer_mem1 && driverdata->mem1_heap) {
        block = MEMAllocFromExpHeapEx(driverdata->mem1_heap, size, (int)alignment);
        if (block) {
            return block;
        }
    }

    if (fallback_to_foreground && driverdata->foreground_heap) {
        block = MEMAllocFromExpHeapEx(driverdata->foreground_heap, size, (int)alignment);
    }

    return block;
}

static void SDL_WIIU_BindScanTarget(SDL_WiiUScanTarget *target)
{
    if (target->kind == SDL_WIIU_DISPLAY_TV) {
        GX2SetTVBuffer(target->scan_buffer, target->total_size, target->tv_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE);
        GX2SetTVScale(target->scan_width, target->scan_height);
    } else {
        GX2SetDRCBuffer(target->scan_buffer, target->total_size, target->drc_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE);
        GX2SetDRCScale(target->scan_width, target->scan_height);
    }
}

static bool SDL_WIIU_AllocateScanBuffer(SDL_VideoData *driverdata, SDL_WiiUScanTarget *target)
{
    uint32_t unused_size;

    if (target->kind == SDL_WIIU_DISPLAY_TV) {
        GX2CalcTVSize(target->tv_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE, &target->total_size, &unused_size);
    } else {
        GX2CalcDRCSize(target->drc_render_mode, target->surface_format, GX2_BUFFERING_MODE_DOUBLE, &target->total_size, &unused_size);
    }

    // Scanout is sequential and read once. External graphics reserve MEM1 for
    // repeatedly accessed render/depth/MSAA storage instead.
    SDL_Window *window = SDL_WIIU_GetOutputWindow(SDL_WIIU_video_device, target->kind);
    SDL_WindowData *windowdata = window ? window->internal : NULL;
    const bool external = windowdata && windowdata->external_graphics;
    target->scan_buffer = SDL_WIIU_AllocForegroundMemory(driverdata, target->total_size, GX2_SCAN_BUFFER_ALIGNMENT, !external, true);
    if (!target->scan_buffer) {
        SDL_SetError("Unable to allocate Wii U scan buffer");
        return false;
    }

    SDL_memset(target->scan_buffer, 0, target->total_size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, target->scan_buffer, target->total_size);

    SDL_zero(target->color_buffer);
    target->context_state = NULL;
    SDL_WIIU_BindScanTarget(target);
    return true;
}

static bool SDL_WIIU_AllocateScanTargetRenderBuffer(SDL_VideoData *driverdata, SDL_WiiUScanTarget *target)
{
    GX2Surface *color_surface = &target->color_buffer.surface;

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

    SDL_Window *window = SDL_WIIU_GetOutputWindow(SDL_WIIU_video_device, target->kind);
    SDL_WindowData *windowdata = window ? window->internal : NULL;
    if (!windowdata || !windowdata->external_graphics) {
        color_surface->image = SDL_WIIU_AllocForegroundMemory(driverdata, color_surface->imageSize, color_surface->alignment, true, true);
        if (!color_surface->image) {
            return SDL_SetError("Unable to allocate Wii U color buffer");
        }
        SDL_memset(color_surface->image, 0, color_surface->imageSize);
        GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_COLOR_BUFFER), color_surface->image, color_surface->imageSize);
    }

    target->context_state = MEMAllocFromDefaultHeapEx(sizeof(*target->context_state), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!target->context_state) {
        return SDL_SetError("Unable to allocate Wii U GX2 context state");
    }
    GX2SetupContextStateEx(target->context_state, TRUE);
    GX2SetContextState(target->context_state);
    if (color_surface->image) {
        GX2SetColorBuffer(&target->color_buffer, GX2_RENDER_TARGET_0);
    }
    GX2SetViewport(0.0f, 0.0f, (float)target->render_width, (float)target->render_height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, target->render_width, target->render_height);

    SDL_WIIU_BindScanTarget(target);
    return true;
}

static void SDL_WIIU_ReleaseForegroundRenderers(SDL_VideoDevice *_this, SDL_VideoData *driverdata)
{
    SDL_Window *window;

    for (window = _this->windows; window; window = window->next) {
        int i;

        SDL_WindowData *windowdata = window->internal;
        if (windowdata && windowdata->external_graphics) {
            SDL_WiiUExternalGraphics *graphics = windowdata->external_graphics;
            if (graphics->release) {
                graphics->release(graphics->userdata);
            }
            graphics->color = NULL;
        }
        for (i = 0; i < window->num_renderers; ++i) {
            SDL_WIIU_ReleaseForegroundRendererResources(window->renderers[i], driverdata->mem1_heap, driverdata->foreground_heap);
        }
    }
}

static void SDL_WIIU_DestroyForegroundResources(SDL_VideoDevice *_this, SDL_VideoData *driverdata)
{
    if (!driverdata->foreground_ready && !driverdata->foreground_heap && !driverdata->mem1_heap) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "WiiU video: destroying foreground resources (ready=%d, tv_buffer=%p, drc_buffer=%p)",
                driverdata->foreground_ready ? 1 : 0,
                driverdata->tv.scan_buffer,
                driverdata->drc.scan_buffer);
    driverdata->pending_present_mask = 0;
    SDL_WIIU_ReleaseForegroundRenderers(_this, driverdata);
    SDL_WIIU_ClearScanTarget(&driverdata->tv);
    SDL_WIIU_ClearScanTarget(&driverdata->drc);
    SDL_WIIU_DestroyMEM1Heap(driverdata);
    SDL_WIIU_DestroyForegroundHeap(driverdata);
    driverdata->foreground_ready = false;
    SDL_WIIU_UpdateAllWindowGX2Properties(_this);
}

static void SDL_WIIU_ForegroundAcquired(void *userdata)
{
    SDL_VideoDevice *_this = (SDL_VideoDevice *)userdata;
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    const SDL_WiiUDRCState *drc_state;

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "WiiU video: foreground acquire start (ready=%d, foreground_heap=%p, mem1_heap=%p)",
                driverdata->foreground_ready ? 1 : 0,
                driverdata->foreground_heap,
                driverdata->mem1_heap);
    if (driverdata->foreground_ready || driverdata->foreground_heap || driverdata->mem1_heap) {
        SDL_WIIU_DestroyForegroundResources(_this, driverdata);
    }

    if (driverdata->gx2_initialized) {
        GX2SetTVEnable(FALSE);
        GX2SetDRCEnable(FALSE);
    }

    if (!SDL_WIIU_InitForegroundHeap(driverdata)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "WiiU video: failed to initialize foreground heap: %s", SDL_GetError());
        return;
    }

    if (!SDL_WIIU_InitMEM1Heap(driverdata)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "WiiU video: failed to initialize MEM1 heap: %s", SDL_GetError());
        SDL_WIIU_DestroyForegroundHeap(driverdata);
        return;
    }

    if (!SDL_WIIU_AllocateScanBuffer(driverdata, &driverdata->tv) ||
        !SDL_WIIU_AllocateScanBuffer(driverdata, &driverdata->drc) ||
        !SDL_WIIU_AllocateScanTargetRenderBuffer(driverdata, &driverdata->tv) ||
        (!driverdata->drc_uses_tv_render_buffer && !SDL_WIIU_AllocateScanTargetRenderBuffer(driverdata, &driverdata->drc))) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "WiiU video: failed to allocate scan targets: %s", SDL_GetError());
        SDL_WIIU_DestroyForegroundResources(_this, driverdata);
        return;
    }

    GX2SetDRCConnectCallback(0, SDL_WIIU_DRCConnectCallback);
    drc_state = SDL_WIIU_GetDRCState();
    driverdata->drc_connected = (!drc_state || !drc_state->has_status) ? true : drc_state->connected;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "WiiU video: DRC connection refresh after foreground acquire -> %s",
                driverdata->drc_connected ? "connected" : "disconnected");
    GX2SetSwapInterval(1);
    driverdata->foreground_ready = true;
    ++driverdata->foreground_generation;
    if (driverdata->foreground_generation == 0) {
        driverdata->foreground_generation = 1;
    }
    SDL_WIIU_UpdateAllWindowGX2Properties(_this);
    if (!SDL_WIIU_KPADForegroundAcquired()) {
        SDL_LogError(SDL_LOG_CATEGORY_INPUT, "WiiU video: failed to restart KPAD runtime after foreground reacquire");
    }
    SDL_WIIU_JoystickForegroundAcquired();
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "WiiU video: foreground acquire complete (generation=%u, drc_connected=%d, tv_buffer=%p, drc_buffer=%p, outputs_pending=1)",
                (unsigned int)driverdata->foreground_generation,
                driverdata->drc_connected ? 1 : 0,
                driverdata->tv.scan_buffer,
                driverdata->drc.scan_buffer);
}

static void SDL_WIIU_ForegroundReleased(void *userdata)
{
    SDL_VideoDevice *_this = (SDL_VideoDevice *)userdata;
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "WiiU video: foreground release start (ready=%d, drc_connected=%d)",
                driverdata->foreground_ready ? 1 : 0,
                driverdata->drc_connected ? 1 : 0);
    SDL_ResetKeyboard();
    SDL_WIIU_SuspendMouse();
    SDL_WIIU_SuspendTouch();
    SDL_WIIU_KPADForegroundReleased();
    SDL_WIIU_JoystickForegroundReleased();

    if (!driverdata->foreground_ready && !driverdata->foreground_heap && !driverdata->mem1_heap) {
        return;
    }

    GX2SetDRCConnectCallback(0, NULL);
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
    GX2DrawDone();
    SDL_WIIU_DestroyForegroundResources(_this, driverdata);
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "WiiU video: foreground release complete");
}

static void SDL_WIIU_DRCConnectCallback(uint32_t drc_slot, BOOL attached)
{
    SDL_VideoDevice *_this = SDL_WIIU_video_device;
    SDL_VideoData *driverdata;

    if (!_this) {
        return;
    }

    if (!attached && !SDL_WIIU_IsInForeground()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                    "WiiU video: ignoring transient DRC disconnect while not in foreground");
        return;
    }

    driverdata = (SDL_VideoData *)_this->internal;
    driverdata->drc_connected = attached ? true : false;
    driverdata->pending_present_mask = 0;
    SDL_WIIU_UpdateAllWindowGX2Properties(_this);
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "WiiU video: DRC callback (slot=%u, attached=%d, foreground=%d)",
                (unsigned int)drc_slot,
                attached ? 1 : 0,
                SDL_WIIU_IsInForeground() ? 1 : 0);
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

    SDL_WIIU_video_device = _this;
    driverdata->vpad_initialized = SDL_WIIU_InitDRC();
    SDL_WIIU_SetLifecycleCallbacks(SDL_WIIU_ForegroundAcquired, SDL_WIIU_ForegroundReleased, _this);
    SDL_WIIU_ForegroundAcquired(_this);
    SDL_WIIU_InitTouch();
    driverdata->mouse_initialized = SDL_WIIU_InitMouse(_this);
    driverdata->keyboard_initialized = SDL_WIIU_InitKeyboard(_this);

    return true;
}

static void SDL_WIIU_VideoQuit(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    const bool procui_running = ProcUIIsRunning();
    const bool procui_foreground = !procui_running || ProcUIInForeground();
    const bool can_use_foreground_only_shutdown = driverdata->foreground_ready && procui_foreground;

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "WiiU video: video quit start (foreground_ready=%d, procui_running=%d, procui_foreground=%d, gx2_initialized=%d, vpad_initialized=%d)",
                driverdata->foreground_ready ? 1 : 0,
                procui_running ? 1 : 0,
                procui_foreground ? 1 : 0,
                driverdata->gx2_initialized ? 1 : 0,
                driverdata->vpad_initialized ? 1 : 0);

    SDL_WIIU_video_device = NULL;
    SDL_WIIU_SetLifecycleCallbacks(NULL, NULL, NULL);

    if (!can_use_foreground_only_shutdown) {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                    "WiiU video: skipping foreground-only video shutdown because foreground is no longer owned during exit");
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "WiiU video: running foreground-owned video shutdown");
    SDL_WIIU_QuitFramebufferRenderer(_this);
    SDL_WIIU_QuitKeyboard();
    SDL_WIIU_QuitTextUI();
    if (driverdata->mouse_initialized) {
        SDL_WIIU_QuitMouse();
        driverdata->mouse_initialized = false;
    }
    SDL_WIIU_QuitTouch();

    if (driverdata->vpad_initialized) {
        SDL_WIIU_QuitDRC();
        driverdata->vpad_initialized = false;
    }

    GX2DrawDone();
    GX2SetTVEnable(FALSE);
    GX2SetDRCEnable(FALSE);
    GX2SetDRCConnectCallback(0, NULL);
    SDL_WIIU_DestroyForegroundResources(_this, driverdata);

    if (driverdata->gx2_initialized) {
        GX2RSetAllocator(NULL, NULL);
        GX2Shutdown();
        driverdata->gx2_initialized = false;
    }

    if (driverdata->command_buffer_pool) {
        MEMFreeToDefaultHeap(driverdata->command_buffer_pool);
        driverdata->command_buffer_pool = NULL;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "WiiU video: video quit complete");
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
    SDL_DisplayData *displaydata = display->internal;
    SDL_WiiUTVMode modes[3];
    SDL_DisplayMode mode;
    int count;
    int i;

    (void)_this;

    if (displaydata->kind == SDL_WIIU_DISPLAY_TV) {
        count = SDL_WIIU_GetSupportedTVModes(modes, (int)SDL_arraysize(modes));
        for (i = 0; i < count; ++i) {
            SDL_WIIU_InitDisplayMode(&mode, display->id, modes[i].w, modes[i].h);
            if (!SDL_AddFullscreenDisplayMode(display, &mode)) {
                return false;
            }
        }
        return true;
    }

    SDL_WIIU_InitDisplayMode(&mode, display->id, WIIU_DRC_NATIVE_WIDTH, WIIU_DRC_NATIVE_HEIGHT);
    return SDL_AddFullscreenDisplayMode(display, &mode);
}

static bool SDL_WIIU_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    SDL_DisplayData *displaydata = display->internal;
    SDL_VideoDisplay *tv_display;
    SDL_Window *window;
    int target_w;
    int target_h;
    int tv_height;

    if (!mode) {
        mode = &display->desktop_mode;
    }

    if (displaydata->kind == SDL_WIIU_DISPLAY_TV) {
        if (!SDL_WIIU_GetTVModeForSize(mode->w, mode->h)) {
            return SDL_SetError("Unsupported Wii U TV display size for the current scan mode");
        }
    } else if (mode->w != WIIU_DRC_NATIVE_WIDTH || mode->h != WIIU_DRC_NATIVE_HEIGHT) {
        return SDL_SetError("Wii U GamePad only supports the native 854x480 display mode");
    }

    if (mode->format != display->desktop_mode.format) {
        return SDL_SetError("Wii U only supports RGBA8888 display modes");
    }

    target_w = mode->w;
    target_h = mode->h;
    tv_display = SDL_GetVideoDisplay(driverdata->tv_display);
    tv_height = (tv_display && tv_display->current_mode) ? tv_display->current_mode->h : (int)driverdata->tv.height;
    if (displaydata->kind == SDL_WIIU_DISPLAY_TV) {
        SDL_WIIU_UpdateDisplayLayout(driverdata, target_h);
    } else {
        SDL_WIIU_UpdateDisplayLayout(driverdata, tv_height);
    }

    window = SDL_WIIU_GetWindowForDisplay(_this, display->id);
    if (window) {
        SDL_WIIU_SetWindowSizeInternal(window, target_w, target_h);
        window->surface_valid = false;
    }

    return SDL_WIIU_UpdateOutputWindows(_this);
}

static bool SDL_WIIU_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_Window *other = NULL;
    SDL_DisplayID target_display_id;
    SDL_VideoDisplay *display;
    SDL_DisplayData *displaydata;
    SDL_WindowData *windowdata;
    int active_window_count = 0;
    int requested_w;
    int requested_h;
    int supported_w;
    int supported_h;
    const SDL_DisplayMode *target_mode = NULL;

    for (SDL_Window *candidate = _this->windows; candidate; candidate = candidate->next) {
        if (candidate != window && SDL_WIIU_IsActiveWindow(candidate)) {
            other = candidate;
            ++active_window_count;
        }
    }

    if (active_window_count >= 2) {
        return SDL_SetError("Wii U video supports at most two top-level windows");
    }

    target_display_id = window->pending_displayID ? window->pending_displayID : window->displayID;
    if (!target_display_id) {
        target_display_id = SDL_GetPrimaryDisplay();
    }

    displaydata = SDL_GetDisplayDriverData(target_display_id);
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

    windowdata->display_id = target_display_id;
    windowdata->display_kind = displaydata->kind;
    windowdata->role = SDL_WIIU_WINDOW_ROLE_SHARED;
    windowdata->framebuffer_vsync = 1;
    windowdata->external_graphics = SDL_GetPointerProperty(create_props, SDL_PROP_WINDOW_CREATE_WIIU_EXTERNAL_GRAPHICS_POINTER, NULL);
    window->internal = windowdata;
    window->displayID = target_display_id;

    requested_w = (int)SDL_GetNumberProperty(create_props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window->w);
    requested_h = (int)SDL_GetNumberProperty(create_props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window->h);
    SDL_WIIU_GetSupportedWindowSize(displaydata->kind, requested_w, requested_h, &supported_w, &supported_h);
    SDL_WIIU_SetWindowSizeInternal(window, supported_w, supported_h);

    display = SDL_GetVideoDisplay(target_display_id);
    if (!display) {
        window->internal = NULL;
        SDL_free(windowdata);
        return SDL_SetError("Unable to resolve the SDL display for the window");
    }
    target_mode = SDL_WIIU_GetExactFullscreenDisplayMode(display, supported_w, supported_h);
    if (target_mode && target_mode != display->current_mode) {
        if (!SDL_SetDisplayModeForDisplay(display, (SDL_DisplayMode *)target_mode)) {
            window->internal = NULL;
            SDL_free(windowdata);
            return false;
        }
    }

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
        SDL_WindowData *windowdata = window->internal;
        if (windowdata->external_graphics && windowdata->external_graphics->release) {
            GX2DrawDone();
            windowdata->external_graphics->release(windowdata->external_graphics->userdata);
            windowdata->external_graphics->color = NULL;
        }
        SDL_free(window->internal);
        window->internal = NULL;
    }

    SDL_WIIU_UpdateOutputWindows(_this);

    for (SDL_Window *candidate = _this->windows; candidate; candidate = candidate->next) {
        if (SDL_WIIU_IsActiveWindow(candidate)) {
            SDL_SetKeyboardFocus(candidate);
            break;
        }
    }
}

static SDL_DisplayID SDL_WIIU_GetDisplayForWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *windowdata = window->internal;
    (void)_this;

    if (!windowdata) {
        return window->pending_displayID;
    }
    return windowdata->display_id;
}

static void SDL_WIIU_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    const int requested_w = window->last_size_pending ? window->pending.w : window->w;
    const int requested_h = window->last_size_pending ? window->pending.h : window->h;

    SDL_WIIU_ApplyWindowSize(_this, window, requested_w, requested_h);
}

static bool SDL_WIIU_SyncWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)window;
    return SDL_WIIU_UpdateOutputWindows(_this);
}

static void SDL_WIIU_GetWindowSizeInPixels(SDL_VideoDevice *_this, SDL_Window *window, int *w, int *h)
{
    (void)_this;

    if (window && (window->flags & SDL_WINDOW_FULLSCREEN) != 0) {
        const SDL_DisplayMode *fullscreen_mode = SDL_GetWindowFullscreenMode(window);
        if (fullscreen_mode) {
            if (w) {
                *w = fullscreen_mode->w;
            }
            if (h) {
                *h = fullscreen_mode->h;
            }
            return;
        }
    }

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
    (void)window;
    return SDL_WIIU_ShowTextInput(_this, props);
}

static bool SDL_WIIU_StopTextInput(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    (void)window;
    return true;
}

#endif // SDL_VIDEO_DRIVER_WIIU
