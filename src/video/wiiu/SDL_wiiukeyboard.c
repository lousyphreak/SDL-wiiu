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

#include "../../core/wiiu/SDL_wiiulifecycle.h"
#include "../../events/SDL_keyboard_c.h"

#include <nsyskbd/nsyskbd.h>

static SDL_VideoDevice *SDL_WIIU_keyboard_device;
static bool SDL_WIIU_keyboard_connected[4];

static SDL_KeyboardID SDL_WIIU_GetKeyboardID(uint8_t channel)
{
    return (SDL_KeyboardID)(channel + 1);
}

static void SDL_WIIU_AddKeyboardChannel(uint8_t channel)
{
    if (channel < SDL_arraysize(SDL_WIIU_keyboard_connected) && !SDL_WIIU_keyboard_connected[channel]) {
        SDL_WIIU_keyboard_connected[channel] = true;
        SDL_AddKeyboard(SDL_WIIU_GetKeyboardID(channel), "Wii U Keyboard");
    }
}

static void SDL_WIIU_AttachCallback(KBDAttachEvent *event)
{
    SDL_WIIU_AddKeyboardChannel(event->channel);
}

static void SDL_WIIU_DetachCallback(KBDAttachEvent *event)
{
    if (event->channel < SDL_arraysize(SDL_WIIU_keyboard_connected) && SDL_WIIU_keyboard_connected[event->channel]) {
        SDL_WIIU_keyboard_connected[event->channel] = false;
        SDL_RemoveKeyboard(SDL_WIIU_GetKeyboardID(event->channel));
    }
}

static void SDL_WIIU_KeyCallback(KBDKeyEvent *event)
{
    SDL_KeyboardID keyboard_id = SDL_WIIU_GetKeyboardID(event->channel);
    SDL_Scancode scancode = (event->hidCode < SDL_SCANCODE_COUNT) ? (SDL_Scancode)event->hidCode : SDL_SCANCODE_UNKNOWN;

    if (!SDL_WIIU_IsInForeground()) {
        return;
    }

    SDL_WIIU_AddKeyboardChannel(event->channel);
    SDL_SendKeyboardKey(0, keyboard_id, event->hidCode, scancode, event->isPressedDown ? true : false);

    if (event->isPressedDown && event->asUTF16Character != 0) {
        char text[5];
        char *end;

        SDL_zeroa(text);
        end = SDL_UCS4ToUTF8(event->asUTF16Character, text);
        if (end) {
            *end = '\0';
            SDL_SendKeyboardText(text);
        }
    }
}

bool SDL_WIIU_InitKeyboard(SDL_VideoDevice *_this)
{
    KDBError error;

    SDL_WIIU_keyboard_device = _this;
    SDL_zeroa(SDL_WIIU_keyboard_connected);

    error = KBDInit(0, SDL_WIIU_AttachCallback, SDL_WIIU_DetachCallback, SDL_WIIU_KeyCallback);
    if (error == KDB_ERROR_ALREADY_INITIALIZED) {
        error = KBDSetup(SDL_WIIU_AttachCallback, SDL_WIIU_DetachCallback, SDL_WIIU_KeyCallback);
    }

    return error == KDB_ERROR_NONE;
}

void SDL_WIIU_QuitKeyboard(void)
{
    KBDTeardown();

    for (int i = 0; i < SDL_arraysize(SDL_WIIU_keyboard_connected); ++i) {
        if (SDL_WIIU_keyboard_connected[i]) {
            SDL_RemoveKeyboard(SDL_WIIU_GetKeyboardID((uint8_t)i));
            SDL_WIIU_keyboard_connected[i] = false;
        }
    }

    SDL_WIIU_keyboard_device = NULL;
}

#endif // SDL_VIDEO_DRIVER_WIIU
