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

#include "../../events/SDL_touch_c.h"

#include <vpad/input.h>

#define WIIU_TOUCH_ID     1
#define WIIU_TOUCH_FINGER 1

typedef struct SDL_WiiUTouchState
{
    bool pressed;
    SDL_Window *window;
    float x;
    float y;
} SDL_WiiUTouchState;

static SDL_WiiUTouchState SDL_WIIU_touch_state;

static SDL_Window *SDL_WIIU_GetValidTouchWindow(SDL_Window *window)
{
    if (window && SDL_ObjectValid(window, SDL_OBJECT_TYPE_WINDOW) && !window->is_destroying) {
        return window;
    }
    return NULL;
}

static void SDL_WIIU_SendTouchRelease(void)
{
    if (!SDL_WIIU_touch_state.pressed) {
        return;
    }

    SDL_SendTouch(0, WIIU_TOUCH_ID, WIIU_TOUCH_FINGER,
                  SDL_WIIU_GetValidTouchWindow(SDL_WIIU_touch_state.window),
                  SDL_EVENT_FINGER_UP,
                  SDL_WIIU_touch_state.x, SDL_WIIU_touch_state.y, 0.0f);
    SDL_zero(SDL_WIIU_touch_state);
}

static bool SDL_WIIU_NormalizeTouchForWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoData *driverdata, const VPADTouchData *touch, float *x, float *y)
{
    SDL_Rect content_rect;
    int src_w;
    int src_h;

    if (!window) {
        return false;
    }

    SDL_WIIU_GetWindowPresentationSize(_this, window, &src_w, &src_h);
    SDL_WIIU_GetPresentationRect(src_w, src_h, (int)driverdata->drc.width, (int)driverdata->drc.height, &content_rect);
    if (content_rect.w <= 0 || content_rect.h <= 0) {
        return false;
    }

    if ((int)touch->x < content_rect.x ||
        (int)touch->x >= (content_rect.x + content_rect.w) ||
        (int)touch->y < content_rect.y ||
        (int)touch->y >= (content_rect.y + content_rect.h)) {
        return false;
    }

    if (content_rect.w <= 1) {
        *x = 0.5f;
    } else {
        *x = ((float)touch->x - (float)content_rect.x) / (float)(content_rect.w - 1);
    }

    if (content_rect.h <= 1) {
        *y = 0.5f;
    } else {
        *y = ((float)touch->y - (float)content_rect.y) / (float)(content_rect.h - 1);
    }

    *x = SDL_clamp(*x, 0.0f, 1.0f);
    *y = SDL_clamp(*y, 0.0f, 1.0f);
    return true;
}

void SDL_WIIU_InitTouch(void)
{
    SDL_AddTouch(WIIU_TOUCH_ID, SDL_TOUCH_DEVICE_DIRECT, "Wii U GamePad Touch");
    SDL_zero(SDL_WIIU_touch_state);
}

void SDL_WIIU_QuitTouch(void)
{
    SDL_WIIU_SendTouchRelease();
    SDL_DelTouch(WIIU_TOUCH_ID);
    SDL_zero(SDL_WIIU_touch_state);
}

void SDL_WIIU_PollTouch(SDL_VideoDevice *_this)
{
    SDL_VideoData *driverdata = (SDL_VideoData *)_this->internal;
    VPADStatus status;
    VPADTouchData touch;
    VPADReadError error = VPAD_READ_SUCCESS;
    SDL_Window *window = NULL;
    bool pressed = false;
    float x = 0.0f;
    float y = 0.0f;

    SDL_zero(status);
    SDL_zero(touch);

    if (driverdata->drc_connected && VPADRead(VPAD_CHAN_0, &status, 1, &error) > 0 && error == VPAD_READ_SUCCESS) {
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &touch, &status.tpNormal);
        window = SDL_WIIU_GetOutputWindow(_this, SDL_WIIU_DISPLAY_DRC);
        pressed = (touch.touched != 0) &&
                  (touch.validity == VPAD_VALID) &&
                  SDL_WIIU_NormalizeTouchForWindow(_this, window, driverdata, &touch, &x, &y);
    }

    if (pressed && SDL_WIIU_touch_state.pressed &&
        SDL_WIIU_GetValidTouchWindow(SDL_WIIU_touch_state.window) != SDL_WIIU_GetValidTouchWindow(window)) {
        SDL_WIIU_SendTouchRelease();
    }

    if (!pressed) {
        SDL_WIIU_SendTouchRelease();
        return;
    }

    if (!SDL_WIIU_touch_state.pressed) {
        SDL_SendTouch(0, WIIU_TOUCH_ID, WIIU_TOUCH_FINGER, window,
                      SDL_EVENT_FINGER_DOWN, x, y, 1.0f);
        SDL_WIIU_touch_state.pressed = true;
        SDL_WIIU_touch_state.window = window;
        SDL_WIIU_touch_state.x = x;
        SDL_WIIU_touch_state.y = y;
        return;
    }

    SDL_WIIU_touch_state.window = window;
    SDL_WIIU_touch_state.x = x;
    SDL_WIIU_touch_state.y = y;
    SDL_SendTouchMotion(0, WIIU_TOUCH_ID, WIIU_TOUCH_FINGER, window, x, y, 1.0f);
}

#endif // SDL_VIDEO_DRIVER_WIIU
