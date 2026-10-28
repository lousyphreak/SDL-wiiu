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
#include "../../core/wiiu/SDL_wiiukpad.h"
#include "../../core/wiiu/SDL_wiiulifecycle.h"

#define WIIU_DRC_NAME "Nintendo Wii U GamePad"
#define WIIU_PRO_NAME "Nintendo Wii U Pro Controller"
#define WIIU_CLASSIC_NAME "Nintendo Wii Remote with Classic Controller"
#define WIIU_REMOTE_NAME "Nintendo Wii Remote"
#define WIIU_NUNCHUK_NAME "Nintendo Wii Remote + Nunchuk"
#define WIIU_MAX_DEVICES (1 + WIIU_MAX_KPAD_CHANNELS)
#define WIIU_SENSOR_RATE_HZ 100.0f
#define WIIU_DRC_RUMBLE_PATTERN_BITS 120
#define WIIU_DRC_RUMBLE_PATTERN_BYTES (WIIU_DRC_RUMBLE_PATTERN_BITS / 8)
#define WIIU_DRC_RUMBLE_REQUEUE_MS 500

typedef enum WIIUDeviceSource
{
    WIIU_DEVICE_SOURCE_DRC,
    WIIU_DEVICE_SOURCE_KPAD
} WIIUDeviceSource;

typedef enum WIIUDeviceFamily
{
    WIIU_DEVICE_FAMILY_NONE,
    WIIU_DEVICE_FAMILY_PENDING,
    WIIU_DEVICE_FAMILY_DRC,
    WIIU_DEVICE_FAMILY_WIIMOTE,
    WIIU_DEVICE_FAMILY_NUNCHUK,
    WIIU_DEVICE_FAMILY_PRO,
    WIIU_DEVICE_FAMILY_CLASSIC
} WIIUDeviceFamily;

enum
{
    WIIU_AXIS_LEFTX,
    WIIU_AXIS_LEFTY,
    WIIU_AXIS_RIGHTX,
    WIIU_AXIS_RIGHTY,
    WIIU_AXIS_LEFT_TRIGGER,
    WIIU_AXIS_RIGHT_TRIGGER,
    WIIU_AXIS_COUNT
};

enum
{
    WIIU_BUTTON_A,
    WIIU_BUTTON_B,
    WIIU_BUTTON_X,
    WIIU_BUTTON_Y,
    WIIU_BUTTON_LEFT,
    WIIU_BUTTON_RIGHT,
    WIIU_BUTTON_UP,
    WIIU_BUTTON_DOWN,
    WIIU_BUTTON_ZL,
    WIIU_BUTTON_ZR,
    WIIU_BUTTON_L,
    WIIU_BUTTON_R,
    WIIU_BUTTON_PLUS,
    WIIU_BUTTON_MINUS,
    WIIU_BUTTON_HOME,
    WIIU_BUTTON_STICK_R,
    WIIU_BUTTON_STICK_L,
    WIIU_BUTTON_MISC1,
    WIIU_BUTTON_MISC2,
    WIIU_BUTTON_ONE,
    WIIU_BUTTON_TWO,
    WIIU_BUTTON_C,
    WIIU_BUTTON_Z,
    WIIU_BUTTON_COUNT
};

typedef struct WIIUDeviceInfo
{
    SDL_JoystickID instance_id;
    WIIUDeviceSource source;
    WIIUDeviceFamily family;
    int channel;
    WPADExtensionType extension;
    const char *name;
    SDL_GUID guid;
} WIIUDeviceInfo;

typedef struct joystick_hwdata
{
    SDL_JoystickID instance_id;
    WIIUDeviceSource source;
    WIIUDeviceFamily family;
    int channel;
    Uint32 buttons;
    Sint16 axes[WIIU_AXIS_COUNT];
    bool sensors_enabled;
    bool touch_down;
    float touch_x;
    float touch_y;
    float touch_pressure;
    Uint64 drc_rumble_next_update;
    SDL_PowerState battery_state;
    int battery_percent;
} joystick_hwdata;

static WIIUDeviceInfo SDL_WIIU_devices[WIIU_MAX_DEVICES];
static int SDL_WIIU_numjoysticks;
static SDL_JoystickID SDL_WIIU_next_instance_id;
static bool SDL_WIIU_joystick_initialized;
static bool SDL_WIIU_joystick_runtime_ready;

static void WIIU_JoystickDetect(void);

static bool SDL_WIIU_InitJoystickRuntime(void)
{
    if (!SDL_WIIU_InitDRC()) {
        return false;
    }

    if (!SDL_WIIU_InitKPAD()) {
        SDL_WIIU_QuitDRC();
        return false;
    }

    SDL_WIIU_joystick_runtime_ready = true;
    return true;
}

static void SDL_WIIU_ShutdownJoystickRuntime(void)
{
    if (!SDL_WIIU_joystick_runtime_ready) {
        return;
    }

    SDL_WIIU_QuitKPAD();
    SDL_WIIU_QuitDRC();
    SDL_WIIU_joystick_runtime_ready = false;
}

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

static Sint16 SDL_WIIU_ConvertTriggerAxis(float value)
{
    const float normalized = SDL_clamp(value, 0.0f, 1.0f);

    if (normalized <= 0.0f) {
        return SDL_JOYSTICK_AXIS_MIN;
    }
    if (normalized >= 1.0f) {
        return SDL_JOYSTICK_AXIS_MAX;
    }
    return (Sint16)SDL_lroundf((normalized * 65535.0f) - 32768.0f);
}

static Sint16 SDL_WIIU_ConvertDigitalTriggerAxis(bool down)
{
    return down ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN;
}

static int SDL_WIIU_GetBatteryPercentFromLevel(uint8_t level)
{
    if (level >= 4) {
        return 100;
    }
    if (level == 3) {
        return 70;
    }
    if (level == 2) {
        return 40;
    }
    if (level == 1) {
        return 10;
    }
    return 3;
}

static SDL_JoystickID SDL_WIIU_AllocateInstanceID(void)
{
    if (SDL_WIIU_next_instance_id <= 0) {
        SDL_WIIU_next_instance_id = 1;
    }
    return SDL_WIIU_next_instance_id++;
}

static const char *SDL_WIIU_GetDeviceNameForFamily(WIIUDeviceFamily family)
{
    switch (family) {
    case WIIU_DEVICE_FAMILY_DRC:
        return WIIU_DRC_NAME;
    case WIIU_DEVICE_FAMILY_WIIMOTE:
        return WIIU_REMOTE_NAME;
    case WIIU_DEVICE_FAMILY_NUNCHUK:
        return WIIU_NUNCHUK_NAME;
    case WIIU_DEVICE_FAMILY_PRO:
        return WIIU_PRO_NAME;
    case WIIU_DEVICE_FAMILY_CLASSIC:
        return WIIU_CLASSIC_NAME;
    default:
        return NULL;
    }
}

static WPADDataFormat SDL_WIIU_GetDataFormatForFamily(WIIUDeviceFamily family)
{
    switch (family) {
    case WIIU_DEVICE_FAMILY_WIIMOTE:
        return WPAD_FMT_CORE_ACC_DPD;
    case WIIU_DEVICE_FAMILY_NUNCHUK:
        return WPAD_FMT_NUNCHUK_ACC_DPD;
    case WIIU_DEVICE_FAMILY_PRO:
        return WPAD_FMT_PRO_CONTROLLER;
    case WIIU_DEVICE_FAMILY_CLASSIC:
        return WPAD_FMT_CLASSIC_ACC;
    default:
        return WPAD_FMT_CORE;
    }
}

static bool SDL_WIIU_FamilyHasPrimaryAccelerometer(WIIUDeviceFamily family)
{
    return family == WIIU_DEVICE_FAMILY_DRC ||
           family == WIIU_DEVICE_FAMILY_WIIMOTE ||
           family == WIIU_DEVICE_FAMILY_NUNCHUK ||
           family == WIIU_DEVICE_FAMILY_CLASSIC;
}

static bool SDL_WIIU_FamilyHasSecondaryAccelerometer(WIIUDeviceFamily family)
{
    return family == WIIU_DEVICE_FAMILY_NUNCHUK;
}

static bool SDL_WIIU_FamilyHasGyroscope(WIIUDeviceFamily family)
{
    return family == WIIU_DEVICE_FAMILY_DRC;
}

static bool SDL_WIIU_FamilyHasSensors(WIIUDeviceFamily family)
{
    return SDL_WIIU_FamilyHasPrimaryAccelerometer(family) ||
           SDL_WIIU_FamilyHasSecondaryAccelerometer(family) ||
           SDL_WIIU_FamilyHasGyroscope(family);
}

static bool SDL_WIIU_FamilyHasRumble(WIIUDeviceFamily family)
{
    switch (family) {
    case WIIU_DEVICE_FAMILY_DRC:
    case WIIU_DEVICE_FAMILY_WIIMOTE:
    case WIIU_DEVICE_FAMILY_NUNCHUK:
    case WIIU_DEVICE_FAMILY_PRO:
    case WIIU_DEVICE_FAMILY_CLASSIC:
        return true;
    default:
        return false;
    }
}

static uint8_t SDL_WIIU_BuildDRCRumblePattern(Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, uint8_t *pattern, size_t pattern_size)
{
    const Uint32 intensity = SDL_max((Uint32)low_frequency_rumble, (Uint32)high_frequency_rumble);
    Uint32 accumulator = 0;
    uint8_t active_samples = 0;
    uint8_t i;
    const uint8_t bit_length = (uint8_t)SDL_min(pattern_size * 8, (size_t)WIIU_DRC_RUMBLE_PATTERN_BITS);

    if (!pattern || pattern_size == 0 || intensity == 0 || bit_length == 0) {
        return 0;
    }

    /* VPAD consumes a 120 Hz bitstream, not a byte-per-sample pattern. */
    SDL_memset(pattern, 0, pattern_size);
    for (i = 0; i < bit_length; ++i) {
        accumulator += intensity;
        if (accumulator >= 0x10000u) {
            pattern[i / 8] |= (uint8_t)(1u << (i % 8));
            accumulator -= 0x10000u;
            ++active_samples;
        }
    }

    if (active_samples == 0) {
        pattern[0] = 0x01;
    }

    return bit_length;
}

static void SDL_WIIU_ScheduleDRCRumbleUpdate(joystick_hwdata *hwdata, Uint64 now)
{
    hwdata->drc_rumble_next_update = now + WIIU_DRC_RUMBLE_REQUEUE_MS;
    if (hwdata->drc_rumble_next_update == 0) {
        hwdata->drc_rumble_next_update = 1;
    }
}

static bool SDL_WIIU_QueueDRCRumble(Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    uint8_t pattern[WIIU_DRC_RUMBLE_PATTERN_BYTES];
    const uint8_t length = SDL_WIIU_BuildDRCRumblePattern(low_frequency_rumble, high_frequency_rumble,
                                                          pattern, SDL_arraysize(pattern));
    return (length > 0 && VPADControlMotor(VPAD_CHAN_0, pattern, length) >= 0);
}

static void SDL_WIIU_AddSensorsForFamily(SDL_Joystick *joystick, WIIUDeviceFamily family)
{
    if (SDL_WIIU_FamilyHasPrimaryAccelerometer(family)) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, WIIU_SENSOR_RATE_HZ);
    }
    if (SDL_WIIU_FamilyHasSecondaryAccelerometer(family)) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL_L, WIIU_SENSOR_RATE_HZ);
    }
    if (SDL_WIIU_FamilyHasGyroscope(family)) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, WIIU_SENSOR_RATE_HZ);
    }
}

static bool SDL_WIIU_FamilyUsesPointer(WIIUDeviceFamily family)
{
    return family == WIIU_DEVICE_FAMILY_WIIMOTE ||
           family == WIIU_DEVICE_FAMILY_NUNCHUK;
}

static bool SDL_WIIU_EnsureKPADChannelConfigured(int channel, WIIUDeviceFamily family)
{
    const WPADDataFormat desired_format = SDL_WIIU_GetDataFormatForFamily(family);

    return SDL_WIIU_EnsureKPADDataFormat(channel, desired_format, SDL_WIIU_FamilyUsesPointer(family));
}

static int SDL_WIIU_GetAxisCountForFamily(WIIUDeviceFamily family)
{
    switch (family) {
    case WIIU_DEVICE_FAMILY_DRC:
    case WIIU_DEVICE_FAMILY_PRO:
    case WIIU_DEVICE_FAMILY_CLASSIC:
        return WIIU_AXIS_COUNT;
    case WIIU_DEVICE_FAMILY_NUNCHUK:
        return 2;
    default:
        return 0;
    }
}

static int SDL_WIIU_GetButtonCountForFamily(WIIUDeviceFamily family)
{
    switch (family) {
    case WIIU_DEVICE_FAMILY_DRC:
        return WIIU_BUTTON_MISC2 + 1;
    case WIIU_DEVICE_FAMILY_WIIMOTE:
        return WIIU_BUTTON_TWO + 1;
    case WIIU_DEVICE_FAMILY_NUNCHUK:
        return WIIU_BUTTON_Z + 1;
    case WIIU_DEVICE_FAMILY_PRO:
        return WIIU_BUTTON_STICK_L + 1;
    case WIIU_DEVICE_FAMILY_CLASSIC:
        return WIIU_BUTTON_HOME + 1;
    default:
        return 0;
    }
}

static WIIUDeviceInfo *SDL_WIIU_GetDeviceInfo(int device_index)
{
    if (device_index < 0 || device_index >= SDL_WIIU_numjoysticks) {
        return NULL;
    }
    return &SDL_WIIU_devices[device_index];
}

static int SDL_WIIU_FindDeviceByLocation(WIIUDeviceSource source, int channel)
{
    int i;

    for (i = 0; i < SDL_WIIU_numjoysticks; ++i) {
        if (SDL_WIIU_devices[i].source == source &&
            SDL_WIIU_devices[i].channel == channel) {
            return i;
        }
    }
    return -1;
}

static void SDL_WIIU_UpdatePowerInfo(SDL_Joystick *joystick, joystick_hwdata *hwdata, SDL_PowerState battery_state, int battery_percent)
{
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

static void SDL_WIIU_ConvertSensorAxes(float *data, float x, float y, float z, float scale)
{
    data[0] = -x * scale;
    data[1] = z * scale;
    data[2] = y * scale;
}

static void SDL_WIIU_SendSensorData(SDL_Joystick *joystick, joystick_hwdata *hwdata, Uint64 timestamp, SDL_SensorType type, float x, float y, float z, float scale)
{
    float data[3];

    if (!hwdata->sensors_enabled) {
        return;
    }

    SDL_WIIU_ConvertSensorAxes(data, x, y, z, scale);
    SDL_SendJoystickSensor(timestamp, joystick, type, timestamp, data, SDL_arraysize(data));
}

static void SDL_WIIU_SendAxes(SDL_Joystick *joystick, joystick_hwdata *hwdata, Uint64 timestamp, const Sint16 *axes)
{
    int i;

    for (i = 0; i < joystick->naxes; ++i) {
        if (axes[i] != hwdata->axes[i]) {
            SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, axes[i]);
            hwdata->axes[i] = axes[i];
        }
    }
}

static void SDL_WIIU_SendButtons(SDL_Joystick *joystick, joystick_hwdata *hwdata, Uint64 timestamp, Uint32 buttons)
{
    int i;

    for (i = 0; i < joystick->nbuttons; ++i) {
        const Uint32 mask = (Uint32)1u << i;
        const bool down = (buttons & mask) != 0;
        const bool was_down = (hwdata->buttons & mask) != 0;

        if (down != was_down) {
            SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, down);
        }
    }

    hwdata->buttons = buttons;
}

static void SDL_WIIU_SetButton(Uint32 *buttons, int button, bool down)
{
    if (down) {
        *buttons |= ((Uint32)1u << button);
    }
}

static Uint32 SDL_WIIU_GetDRCButtons(const VPADStatus *status)
{
    const Uint32 hold = status->hold;
    Uint32 buttons = 0;

    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_A, (hold & VPAD_BUTTON_A) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_B, (hold & VPAD_BUTTON_B) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_X, (hold & VPAD_BUTTON_X) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_Y, (hold & VPAD_BUTTON_Y) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_LEFT, (hold & VPAD_BUTTON_LEFT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_RIGHT, (hold & VPAD_BUTTON_RIGHT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_UP, (hold & VPAD_BUTTON_UP) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_DOWN, (hold & VPAD_BUTTON_DOWN) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ZL, (hold & VPAD_BUTTON_ZL) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ZR, (hold & VPAD_BUTTON_ZR) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_L, (hold & VPAD_BUTTON_L) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_R, (hold & VPAD_BUTTON_R) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_PLUS, (hold & VPAD_BUTTON_PLUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_MINUS, (hold & VPAD_BUTTON_MINUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_HOME, (hold & VPAD_BUTTON_HOME) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_STICK_R, (hold & VPAD_BUTTON_STICK_R) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_STICK_L, (hold & VPAD_BUTTON_STICK_L) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_MISC1, (hold & VPAD_BUTTON_TV) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_MISC2, (hold & VPAD_BUTTON_SYNC) != 0);
    return buttons;
}

static Uint32 SDL_WIIU_GetProButtons(const KPADStatus *status)
{
    const Uint32 hold = status->pro.hold;
    Uint32 buttons = 0;

    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_A, (hold & WPAD_PRO_BUTTON_A) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_B, (hold & WPAD_PRO_BUTTON_B) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_X, (hold & WPAD_PRO_BUTTON_X) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_Y, (hold & WPAD_PRO_BUTTON_Y) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_LEFT, (hold & WPAD_PRO_BUTTON_LEFT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_RIGHT, (hold & WPAD_PRO_BUTTON_RIGHT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_UP, (hold & WPAD_PRO_BUTTON_UP) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_DOWN, (hold & WPAD_PRO_BUTTON_DOWN) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ZL, (hold & WPAD_PRO_BUTTON_ZL) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ZR, (hold & WPAD_PRO_BUTTON_ZR) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_L, (hold & WPAD_PRO_BUTTON_L) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_R, (hold & WPAD_PRO_BUTTON_R) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_PLUS, (hold & WPAD_PRO_BUTTON_PLUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_MINUS, (hold & WPAD_PRO_BUTTON_MINUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_HOME, (hold & WPAD_PRO_BUTTON_HOME) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_STICK_R, (hold & WPAD_PRO_BUTTON_STICK_R) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_STICK_L, (hold & WPAD_PRO_BUTTON_STICK_L) != 0);
    return buttons;
}

static Uint32 SDL_WIIU_GetClassicButtons(const KPADStatus *status)
{
    const Uint32 hold = status->classic.hold;
    Uint32 buttons = 0;

    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_A, (hold & WPAD_CLASSIC_BUTTON_A) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_B, (hold & WPAD_CLASSIC_BUTTON_B) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_X, (hold & WPAD_CLASSIC_BUTTON_X) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_Y, (hold & WPAD_CLASSIC_BUTTON_Y) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_LEFT, (hold & WPAD_CLASSIC_BUTTON_LEFT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_RIGHT, (hold & WPAD_CLASSIC_BUTTON_RIGHT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_UP, (hold & WPAD_CLASSIC_BUTTON_UP) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_DOWN, (hold & WPAD_CLASSIC_BUTTON_DOWN) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ZL, (hold & WPAD_CLASSIC_BUTTON_ZL) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ZR, (hold & WPAD_CLASSIC_BUTTON_ZR) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_L, (hold & WPAD_CLASSIC_BUTTON_L) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_R, (hold & WPAD_CLASSIC_BUTTON_R) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_PLUS, (hold & WPAD_CLASSIC_BUTTON_PLUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_MINUS, (hold & WPAD_CLASSIC_BUTTON_MINUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_HOME, (hold & WPAD_CLASSIC_BUTTON_HOME) != 0);
    return buttons;
}

static Uint32 SDL_WIIU_GetWiimoteButtons(const KPADStatus *status)
{
    const Uint32 hold = status->hold;
    Uint32 buttons = 0;

    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_A, (hold & WPAD_BUTTON_A) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_B, (hold & WPAD_BUTTON_B) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_LEFT, (hold & WPAD_BUTTON_LEFT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_RIGHT, (hold & WPAD_BUTTON_RIGHT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_UP, (hold & WPAD_BUTTON_UP) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_DOWN, (hold & WPAD_BUTTON_DOWN) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_PLUS, (hold & WPAD_BUTTON_PLUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_MINUS, (hold & WPAD_BUTTON_MINUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_HOME, (hold & WPAD_BUTTON_HOME) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ONE, (hold & WPAD_BUTTON_1) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_TWO, (hold & WPAD_BUTTON_2) != 0);
    return buttons;
}

static Uint32 SDL_WIIU_GetNunchukButtons(const KPADStatus *status)
{
    const Uint32 hold = status->hold | status->nunchuk.hold;
    Uint32 buttons = 0;

    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_A, (hold & WPAD_BUTTON_A) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_B, (hold & WPAD_BUTTON_B) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_LEFT, (hold & WPAD_BUTTON_LEFT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_RIGHT, (hold & WPAD_BUTTON_RIGHT) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_UP, (hold & WPAD_BUTTON_UP) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_DOWN, (hold & WPAD_BUTTON_DOWN) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_PLUS, (hold & WPAD_BUTTON_PLUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_MINUS, (hold & WPAD_BUTTON_MINUS) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_HOME, (hold & WPAD_BUTTON_HOME) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_ONE, (hold & WPAD_BUTTON_1) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_TWO, (hold & WPAD_BUTTON_2) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_C, (hold & WPAD_BUTTON_C) != 0);
    SDL_WIIU_SetButton(&buttons, WIIU_BUTTON_Z, (hold & WPAD_BUTTON_Z) != 0);
    return buttons;
}

static WIIUDeviceFamily SDL_WIIU_GetKPADFamilyForExtension(WPADExtensionType extension)
{
    switch (extension) {
    case WPAD_EXT_CORE:
        return WIIU_DEVICE_FAMILY_WIIMOTE;
    case WPAD_EXT_NUNCHUK:
        return WIIU_DEVICE_FAMILY_NUNCHUK;
    case WPAD_EXT_PRO_CONTROLLER:
        return WIIU_DEVICE_FAMILY_PRO;
    case WPAD_EXT_CLASSIC:
        return WIIU_DEVICE_FAMILY_CLASSIC;
    case WPAD_EXT_DEV_NOT_FOUND:
        return WIIU_DEVICE_FAMILY_NONE;
    default:
        return WIIU_DEVICE_FAMILY_NONE;
    }
}

static WIIUDeviceFamily SDL_WIIU_QueryKPADFamily(int channel, WPADExtensionType *out_extension)
{
    WPADExtensionType extension = WPAD_EXT_DEV_NOT_FOUND;
    const WPADError probe_error = WPADProbe((WPADChan)channel, &extension);

    if (out_extension) {
        *out_extension = extension;
    }

    if (probe_error == WPAD_ERROR_NONE) {
        return SDL_WIIU_GetKPADFamilyForExtension(extension);
    }
    if (probe_error == WPAD_ERROR_NOT_READY) {
        return WIIU_DEVICE_FAMILY_PENDING;
    }
    return WIIU_DEVICE_FAMILY_NONE;
}

static void SDL_WIIU_AddDevice(WIIUDeviceSource source, WIIUDeviceFamily family, int channel, WPADExtensionType extension)
{
    WIIUDeviceInfo *device;
    const char *name;

    if (SDL_WIIU_numjoysticks >= WIIU_MAX_DEVICES) {
        return;
    }

    name = SDL_WIIU_GetDeviceNameForFamily(family);
    if (!name) {
        return;
    }

    device = &SDL_WIIU_devices[SDL_WIIU_numjoysticks++];
    device->instance_id = SDL_WIIU_AllocateInstanceID();
    device->source = source;
    device->family = family;
    device->channel = channel;
    device->extension = extension;
    device->name = name;
    device->guid = SDL_CreateJoystickGUIDForName(name);
    SDL_PrivateJoystickAdded(device->instance_id);
}

static void SDL_WIIU_RemoveDeviceAtIndex(int index)
{
    SDL_JoystickID instance_id;
    int i;

    if (index < 0 || index >= SDL_WIIU_numjoysticks) {
        return;
    }

    instance_id = SDL_WIIU_devices[index].instance_id;
    SDL_PrivateJoystickRemoved(instance_id);

    for (i = index; i < (SDL_WIIU_numjoysticks - 1); ++i) {
        SDL_WIIU_devices[i] = SDL_WIIU_devices[i + 1];
    }
    --SDL_WIIU_numjoysticks;
}

static void SDL_WIIU_SyncDRCDevice(const SDL_WiiUDRCState *state)
{
    const bool connected = state && state->connected;
    const int index = SDL_WIIU_FindDeviceByLocation(WIIU_DEVICE_SOURCE_DRC, 0);

    if (connected) {
        if (index < 0) {
            SDL_WIIU_AddDevice(WIIU_DEVICE_SOURCE_DRC, WIIU_DEVICE_FAMILY_DRC, 0, WPAD_EXT_UNKNOWN);
        }
    } else if (index >= 0) {
        SDL_WIIU_RemoveDeviceAtIndex(index);
    }
}

static void SDL_WIIU_SyncKPADDevices(void)
{
    int channel;

    for (channel = 0; channel < WIIU_MAX_KPAD_CHANNELS; ++channel) {
        WPADExtensionType extension = WPAD_EXT_DEV_NOT_FOUND;
        const WIIUDeviceFamily family = SDL_WIIU_QueryKPADFamily(channel, &extension);
        int index = SDL_WIIU_FindDeviceByLocation(WIIU_DEVICE_SOURCE_KPAD, channel);

        if (family == WIIU_DEVICE_FAMILY_PENDING) {
            continue;
        }

        if (family == WIIU_DEVICE_FAMILY_NONE) {
            if (index >= 0) {
                SDL_WIIU_RemoveDeviceAtIndex(index);
            }
            continue;
        }

        if (!SDL_WIIU_EnsureKPADChannelConfigured(channel, family)) {
            continue;
        }

        if (index >= 0) {
            const WIIUDeviceInfo *device = &SDL_WIIU_devices[index];
            if (device->family == family && device->extension == extension) {
                continue;
            }

            SDL_WIIU_RemoveDeviceAtIndex(index);
            index = -1;
        }

        SDL_WIIU_AddDevice(WIIU_DEVICE_SOURCE_KPAD, family, channel, extension);
    }
}

static bool WIIU_JoystickInit(void)
{
    if (!SDL_WIIU_InitJoystickRuntime()) {
        return false;
    }

    SDL_WIIU_numjoysticks = 0;
    SDL_WIIU_next_instance_id = 1;
    SDL_WIIU_joystick_initialized = true;

    WIIU_JoystickDetect();
    return true;
}

static int WIIU_JoystickGetCount(void)
{
    return SDL_WIIU_numjoysticks;
}

static void WIIU_JoystickDetect(void)
{
    const SDL_WiiUDRCState *state = SDL_WIIU_GetDRCState();

    if (!SDL_WIIU_joystick_runtime_ready || !SDL_WIIU_IsInForeground()) {
        return;
    }

    SDL_WIIU_SyncDRCDevice(state);
    SDL_WIIU_SyncKPADDevices();
}

static bool WIIU_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    return false;
}

static const char *WIIU_JoystickGetDeviceName(int device_index)
{
    WIIUDeviceInfo *device = SDL_WIIU_GetDeviceInfo(device_index);
    return device ? device->name : NULL;
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
    return -1;
}

static void WIIU_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
}

static SDL_GUID WIIU_JoystickGetDeviceGUID(int device_index)
{
    WIIUDeviceInfo *device = SDL_WIIU_GetDeviceInfo(device_index);
    SDL_GUID guid;

    if (device) {
        return device->guid;
    }

    SDL_zero(guid);
    return guid;
}

static SDL_JoystickID WIIU_JoystickGetDeviceInstanceID(int device_index)
{
    WIIUDeviceInfo *device = SDL_WIIU_GetDeviceInfo(device_index);
    return device ? device->instance_id : 0;
}

static bool WIIU_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    WIIUDeviceInfo *device = SDL_WIIU_GetDeviceInfo(device_index);
    joystick_hwdata *hwdata;

    if (!device) {
        return SDL_SetError("Invalid Wii U joystick index");
    }

    hwdata = (joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return false;
    }

    hwdata->instance_id = device->instance_id;
    hwdata->source = device->source;
    hwdata->family = device->family;
    hwdata->channel = device->channel;
    hwdata->battery_percent = -1;

    joystick->hwdata = hwdata;
    joystick->nbuttons = SDL_WIIU_GetButtonCountForFamily(device->family);
    joystick->naxes = SDL_WIIU_GetAxisCountForFamily(device->family);
    joystick->nhats = 0;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    if (device->family == WIIU_DEVICE_FAMILY_DRC) {
        SDL_PrivateJoystickAddTouchpad(joystick, 1);
    }
    SDL_WIIU_AddSensorsForFamily(joystick, device->family);

    SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick),
                           SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN,
                           SDL_WIIU_FamilyHasRumble(device->family));
    return true;
}

static bool WIIU_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    joystick_hwdata *hwdata = joystick->hwdata;
    const bool active = (low_frequency_rumble || high_frequency_rumble);

    if (!hwdata) {
        return SDL_SetError("Invalid Wii U joystick");
    }
    if (!SDL_WIIU_FamilyHasRumble(hwdata->family)) {
        return SDL_Unsupported();
    }

    if (hwdata->source == WIIU_DEVICE_SOURCE_DRC) {
        if (active) {
            const Uint64 now = SDL_GetTicks();

            if (!SDL_WIIU_QueueDRCRumble(low_frequency_rumble, high_frequency_rumble)) {
                hwdata->drc_rumble_next_update = 0;
                return false;
            }

            SDL_WIIU_ScheduleDRCRumbleUpdate(hwdata, now);
            return true;
        }

        hwdata->drc_rumble_next_update = 0;
        VPADStopMotor(VPAD_CHAN_0);
        return true;
    }

    if (active && !WPADIsMotorEnabled()) {
        WPADEnableMotor(TRUE);
    }
    WPADControlMotor((WPADChan)hwdata->channel, active ? TRUE : FALSE);
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
    joystick_hwdata *hwdata = joystick->hwdata;

    SDL_AssertJoysticksLocked();

    if (!hwdata) {
        return SDL_SetError("Invalid Wii U joystick");
    }
    if (!SDL_WIIU_FamilyHasSensors(hwdata->family)) {
        return SDL_Unsupported();
    }

    hwdata->sensors_enabled = enabled;
    return true;
}

static void SDL_WIIU_UpdateDRC(SDL_Joystick *joystick, joystick_hwdata *hwdata)
{
    const SDL_WiiUDRCState *state = SDL_WIIU_GetDRCState();
    const Uint64 timestamp = state ? state->timestamp_ns : SDL_GetTicksNS();
    const bool valid = state && state->connected && state->has_status;
    const Sint16 axes[WIIU_AXIS_COUNT] = {
        valid ? SDL_WIIU_ConvertStickAxis(state->status.leftStick.x, false) : 0,
        valid ? SDL_WIIU_ConvertStickAxis(state->status.leftStick.y, true) : 0,
        valid ? SDL_WIIU_ConvertStickAxis(state->status.rightStick.x, false) : 0,
        valid ? SDL_WIIU_ConvertStickAxis(state->status.rightStick.y, true) : 0,
        valid ? SDL_WIIU_ConvertDigitalTriggerAxis((state->status.hold & VPAD_BUTTON_ZL) != 0) : SDL_JOYSTICK_AXIS_MIN,
        valid ? SDL_WIIU_ConvertDigitalTriggerAxis((state->status.hold & VPAD_BUTTON_ZR) != 0) : SDL_JOYSTICK_AXIS_MIN
    };
    const Uint32 buttons = valid ? SDL_WIIU_GetDRCButtons(&state->status) : 0;
    SDL_PowerState battery_state = SDL_POWERSTATE_UNKNOWN;
    int battery_percent = -1;

    SDL_WIIU_SendAxes(joystick, hwdata, timestamp, axes);
    SDL_WIIU_SendButtons(joystick, hwdata, timestamp, buttons);
    SDL_WIIU_UpdateTouchpad(joystick, hwdata, timestamp, state);

    if (valid) {
        SDL_WIIU_SendSensorData(joystick, hwdata, timestamp, SDL_SENSOR_ACCEL,
                                state->status.accelerometer.acc.x,
                                state->status.accelerometer.acc.y,
                                state->status.accelerometer.acc.z,
                                SDL_STANDARD_GRAVITY);
        SDL_WIIU_SendSensorData(joystick, hwdata, timestamp, SDL_SENSOR_GYRO,
                                state->status.gyro.x,
                                state->status.gyro.y,
                                state->status.gyro.z,
                                1.0f);
        battery_state = SDL_POWERSTATE_ON_BATTERY;
        battery_percent = SDL_min(((int)state->status.battery * 100 + 96) / 192, 100);
    }

    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    SDL_WIIU_UpdatePowerInfo(joystick, hwdata, battery_state, battery_percent);
}

static void SDL_WIIU_UpdatePro(SDL_Joystick *joystick, joystick_hwdata *hwdata, const KPADStatus *status, Uint64 timestamp)
{
    const Uint32 buttons = SDL_WIIU_GetProButtons(status);
    const Sint16 axes[WIIU_AXIS_COUNT] = {
        SDL_WIIU_ConvertStickAxis(status->pro.leftStick.x, false),
        SDL_WIIU_ConvertStickAxis(status->pro.leftStick.y, true),
        SDL_WIIU_ConvertStickAxis(status->pro.rightStick.x, false),
        SDL_WIIU_ConvertStickAxis(status->pro.rightStick.y, true),
        SDL_WIIU_ConvertDigitalTriggerAxis((status->pro.hold & WPAD_PRO_BUTTON_ZL) != 0),
        SDL_WIIU_ConvertDigitalTriggerAxis((status->pro.hold & WPAD_PRO_BUTTON_ZR) != 0)
    };
    const bool wired = status->pro.wired != 0;
    const bool charging = status->pro.charging != 0;
    const SDL_PowerState battery_state = charging ? SDL_POWERSTATE_CHARGING :
        (wired ? SDL_POWERSTATE_CHARGED : SDL_POWERSTATE_ON_BATTERY);
    const int battery_percent = SDL_WIIU_GetBatteryPercentFromLevel(WPADGetBatteryLevel((WPADChan)hwdata->channel));

    SDL_WIIU_SendAxes(joystick, hwdata, timestamp, axes);
    SDL_WIIU_SendButtons(joystick, hwdata, timestamp, buttons);
    joystick->connection_state = wired ? SDL_JOYSTICK_CONNECTION_WIRED : SDL_JOYSTICK_CONNECTION_WIRELESS;
    SDL_WIIU_UpdatePowerInfo(joystick, hwdata, battery_state, battery_percent);
}

static void SDL_WIIU_UpdateClassic(SDL_Joystick *joystick, joystick_hwdata *hwdata, const KPADStatus *status, Uint64 timestamp)
{
    const Uint32 buttons = SDL_WIIU_GetClassicButtons(status);
    const Sint16 axes[WIIU_AXIS_COUNT] = {
        SDL_WIIU_ConvertStickAxis(status->classic.leftStick.x, false),
        SDL_WIIU_ConvertStickAxis(status->classic.leftStick.y, true),
        SDL_WIIU_ConvertStickAxis(status->classic.rightStick.x, false),
        SDL_WIIU_ConvertStickAxis(status->classic.rightStick.y, true),
        SDL_WIIU_ConvertTriggerAxis(status->classic.leftTrigger),
        SDL_WIIU_ConvertTriggerAxis(status->classic.rightTrigger)
    };
    const int battery_percent = SDL_WIIU_GetBatteryPercentFromLevel(WPADGetBatteryLevel((WPADChan)hwdata->channel));

    SDL_WIIU_SendAxes(joystick, hwdata, timestamp, axes);
    SDL_WIIU_SendButtons(joystick, hwdata, timestamp, buttons);
    SDL_WIIU_SendSensorData(joystick, hwdata, timestamp, SDL_SENSOR_ACCEL,
                            status->acc.x, status->acc.y, status->acc.z,
                            SDL_STANDARD_GRAVITY);
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    SDL_WIIU_UpdatePowerInfo(joystick, hwdata, SDL_POWERSTATE_ON_BATTERY, battery_percent);
}

static void SDL_WIIU_UpdateWiimote(SDL_Joystick *joystick, joystick_hwdata *hwdata, const KPADStatus *status, Uint64 timestamp)
{
    const Uint32 buttons = SDL_WIIU_GetWiimoteButtons(status);
    const int battery_percent = SDL_WIIU_GetBatteryPercentFromLevel(WPADGetBatteryLevel((WPADChan)hwdata->channel));

    SDL_WIIU_SendButtons(joystick, hwdata, timestamp, buttons);
    SDL_WIIU_SendSensorData(joystick, hwdata, timestamp, SDL_SENSOR_ACCEL,
                            status->acc.x, status->acc.y, status->acc.z,
                            SDL_STANDARD_GRAVITY);
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    SDL_WIIU_UpdatePowerInfo(joystick, hwdata, SDL_POWERSTATE_ON_BATTERY, battery_percent);
}

static void SDL_WIIU_UpdateNunchuk(SDL_Joystick *joystick, joystick_hwdata *hwdata, const KPADStatus *status, Uint64 timestamp)
{
    const Uint32 buttons = SDL_WIIU_GetNunchukButtons(status);
    const Sint16 axes[] = {
        SDL_WIIU_ConvertStickAxis(status->nunchuk.stick.x, false),
        SDL_WIIU_ConvertStickAxis(status->nunchuk.stick.y, true)
    };
    const int battery_percent = SDL_WIIU_GetBatteryPercentFromLevel(WPADGetBatteryLevel((WPADChan)hwdata->channel));

    SDL_WIIU_SendAxes(joystick, hwdata, timestamp, axes);
    SDL_WIIU_SendButtons(joystick, hwdata, timestamp, buttons);
    SDL_WIIU_SendSensorData(joystick, hwdata, timestamp, SDL_SENSOR_ACCEL,
                            status->acc.x, status->acc.y, status->acc.z,
                            SDL_STANDARD_GRAVITY);
    SDL_WIIU_SendSensorData(joystick, hwdata, timestamp, SDL_SENSOR_ACCEL_L,
                            status->nunchuk.acc.x, status->nunchuk.acc.y, status->nunchuk.acc.z,
                            SDL_STANDARD_GRAVITY);
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    SDL_WIIU_UpdatePowerInfo(joystick, hwdata, SDL_POWERSTATE_ON_BATTERY, battery_percent);
}

static void SDL_WIIU_UpdateKPAD(SDL_Joystick *joystick, joystick_hwdata *hwdata)
{
    const SDL_WiiUKPADState *kpad_state = SDL_WIIU_GetKPADState(hwdata->channel);
    const WPADDataFormat expected_format = SDL_WIIU_GetDataFormatForFamily(hwdata->family);
    const KPADStatus *status;
    const Uint64 timestamp = kpad_state ? kpad_state->timestamp_ns : SDL_GetTicksNS();

    if (!kpad_state || !kpad_state->connected || !kpad_state->has_status) {
        return;
    }

    status = &kpad_state->status;

    if (status->format != expected_format) {
        SDL_WIIU_EnsureKPADChannelConfigured(hwdata->channel, hwdata->family);
        return;
    }

    switch (hwdata->family) {
    case WIIU_DEVICE_FAMILY_WIIMOTE:
        if (status->extensionType == WPAD_EXT_CORE) {
            SDL_WIIU_UpdateWiimote(joystick, hwdata, status, timestamp);
        }
        break;
    case WIIU_DEVICE_FAMILY_NUNCHUK:
        if (status->extensionType == WPAD_EXT_NUNCHUK) {
            SDL_WIIU_UpdateNunchuk(joystick, hwdata, status, timestamp);
        }
        break;
    case WIIU_DEVICE_FAMILY_PRO:
        if (status->extensionType == WPAD_EXT_PRO_CONTROLLER) {
            SDL_WIIU_UpdatePro(joystick, hwdata, status, timestamp);
        }
        break;
    case WIIU_DEVICE_FAMILY_CLASSIC:
        if (status->extensionType == WPAD_EXT_CLASSIC) {
            SDL_WIIU_UpdateClassic(joystick, hwdata, status, timestamp);
        }
        break;
    default:
        break;
    }
}

static void SDL_WIIU_UpdateDRCRumble(SDL_Joystick *joystick, joystick_hwdata *hwdata)
{
    const Uint64 now = SDL_GetTicks();

    if (!joystick->low_frequency_rumble && !joystick->high_frequency_rumble) {
        hwdata->drc_rumble_next_update = 0;
        return;
    }

    /* SDL's generic rumble resend cadence is longer than a single VPAD pattern. */
    if (hwdata->drc_rumble_next_update != 0 && now >= hwdata->drc_rumble_next_update) {
        if (SDL_WIIU_QueueDRCRumble(joystick->low_frequency_rumble, joystick->high_frequency_rumble)) {
            SDL_WIIU_ScheduleDRCRumbleUpdate(hwdata, now);
        } else {
            hwdata->drc_rumble_next_update = 0;
        }
    }
}

static void WIIU_JoystickUpdate(SDL_Joystick *joystick)
{
    joystick_hwdata *hwdata = joystick->hwdata;

    if (!hwdata) {
        return;
    }

    if (!SDL_WIIU_joystick_runtime_ready || !SDL_WIIU_IsInForeground()) {
        return;
    }

    if (hwdata->source == WIIU_DEVICE_SOURCE_DRC) {
        SDL_WIIU_UpdateDRC(joystick, hwdata);
        SDL_WIIU_UpdateDRCRumble(joystick, hwdata);
    } else {
        SDL_WIIU_UpdateKPAD(joystick, hwdata);
    }
}

static void WIIU_JoystickClose(SDL_Joystick *joystick)
{
    joystick_hwdata *hwdata = joystick->hwdata;

    if (!hwdata) {
        return;
    }

    if (SDL_WIIU_FamilyHasRumble(hwdata->family)) {
        if (hwdata->source == WIIU_DEVICE_SOURCE_DRC) {
            hwdata->drc_rumble_next_update = 0;
            VPADStopMotor(VPAD_CHAN_0);
        } else {
            WPADControlMotor((WPADChan)hwdata->channel, FALSE);
        }
    }

    SDL_free(hwdata);
    joystick->hwdata = NULL;
}

static void WIIU_JoystickQuit(void)
{
    SDL_WIIU_numjoysticks = 0;
    SDL_WIIU_ShutdownJoystickRuntime();
    SDL_WIIU_joystick_initialized = false;
}

void SDL_WIIU_JoystickForegroundReleased(void)
{
    if (!SDL_WIIU_joystick_initialized) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "WiiU joystick: shutting down foreground controller runtime");
    SDL_WIIU_ShutdownJoystickRuntime();
}

void SDL_WIIU_JoystickForegroundAcquired(void)
{
    if (!SDL_WIIU_joystick_initialized) {
        return;
    }

    if (!SDL_WIIU_InitJoystickRuntime()) {
        SDL_LogError(SDL_LOG_CATEGORY_INPUT, "WiiU joystick: failed to restart controller runtime");
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "WiiU joystick: restarted controller runtime after foreground reacquire");
    WIIU_JoystickDetect();
}

static bool WIIU_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    WIIUDeviceInfo *device = SDL_WIIU_GetDeviceInfo(device_index);

    if (!device) {
        return false;
    }

    SDL_zero(*out);
    out->a = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_A };
    out->b = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_B };
    out->x = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_X };
    out->y = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_Y };
    out->back = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_MINUS };
    out->guide = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_HOME };
    out->start = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_PLUS };
    out->leftshoulder = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_L };
    out->rightshoulder = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_R };
    out->dpup = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_UP };
    out->dpdown = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_DOWN };
    out->dpleft = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_LEFT };
    out->dpright = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_RIGHT };
    out->leftx = (SDL_InputMapping){ EMappingKind_Axis, WIIU_AXIS_LEFTX };
    out->lefty = (SDL_InputMapping){ EMappingKind_Axis, WIIU_AXIS_LEFTY };
    out->rightx = (SDL_InputMapping){ EMappingKind_Axis, WIIU_AXIS_RIGHTX };
    out->righty = (SDL_InputMapping){ EMappingKind_Axis, WIIU_AXIS_RIGHTY };
    out->touchpad = (SDL_InputMapping){ EMappingKind_None, 255 };

    switch (device->family) {
    case WIIU_DEVICE_FAMILY_DRC:
        out->lefttrigger = (SDL_InputMapping){ EMappingKind_Axis, WIIU_AXIS_LEFT_TRIGGER };
        out->righttrigger = (SDL_InputMapping){ EMappingKind_Axis, WIIU_AXIS_RIGHT_TRIGGER };
        out->leftstick = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_STICK_L };
        out->rightstick = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_STICK_R };
        out->misc1 = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_MISC1 };
        out->misc2 = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_MISC2 };
        return true;
    case WIIU_DEVICE_FAMILY_PRO:
        out->lefttrigger = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_ZL };
        out->righttrigger = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_ZR };
        out->leftstick = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_STICK_L };
        out->rightstick = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_STICK_R };
        return true;
    case WIIU_DEVICE_FAMILY_CLASSIC:
        out->lefttrigger = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_ZL };
        out->righttrigger = (SDL_InputMapping){ EMappingKind_Button, WIIU_BUTTON_ZR };
        out->leftstick = (SDL_InputMapping){ EMappingKind_None, 255 };
        out->rightstick = (SDL_InputMapping){ EMappingKind_None, 255 };
        return true;
    case WIIU_DEVICE_FAMILY_WIIMOTE:
    case WIIU_DEVICE_FAMILY_NUNCHUK:
    default:
        return false;
    }
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
