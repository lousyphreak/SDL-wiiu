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

#include <coreinit/thread.h>

#include "../SDL_systhread.h"
#include "../SDL_thread_c.h"

#define WIIU_THREAD_STACK_ALIGNMENT       16
#define WIIU_THREAD_STACK_SIZE_DEFAULT    (128 * 1024)
#define WIIU_THREAD_PRIORITY_LOW          20
#define WIIU_THREAD_PRIORITY_NORMAL       16
#define WIIU_THREAD_PRIORITY_HIGH         12
#define WIIU_THREAD_PRIORITY_TIMECRITICAL 8
#define WIIU_THREAD_RUN_QUANTUM_US        1000

SDL_COMPILE_TIME_ASSERT(wiiu_thread_pointer_fits_in_int32, sizeof(void *) <= sizeof(int32_t));

typedef struct SDL_WiiUThreadData
{
    OSThread thread;
    void *stack_base;
} SDL_WiiUThreadData;

static size_t SDL_WiiUGetStackSize(size_t requested_size)
{
    size_t stack_size = requested_size ? requested_size : WIIU_THREAD_STACK_SIZE_DEFAULT;
    return (stack_size + (WIIU_THREAD_STACK_ALIGNMENT - 1)) & ~(WIIU_THREAD_STACK_ALIGNMENT - 1);
}

static int SDL_WiiUMapThreadPriority(SDL_ThreadPriority priority)
{
    switch (priority) {
    case SDL_THREAD_PRIORITY_LOW:
        return WIIU_THREAD_PRIORITY_LOW;
    case SDL_THREAD_PRIORITY_HIGH:
        return WIIU_THREAD_PRIORITY_HIGH;
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        return WIIU_THREAD_PRIORITY_TIMECRITICAL;
    case SDL_THREAD_PRIORITY_NORMAL:
    default:
        return WIIU_THREAD_PRIORITY_NORMAL;
    }
}

static int SDL_WiiUThreadEntry(int argc, const char **argv)
{
    (void)argv;
    SDL_RunThread((SDL_Thread *)(uintptr_t)(uint32_t)argc);
    return 0;
}

static void SDL_WiiUThreadDeallocator(OSThread *thread, void *stack)
{
    SDL_WiiUThreadData *data = (SDL_WiiUThreadData *)thread;
    (void)stack;

    SDL_aligned_free(data->stack_base);
    SDL_free(data);
}

bool SDL_SYS_CreateThread(SDL_Thread *thread,
                          SDL_FunctionPointer pfnBeginThread,
                          SDL_FunctionPointer pfnEndThread)
{
    SDL_WiiUThreadData *data;
    OSThread *current_thread;
    size_t stack_size;
    int priority = WIIU_THREAD_PRIORITY_NORMAL;

    (void)pfnBeginThread;
    (void)pfnEndThread;

    data = (SDL_WiiUThreadData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return false;
    }

    stack_size = SDL_WiiUGetStackSize(thread->stacksize);
    data->stack_base = SDL_aligned_alloc(WIIU_THREAD_STACK_ALIGNMENT, stack_size);
    if (!data->stack_base) {
        SDL_free(data);
        return false;
    }

    current_thread = OSGetCurrentThread();
    if (current_thread) {
        const int current_priority = OSGetThreadPriority(current_thread);
        if (current_priority >= 0 && current_priority <= 31) {
            priority = current_priority;
        }
    }

    if (!OSCreateThread(&data->thread,
                        SDL_WiiUThreadEntry,
                        (int32_t)(uintptr_t)thread,
                        NULL,
                        (char *)data->stack_base + stack_size,
                        (uint32_t)stack_size,
                        priority,
                        OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        SDL_aligned_free(data->stack_base);
        SDL_free(data);
        return SDL_SetError("OSCreateThread() failed");
    }

    thread->handle = &data->thread;
    thread->threadid = (SDL_ThreadID)(uintptr_t)thread->handle;
    thread->data = data;

    OSSetThreadRunQuantum(&data->thread, WIIU_THREAD_RUN_QUANTUM_US);
    OSResumeThread(&data->thread);
    return true;
}

void SDL_SYS_SetupThread(const char *name)
{
    if (name && *name) {
        OSSetThreadName(OSGetCurrentThread(), name);
    }
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
    return (SDL_ThreadID)(uintptr_t)OSGetCurrentThread();
}

bool SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
    OSThread *current_thread = OSGetCurrentThread();
    if (!current_thread) {
        return SDL_SetError("OSGetCurrentThread() failed");
    }

    if (!OSSetThreadPriority(current_thread, SDL_WiiUMapThreadPriority(priority))) {
        return SDL_SetError("OSSetThreadPriority() failed");
    }

    return true;
}

void SDL_SYS_WaitThread(SDL_Thread *thread)
{
    SDL_WiiUThreadData *data = (SDL_WiiUThreadData *)thread->data;
    int result = 0;
    const BOOL ok = OSJoinThread(thread->handle, &result);

    SDL_assert(ok);
    if (ok && data) {
        SDL_aligned_free(data->stack_base);
        SDL_free(data);
        thread->data = NULL;
    }
}

void SDL_SYS_DetachThread(SDL_Thread *thread)
{
    SDL_WiiUThreadData *data = (SDL_WiiUThreadData *)thread->data;
    if (data) {
        OSSetThreadDeallocator(thread->handle, SDL_WiiUThreadDeallocator);
        OSDetachThread(thread->handle);
        thread->data = NULL;
    }
}

#endif // SDL_THREAD_WIIU
