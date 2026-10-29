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

#include <stdint.h>
#include <coreinit/filesystem.h>
#include <coreinit/title.h>

#define SDL_WIIU_CONTENT_ROOT "/vol/content/"
#define SDL_WIIU_CONTENT_ROOT_NO_SLASH "/vol/content"
#define SDL_WIIU_EXTERNAL_VOLUME_ROOT "/vol/external01"
#define SDL_WIIU_EXTERNAL_SAVE_ROOT "/vol/external01/save/"
#define SDL_WIIU_PATH_CAPACITY (FS_MAX_PATH + 2)
#define SDL_WIIU_FSTIME_EPOCH_DIFF_SECONDS 315532800ULL
#define SDL_WIIU_FS_ERROR_MASK FS_ERROR_FLAG_ALL

typedef struct SDL_WiiUFSState
{
    bool fs_initialized;
    bool fs_client_ready;
    FSClient fs_client;
    FSCmdBlock fs_cmd_block;
} SDL_WiiUFSState;

struct SDL_WIIUFile
{
    FSFileHandle handle;
    bool readable;
    bool writable;
    bool append;
    Uint8 *buffer;
    size_t buffer_capacity;
};

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

static const char *SDL_WIIU_FSStatusString(FSStatus status)
{
    switch (status) {
    case FS_STATUS_OK:
        return "success";
    case FS_STATUS_CANCELLED:
        return "cancelled";
    case FS_STATUS_END:
        return "end of file";
    case FS_STATUS_ALREADY_OPEN:
        return "already open";
    case FS_STATUS_EXISTS:
        return "already exists";
    case FS_STATUS_NOT_FOUND:
        return "not found";
    case FS_STATUS_NOT_FILE:
        return "not a file";
    case FS_STATUS_NOT_DIR:
        return "not a directory";
    case FS_STATUS_ACCESS_ERROR:
        return "access error";
    case FS_STATUS_PERMISSION_ERROR:
        return "permission error";
    case FS_STATUS_FILE_TOO_BIG:
        return "file too big";
    case FS_STATUS_STORAGE_FULL:
        return "storage full";
    case FS_STATUS_JOURNAL_FULL:
        return "journal full";
    case FS_STATUS_UNSUPPORTED_CMD:
        return "unsupported command";
    case FS_STATUS_MEDIA_NOT_READY:
        return "media not ready";
    case FS_STATUS_MEDIA_ERROR:
        return "media error";
    case FS_STATUS_CORRUPTED:
        return "corrupted";
    default:
        return "filesystem error";
    }
}

extern "C" bool SDL_WIIU_SetFSError(const char *what, FSStatus status)
{
    return SDL_SetError("%s failed: %s (%d)", what, SDL_WIIU_FSStatusString(status), (int) status);
}

static bool SDL_WIIU_IsPathSeparator(char ch)
{
    return ch == '/';
}

static bool SDL_WIIU_IsSafeStorageNameChar(char ch)
{
    return ((ch >= 'a') && (ch <= 'z')) ||
           ((ch >= 'A') && (ch <= 'Z')) ||
           ((ch >= '0') && (ch <= '9')) ||
           (ch == '-') || (ch == '_') || (ch == '.');
}

static void SDL_WIIU_SanitizeStorageName(const char *source, char *destination, size_t destination_size)
{
    size_t length = 0;
    bool pending_dash = false;

    if (!destination || destination_size == 0) {
        return;
    }

    destination[0] = '\0';
    if (!source || !*source) {
        return;
    }

    while (*source && (length + 1) < destination_size) {
        const char ch = *source++;

        if (SDL_WIIU_IsSafeStorageNameChar(ch)) {
            if (pending_dash && length > 0 && (length + 1) < destination_size) {
                destination[length++] = '-';
            }
            destination[length++] = ch;
            pending_dash = false;
        } else {
            pending_dash = true;
        }
    }

    if (length > 0 && destination[length - 1] == '-') {
        --length;
    }
    destination[length] = '\0';
}

static bool SDL_WIIU_GetStableStorageAppName(const char *org, const char *app, char *buffer, size_t buffer_size)
{
    uint64_t title_id;

    if (!buffer || buffer_size == 0) {
        return SDL_InvalidParamError("buffer");
    }

    SDL_WIIU_SanitizeStorageName(app, buffer, buffer_size);
    if (buffer[0]) {
        return true;
    }

    SDL_WIIU_SanitizeStorageName(org, buffer, buffer_size);
    if (buffer[0]) {
        return true;
    }

    title_id = OSGetTitleID();
    if (title_id != 0) {
        SDL_snprintf(buffer, buffer_size, "title-%016" SDL_PRIX64, (Uint64) title_id);
        return true;
    }

    SDL_strlcpy(buffer, "application", buffer_size);
    return true;
}

static SDL_Time SDL_WIIU_TranslateFSTime(FSTime value)
{
    const Uint64 micros = (Uint64) value;
    const Uint64 seconds = (micros / 1000000ULL) + SDL_WIIU_FSTIME_EPOCH_DIFF_SECONDS;
    const Uint64 nanos = (micros % 1000000ULL) * 1000ULL;
    return (SDL_Time) SDL_SECONDS_TO_NS(seconds) + (SDL_Time) nanos;
}

static SDL_PathType SDL_WIIU_GetPathTypeFromStat(const FSStat *stat)
{
    if ((stat->flags & FS_STAT_LINK) == FS_STAT_LINK) {
        return SDL_PATHTYPE_OTHER;
    } else if ((stat->flags & FS_STAT_DIRECTORY) == FS_STAT_DIRECTORY) {
        return SDL_PATHTYPE_DIRECTORY;
    } else if ((stat->flags & FS_STAT_FILE) == FS_STAT_FILE) {
        return SDL_PATHTYPE_FILE;
    } else if (stat->size == 0) {
        return SDL_PATHTYPE_DIRECTORY;
    } else if (stat->size > 0) {
        return SDL_PATHTYPE_FILE;
    }
    return SDL_PATHTYPE_OTHER;
}

static void SDL_WIIU_FillPathInfo(const FSStat *stat, SDL_PathInfo *info)
{
    if (!info) {
        return;
    }

    info->type = SDL_WIIU_GetPathTypeFromStat(stat);
    info->size = (info->type == SDL_PATHTYPE_DIRECTORY) ? 0 : (Uint64) stat->size;
    info->create_time = SDL_WIIU_TranslateFSTime(stat->created);
    info->modify_time = SDL_WIIU_TranslateFSTime(stat->modified);
    info->access_time = info->modify_time;
}

static bool SDL_WIIU_ResolvePath(const char *path, char **path_out)
{
    const char *src;
    const bool absolute = path && (*path == '/');
    const char *base = absolute ? "/" : SDL_WIIU_CONTENT_ROOT_NO_SLASH;
    const size_t base_len = SDL_strlen(base);
    const size_t input_len = path ? SDL_strlen(path) : 0;
    const size_t capacity = base_len + input_len + 2;
    char *resolved;
    size_t length = 0;

    if (!path_out) {
        return SDL_InvalidParamError("path_out");
    }

    *path_out = NULL;

    resolved = (char *) SDL_malloc(capacity);
    if (!resolved) {
        return false;
    }

    SDL_memcpy(resolved, base, base_len);
    length = base_len;
    resolved[length] = '\0';

    src = absolute ? (path + 1) : path;
    while (src && *src) {
        const char *segment_end;
        size_t segment_len;

        while (SDL_WIIU_IsPathSeparator(*src)) {
            ++src;
        }
        if (!*src) {
            break;
        }

        segment_end = src;
        while (*segment_end && !SDL_WIIU_IsPathSeparator(*segment_end)) {
            ++segment_end;
        }

        segment_len = (size_t) (segment_end - src);
        if (segment_len == 1 && src[0] == '.') {
            src = segment_end;
            continue;
        }
        if (segment_len == 2 && src[0] == '.' && src[1] == '.') {
            SDL_free(resolved);
            return SDL_SetError("Wii U paths do not support parent directory traversal");
        }

        if ((length + 1 + segment_len + 1) > capacity || (length + 1 + segment_len) >= SDL_WIIU_PATH_CAPACITY) {
            SDL_free(resolved);
            return SDL_SetError("Wii U path is too long");
        }

        if ((length == 0) || !SDL_WIIU_IsPathSeparator(resolved[length - 1])) {
            resolved[length++] = '/';
        }

        SDL_memcpy(resolved + length, src, segment_len);
        length += segment_len;
        resolved[length] = '\0';
        src = segment_end;
    }

    *path_out = resolved;
    return true;
}

static bool SDL_WIIU_EnsureFSClientLocked(void)
{
    FSStatus fs_status;

    if (SDL_wiiu_fs_state.fs_client_ready) {
        return true;
    }

    if (!SDL_wiiu_fs_state.fs_initialized) {
        FSInit();
        SDL_wiiu_fs_state.fs_initialized = true;
    }

    SDL_zero(SDL_wiiu_fs_state.fs_client);
    fs_status = FSAddClient(&SDL_wiiu_fs_state.fs_client, SDL_WIIU_FS_ERROR_MASK);
    if (fs_status != FS_STATUS_OK) {
        SDL_WIIU_SetFSError("FSAddClient", fs_status);
        return false;
    }
    SDL_wiiu_fs_state.fs_client_ready = true;

    SDL_zero(SDL_wiiu_fs_state.fs_cmd_block);
    FSInitCmdBlock(&SDL_wiiu_fs_state.fs_cmd_block);
    return true;
}

static bool SDL_WIIU_BeginPathOperation(const char *path)
{
    (void) path;

    if (!SDL_WIIU_LockFSState()) {
        return false;
    }

    if (!SDL_WIIU_EnsureFSClientLocked()) {
        SDL_WIIU_UnlockFSState();
        return false;
    }

    return true;
}

static FSStatus SDL_WIIU_GetStatForPathLocked(const char *path, FSStat *stat)
{
    return FSGetStat(&SDL_wiiu_fs_state.fs_client,
                     &SDL_wiiu_fs_state.fs_cmd_block,
                     path,
                     stat,
                     SDL_WIIU_FS_ERROR_MASK);
}

static bool SDL_WIIU_CreateSingleDirectoryLocked(const char *path)
{
    FSStat stat;
    FSStatus status;

    status = SDL_WIIU_GetStatForPathLocked(path, &stat);
    if (status == FS_STATUS_OK) {
        if (SDL_WIIU_GetPathTypeFromStat(&stat) == SDL_PATHTYPE_DIRECTORY) {
            return true;
        }
        return SDL_SetError("%s exists but is not a directory", path);
    }
    if (status != FS_STATUS_NOT_FOUND) {
        return SDL_WIIU_SetFSError("FSGetStat", status);
    }

    status = FSMakeDir(&SDL_wiiu_fs_state.fs_client,
                       &SDL_wiiu_fs_state.fs_cmd_block,
                       path,
                       SDL_WIIU_FS_ERROR_MASK);
    if (status == FS_STATUS_OK) {
        return true;
    }

    return SDL_WIIU_SetFSError("FSMakeDir", status);
}

static const char *SDL_WIIU_GetDirectoryCreateStart(const char *path)
{
    int component_count = 0;
    const char *cursor = path;

    while (cursor && *cursor) {
        while (*cursor == '/') {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }

        ++component_count;
        while (*cursor && *cursor != '/') {
            ++cursor;
        }
        if (component_count == 2) {
            return *cursor == '/' ? (cursor + 1) : cursor;
        }
    }

    return path;
}

static bool SDL_WIIU_BuildPrefPathLocked(const char *org, const char *app, char **path_out)
{
    char app_name[96];
    char *path = NULL;
    int rc;

    if (!path_out) {
        return SDL_InvalidParamError("path_out");
    }

    if (!SDL_WIIU_GetStableStorageAppName(org, app, app_name, sizeof(app_name))) {
        return false;
    }

    rc = SDL_asprintf(&path, "%s%s/", SDL_WIIU_EXTERNAL_SAVE_ROOT, app_name);
    if (rc < 0 || !path) {
        return false;
    }

    *path_out = path;
    return true;
}

extern "C" char *SDL_WIIU_GetBasePath(void)
{
    SDL_PathInfo info;

    if (!SDL_WIIU_GetPathInfo(SDL_WIIU_CONTENT_ROOT, &info)) {
        return NULL;
    }
    if (info.type != SDL_PATHTYPE_DIRECTORY) {
        SDL_SetError("%s is not a directory", SDL_WIIU_CONTENT_ROOT_NO_SLASH);
        return NULL;
    }
    return SDL_strdup(SDL_WIIU_CONTENT_ROOT);
}

extern "C" char *SDL_WIIU_GetCurrentDirectory(void)
{
    return SDL_strdup(SDL_WIIU_CONTENT_ROOT);
}

extern "C" char *SDL_WIIU_ResolveFSPath(const char *path)
{
    char *resolved = NULL;
    if (!SDL_WIIU_ResolvePath(path, &resolved)) {
        return NULL;
    }
    return resolved;
}

extern "C" char *SDL_WIIU_GetPrefPath(const char *org, const char *app)
{
    char *path = NULL;

    if (!SDL_WIIU_LockFSState()) {
        return NULL;
    }

    if (!SDL_WIIU_BuildPrefPathLocked(org, app, &path)) {
        SDL_WIIU_UnlockFSState();
        return NULL;
    }

    SDL_WIIU_UnlockFSState();

    if (!SDL_WIIU_CreateDirectory(path)) {
        SDL_free(path);
        return NULL;
    }

    return path;
}

extern "C" bool SDL_WIIU_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback cb, void *userdata)
{
    char *resolved = NULL;
    char *callback_dir = NULL;
    FSDirectoryHandle handle = 0;
    SDL_EnumerationResult result = SDL_ENUM_CONTINUE;
    bool success = true;

    if (!cb) {
        return SDL_InvalidParamError("cb");
    }
    if (!SDL_WIIU_ResolvePath(path, &resolved)) {
        return false;
    }
    if (SDL_asprintf(&callback_dir, "%s%s", resolved, (*resolved && resolved[SDL_strlen(resolved) - 1] == '/') ? "" : "/") < 0) {
        SDL_free(resolved);
        return false;
    }
    if (!SDL_WIIU_BeginPathOperation(resolved)) {
        SDL_free(callback_dir);
        SDL_free(resolved);
        return false;
    }

    {
        const FSStatus status = FSOpenDir(&SDL_wiiu_fs_state.fs_client,
                                          &SDL_wiiu_fs_state.fs_cmd_block,
                                          resolved,
                                          &handle,
                                          SDL_WIIU_FS_ERROR_MASK);
        SDL_WIIU_UnlockFSState();
        if (status != FS_STATUS_OK) {
            SDL_free(callback_dir);
            SDL_free(resolved);
            return SDL_WIIU_SetFSError("FSOpenDir", status);
        }
    }

    while (result == SDL_ENUM_CONTINUE) {
        FSDirectoryEntry entry;
        FSStatus status;

        if (!SDL_WIIU_LockFSState()) {
            success = false;
            break;
        }
        status = FSReadDir(&SDL_wiiu_fs_state.fs_client,
                           &SDL_wiiu_fs_state.fs_cmd_block,
                           handle,
                           &entry,
                           SDL_WIIU_FS_ERROR_MASK);
        SDL_WIIU_UnlockFSState();

        if (status == FS_STATUS_END) {
            break;
        }
        if (status != FS_STATUS_OK) {
            success = SDL_WIIU_SetFSError("FSReadDir", status);
            break;
        }
        if ((SDL_strcmp(entry.name, ".") == 0) || (SDL_strcmp(entry.name, "..") == 0)) {
            continue;
        }

        result = cb(userdata, callback_dir, entry.name);
    }

    if (SDL_WIIU_LockFSState()) {
        const FSStatus close_status = FSCloseDir(&SDL_wiiu_fs_state.fs_client,
                                                 &SDL_wiiu_fs_state.fs_cmd_block,
                                                 handle,
                                                 SDL_WIIU_FS_ERROR_MASK);
        SDL_WIIU_UnlockFSState();
        if (success && close_status != FS_STATUS_OK) {
            success = SDL_WIIU_SetFSError("FSCloseDir", close_status);
        }
    } else {
        success = false;
    }

    SDL_free(callback_dir);
    SDL_free(resolved);
    return success && (result != SDL_ENUM_FAILURE);
}

extern "C" bool SDL_WIIU_GetPathInfo(const char *path, SDL_PathInfo *info)
{
    char *resolved = NULL;
    FSStat stat;
    FSStatus status;
    bool result;

    if (!SDL_WIIU_ResolvePath(path, &resolved)) {
        return false;
    }
    if (!SDL_WIIU_BeginPathOperation(resolved)) {
        SDL_free(resolved);
        return false;
    }

    status = SDL_WIIU_GetStatForPathLocked(resolved, &stat);
    SDL_WIIU_UnlockFSState();
    SDL_free(resolved);
    if (status != FS_STATUS_OK) {
        return SDL_WIIU_SetFSError("FSGetStat", status);
    }

    if (info) {
        SDL_WIIU_FillPathInfo(&stat, info);
    }
    result = true;
    return result;
}

extern "C" bool SDL_WIIU_CreateDirectory(const char *path)
{
    char *resolved = NULL;
    char *cursor;
    bool result = true;

    if (!SDL_WIIU_ResolvePath(path, &resolved)) {
        return false;
    }
    if (!SDL_WIIU_BeginPathOperation(resolved)) {
        SDL_free(resolved);
        return false;
    }

    cursor = resolved + (SDL_WIIU_GetDirectoryCreateStart(resolved) - resolved);
    while (result && cursor && *cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (*resolved) {
                result = SDL_WIIU_CreateSingleDirectoryLocked(resolved);
            }
            *cursor = '/';
        }
        ++cursor;
    }

    if (result) {
        result = SDL_WIIU_CreateSingleDirectoryLocked(resolved);
    }

    SDL_WIIU_UnlockFSState();
    SDL_free(resolved);
    return result;
}

extern "C" bool SDL_WIIU_RemovePath(const char *path)
{
    char *resolved = NULL;
    FSStatus status;
    bool result;

    if (!SDL_WIIU_ResolvePath(path, &resolved)) {
        return false;
    }
    if (!SDL_WIIU_BeginPathOperation(resolved)) {
        SDL_free(resolved);
        return false;
    }

    status = FSRemove(&SDL_wiiu_fs_state.fs_client,
                      &SDL_wiiu_fs_state.fs_cmd_block,
                      resolved,
                      SDL_WIIU_FS_ERROR_MASK);
    if (status == FS_STATUS_NOT_FOUND) {
        status = FS_STATUS_OK;
    }

    if (status != FS_STATUS_OK) {
        result = SDL_WIIU_SetFSError("FSRemove", status);
    } else {
        result = true;
    }

    SDL_WIIU_UnlockFSState();
    SDL_free(resolved);
    return result;
}

extern "C" bool SDL_WIIU_RenamePath(const char *oldpath, const char *newpath)
{
    char *resolved_old = NULL;
    char *resolved_new = NULL;
    FSStatus status;
    bool result;

    if (!SDL_WIIU_ResolvePath(oldpath, &resolved_old) || !SDL_WIIU_ResolvePath(newpath, &resolved_new)) {
        SDL_free(resolved_old);
        SDL_free(resolved_new);
        return false;
    }
    if (!SDL_WIIU_LockFSState()) {
        SDL_free(resolved_old);
        SDL_free(resolved_new);
        return false;
    }
    if (!SDL_WIIU_EnsureFSClientLocked()) {
        SDL_WIIU_UnlockFSState();
        SDL_free(resolved_old);
        SDL_free(resolved_new);
        return false;
    }

    status = FSRename(&SDL_wiiu_fs_state.fs_client,
                      &SDL_wiiu_fs_state.fs_cmd_block,
                      resolved_old,
                      resolved_new,
                      SDL_WIIU_FS_ERROR_MASK);
    if (status != FS_STATUS_OK) {
        result = SDL_WIIU_SetFSError("FSRename", status);
    } else {
        result = true;
    }

    SDL_WIIU_UnlockFSState();
    SDL_free(resolved_old);
    SDL_free(resolved_new);
    return result;
}

typedef struct SDL_WIIUOpenMode
{
    char fs_mode[4];
    char append_fallback_mode[4];
    bool readable;
    bool writable;
    bool append;
    bool exclusive;
} SDL_WIIUOpenMode;

static bool SDL_WIIU_ParseOpenMode(const char *mode, SDL_WIIUOpenMode *parsed)
{
    const char *ptr = mode;
    char base = '\0';
    bool plus = false;
    bool binary = false;
    bool exclusive = false;

    if (!parsed) {
        return SDL_InvalidParamError("parsed");
    }

    SDL_zero(*parsed);

    while (*ptr) {
        switch (*ptr) {
        case 'r':
        case 'w':
        case 'a':
            if (base != '\0') {
                return SDL_SetError("Unsupported Wii U file mode '%s'", mode);
            }
            base = *ptr;
            break;
        case '+':
            if (plus) {
                return SDL_SetError("Unsupported Wii U file mode '%s'", mode);
            }
            plus = true;
            break;
        case 'b':
            binary = true;
            break;
        case 'x':
            exclusive = true;
            break;
        default:
            return SDL_SetError("Unsupported Wii U file mode '%s'", mode);
        }
        ++ptr;
    }

    (void) binary;

    if (base == '\0' || (exclusive && base != 'w')) {
        return SDL_SetError("Unsupported Wii U file mode '%s'", mode);
    }

    parsed->readable = (base == 'r') || plus;
    parsed->writable = (base == 'w') || (base == 'a') || plus;
    parsed->append = (base == 'a');
    parsed->exclusive = exclusive;

    if (parsed->append) {
        SDL_strlcpy(parsed->fs_mode, "r+", sizeof(parsed->fs_mode));
        if (plus) {
            SDL_strlcpy(parsed->append_fallback_mode, "w+", sizeof(parsed->append_fallback_mode));
        } else {
            SDL_strlcpy(parsed->append_fallback_mode, "w", sizeof(parsed->append_fallback_mode));
        }
    } else {
        parsed->fs_mode[0] = base;
        parsed->fs_mode[1] = plus ? '+' : '\0';
        parsed->fs_mode[plus ? 2 : 1] = '\0';
    }

    return true;
}

static size_t SDL_WIIU_TransferFile(SDL_WIIUFile *file, void *ptr, size_t size, bool writing, SDL_IOStatus *status)
{
    Uint8 *dst = (Uint8 *) ptr;
    const Uint8 *src = (const Uint8 *) ptr;
    size_t total = 0;

    while (total < size) {
        Uint8 *io_ptr;
        size_t chunk = SDL_WIIU_GetTransferSize(src + total, size - total);
        FSStatus fs_status;

        if ((((uintptr_t) (src + total) | chunk) & (SDL_WIIU_IO_ALIGNMENT - 1)) == 0) {
            io_ptr = writing ? (Uint8 *) (src + total) : (dst + total);
        } else {
            if (chunk > file->buffer_capacity) {
                const size_t capacity = (chunk + SDL_WIIU_IO_ALIGNMENT - 1) & ~(SDL_WIIU_IO_ALIGNMENT - 1);
                Uint8 *buffer = (Uint8 *) SDL_aligned_alloc(SDL_WIIU_IO_ALIGNMENT, capacity);
                if (!buffer) {
                    if (status) {
                        *status = SDL_IO_STATUS_ERROR;
                    }
                    break;
                }
                SDL_aligned_free(file->buffer);
                file->buffer = buffer;
                file->buffer_capacity = capacity;
            }
            io_ptr = file->buffer;
            if (writing) {
                SDL_memcpy(io_ptr, src + total, chunk);
            }
        }

        if (!SDL_WIIU_LockFSState()) {
            if (status) {
                *status = SDL_IO_STATUS_ERROR;
            }
            break;
        }

        if (writing && file->append) {
            FSStat stat;
            fs_status = FSGetStatFile(&SDL_wiiu_fs_state.fs_client,
                                      &SDL_wiiu_fs_state.fs_cmd_block,
                                      file->handle,
                                      &stat,
                                      SDL_WIIU_FS_ERROR_MASK);
            if (fs_status == FS_STATUS_OK) {
                fs_status = FSSetPosFile(&SDL_wiiu_fs_state.fs_client,
                                         &SDL_wiiu_fs_state.fs_cmd_block,
                                         file->handle,
                                         stat.size,
                                         SDL_WIIU_FS_ERROR_MASK);
            }
            if (fs_status != FS_STATUS_OK) {
                SDL_WIIU_SetFSError("FSSetPosFile", fs_status);
                SDL_WIIU_UnlockFSState();
                if (status) {
                    *status = SDL_IO_STATUS_ERROR;
                }
                break;
            }
        }

        fs_status = writing ? FSWriteFile(&SDL_wiiu_fs_state.fs_client,
                                          &SDL_wiiu_fs_state.fs_cmd_block,
                                          io_ptr,
                                          1,
                                          (uint32_t) chunk,
                                          file->handle,
                                          0,
                                          SDL_WIIU_FS_ERROR_MASK)
                            : FSReadFile(&SDL_wiiu_fs_state.fs_client,
                                         &SDL_wiiu_fs_state.fs_cmd_block,
                                         io_ptr,
                                         1,
                                         (uint32_t) chunk,
                                         file->handle,
                                         0,
                                         SDL_WIIU_FS_ERROR_MASK);
        SDL_WIIU_UnlockFSState();

        if (fs_status < 0) {
            if (!writing && fs_status == FS_STATUS_END) {
                if (status) {
                    *status = SDL_IO_STATUS_EOF;
                }
            } else {
                SDL_WIIU_SetFSError(writing ? "FSWriteFile" : "FSReadFile", fs_status);
                if (status) {
                    *status = SDL_IO_STATUS_ERROR;
                }
            }
            break;
        }

        if (fs_status == 0 && chunk > 0) {
            if (status) {
                *status = writing ? SDL_IO_STATUS_NOT_READY : SDL_IO_STATUS_EOF;
            }
            break;
        }

        if (!writing && io_ptr == file->buffer) {
            SDL_memcpy(dst + total, io_ptr, (size_t) fs_status);
        }

        total += (size_t) fs_status;
        if ((size_t) fs_status < chunk) {
            if (status) {
                *status = writing ? SDL_IO_STATUS_NOT_READY : SDL_IO_STATUS_EOF;
            }
            break;
        }
    }

    return total;
}

extern "C" SDL_WIIUFile *SDL_WIIU_OpenFile(const char *path, const char *mode)
{
    char *resolved = NULL;
    SDL_WIIUOpenMode parsed;
    SDL_WIIUFile *file = NULL;
    FSFileHandle handle = 0;
    FSStatus status;

    if (!SDL_WIIU_ParseOpenMode(mode, &parsed)) {
        return NULL;
    }
    if (!SDL_WIIU_ResolvePath(path, &resolved)) {
        return NULL;
    }
    if (!SDL_WIIU_BeginPathOperation(resolved)) {
        SDL_free(resolved);
        return NULL;
    }

    if (parsed.exclusive) {
        FSStat stat;
        status = SDL_WIIU_GetStatForPathLocked(resolved, &stat);
        if (status == FS_STATUS_OK) {
            SDL_WIIU_UnlockFSState();
            SDL_free(resolved);
            SDL_SetError("Couldn't open %s: already exists", path);
            return NULL;
        }
        if (status != FS_STATUS_NOT_FOUND) {
            SDL_WIIU_UnlockFSState();
            SDL_free(resolved);
            SDL_WIIU_SetFSError("FSGetStat", status);
            return NULL;
        }
    }

    status = FSOpenFile(&SDL_wiiu_fs_state.fs_client,
                        &SDL_wiiu_fs_state.fs_cmd_block,
                        resolved,
                        parsed.fs_mode,
                        &handle,
                        SDL_WIIU_FS_ERROR_MASK);
    if (parsed.append && status == FS_STATUS_NOT_FOUND) {
        status = FSOpenFile(&SDL_wiiu_fs_state.fs_client,
                            &SDL_wiiu_fs_state.fs_cmd_block,
                            resolved,
                            parsed.append_fallback_mode,
                            &handle,
                            SDL_WIIU_FS_ERROR_MASK);
    }
    if (status != FS_STATUS_OK) {
        SDL_WIIU_UnlockFSState();
        SDL_free(resolved);
        SDL_WIIU_SetFSError("FSOpenFile", status);
        return NULL;
    }

    SDL_WIIU_UnlockFSState();

    file = (SDL_WIIUFile *) SDL_calloc(1, sizeof(*file));
    if (!file) {
        if (SDL_WIIU_LockFSState()) {
            (void) FSCloseFile(&SDL_wiiu_fs_state.fs_client,
                               &SDL_wiiu_fs_state.fs_cmd_block,
                               handle,
                               SDL_WIIU_FS_ERROR_MASK);
            SDL_WIIU_UnlockFSState();
        }
        SDL_free(resolved);
        return NULL;
    }

    file->handle = handle;
    file->readable = parsed.readable;
    file->writable = parsed.writable;
    file->append = parsed.append;
    SDL_free(resolved);
    return file;
}

extern "C" Sint64 SDL_WIIU_FileSize(SDL_WIIUFile *file)
{
    FSStat stat;
    FSStatus status;

    if (!file) {
        SDL_InvalidParamError("file");
        return -1;
    }
    if (!SDL_WIIU_LockFSState()) {
        return -1;
    }

    status = FSGetStatFile(&SDL_wiiu_fs_state.fs_client,
                           &SDL_wiiu_fs_state.fs_cmd_block,
                           file->handle,
                           &stat,
                           SDL_WIIU_FS_ERROR_MASK);
    SDL_WIIU_UnlockFSState();
    if (status != FS_STATUS_OK) {
        SDL_WIIU_SetFSError("FSGetStatFile", status);
        return -1;
    }

    return (Sint64) stat.size;
}

extern "C" Sint64 SDL_WIIU_FileSeek(SDL_WIIUFile *file, Sint64 offset, SDL_IOWhence whence)
{
    uint32_t origin = 0;
    uint64_t target = 0;
    FSStatus status;

    if (!file) {
        SDL_InvalidParamError("file");
        return -1;
    }
    if (!SDL_WIIU_LockFSState()) {
        return -1;
    }

    switch (whence) {
    case SDL_IO_SEEK_SET:
        target = (offset < 0) ? UINT64_MAX : (uint64_t) offset;
        break;
    case SDL_IO_SEEK_CUR:
        status = FSGetPosFile(&SDL_wiiu_fs_state.fs_client,
                              &SDL_wiiu_fs_state.fs_cmd_block,
                              file->handle,
                              &origin,
                              SDL_WIIU_FS_ERROR_MASK);
        if (status == FS_STATUS_OK && offset >= -(Sint64) origin) {
            target = (uint64_t) ((Sint64) origin + offset);
        } else {
            target = UINT64_MAX;
        }
        break;
    case SDL_IO_SEEK_END:
    {
        FSStat stat;
        status = FSGetStatFile(&SDL_wiiu_fs_state.fs_client,
                               &SDL_wiiu_fs_state.fs_cmd_block,
                               file->handle,
                               &stat,
                               SDL_WIIU_FS_ERROR_MASK);
        if (status == FS_STATUS_OK && offset >= -(Sint64) stat.size) {
            target = (uint64_t) ((Sint64) stat.size + offset);
        } else {
            target = UINT64_MAX;
        }
        break;
    }
    default:
        SDL_WIIU_UnlockFSState();
        SDL_SetError("Unknown value for 'whence'");
        return -1;
    }

    if (target > UINT32_MAX) {
        SDL_WIIU_UnlockFSState();
        SDL_SetError("Seek offset out of range");
        return -1;
    }

    status = FSSetPosFile(&SDL_wiiu_fs_state.fs_client,
                          &SDL_wiiu_fs_state.fs_cmd_block,
                          file->handle,
                          (uint32_t) target,
                          SDL_WIIU_FS_ERROR_MASK);
    SDL_WIIU_UnlockFSState();
    if (status != FS_STATUS_OK) {
        SDL_WIIU_SetFSError("FSSetPosFile", status);
        return -1;
    }

    return (Sint64) target;
}

extern "C" size_t SDL_WIIU_FileRead(SDL_WIIUFile *file, void *ptr, size_t size, SDL_IOStatus *status)
{
    if (status) {
        *status = SDL_IO_STATUS_READY;
    }
    if (!file) {
        if (status) {
            *status = SDL_IO_STATUS_ERROR;
        }
        SDL_InvalidParamError("file");
        return 0;
    }
    if (!file->readable) {
        if (status) {
            *status = SDL_IO_STATUS_WRITEONLY;
        }
        return 0;
    }
    return SDL_WIIU_TransferFile(file, ptr, size, false, status);
}

extern "C" size_t SDL_WIIU_FileWrite(SDL_WIIUFile *file, const void *ptr, size_t size, SDL_IOStatus *status)
{
    if (status) {
        *status = SDL_IO_STATUS_READY;
    }
    if (!file) {
        if (status) {
            *status = SDL_IO_STATUS_ERROR;
        }
        SDL_InvalidParamError("file");
        return 0;
    }
    if (!file->writable) {
        if (status) {
            *status = SDL_IO_STATUS_READONLY;
        }
        return 0;
    }
    return SDL_WIIU_TransferFile(file, (void *) ptr, size, true, status);
}

extern "C" bool SDL_WIIU_FileFlush(SDL_WIIUFile *file, SDL_IOStatus *status)
{
    FSStatus fs_status;

    if (status) {
        *status = SDL_IO_STATUS_READY;
    }
    if (!file) {
        return SDL_InvalidParamError("file");
    }
    if (!file->writable) {
        return true;
    }
    if (!SDL_WIIU_LockFSState()) {
        if (status) {
            *status = SDL_IO_STATUS_ERROR;
        }
        return false;
    }

    fs_status = FSFlushFile(&SDL_wiiu_fs_state.fs_client,
                            &SDL_wiiu_fs_state.fs_cmd_block,
                            file->handle,
                            SDL_WIIU_FS_ERROR_MASK);
    if (fs_status != FS_STATUS_OK) {
        SDL_WIIU_SetFSError("FSFlushFile", fs_status);
    }
    SDL_WIIU_UnlockFSState();

    if (fs_status != FS_STATUS_OK) {
        if (status) {
            *status = SDL_IO_STATUS_ERROR;
        }
        return false;
    }

    return true;
}

extern "C" bool SDL_WIIU_FileClose(SDL_WIIUFile *file)
{
    FSStatus close_status = FS_STATUS_OK;
    bool result = true;

    if (!file) {
        return true;
    }

    if (SDL_WIIU_LockFSState()) {
        /*
         * Cafe FS already flushes cached file data as part of FSCloseFile().
         * Calling FSFlushFile() first forces an extra synchronous media flush,
         * which adds a visible stall to small save writes during startup.
         */
        close_status = FSCloseFile(&SDL_wiiu_fs_state.fs_client,
                                   &SDL_wiiu_fs_state.fs_cmd_block,
                                   file->handle,
                                   SDL_WIIU_FS_ERROR_MASK);
        SDL_WIIU_UnlockFSState();
        if (close_status != FS_STATUS_OK) {
            result = SDL_WIIU_SetFSError("FSCloseFile", close_status) && result;
        }
    } else {
        result = false;
    }

    SDL_aligned_free(file->buffer);
    SDL_free(file);
    return result;
}

extern "C" bool SDL_WIIU_FlushUserSave(void)
{
    return true;
}

extern "C" Uint64 SDL_WIIU_GetUserSaveSpaceRemaining(void)
{
    SDL_Unsupported();
    return 0;
}

#endif // SDL_PLATFORM_WIIU
