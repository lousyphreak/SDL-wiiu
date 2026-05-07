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

#ifdef SDL_THREAD_WIIU

#include <coreinit/semaphore.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

struct SDL_Semaphore
{
    OSSemaphore semaphore;
};

SDL_Semaphore *SDL_CreateSemaphore(Uint32 initial_value)
{
    SDL_Semaphore *sem;

    if (initial_value > SDL_MAX_SINT32) {
        SDL_SetError("Initial semaphore value too high for this platform");
        return NULL;
    }

    sem = (SDL_Semaphore *)SDL_malloc(sizeof(*sem));
    if (sem) {
        OSInitSemaphoreEx(&sem->semaphore, (int32_t)initial_value, "SDL semaphore");
    }

    return sem;
}

void SDL_DestroySemaphore(SDL_Semaphore *sem)
{
    SDL_free(sem);
}

static bool SDL_WiiUWaitSemaphoreTimeout(SDL_Semaphore *sem, Sint64 timeoutNS)
{
    const OSTime sleep_quantum = SDL_max((OSTime)OSMicrosecondsToTicks(100), (OSTime)1);
    const OSTime deadline = OSGetSystemTick() + (OSTime)OSNanosecondsToTicks((uint64_t)timeoutNS);

    while (OSGetSystemTick() < deadline) {
        if (OSTryWaitSemaphore(&sem->semaphore) > 0) {
            return true;
        }

        OSTime remaining = deadline - OSGetSystemTick();
        if (remaining <= 0) {
            break;
        }

        OSSleepTicks(SDL_min(remaining, sleep_quantum));
    }

    return OSTryWaitSemaphore(&sem->semaphore) > 0;
}

bool SDL_WaitSemaphoreTimeoutNS(SDL_Semaphore *sem, Sint64 timeoutNS)
{
    if (!sem) {
        return true;
    }

    if (timeoutNS < 0) {
        OSWaitSemaphore(&sem->semaphore);
        return true;
    }

    if (timeoutNS == 0) {
        return OSTryWaitSemaphore(&sem->semaphore) > 0;
    }

    if (OSTryWaitSemaphore(&sem->semaphore) > 0) {
        return true;
    }

    return SDL_WiiUWaitSemaphoreTimeout(sem, timeoutNS);
}

Uint32 SDL_GetSemaphoreValue(SDL_Semaphore *sem)
{
    int32_t count;

    if (!sem) {
        return 0;
    }

    count = OSGetSemaphoreCount(&sem->semaphore);
    return (Uint32)SDL_max(count, 0);
}

void SDL_SignalSemaphore(SDL_Semaphore *sem)
{
    if (!sem) {
        return;
    }

    OSSignalSemaphore(&sem->semaphore);
}

#endif // SDL_THREAD_WIIU
