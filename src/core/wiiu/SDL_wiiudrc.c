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

#include "SDL_wiiudrc.h"

static int SDL_WIIU_drc_refcount;
static Uint64 SDL_WIIU_drc_cycle;
static Uint64 SDL_WIIU_drc_sample_cycle;
static SDL_WiiUDRCState SDL_WIIU_drc_state;

static void SDL_WIIU_ReadDRCState(void)
{
    VPADStatus status;
    VPADTouchData touch;
    VPADReadError error = VPAD_READ_UNINITIALIZED;
    const int result = VPADRead(VPAD_CHAN_0, &status, 1, &error);

    SDL_WIIU_drc_state.timestamp_ns = SDL_GetTicksNS();
    SDL_WIIU_drc_state.error = error;

    if (result > 0 && error == VPAD_READ_SUCCESS) {
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &touch, &status.tpNormal);
        SDL_WIIU_drc_state.status = status;
        SDL_WIIU_drc_state.touch = touch;
        SDL_WIIU_drc_state.has_status = true;
    } else if (!SDL_WIIU_drc_state.has_status) {
        SDL_zero(SDL_WIIU_drc_state.status);
        SDL_zero(SDL_WIIU_drc_state.touch);
    }

    SDL_WIIU_drc_state.connected = (error == VPAD_READ_SUCCESS ||
                                    error == VPAD_READ_NO_SAMPLES ||
                                    error == VPAD_READ_BUSY);
}

bool SDL_WIIU_InitDRC(void)
{
    if (SDL_WIIU_drc_refcount++ == 0) {
        VPADInit();
        SDL_zero(SDL_WIIU_drc_state);
        SDL_WIIU_drc_sample_cycle = SDL_MAX_UINT64;
        SDL_WIIU_drc_cycle = 0;
    }

    return true;
}

void SDL_WIIU_QuitDRC(void)
{
    if (SDL_WIIU_drc_refcount <= 0) {
        return;
    }

    if (--SDL_WIIU_drc_refcount == 0) {
        VPADShutdown();
        SDL_zero(SDL_WIIU_drc_state);
        SDL_WIIU_drc_sample_cycle = SDL_MAX_UINT64;
        SDL_WIIU_drc_cycle = 0;
    }
}

void SDL_WIIU_BeginDRCUpdateCycle(void)
{
    if (SDL_WIIU_drc_refcount <= 0) {
        return;
    }

    ++SDL_WIIU_drc_cycle;
    if (SDL_WIIU_drc_cycle == SDL_MAX_UINT64) {
        SDL_WIIU_drc_cycle = 1;
    }
}

const SDL_WiiUDRCState *SDL_WIIU_GetDRCState(void)
{
    const Uint64 now = SDL_GetTicksNS();

    if (SDL_WIIU_drc_refcount <= 0) {
        return NULL;
    }

    if (SDL_WIIU_drc_sample_cycle != SDL_WIIU_drc_cycle ||
        (SDL_WIIU_drc_cycle == 0 && (now - SDL_WIIU_drc_state.timestamp_ns) > 5000000ULL)) {
        SDL_WIIU_ReadDRCState();
        SDL_WIIU_drc_sample_cycle = SDL_WIIU_drc_cycle;
    }

    return &SDL_WIIU_drc_state;
}
