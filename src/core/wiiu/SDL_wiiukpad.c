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

#include "SDL_wiiukpad.h"

static int SDL_WIIU_kpad_refcount;
static bool SDL_WIIU_kpad_runtime_ready;
static bool SDL_WIIU_kpad_foreground_owned = true;
static Uint64 SDL_WIIU_kpad_cycle;
static Uint64 SDL_WIIU_kpad_sample_cycle[WIIU_MAX_KPAD_CHANNELS];
static SDL_WiiUKPADState SDL_WIIU_kpad_state[WIIU_MAX_KPAD_CHANNELS];

static void SDL_WIIU_ResetKPADState(void)
{
    int i;

    SDL_zero(SDL_WIIU_kpad_state);
    SDL_WIIU_kpad_cycle = 0;
    for (i = 0; i < WIIU_MAX_KPAD_CHANNELS; ++i) {
        SDL_WIIU_kpad_sample_cycle[i] = SDL_MAX_UINT64;
    }
}

static bool SDL_WIIU_StartKPADRuntime(void)
{
    WPADInit();
    KPADInit();
    KPADSetMaxControllers(WIIU_MAX_KPAD_CHANNELS);
    WPADEnableWiiRemote(TRUE);
    WPADEnableURCC(TRUE);
    WPADEnableMotor(TRUE);
    SDL_WIIU_kpad_runtime_ready = true;
    SDL_WIIU_ResetKPADState();
    return true;
}

static void SDL_WIIU_StopKPADRuntime(void)
{
    if (!SDL_WIIU_kpad_runtime_ready) {
        return;
    }

    WPADEnableMotor(FALSE);
    KPADShutdown();
    WPADShutdown();
    SDL_WIIU_kpad_runtime_ready = false;
    SDL_WIIU_ResetKPADState();
}

static void SDL_WIIU_ReadKPADState(int channel)
{
    SDL_WiiUKPADState *state = &SDL_WIIU_kpad_state[channel];
    KPADStatus status;
    KPADError error = KPAD_ERROR_OK;
    const Uint32 result = KPADReadEx((KPADChan)channel, &status, 1, &error);

    state->timestamp_ns = SDL_GetTicksNS();
    state->read_error = error;
    state->sample_count = result;

    if (result > 0 && error == KPAD_ERROR_OK && status.error == WPAD_ERROR_NONE) {
        state->status = status;
        state->has_status = true;
    } else if (!state->has_status) {
        SDL_zero(state->status);
    }

    state->connected = (error == KPAD_ERROR_OK ||
                        error == KPAD_ERROR_NO_SAMPLES ||
                        error == KPAD_ERROR_BUSY);
}

bool SDL_WIIU_InitKPAD(void)
{
    if (SDL_WIIU_kpad_refcount++ == 0) {
        if (!SDL_WIIU_kpad_foreground_owned) {
            SDL_WIIU_ResetKPADState();
            return true;
        }

        if (!SDL_WIIU_StartKPADRuntime()) {
            SDL_WIIU_kpad_refcount = 0;
            return false;
        }
    }

    return true;
}

void SDL_WIIU_QuitKPAD(void)
{
    if (SDL_WIIU_kpad_refcount <= 0) {
        return;
    }

    if (--SDL_WIIU_kpad_refcount == 0) {
        SDL_WIIU_StopKPADRuntime();
    }
}

void SDL_WIIU_KPADForegroundReleased(void)
{
    SDL_WIIU_kpad_foreground_owned = false;
    SDL_WIIU_StopKPADRuntime();
}

bool SDL_WIIU_KPADForegroundAcquired(void)
{
    SDL_WIIU_kpad_foreground_owned = true;

    if (SDL_WIIU_kpad_refcount > 0 && !SDL_WIIU_kpad_runtime_ready) {
        return SDL_WIIU_StartKPADRuntime();
    }

    return true;
}

void SDL_WIIU_BeginKPADUpdateCycle(void)
{
    if (!SDL_WIIU_kpad_runtime_ready) {
        return;
    }

    ++SDL_WIIU_kpad_cycle;
    if (SDL_WIIU_kpad_cycle == SDL_MAX_UINT64) {
        SDL_WIIU_kpad_cycle = 1;
    }
}

const SDL_WiiUKPADState *SDL_WIIU_GetKPADState(int channel)
{
    const Uint64 now = SDL_GetTicksNS();

    if (channel < 0 || channel >= WIIU_MAX_KPAD_CHANNELS || !SDL_WIIU_kpad_runtime_ready) {
        return NULL;
    }

    if (SDL_WIIU_kpad_sample_cycle[channel] != SDL_WIIU_kpad_cycle ||
        (SDL_WIIU_kpad_cycle == 0 && (now - SDL_WIIU_kpad_state[channel].timestamp_ns) > 5000000ULL)) {
        SDL_WIIU_ReadKPADState(channel);
        SDL_WIIU_kpad_sample_cycle[channel] = SDL_WIIU_kpad_cycle;
    }

    return &SDL_WIIU_kpad_state[channel];
}

bool SDL_WIIU_EnsureKPADDataFormat(int channel, WPADDataFormat format, bool enable_dpd)
{
    const KPADChan kpad_channel = (KPADChan)channel;
    const WPADChan wpad_channel = (WPADChan)channel;
    const WPADDataFormat current_format = WPADGetDataFormat(wpad_channel);

    if (channel < 0 || channel >= WIIU_MAX_KPAD_CHANNELS || !SDL_WIIU_kpad_runtime_ready) {
        return false;
    }

    KPADSetButtonProcMode(kpad_channel, KPAD_BUTTON_PROC_MODE_TIGHT);

    if (enable_dpd) {
        if (current_format != format) {
            KPADEnableDPD(kpad_channel);
            KPADEnableAimingMode(kpad_channel);
            SDL_WIIU_kpad_sample_cycle[channel] = SDL_MAX_UINT64;
        }
        return (current_format == format || WPADGetDataFormat(wpad_channel) == format);
    }

    KPADDisableDPD(kpad_channel);

    if (current_format != format) {
        const WPADError error = WPADSetDataFormat(wpad_channel, format);

        if (error != WPAD_ERROR_NONE && error != WPAD_ERROR_NOT_READY) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "WiiU KPAD: failed to set data format %d on channel %d (error %d)",
                        (int)format, channel, (int)error);
            return false;
        }
    }

    SDL_WIIU_kpad_sample_cycle[channel] = SDL_MAX_UINT64;
    return WPADGetDataFormat(wpad_channel) == format;
}
