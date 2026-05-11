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

#include "SDL_wiiulifecycle.h"

#include "../../events/SDL_events_c.h"
#include "../../video/SDL_sysvideo.h"

#include <coreinit/core.h>
#include <coreinit/foreground.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>

typedef enum SDL_WiiULifecyclePhase
{
    SDL_WIIU_LIFECYCLE_FOREGROUND,
    SDL_WIIU_LIFECYCLE_RELEASING_FOREGROUND,
    SDL_WIIU_LIFECYCLE_BACKGROUND,
    SDL_WIIU_LIFECYCLE_EXITING
} SDL_WiiULifecyclePhase;

typedef struct SDL_WiiULifecycleState
{
    bool initialized;
    bool procui_active;
    bool release_acknowledged;
    bool background_notified;
    bool exit_requested;
    bool has_last_status;
    SDL_WiiULifecyclePhase phase;
    ProcUIStatus last_status;
    SDL_WIIU_LifecycleCallback on_acquire;
    SDL_WIIU_LifecycleCallback on_release;
    void *callback_userdata;
} SDL_WiiULifecycleState;

static SDL_WiiULifecycleState SDL_wiiu_lifecycle;

static const char *SDL_WIIU_GetLifecyclePhaseName(SDL_WiiULifecyclePhase phase)
{
    switch (phase) {
    case SDL_WIIU_LIFECYCLE_FOREGROUND:
        return "foreground";
    case SDL_WIIU_LIFECYCLE_RELEASING_FOREGROUND:
        return "releasing-foreground";
    case SDL_WIIU_LIFECYCLE_BACKGROUND:
        return "background";
    case SDL_WIIU_LIFECYCLE_EXITING:
        return "exiting";
    default:
        return "unknown";
    }
}

static const char *SDL_WIIU_GetProcUIStatusName(ProcUIStatus status)
{
    switch (status) {
    case PROCUI_STATUS_IN_FOREGROUND:
        return "PROCUI_STATUS_IN_FOREGROUND";
    case PROCUI_STATUS_IN_BACKGROUND:
        return "PROCUI_STATUS_IN_BACKGROUND";
    case PROCUI_STATUS_RELEASE_FOREGROUND:
        return "PROCUI_STATUS_RELEASE_FOREGROUND";
    case PROCUI_STATUS_EXITING:
        return "PROCUI_STATUS_EXITING";
    default:
        return "PROCUI_STATUS_UNKNOWN";
    }
}

static void SDL_WIIU_LogProcUIStatus(ProcUIStatus status)
{
    if (SDL_wiiu_lifecycle.has_last_status && SDL_wiiu_lifecycle.last_status == status) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                "WiiU lifecycle: ProcUI status -> %s (phase=%s, release_ack=%d)",
                SDL_WIIU_GetProcUIStatusName(status),
                SDL_WIIU_GetLifecyclePhaseName(SDL_wiiu_lifecycle.phase),
                SDL_wiiu_lifecycle.release_acknowledged ? 1 : 0);
    SDL_wiiu_lifecycle.last_status = status;
    SDL_wiiu_lifecycle.has_last_status = true;
}

static uint32_t SDL_WIIUSaveCallback(void *userdata)
{
    (void)userdata;
    OSSavesDone_ReadyToRelease();
    return 0;
}

static void SDL_WIIUSetExiting(bool send_events)
{
    if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_EXITING) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                "WiiU lifecycle: entering exit phase from %s (send_events=%d)",
                SDL_WIIU_GetLifecyclePhaseName(SDL_wiiu_lifecycle.phase),
                send_events ? 1 : 0);
    SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_EXITING;
    if (send_events) {
        SDL_SendQuit();
        SDL_OnApplicationWillTerminate();
    }
}

static void SDL_WIIURequestSystemExit(void)
{
    if (!SDL_wiiu_lifecycle.initialized || !SDL_wiiu_lifecycle.procui_active) {
        return;
    }

    if (ProcUIInShutdown()) {
        SDL_WIIUSetExiting(false);
        return;
    }

    if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_EXITING || SDL_wiiu_lifecycle.exit_requested) {
        return;
    }

    if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_BACKGROUND) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                    "WiiU lifecycle: skipping SYSLaunchMenu() while in background");
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                "WiiU lifecycle: requesting HOME menu for application exit");
    SDL_wiiu_lifecycle.exit_requested = true;
    SYSLaunchMenu();
}

static void SDL_WIIUReleaseForeground(void)
{
    if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_FOREGROUND) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: release requested, starting foreground handoff");
        SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_RELEASING_FOREGROUND;
        SDL_wiiu_lifecycle.background_notified = false;
        SDL_OnApplicationWillEnterBackground();
        if (SDL_wiiu_lifecycle.on_release) {
            SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: invoking release callback");
            SDL_wiiu_lifecycle.on_release(SDL_wiiu_lifecycle.callback_userdata);
        }
    }

    if (!SDL_wiiu_lifecycle.release_acknowledged) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: acknowledging foreground release with ProcUIDrawDoneRelease()");
        ProcUIDrawDoneRelease();
        SDL_wiiu_lifecycle.release_acknowledged = true;
    }
}

static void SDL_WIIUEnterBackground(const char *reason)
{
    if (!SDL_wiiu_lifecycle.background_notified) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: entered background (%s)", reason ? reason : "unknown");
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: leaving foreground-only work suspended until acquire or exit");
        SDL_OnApplicationDidEnterBackground();
        SDL_wiiu_lifecycle.background_notified = true;
    }

    SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_BACKGROUND;
}

static void SDL_WIIUAcquireForeground(void)
{
    SDL_WiiULifecyclePhase previous_phase = SDL_wiiu_lifecycle.phase;

    if (previous_phase == SDL_WIIU_LIFECYCLE_RELEASING_FOREGROUND &&
        !SDL_wiiu_lifecycle.background_notified) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                    "WiiU lifecycle: ProcUI restored foreground without surfacing PROCUI_STATUS_IN_BACKGROUND; synthesizing background completion");
        SDL_WIIUEnterBackground("blocking ProcUI wait");
        previous_phase = SDL_wiiu_lifecycle.phase;
    }

    if (previous_phase == SDL_WIIU_LIFECYCLE_RELEASING_FOREGROUND ||
        previous_phase == SDL_WIIU_LIFECYCLE_BACKGROUND) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                    "WiiU lifecycle: foreground restored from %s",
                    SDL_WIIU_GetLifecyclePhaseName(previous_phase));
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                    "WiiU lifecycle: acquire callback present=%d, release callback present=%d",
                    SDL_wiiu_lifecycle.on_acquire ? 1 : 0,
                    SDL_wiiu_lifecycle.on_release ? 1 : 0);
        SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_FOREGROUND;
        SDL_OnApplicationWillEnterForeground();
        if (SDL_wiiu_lifecycle.on_acquire) {
            SDL_wiiu_lifecycle.on_acquire(SDL_wiiu_lifecycle.callback_userdata);
        }
        SDL_OnApplicationDidEnterForeground();
    }

    SDL_wiiu_lifecycle.release_acknowledged = false;
    SDL_wiiu_lifecycle.background_notified = false;
}

static void SDL_WIIUHandleLifecycleStatus(ProcUIStatus status, bool send_exit_events)
{
    SDL_WIIU_LogProcUIStatus(status);

    switch (status) {
    case PROCUI_STATUS_IN_FOREGROUND:
        SDL_WIIUAcquireForeground();
        break;

    case PROCUI_STATUS_RELEASE_FOREGROUND:
        SDL_WIIUReleaseForeground();
        break;

    case PROCUI_STATUS_IN_BACKGROUND:
        if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_FOREGROUND) {
            SDL_WIIUReleaseForeground();
        }
        SDL_WIIUEnterBackground("ProcUI status");
        break;

    case PROCUI_STATUS_EXITING:
        SDL_WIIUSetExiting(send_exit_events);
        break;
    }
}

static void SDL_WIIU_PumpLifecycleInternal(bool send_exit_events)
{
    ProcUIStatus status;

    if (!SDL_wiiu_lifecycle.initialized || !SDL_wiiu_lifecycle.procui_active) {
        return;
    }

    if (!OSIsMainCore()) {
        ProcUISubProcessMessages(TRUE);
        return;
    }

    do {
        status = ProcUIProcessMessages(TRUE);
        SDL_WIIUHandleLifecycleStatus(status, send_exit_events);
    } while (status == PROCUI_STATUS_RELEASE_FOREGROUND &&
             SDL_wiiu_lifecycle.phase != SDL_WIIU_LIFECYCLE_EXITING);

    if (status == PROCUI_STATUS_IN_BACKGROUND &&
        SDL_wiiu_lifecycle.phase != SDL_WIIU_LIFECYCLE_EXITING) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: waiting in background for foreground restore or exit");
        do {
            status = ProcUIProcessMessages(TRUE);
            SDL_WIIUHandleLifecycleStatus(status, send_exit_events);
        } while (status == PROCUI_STATUS_IN_BACKGROUND &&
                 SDL_wiiu_lifecycle.phase != SDL_WIIU_LIFECYCLE_EXITING);
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                    "WiiU lifecycle: background wait finished on %s",
                    SDL_WIIU_GetProcUIStatusName(status));
    }
}

bool SDL_WIIU_InitLifecycle(void)
{
    if (SDL_wiiu_lifecycle.initialized) {
        return true;
    }

    SDL_zero(SDL_wiiu_lifecycle);
    SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_FOREGROUND;

    SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: initializing ProcUI");
    ProcUIInitEx(SDL_WIIUSaveCallback, NULL);
    SDL_wiiu_lifecycle.procui_active = true;
    SDL_wiiu_lifecycle.initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM, "WiiU lifecycle: ProcUI initialized");
    return true;
}

void SDL_WIIU_EnsureSystemExit(void)
{
    if (!SDL_wiiu_lifecycle.initialized || !SDL_wiiu_lifecycle.procui_active) {
        return;
    }

    if (!OSIsMainCore()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                    "WiiU lifecycle: skipping exit request outside the main core");
        return;
    }

    if (ProcUIInShutdown()) {
        SDL_WIIUSetExiting(false);
        return;
    }

    SDL_WIIURequestSystemExit();

    if (SDL_wiiu_lifecycle.exit_requested &&
        SDL_wiiu_lifecycle.phase != SDL_WIIU_LIFECYCLE_EXITING) {
        SDL_WIIU_PumpLifecycleInternal(false);
    }
}

void SDL_WIIU_QuitLifecycle(void)
{
    if (!SDL_wiiu_lifecycle.initialized) {
        return;
    }

    if (SDL_wiiu_lifecycle.procui_active && ProcUIIsRunning()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                    "WiiU lifecycle: shutting down ProcUI from phase %s",
                    SDL_WIIU_GetLifecyclePhaseName(SDL_wiiu_lifecycle.phase));
        ProcUIShutdown();
    }

    SDL_zero(SDL_wiiu_lifecycle);
}

void SDL_WIIU_PumpLifecycle(void)
{
    SDL_WIIU_PumpLifecycleInternal(true);
}

bool SDL_WIIU_IsInForeground(void)
{
    return SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_FOREGROUND;
}

void SDL_WIIU_SetLifecycleCallbacks(SDL_WIIU_LifecycleCallback on_acquire,
                                    SDL_WIIU_LifecycleCallback on_release,
                                    void *userdata)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_SYSTEM,
                "WiiU lifecycle: set callbacks acquire=%d release=%d userdata=%p",
                on_acquire ? 1 : 0,
                on_release ? 1 : 0,
                userdata);
    SDL_wiiu_lifecycle.on_acquire = on_acquire;
    SDL_wiiu_lifecycle.on_release = on_release;
    SDL_wiiu_lifecycle.callback_userdata = userdata;
}

#endif // SDL_PLATFORM_WIIU
