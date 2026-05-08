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

#ifdef SDL_JOYSTICK_WIIU

#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"
#include "../../core/wiiu/SDL_wiiudrc.h"

#define WIIU_DRC_INSTANCE_ID 1
#define WIIU_DRC_NAME "Nintendo Wii U GamePad"

enum
{
    WIIU_DRC_AXIS_LEFTX,
    WIIU_DRC_AXIS_LEFTY,
    WIIU_DRC_AXIS_RIGHTX,
    WIIU_DRC_AXIS_RIGHTY,
    WIIU_DRC_AXIS_COUNT
};

enum
{
    WIIU_DRC_BUTTON_A,
    WIIU_DRC_BUTTON_B,
    WIIU_DRC_BUTTON_X,
    WIIU_DRC_BUTTON_Y,
    WIIU_DRC_BUTTON_LEFT,
    WIIU_DRC_BUTTON_RIGHT,
    WIIU_DRC_BUTTON_UP,
    WIIU_DRC_BUTTON_DOWN,
    WIIU_DRC_BUTTON_ZL,
    WIIU_DRC_BUTTON_ZR,
    WIIU_DRC_BUTTON_L,
    WIIU_DRC_BUTTON_R,
    WIIU_DRC_BUTTON_PLUS,
    WIIU_DRC_BUTTON_MINUS,
    WIIU_DRC_BUTTON_HOME,
    WIIU_DRC_BUTTON_SYNC,
    WIIU_DRC_BUTTON_STICK_R,
    WIIU_DRC_BUTTON_STICK_L,
    WIIU_DRC_BUTTON_TV,
    WIIU_DRC_BUTTON_COUNT
};

typedef struct joystick_hwdata
{
    Uint32 buttons;
    Sint16 axes[WIIU_DRC_AXIS_COUNT];
    bool touch_down;
    float touch_x;
    float touch_y;
    float touch_pressure;
    SDL_PowerState battery_state;
    int battery_percent;
} joystick_hwdata;

static const Uint32 SDL_WIIU_button_map[WIIU_DRC_BUTTON_COUNT] = {
    VPAD_BUTTON_A,
    VPAD_BUTTON_B,
    VPAD_BUTTON_X,
    VPAD_BUTTON_Y,
    VPAD_BUTTON_LEFT,
    VPAD_BUTTON_RIGHT,
    VPAD_BUTTON_UP,
    VPAD_BUTTON_DOWN,
    VPAD_BUTTON_ZL,
    VPAD_BUTTON_ZR,
    VPAD_BUTTON_L,
    VPAD_BUTTON_R,
    VPAD_BUTTON_PLUS,
    VPAD_BUTTON_MINUS,
    VPAD_BUTTON_HOME,
    VPAD_BUTTON_SYNC,
    VPAD_BUTTON_STICK_R,
    VPAD_BUTTON_STICK_L,
    VPAD_BUTTON_TV
};

static Sint16 SDL_WIIU_ConvertStickAxis(float value, bool invert)
{
    const float normalized = SDL_clamp(invert ? -value : value, -1.0f, 1.0f);

    if (normalized <= -1.0f) {
        return SDL_JOYSTICK_AXIS_MIN;
    }
    if (normalized >= 1.0f) {
        return SDL_JOYSTICK_AXIS_MAX;
    }
    return (Sint16)SDL_lroundf(normalized * (normalized < 0.0f ? 32768.0f : 32767.0f));
}

static void SDL_WIIU_UpdateBattery(SDL_Joystick *joystick, joystick_hwdata *hwdata, const SDL_WiiUDRCState *state)
{
    SDL_PowerState battery_state = SDL_POWERSTATE_UNKNOWN;
    int battery_percent = -1;

    if (state && state->connected && state->has_status) {
        battery_state = SDL_POWERSTATE_ON_BATTERY;
        battery_percent = SDL_min(((int)state->status.battery * 100 + 96) / 192, 100);
    }

    if (battery_state != hwdata->battery_state || battery_percent != hwdata->battery_percent) {
        SDL_SendJoystickPowerInfo(joystick, battery_state, battery_percent);
        hwdata->battery_state = battery_state;
        hwdata->battery_percent = battery_percent;
    }
}

static void SDL_WIIU_UpdateTouchpad(SDL_Joystick *joystick, joystick_hwdata *hwdata, Uint64 timestamp, const SDL_WiiUDRCState *state)
{
    bool down = false;
    float x = 0.0f;
    float y = 0.0f;
    float pressure = 0.0f;

    if (state && state->connected && state->has_status &&
        state->touch.touched != 0 && state->touch.validity == VPAD_VALID) {
        down = true;
        x = SDL_clamp((float)state->touch.x / 853.0f, 0.0f, 1.0f);
        y = SDL_clamp((float)state->touch.y / 479.0f, 0.0f, 1.0f);
        pressure = 1.0f;
    }

    if (down != hwdata->touch_down ||
        (down && (x != hwdata->touch_x || y != hwdata->touch_y || pressure != hwdata->touch_pressure))) {
        SDL_SendJoystickTouchpad(timestamp, joystick, 0, 0, down, x, y, pressure);
        hwdata->touch_down = down;
        hwdata->touch_x = x;
        hwdata->touch_y = y;
        hwdata->touch_pressure = pressure;
    }
}

static bool WIIU_JoystickInit(void)
{
    if (!SDL_WIIU_InitDRC()) {
        return false;
    }

    SDL_PrivateJoystickAdded(WIIU_DRC_INSTANCE_ID);
    return true;
}

static int WIIU_JoystickGetCount(void)
{
    return 1;
}

static void WIIU_JoystickDetect(void)
{
}

static bool WIIU_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    return false;
}

static const char *WIIU_JoystickGetDeviceName(int device_index)
{
    return WIIU_DRC_NAME;
}

static const char *WIIU_JoystickGetDevicePath(int device_index)
{
    return NULL;
}

static int WIIU_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    return -1;
}

static int WIIU_JoystickGetDevicePlayerIndex(int device_index)
{
    return 0;
}

static void WIIU_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
}

static SDL_GUID WIIU_JoystickGetDeviceGUID(int device_index)
{
    return SDL_CreateJoystickGUIDForName(WIIU_DRC_NAME);
}

static SDL_JoystickID WIIU_JoystickGetDeviceInstanceID(int device_index)
{
    return WIIU_DRC_INSTANCE_ID;
}

static bool WIIU_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    joystick_hwdata *hwdata = (joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));

    if (!hwdata) {
        return false;
    }

    joystick->hwdata = hwdata;
    joystick->nbuttons = WIIU_DRC_BUTTON_COUNT;
    joystick->naxes = WIIU_DRC_AXIS_COUNT;
    joystick->nhats = 0;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    SDL_PrivateJoystickAddTouchpad(joystick, 1);
    SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);
    return true;
}

static bool WIIU_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    if (low_frequency_rumble || high_frequency_rumble) {
        uint8_t pattern[1] = { 0xFF };
        return VPADControlMotor(VPAD_CHAN_0, pattern, (uint8_t)SDL_arraysize(pattern)) >= 0;
    }

    VPADStopMotor(VPAD_CHAN_0);
    return true;
}

static bool WIIU_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static bool WIIU_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool WIIU_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool WIIU_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void WIIU_JoystickUpdate(SDL_Joystick *joystick)
{
    joystick_hwdata *hwdata = joystick->hwdata;
    const SDL_WiiUDRCState *state = SDL_WIIU_GetDRCState();
    const Uint64 timestamp = state ? state->timestamp_ns : SDL_GetTicksNS();
    const Uint32 buttons = (state && state->connected && state->has_status) ? state->status.hold : 0;
    const Sint16 axes[WIIU_DRC_AXIS_COUNT] = {
        (state && state->connected && state->has_status) ? SDL_WIIU_ConvertStickAxis(state->status.leftStick.x, false) : 0,
        (state && state->connected && state->has_status) ? SDL_WIIU_ConvertStickAxis(state->status.leftStick.y, true) : 0,
        (state && state->connected && state->has_status) ? SDL_WIIU_ConvertStickAxis(state->status.rightStick.x, false) : 0,
        (state && state->connected && state->has_status) ? SDL_WIIU_ConvertStickAxis(state->status.rightStick.y, true) : 0
    };
    int i;

    if (!hwdata) {
        return;
    }

    for (i = 0; i < SDL_arraysize(axes); ++i) {
        if (axes[i] != hwdata->axes[i]) {
            SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, axes[i]);
            hwdata->axes[i] = axes[i];
        }
    }

    for (i = 0; i < SDL_arraysize(SDL_WIIU_button_map); ++i) {
        const Uint32 mask = SDL_WIIU_button_map[i];
        const bool down = (buttons & mask) != 0;
        const bool was_down = (hwdata->buttons & mask) != 0;

        if (down != was_down) {
            SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, down);
        }
    }

    hwdata->buttons = buttons;
    SDL_WIIU_UpdateTouchpad(joystick, hwdata, timestamp, state);
    SDL_WIIU_UpdateBattery(joystick, hwdata, state);
}

static void WIIU_JoystickClose(SDL_Joystick *joystick)
{
    if (joystick->hwdata) {
        VPADStopMotor(VPAD_CHAN_0);
        SDL_free(joystick->hwdata);
        joystick->hwdata = NULL;
    }
}

static void WIIU_JoystickQuit(void)
{
    SDL_WIIU_QuitDRC();
}

static bool WIIU_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    *out = (SDL_GamepadMapping){
        .a = { EMappingKind_Button, WIIU_DRC_BUTTON_A },
        .b = { EMappingKind_Button, WIIU_DRC_BUTTON_B },
        .x = { EMappingKind_Button, WIIU_DRC_BUTTON_X },
        .y = { EMappingKind_Button, WIIU_DRC_BUTTON_Y },
        .back = { EMappingKind_Button, WIIU_DRC_BUTTON_MINUS },
        .guide = { EMappingKind_Button, WIIU_DRC_BUTTON_HOME },
        .start = { EMappingKind_Button, WIIU_DRC_BUTTON_PLUS },
        .leftstick = { EMappingKind_Button, WIIU_DRC_BUTTON_STICK_L },
        .rightstick = { EMappingKind_Button, WIIU_DRC_BUTTON_STICK_R },
        .leftshoulder = { EMappingKind_Button, WIIU_DRC_BUTTON_L },
        .rightshoulder = { EMappingKind_Button, WIIU_DRC_BUTTON_R },
        .dpup = { EMappingKind_Button, WIIU_DRC_BUTTON_UP },
        .dpdown = { EMappingKind_Button, WIIU_DRC_BUTTON_DOWN },
        .dpleft = { EMappingKind_Button, WIIU_DRC_BUTTON_LEFT },
        .dpright = { EMappingKind_Button, WIIU_DRC_BUTTON_RIGHT },
        .misc1 = { EMappingKind_Button, WIIU_DRC_BUTTON_TV },
        .misc2 = { EMappingKind_Button, WIIU_DRC_BUTTON_SYNC },
        .leftx = { EMappingKind_Axis, WIIU_DRC_AXIS_LEFTX },
        .lefty = { EMappingKind_Axis, WIIU_DRC_AXIS_LEFTY },
        .rightx = { EMappingKind_Axis, WIIU_DRC_AXIS_RIGHTX },
        .righty = { EMappingKind_Axis, WIIU_DRC_AXIS_RIGHTY },
        .lefttrigger = { EMappingKind_Button, WIIU_DRC_BUTTON_ZL },
        .righttrigger = { EMappingKind_Button, WIIU_DRC_BUTTON_ZR },
        .touchpad = { EMappingKind_None, 255 }
    };
    return true;
}

SDL_JoystickDriver SDL_WIIU_JoystickDriver = {
    WIIU_JoystickInit,
    WIIU_JoystickGetCount,
    WIIU_JoystickDetect,
    WIIU_JoystickIsDevicePresent,
    WIIU_JoystickGetDeviceName,
    WIIU_JoystickGetDevicePath,
    WIIU_JoystickGetDeviceSteamVirtualGamepadSlot,
    WIIU_JoystickGetDevicePlayerIndex,
    WIIU_JoystickSetDevicePlayerIndex,
    WIIU_JoystickGetDeviceGUID,
    WIIU_JoystickGetDeviceInstanceID,
    WIIU_JoystickOpen,
    WIIU_JoystickRumble,
    WIIU_JoystickRumbleTriggers,
    WIIU_JoystickSetLED,
    WIIU_JoystickSendEffect,
    WIIU_JoystickSetSensorsEnabled,
    WIIU_JoystickUpdate,
    WIIU_JoystickClose,
    WIIU_JoystickQuit,
    WIIU_JoystickGetGamepadMapping
};

#endif // SDL_JOYSTICK_WIIU
