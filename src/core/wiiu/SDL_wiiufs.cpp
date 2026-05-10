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

#include "SDL_wiiufs.h"

#include "../../filesystem/SDL_filesystem_c.h"

#include <coreinit/filesystem.h>
#include <coreinit/title.h>
#include <nn/act.h>
#include <nn/acp/client.h>
#include <nn/acp/device.h>
#include <nn/acp/result.h>
#include <nn/acp/save.h>
#include <nn/acp/title.h>
#include <nn/result.h>
#include <nn/save/save.h>

#define SDL_WIIU_CONTENT_ROOT "/vol/content/"
#define SDL_WIIU_COMMON_SAVE_SLOT 0xFF
#define SDL_WIIU_MAX_ACCOUNT_SLOT 12
#define SDL_WIIU_SAVE_ROOT_CAPACITY 128
#define SDL_WIIU_SAVE_DIR_QUERY_CAPACITY 16

typedef struct SDL_WiiUFSState
{
    bool initialized;
    bool fs_initialized;
    bool fs_client_ready;
    bool acp_initialized;
    bool act_initialized;
    bool save_initialized;
    bool save_support_ready;
    bool external_storage_mounted;
    bool save_dir_mounted;
    ACPTitleId title_id;
    ACPDeviceType device_type;
    Uint8 save_slot;
    Uint32 persistent_id;
    bool using_account_save;
    char save_root[SDL_WIIU_SAVE_ROOT_CAPACITY];
    FSClient fs_client;
    FSCmdBlock fs_cmd_block;
} SDL_WiiUFSState;

static SDL_WiiUFSState SDL_wiiu_fs_state;
static SDL_Mutex *SDL_wiiu_fs_lock;

static bool SDL_WIIU_LockFSState(void)
{
    if (!SDL_wiiu_fs_lock) {
        SDL_wiiu_fs_lock = SDL_CreateMutex();
        if (!SDL_wiiu_fs_lock) {
            return false;
        }
    }

    SDL_LockMutex(SDL_wiiu_fs_lock);
    return true;
}

static void SDL_WIIU_UnlockFSState(void)
{
    if (SDL_wiiu_fs_lock) {
        SDL_UnlockMutex(SDL_wiiu_fs_lock);
    }
}

static bool SDL_WIIU_SetACPError(const char *what, ACPResult result)
{
    return SDL_SetError("%s failed (%d)", what, (int) result);
}

static bool SDL_WIIU_SetFSError(const char *what, FSStatus status)
{
    return SDL_SetError("%s failed (%d)", what, (int) status);
}

static bool SDL_WIIU_SetSaveError(const char *what, SAVEStatus status)
{
    return SDL_SetError("%s failed (%d)", what, (int) status);
}

static bool SDL_WIIU_AcceptACPResult(ACPResult result)
{
    return (result == ACP_RESULT_SUCCESS) ||
           (result == ACP_RESULT_ALREADY_DONE) ||
           (result == ACP_RESULT_ALREADY_EXISTS);
}

static bool SDL_WIIU_IsValidAccountSlot(Uint8 slot)
{
    return (slot >= 1) && (slot <= SDL_WIIU_MAX_ACCOUNT_SLOT);
}

static void SDL_WIIU_MakeFallbackSaveRoot(char *buffer, size_t buffer_size, Uint32 persistent_id)
{
    if (persistent_id != 0) {
        (void) SDL_snprintf(buffer, buffer_size, "/vol/save/%08" SDL_PRIX32 "/", persistent_id);
    } else {
        (void) SDL_strlcpy(buffer, "/vol/save/common/", buffer_size);
    }
}

static void SDL_WIIU_NormalizeSaveRoot(char *buffer, size_t buffer_size, const char *path)
{
    size_t length;

    if (!buffer || (buffer_size == 0)) {
        return;
    }

    buffer[0] = '\0';
    if (!path || !*path) {
        return;
    }

    (void) SDL_strlcpy(buffer, path, buffer_size);
    length = SDL_strlen(buffer);
    if ((length > 0) && (buffer[length - 1] != '/')) {
        if ((length + 1) < buffer_size) {
            buffer[length] = '/';
            buffer[length + 1] = '\0';
        }
    }
}

static bool SDL_WIIU_TryGetCurrentAccount(Uint8 *slot_out, Uint32 *persistent_id_out)
{
    Uint8 slot;
    Uint32 persistent_id;

    if (!SDL_wiiu_fs_state.act_initialized) {
        return false;
    }

    slot = nn::act::GetSlotNo();
    if (!SDL_WIIU_IsValidAccountSlot(slot)) {
        slot = nn::act::GetDefaultAccount();
    }
    if (!SDL_WIIU_IsValidAccountSlot(slot)) {
        return false;
    }

    persistent_id = nn::act::GetPersistentIdEx(slot);
    if (persistent_id == 0) {
        return false;
    }

    if (slot_out) {
        *slot_out = slot;
    }
    if (persistent_id_out) {
        *persistent_id_out = persistent_id;
    }
    return true;
}

static bool SDL_WIIU_SelectSaveTarget(ACPTitleId title_id, bool have_meta, const ACPMetaXml *meta)
{
    const bool can_use_account = !have_meta || (meta->account_save_size > 0);
    const bool can_use_common = !have_meta || (meta->common_save_size > 0);
    Uint8 account_slot = 0;
    Uint32 persistent_id = 0;

    if (can_use_account && SDL_WIIU_TryGetCurrentAccount(&account_slot, &persistent_id)) {
        SDL_wiiu_fs_state.using_account_save = true;
        SDL_wiiu_fs_state.save_slot = account_slot;
        SDL_wiiu_fs_state.persistent_id = persistent_id;
        return true;
    }

    if (can_use_common) {
        SDL_wiiu_fs_state.using_account_save = false;
        SDL_wiiu_fs_state.save_slot = SDL_WIIU_COMMON_SAVE_SLOT;
        SDL_wiiu_fs_state.persistent_id = 0;
        return true;
    }

    if (have_meta && (meta->account_save_size > 0)) {
        return SDL_SetError("Wii U title %" SDL_PRIu64 " only exposes account save, but no current account is available", (Uint64) title_id);
    }

    return SDL_SetError("Wii U title %" SDL_PRIu64 " does not expose account or common save data", (Uint64) title_id);
}

static void SDL_WIIU_CleanupFailedInit(void)
{
    if (SDL_wiiu_fs_state.save_initialized) {
        SAVEShutdown();
    }
    if (SDL_wiiu_fs_state.save_dir_mounted) {
        (void) ACPUnmountSaveDir();
    }
    if (SDL_wiiu_fs_state.external_storage_mounted) {
        (void) ACPUnmountExternalStorage();
    }
    if (SDL_wiiu_fs_state.act_initialized) {
        (void) nn::act::Finalize();
    }
    if (SDL_wiiu_fs_state.acp_initialized) {
        ACPFinalize();
    }
    if (SDL_wiiu_fs_state.fs_client_ready) {
        (void) FSDelClient(&SDL_wiiu_fs_state.fs_client, FS_ERROR_FLAG_NONE);
    }
    if (SDL_wiiu_fs_state.fs_initialized) {
        FSShutdown();
    }
    SDL_zero(SDL_wiiu_fs_state);
}

static void SDL_WIIU_MaybeUseQueriedSaveRoot(ACPTitleId title_id, bool have_meta)
{
    ACPSaveDirInfo *save_dirs = NULL;
    Uint32 count = 0;
    ACPResult result;

    SDL_WIIU_MakeFallbackSaveRoot(SDL_wiiu_fs_state.save_root, sizeof(SDL_wiiu_fs_state.save_root), SDL_wiiu_fs_state.persistent_id);

    save_dirs = (ACPSaveDirInfo *) SDL_aligned_alloc(64, sizeof(*save_dirs) * SDL_WIIU_SAVE_DIR_QUERY_CAPACITY);
    if (!save_dirs) {
        return;
    }

    SDL_memset(save_dirs, 0, sizeof(*save_dirs) * SDL_WIIU_SAVE_DIR_QUERY_CAPACITY);
    if (have_meta) {
        result = ACPGetTitleSaveDirEx(title_id,
                                      SDL_wiiu_fs_state.device_type,
                                      0,
                                      save_dirs,
                                      SDL_WIIU_SAVE_DIR_QUERY_CAPACITY,
                                      &count);
    } else {
        result = ACPGetTitleSaveDirExWithoutMetaCheck(title_id,
                                                      SDL_wiiu_fs_state.device_type,
                                                      0,
                                                      save_dirs,
                                                      SDL_WIIU_SAVE_DIR_QUERY_CAPACITY,
                                                      &count);
    }

    if (result == ACP_RESULT_SUCCESS) {
        for (Uint32 i = 0; i < count; ++i) {
            if (save_dirs[i].persistentId == SDL_wiiu_fs_state.persistent_id &&
                SDL_strncmp(save_dirs[i].path, "/vol/save/", SDL_strlen("/vol/save/")) == 0) {
                SDL_WIIU_NormalizeSaveRoot(SDL_wiiu_fs_state.save_root, sizeof(SDL_wiiu_fs_state.save_root), save_dirs[i].path);
                break;
            }
        }
    }

    SDL_aligned_free(save_dirs);
}

static bool SDL_WIIU_EnsureSaveStateLocked(void)
{
    ACPMetaXml *meta = NULL;
    ACPResult acp_result;
    NNResult act_result;
    SAVEStatus save_status;
    FSStatus fs_status;
    BOOL external_storage_required = FALSE;
    bool have_meta = false;
    ACPTitleId title_id = 0;

    if (SDL_wiiu_fs_state.initialized) {
        return true;
    }

    FSInit();
    SDL_wiiu_fs_state.fs_initialized = true;

    SDL_zero(SDL_wiiu_fs_state.fs_client);
    fs_status = FSAddClient(&SDL_wiiu_fs_state.fs_client, FS_ERROR_FLAG_NONE);
    if (fs_status != FS_STATUS_OK) {
        SDL_WIIU_SetFSError("FSAddClient", fs_status);
        goto failed;
    }
    SDL_wiiu_fs_state.fs_client_ready = true;

    SDL_zero(SDL_wiiu_fs_state.fs_cmd_block);
    FSInitCmdBlock(&SDL_wiiu_fs_state.fs_cmd_block);

    ACPInitialize();
    SDL_wiiu_fs_state.acp_initialized = true;

    act_result = nn::act::Initialize();
    if (NNResult_IsSuccess(act_result)) {
        SDL_wiiu_fs_state.act_initialized = true;
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_SYSTEM, "Wii U ACT initialization failed (%d), falling back to common save when needed", act_result.value);
    }

    acp_result = ACPGetTitleIdOfMainApplication(&title_id);
    if (acp_result != ACP_RESULT_SUCCESS) {
        title_id = OSGetTitleID();
        if (title_id == 0) {
            SDL_WIIU_SetACPError("ACPGetTitleIdOfMainApplication", acp_result);
            goto failed;
        }
    }
    SDL_wiiu_fs_state.title_id = title_id;
    SDL_wiiu_fs_state.device_type = ACP_DEVICE_TYPE_AUTO;

    meta = (ACPMetaXml *) SDL_aligned_alloc(64, sizeof(*meta));
    if (meta) {
        SDL_zero(*meta);
        if (ACPGetTitleMetaXml(title_id, meta) == ACP_RESULT_SUCCESS) {
            have_meta = true;
        }
    }

    if (!SDL_WIIU_SelectSaveTarget(title_id, have_meta, meta)) {
        goto failed;
    }

    acp_result = ACPIsExternalStorageRequired(&external_storage_required);
    if (acp_result != ACP_RESULT_SUCCESS) {
        SDL_WIIU_SetACPError("ACPIsExternalStorageRequired", acp_result);
        goto failed;
    }
    if (external_storage_required) {
        acp_result = ACPMountExternalStorage();
        if (!SDL_WIIU_AcceptACPResult(acp_result)) {
            SDL_WIIU_SetACPError("ACPMountExternalStorage", acp_result);
            goto failed;
        }
        SDL_wiiu_fs_state.external_storage_mounted = true;
    }

    acp_result = ACPCreateSaveDir(SDL_wiiu_fs_state.persistent_id, SDL_wiiu_fs_state.device_type);
    if (!SDL_WIIU_AcceptACPResult(acp_result)) {
        SDL_WIIU_SetACPError("ACPCreateSaveDir", acp_result);
        goto failed;
    }

    acp_result = ACPMountSaveDir();
    if (!SDL_WIIU_AcceptACPResult(acp_result)) {
        SDL_WIIU_SetACPError("ACPMountSaveDir", acp_result);
        goto failed;
    }
    SDL_wiiu_fs_state.save_dir_mounted = true;

    save_status = SAVEInit();
    if (save_status == SAVE_STATUS_OK) {
        SDL_wiiu_fs_state.save_initialized = true;
        if (SDL_wiiu_fs_state.using_account_save) {
            save_status = SAVEInitAccountSaveDir(SDL_wiiu_fs_state.save_slot);
        } else {
            save_status = SAVEInitCommonSaveDir();
        }

        if (save_status == SAVE_STATUS_OK) {
            SDL_wiiu_fs_state.save_support_ready = true;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_SYSTEM,
                        "Wii U SAVEInit* failed (%d), continuing without quota flush helpers",
                        (int) save_status);
        }
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_SYSTEM,
                    "Wii U SAVEInit failed (%d), continuing without quota flush helpers",
                    (int) save_status);
    }

    SDL_WIIU_MaybeUseQueriedSaveRoot(title_id, have_meta);
    SDL_wiiu_fs_state.initialized = true;
    SDL_aligned_free(meta);
    return true;

failed:
    SDL_aligned_free(meta);
    SDL_WIIU_CleanupFailedInit();
    return false;
}

static bool SDL_WIIU_BuildPrefPathLocked(const char *org, const char *app, char **path_out)
{
    char *path = NULL;
    int rc;

    if (!path_out) {
        return SDL_InvalidParamError("path_out");
    }

    if (*org) {
        if (*app) {
            rc = SDL_asprintf(&path, "%s%s/%s/", SDL_wiiu_fs_state.save_root, org, app);
        } else {
            rc = SDL_asprintf(&path, "%s%s/", SDL_wiiu_fs_state.save_root, org);
        }
    } else if (*app) {
        rc = SDL_asprintf(&path, "%s%s/", SDL_wiiu_fs_state.save_root, app);
    } else {
        path = SDL_strdup(SDL_wiiu_fs_state.save_root);
        rc = path ? 0 : -1;
    }

    if (rc < 0 || !path) {
        return false;
    }

    *path_out = path;
    return true;
}

extern "C" char *SDL_WIIU_GetBasePath(void)
{
    return SDL_strdup(SDL_WIIU_CONTENT_ROOT);
}

extern "C" char *SDL_WIIU_GetPrefPath(const char *org, const char *app)
{
    char *path = NULL;

    if (!SDL_WIIU_LockFSState()) {
        return NULL;
    }

    if (!SDL_WIIU_EnsureSaveStateLocked()) {
        SDL_WIIU_UnlockFSState();
        return NULL;
    }
    if (!SDL_WIIU_BuildPrefPathLocked(org, app, &path)) {
        SDL_WIIU_UnlockFSState();
        return NULL;
    }

    SDL_WIIU_UnlockFSState();

    if (!SDL_CreateDirectory(path)) {
        SDL_free(path);
        return NULL;
    }

    return path;
}

extern "C" bool SDL_WIIU_FlushUserSave(void)
{
    bool result = true;

    if (!SDL_WIIU_LockFSState()) {
        return false;
    }

    if (!SDL_WIIU_EnsureSaveStateLocked()) {
        SDL_WIIU_UnlockFSState();
        return false;
    }

    if (SDL_wiiu_fs_state.save_support_ready) {
        const FSStatus save_status = SAVEFlushQuota(&SDL_wiiu_fs_state.fs_client,
                                                    &SDL_wiiu_fs_state.fs_cmd_block,
                                                    SDL_wiiu_fs_state.save_slot,
                                                    FS_ERROR_FLAG_NONE);
        if (save_status != FS_STATUS_OK) {
            result = SDL_WIIU_SetSaveError("SAVEFlushQuota", (SAVEStatus) save_status);
        }
    }

    SDL_WIIU_UnlockFSState();
    return result;
}

extern "C" Uint64 SDL_WIIU_GetUserSaveSpaceRemaining(void)
{
    Uint64 remaining = 0;

    if (!SDL_WIIU_LockFSState()) {
        return 0;
    }

    if (!SDL_WIIU_EnsureSaveStateLocked()) {
        SDL_WIIU_UnlockFSState();
        return 0;
    }

    if (SDL_wiiu_fs_state.save_support_ready) {
        const FSStatus save_status = SAVEGetFreeSpaceSize(&SDL_wiiu_fs_state.fs_client,
                                                          &SDL_wiiu_fs_state.fs_cmd_block,
                                                          SDL_wiiu_fs_state.save_slot,
                                                          &remaining,
                                                          FS_ERROR_FLAG_NONE);
        if (save_status != FS_STATUS_OK) {
            (void) SDL_WIIU_SetSaveError("SAVEGetFreeSpaceSize", (SAVEStatus) save_status);
            remaining = 0;
        }
    } else {
        const FSStatus fs_status = FSGetFreeSpaceSize(&SDL_wiiu_fs_state.fs_client,
                                                      &SDL_wiiu_fs_state.fs_cmd_block,
                                                      SDL_wiiu_fs_state.save_root,
                                                      &remaining,
                                                      FS_ERROR_FLAG_NONE);
        if (fs_status != FS_STATUS_OK) {
            (void) SDL_WIIU_SetFSError("FSGetFreeSpaceSize", fs_status);
            remaining = 0;
        }
    }

    SDL_WIIU_UnlockFSState();
    return remaining;
}

#endif // SDL_PLATFORM_WIIU
