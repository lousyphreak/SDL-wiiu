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

#if defined(SDL_STORAGE_PRIVATE) && defined(SDL_PLATFORM_WIIU)

#include "../SDL_sysstorage.h"
#include "../../core/wiiu/SDL_wiiufs.h"

typedef struct SDL_WiiUStorageData
{
    char *root_path;
} SDL_WiiUStorageData;

typedef struct SDL_WiiUEnumerateData
{
    size_t base_len;
    SDL_EnumerateDirectoryCallback callback;
    void *callback_userdata;
} SDL_WiiUEnumerateData;

static SDL_EnumerationResult SDLCALL WIIU_StopEnumerateCallback(void *userdata, const char *dirname, const char *fname)
{
    (void) userdata;
    (void) dirname;
    (void) fname;
    return SDL_ENUM_SUCCESS;
}

static char *WIIU_CreateFullPath(const char *base, const char *relative)
{
    char *result = NULL;
    SDL_asprintf(&result, "%s%s", base ? base : "", relative ? relative : "");
    return result;
}

static bool WIIU_CloseStorage(void *userdata)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    const bool flushed = SDL_WIIU_FlushUserSave();

    if (data) {
        SDL_free(data->root_path);
        SDL_free(data);
    }
    return flushed;
}

static bool WIIU_StorageReady(void *userdata)
{
    (void) userdata;
    return true;
}

static SDL_EnumerationResult SDLCALL WIIU_EnumerateDirectoryCallback(void *userdata, const char *dirname, const char *fname)
{
    const SDL_WiiUEnumerateData *wrap = (const SDL_WiiUEnumerateData *) userdata;
    return wrap->callback(wrap->callback_userdata, dirname + wrap->base_len, fname);
}

static bool WIIU_EnumerateStorageDirectory(void *userdata, const char *path, SDL_EnumerateDirectoryCallback callback, void *callback_userdata)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    SDL_WiiUEnumerateData wrap;
    char *fullpath;
    bool result;

    fullpath = WIIU_CreateFullPath(data ? data->root_path : NULL, path);
    if (!fullpath) {
        return false;
    }

    wrap.base_len = data ? SDL_strlen(data->root_path) : 0;
    wrap.callback = callback;
    wrap.callback_userdata = callback_userdata;
    result = SDL_EnumerateDirectory(fullpath, WIIU_EnumerateDirectoryCallback, &wrap);
    SDL_free(fullpath);
    return result;
}

static bool WIIU_GetStoragePathInfo(void *userdata, const char *path, SDL_PathInfo *info)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    char *fullpath = WIIU_CreateFullPath(data ? data->root_path : NULL, path);
    bool result = false;

    if (fullpath) {
        SDL_Log("[storage] wiiu-info begin: %s", fullpath);
        result = SDL_WIIU_GetPathInfo(fullpath, info);
        if (!result) {
            SDL_Log("[storage] wiiu-info miss: %s (%s)", fullpath, SDL_GetError());
        } else if (info) {
            SDL_Log("[storage] wiiu-info ok: %s type=%d size=%llu", fullpath,
                    (int) info->type, (unsigned long long) info->size);
        }
        SDL_free(fullpath);
    }
    return result;
}

static bool WIIU_ReadStorageFile(void *userdata, const char *path, void *destination, Uint64 length)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    char *fullpath;
    SDL_IOStream *stream;
    bool result = false;

    if (length > SDL_SIZE_MAX) {
        return SDL_SetError("Read size exceeds SDL_SIZE_MAX");
    }

    fullpath = WIIU_CreateFullPath(data ? data->root_path : NULL, path);
    if (!fullpath) {
        return false;
    }

    stream = SDL_IOFromFile(fullpath, "rb");
    if (stream) {
        if (SDL_ReadIO(stream, destination, (size_t) length) == length) {
            result = true;
        } else {
            SDL_SetError("File length did not exactly match the destination length");
        }
        SDL_CloseIO(stream);
    }

    SDL_free(fullpath);
    return result;
}

static bool WIIU_WriteStorageFile(void *userdata, const char *path, const void *source, Uint64 length)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    char *fullpath;
    SDL_IOStream *stream;
    bool result = false;

    if (length > SDL_SIZE_MAX) {
        return SDL_SetError("Write size exceeds SDL_SIZE_MAX");
    }

    fullpath = WIIU_CreateFullPath(data ? data->root_path : NULL, path);
    if (!fullpath) {
        return false;
    }

    stream = SDL_IOFromFile(fullpath, "wb");
    if (stream) {
        if (SDL_WriteIO(stream, source, (size_t) length) == length) {
            result = true;
        } else {
            SDL_SetError("Resulting file length did not exactly match the source length");
        }
        SDL_CloseIO(stream);
    }

    SDL_free(fullpath);
    return result;
}

static bool WIIU_CreateStorageDirectory(void *userdata, const char *path)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    char *fullpath = WIIU_CreateFullPath(data ? data->root_path : NULL, path);
    bool result = false;

    if (fullpath) {
        result = SDL_CreateDirectory(fullpath);
        SDL_free(fullpath);
    }
    return result;
}

static bool WIIU_RemoveStoragePath(void *userdata, const char *path)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    char *fullpath = WIIU_CreateFullPath(data ? data->root_path : NULL, path);
    bool result = false;

    if (fullpath) {
        result = SDL_RemovePath(fullpath);
        SDL_free(fullpath);
    }
    return result;
}

static bool WIIU_RenameStoragePath(void *userdata, const char *oldpath, const char *newpath)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    char *fulloldpath = WIIU_CreateFullPath(data ? data->root_path : NULL, oldpath);
    char *fullnewpath = WIIU_CreateFullPath(data ? data->root_path : NULL, newpath);
    bool result = false;

    if (fulloldpath && fullnewpath) {
        result = SDL_RenamePath(fulloldpath, fullnewpath);
    }

    SDL_free(fulloldpath);
    SDL_free(fullnewpath);
    return result;
}

static bool WIIU_CopyStorageFile(void *userdata, const char *oldpath, const char *newpath)
{
    SDL_WiiUStorageData *data = (SDL_WiiUStorageData *) userdata;
    char *fulloldpath = WIIU_CreateFullPath(data ? data->root_path : NULL, oldpath);
    char *fullnewpath = WIIU_CreateFullPath(data ? data->root_path : NULL, newpath);
    bool result = false;

    if (fulloldpath && fullnewpath) {
        result = SDL_CopyFile(fulloldpath, fullnewpath);
    }

    SDL_free(fulloldpath);
    SDL_free(fullnewpath);
    return result;
}

static const SDL_StorageInterface WIIU_user_iface = {
    sizeof(SDL_StorageInterface),
    WIIU_CloseStorage,
    WIIU_StorageReady,
    WIIU_EnumerateStorageDirectory,
    WIIU_GetStoragePathInfo,
    WIIU_ReadStorageFile,
    WIIU_WriteStorageFile,
    WIIU_CreateStorageDirectory,
    WIIU_RemoveStoragePath,
    WIIU_RenameStoragePath,
    WIIU_CopyStorageFile,
    NULL
};

static SDL_Storage *WIIU_User_Create(const char *org, const char *app, SDL_PropertiesID props)
{
    SDL_WiiUStorageData *data;
    SDL_Storage *storage;

    (void) props;

    SDL_Log("[storage] wiiu-user create begin");
    data = (SDL_WiiUStorageData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        return NULL;
    }

    data->root_path = SDL_WIIU_GetPrefPath(org ? org : "", app ? app : "");
    if (!data->root_path) {
        SDL_Log("[storage] wiiu-user pref failed: %s", SDL_GetError());
        SDL_free(data);
        return NULL;
    }
    SDL_Log("[storage] wiiu-user pref ready: %s", data->root_path);

    storage = SDL_OpenStorage(&WIIU_user_iface, data);
    if (!storage) {
        SDL_Log("[storage] wiiu-user open-storage failed: %s", SDL_GetError());
        SDL_free(data->root_path);
        SDL_free(data);
    } else {
        SDL_Log("[storage] wiiu-user create done");
    }
    return storage;
}

UserStorageBootStrap PRIVATE_userbootstrap = {
    "wiiu",
    "SDL Wii U user storage driver",
    WIIU_User_Create
};

#endif // SDL_STORAGE_PRIVATE && SDL_PLATFORM_WIIU
