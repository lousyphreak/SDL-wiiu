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
#include <coreinit/systeminfo.h>
#include <coreinit/title.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>

#define SDL_WIIU_HBL_TITLE_ID           0x0005000013374842ULL
#define SDL_WIIU_MII_MAKER_JPN_TITLE_ID 0x000500101004A000ULL
#define SDL_WIIU_MII_MAKER_USA_TITLE_ID 0x000500101004A100ULL
#define SDL_WIIU_MII_MAKER_EUR_TITLE_ID 0x000500101004A200ULL

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
    bool from_hbl;
    bool release_acknowledged;
    SDL_WiiULifecyclePhase phase;
    SDL_AtomicInt home_button_exit_requested;
    SDL_WIIU_LifecycleCallback on_acquire;
    SDL_WIIU_LifecycleCallback on_release;
    void *callback_userdata;
} SDL_WiiULifecycleState;

static SDL_WiiULifecycleState SDL_wiiu_lifecycle;

static bool SDL_WIIU_IsHBLTitleID(uint64_t title_id)
{
    return title_id == SDL_WIIU_HBL_TITLE_ID ||
           title_id == SDL_WIIU_MII_MAKER_JPN_TITLE_ID ||
           title_id == SDL_WIIU_MII_MAKER_USA_TITLE_ID ||
           title_id == SDL_WIIU_MII_MAKER_EUR_TITLE_ID;
}

static uint32_t SDL_WIIUSaveCallback(void *userdata)
{
    (void)userdata;
    OSSavesDone_ReadyToRelease();
    return 0;
}

static uint32_t SDL_WIIUHomeButtonDenied(void *userdata)
{
    (void)userdata;
    SDL_SetAtomicInt(&SDL_wiiu_lifecycle.home_button_exit_requested, 1);
    return 0;
}

static void SDL_WIIURequestExit(void)
{
    if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_EXITING) {
        return;
    }

    SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_EXITING;
    SDL_SendQuit();
    SDL_OnApplicationWillTerminate();
}

bool SDL_WIIU_InitLifecycle(void)
{
    uint64_t title_id;

    if (SDL_wiiu_lifecycle.initialized) {
        return true;
    }

    SDL_zero(SDL_wiiu_lifecycle);
    SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_FOREGROUND;

    title_id = OSGetTitleID();
    SDL_wiiu_lifecycle.from_hbl = SDL_WIIU_IsHBLTitleID(title_id);
    if (SDL_wiiu_lifecycle.from_hbl) {
        OSEnableHomeButtonMenu(FALSE);
    }

    ProcUIInitEx(SDL_WIIUSaveCallback, NULL);
    SDL_wiiu_lifecycle.procui_active = true;

    if (SDL_wiiu_lifecycle.from_hbl) {
        ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED,
                               SDL_WIIUHomeButtonDenied, NULL, 100);
    }

    SDL_SetAtomicInt(&SDL_wiiu_lifecycle.home_button_exit_requested, 0);
    SDL_wiiu_lifecycle.initialized = true;
    return true;
}

void SDL_WIIU_QuitLifecycle(void)
{
    const bool from_hbl = SDL_wiiu_lifecycle.from_hbl;

    if (!SDL_wiiu_lifecycle.initialized) {
        return;
    }

    if (SDL_wiiu_lifecycle.procui_active && ProcUIIsRunning()) {
        ProcUIShutdown();
    }

    SDL_zero(SDL_wiiu_lifecycle);

    if (from_hbl) {
        SYSRelaunchTitle(0, NULL);
    }
}

void SDL_WIIU_PumpLifecycle(void)
{
    ProcUIStatus status;

    if (!SDL_wiiu_lifecycle.initialized || !SDL_wiiu_lifecycle.procui_active) {
        return;
    }

    if (!OSIsMainCore()) {
        ProcUISubProcessMessages(TRUE);
        return;
    }

    status = ProcUIProcessMessages(TRUE);

    switch (status) {
    case PROCUI_STATUS_IN_FOREGROUND:
        if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_RELEASING_FOREGROUND ||
            SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_BACKGROUND) {
            SDL_OnApplicationWillEnterForeground();
            if (SDL_wiiu_lifecycle.on_acquire) {
                SDL_wiiu_lifecycle.on_acquire(SDL_wiiu_lifecycle.callback_userdata);
            }
            SDL_OnApplicationDidEnterForeground();
        }
        SDL_wiiu_lifecycle.release_acknowledged = false;
        SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_FOREGROUND;
        break;

    case PROCUI_STATUS_RELEASE_FOREGROUND:
        if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_FOREGROUND) {
            SDL_OnApplicationWillEnterBackground();
            if (SDL_wiiu_lifecycle.on_release) {
                SDL_wiiu_lifecycle.on_release(SDL_wiiu_lifecycle.callback_userdata);
            }
            SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_RELEASING_FOREGROUND;
        }
        if (!SDL_wiiu_lifecycle.release_acknowledged) {
            ProcUIDrawDoneRelease();
            SDL_wiiu_lifecycle.release_acknowledged = true;
        }
        break;

    case PROCUI_STATUS_IN_BACKGROUND:
        if (SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_FOREGROUND) {
            SDL_OnApplicationWillEnterBackground();
        }
        if (SDL_wiiu_lifecycle.phase != SDL_WIIU_LIFECYCLE_BACKGROUND) {
            SDL_OnApplicationDidEnterBackground();
        }
        SDL_wiiu_lifecycle.phase = SDL_WIIU_LIFECYCLE_BACKGROUND;
        break;

    case PROCUI_STATUS_EXITING:
        SDL_WIIURequestExit();
        break;
    }

    if (SDL_GetAtomicInt(&SDL_wiiu_lifecycle.home_button_exit_requested)) {
        SDL_WIIURequestExit();
    }
}

bool SDL_WIIU_IsInForeground(void)
{
    return SDL_wiiu_lifecycle.phase == SDL_WIIU_LIFECYCLE_FOREGROUND;
}

void SDL_WIIU_SetLifecycleCallbacks(SDL_WIIU_LifecycleCallback on_acquire,
                                    SDL_WIIU_LifecycleCallback on_release,
                                    void *userdata)
{
    SDL_wiiu_lifecycle.on_acquire = on_acquire;
    SDL_wiiu_lifecycle.on_release = on_release;
    SDL_wiiu_lifecycle.callback_userdata = userdata;
}

#endif // SDL_PLATFORM_WIIU
