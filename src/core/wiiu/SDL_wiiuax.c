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

#ifdef SDL_PLATFORM_WIIU

#include "SDL_wiiuax.h"

#include <sndcore2/core.h>

typedef struct SDL_WiiUAXState
{
    SDL_Mutex *lock;
    int refcount;
    bool initialized_by_sdl;
} SDL_WiiUAXState;

static SDL_WiiUAXState SDL_wiiu_ax;

static void SDL_WIIU_AX_WaitForAudioOutReady(void)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (AXIsAudioOutReady()) {
            return;
        }

        SDL_Delay(10);
    }
}

static bool SDL_WIIU_AX_EnsureLock(void)
{
    if (!SDL_wiiu_ax.lock) {
        SDL_wiiu_ax.lock = SDL_CreateMutex();
        if (!SDL_wiiu_ax.lock) {
            return false;
        }
    }
    return true;
}

bool SDL_WIIU_AX_Acquire(Uint32 *sample_rate, Uint32 *frame_samples)
{
    Uint32 runtime_sample_rate;
    Uint32 runtime_frame_samples;

    if (!sample_rate || !frame_samples) {
        return SDL_InvalidParamError("sample_rate/frame_samples");
    }

    if (!SDL_WIIU_AX_EnsureLock()) {
        return false;
    }

    SDL_LockMutex(SDL_wiiu_ax.lock);

    if (SDL_wiiu_ax.refcount == 0) {
        SDL_WIIU_AX_WaitForAudioOutReady();
        if (!AXIsInit()) {
            AXInitParams params = {
                AX_INIT_RENDERER_48KHZ,
                AX_INIT_FRAME_SIZE_3MS,
                AX_INIT_PIPELINE_SINGLE
            };
            const AXInitError init_error = AXInitWithParams(&params);
            if ((init_error != AX_INIT_ERROR_OK) && (init_error != AX_INIT_ERROR_ALREADY_INITIALIZED)) {
                SDL_UnlockMutex(SDL_wiiu_ax.lock);
                return SDL_SetError("AXInitWithParams failed: %d", (int)init_error);
            }
            SDL_wiiu_ax.initialized_by_sdl = true;
        } else {
            SDL_wiiu_ax.initialized_by_sdl = false;
        }
    }

    runtime_sample_rate = AXGetInputSamplesPerSec();
    runtime_frame_samples = AXGetInputSamplesPerFrame();
    if ((runtime_sample_rate == 0) || (runtime_frame_samples == 0)) {
        if ((SDL_wiiu_ax.refcount == 0) && SDL_wiiu_ax.initialized_by_sdl) {
            AXQuit();
            SDL_wiiu_ax.initialized_by_sdl = false;
        }
        SDL_UnlockMutex(SDL_wiiu_ax.lock);
        return SDL_SetError("AX reported invalid runtime timing (%u Hz, %u samples/frame)",
                            (unsigned int)runtime_sample_rate,
                            (unsigned int)runtime_frame_samples);
    }

    ++SDL_wiiu_ax.refcount;
    SDL_UnlockMutex(SDL_wiiu_ax.lock);

    *sample_rate = runtime_sample_rate;
    *frame_samples = runtime_frame_samples;
    return true;
}

void SDL_WIIU_AX_Release(void)
{
    if (!SDL_wiiu_ax.lock) {
        return;
    }

    SDL_LockMutex(SDL_wiiu_ax.lock);
    if ((SDL_wiiu_ax.refcount > 0) && (--SDL_wiiu_ax.refcount == 0) && SDL_wiiu_ax.initialized_by_sdl) {
        AXQuit();
        SDL_wiiu_ax.initialized_by_sdl = false;
    }
    SDL_UnlockMutex(SDL_wiiu_ax.lock);
}

#endif // SDL_PLATFORM_WIIU
