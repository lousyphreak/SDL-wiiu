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

#include "../../core/wiiu/SDL_wiiukpad.h"
#include "../../events/SDL_mouse_c.h"

typedef struct SDL_WiiUMouseState
{
    int active_channel;
    bool has_position;
    bool left_down;
    bool right_down;
    bool middle_down;
    SDL_Window *window;
    float x;
    float y;
} SDL_WiiUMouseState;

static SDL_WiiUMouseState SDL_WIIU_mouse_state;

static void SDL_WIIU_SendMouseButtonIfChanged(Uint64 timestamp, SDL_Window *window, Uint8 button, bool down, bool *state)
{
    if (down == *state) {
        return;
    }

    SDL_SendMouseButton(timestamp, window, SDL_DEFAULT_MOUSE_ID, button, down);
    *state = down;
}

static void SDL_WIIU_ResetMouseButtons(Uint64 timestamp, SDL_Window *window)
{
    SDL_WIIU_SendMouseButtonIfChanged(timestamp, window, SDL_BUTTON_LEFT, false, &SDL_WIIU_mouse_state.left_down);
    SDL_WIIU_SendMouseButtonIfChanged(timestamp, window, SDL_BUTTON_RIGHT, false, &SDL_WIIU_mouse_state.right_down);
    SDL_WIIU_SendMouseButtonIfChanged(timestamp, window, SDL_BUTTON_MIDDLE, false, &SDL_WIIU_mouse_state.middle_down);
}

static SDL_Window *SDL_WIIU_GetPointerWindow(SDL_VideoDevice *_this)
{
    return SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_TV);
}

static bool SDL_WIIU_ProjectMouseForWindow(SDL_VideoDevice *_this, SDL_Window *window, const KPADStatus *status, float *x, float *y)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    SDL_Rect content_rect;
    KPADRect projection_rect;
    KPADVec2D projected;
    int src_w;
    int src_h;

    if (!window || !status) {
        return false;
    }

    SDL_WIIU_GetWindowPresentationSize(_this, window, &src_w, &src_h);
    SDL_WIIU_GetPresentationRect(src_w, src_h, (int)driverdata->tv.width, (int)driverdata->tv.height, &content_rect);
    if (content_rect.w <= 0 || content_rect.h <= 0) {
        return false;
    }

    projection_rect.topLeft.x = (float)content_rect.x;
    projection_rect.topLeft.y = (float)content_rect.y;
    projection_rect.bottomRight.x = (float)(content_rect.x + content_rect.w - 1);
    projection_rect.bottomRight.y = (float)(content_rect.y + content_rect.h - 1);
    KPADGetProjectionPos(&projected, &status->pos, &projection_rect, 1.0f);

    if (src_w <= 1) {
        *x = 0.0f;
    } else {
        const float normalized_x = ((projected.x - (float)content_rect.x) / (float)(content_rect.w - 1));
        *x = SDL_clamp(normalized_x, 0.0f, 1.0f) * (float)(src_w - 1);
    }

    if (src_h <= 1) {
        *y = 0.0f;
    } else {
        const float normalized_y = ((projected.y - (float)content_rect.y) / (float)(content_rect.h - 1));
        *y = SDL_clamp(normalized_y, 0.0f, 1.0f) * (float)(src_h - 1);
    }

    return true;
}

bool SDL_WIIU_InitMouse(SDL_VideoDevice *_this)
{
    (void)_this;

    if (!SDL_WIIU_InitKPAD()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "WiiU mouse: failed to initialize KPAD runtime");
        return false;
    }

    SDL_zero(SDL_WIIU_mouse_state);
    SDL_WIIU_mouse_state.active_channel = -1;
    SDL_AddMouse(SDL_DEFAULT_MOUSE_ID, "Nintendo Wii Remote IR");
    return true;
}

void SDL_WIIU_QuitMouse(void)
{
    SDL_WIIU_SuspendMouse();
    SDL_RemoveMouse(SDL_DEFAULT_MOUSE_ID);
    SDL_WIIU_QuitKPAD();
}

void SDL_WIIU_SuspendMouse(void)
{
    const Uint64 timestamp = SDL_GetTicksNS();

    SDL_WIIU_ResetMouseButtons(timestamp, SDL_WIIU_mouse_state.window);

    SDL_zero(SDL_WIIU_mouse_state);
    SDL_WIIU_mouse_state.active_channel = -1;
}

void SDL_WIIU_PollMouse(SDL_VideoDevice *_this)
{
    const SDL_WiiUKPADState *active_state = NULL;
    SDL_Window *window = SDL_WIIU_GetPointerWindow(_this);
    Uint64 timestamp = SDL_GetTicksNS();
    int active_channel = -1;
    int channel;

    for (channel = 0; channel < WIIU_MAX_KPAD_CHANNELS; ++channel) {
        WPADExtensionType extension = WPAD_EXT_DEV_NOT_FOUND;

        if (WPADProbe((WPADChan)channel, &extension) != WPAD_ERROR_NONE) {
            continue;
        }

        if (extension != WPAD_EXT_CORE && extension != WPAD_EXT_NUNCHUK) {
            continue;
        }

        if (!SDL_WIIU_EnsureKPADDataFormat(channel,
                                           (extension == WPAD_EXT_NUNCHUK) ? WPAD_FMT_NUNCHUK_ACC_DPD : WPAD_FMT_CORE_ACC_DPD,
                                           true)) {
            continue;
        }

        active_state = SDL_WIIU_GetKPADState(channel);
        if (active_state &&
            active_state->connected &&
            active_state->has_status &&
            active_state->status.extensionType == extension) {
            active_channel = channel;
            break;
        }
    }

    if (!active_state || active_channel < 0) {
        SDL_WIIU_SuspendMouse();
        return;
    }

    if (SDL_WIIU_mouse_state.active_channel >= 0 &&
        SDL_WIIU_mouse_state.active_channel != active_channel) {
        SDL_WIIU_SuspendMouse();
    }

    SDL_WIIU_mouse_state.active_channel = active_channel;
    timestamp = active_state->timestamp_ns;

    if (window && active_state->status.posValid > 0) {
        float x = 0.0f;
        float y = 0.0f;

        if (SDL_WIIU_ProjectMouseForWindow(_this, window, &active_state->status, &x, &y)) {
            if (!SDL_WIIU_mouse_state.has_position ||
                SDL_WIIU_mouse_state.window != window ||
                SDL_WIIU_mouse_state.x != x ||
                SDL_WIIU_mouse_state.y != y) {
                SDL_SendMouseMotion(timestamp, window, SDL_DEFAULT_MOUSE_ID, false, x, y);
            }

            SDL_WIIU_mouse_state.has_position = true;
            SDL_WIIU_mouse_state.window = window;
            SDL_WIIU_mouse_state.x = x;
            SDL_WIIU_mouse_state.y = y;
        }
    } else {
        SDL_WIIU_mouse_state.has_position = false;
        if (!SDL_WIIU_mouse_state.window) {
            SDL_WIIU_mouse_state.window = window;
        }
    }

    SDL_WIIU_SendMouseButtonIfChanged(timestamp,
                                      SDL_WIIU_mouse_state.window,
                                      SDL_BUTTON_LEFT,
                                      (active_state->status.hold & WPAD_BUTTON_B) != 0,
                                      &SDL_WIIU_mouse_state.left_down);
    SDL_WIIU_SendMouseButtonIfChanged(timestamp,
                                      SDL_WIIU_mouse_state.window,
                                      SDL_BUTTON_RIGHT,
                                      SDL_WIIU_mouse_state.has_position && (active_state->status.hold & WPAD_BUTTON_A) != 0,
                                      &SDL_WIIU_mouse_state.right_down);
    SDL_WIIU_SendMouseButtonIfChanged(timestamp,
                                      SDL_WIIU_mouse_state.window,
                                      SDL_BUTTON_MIDDLE,
                                      (active_state->status.hold & WPAD_BUTTON_Z) != 0,
                                      &SDL_WIIU_mouse_state.middle_down);
}

#endif // SDL_VIDEO_DRIVER_WIIU
