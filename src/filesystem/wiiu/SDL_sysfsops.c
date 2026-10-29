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

#include "../SDL_sysfilesystem.h"
#include "../../core/wiiu/SDL_wiiufs.h"

bool SDL_SYS_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback cb, void *userdata)
{
    return SDL_WIIU_EnumerateDirectory(path, cb, userdata);
}

bool SDL_SYS_RemovePath(const char *path)
{
    return SDL_WIIU_RemovePath(path);
}

bool SDL_SYS_RenamePath(const char *oldpath, const char *newpath)
{
    return SDL_WIIU_RenamePath(oldpath, newpath);
}

bool SDL_SYS_CopyFile(const char *oldpath, const char *newpath)
{
    char *buffer = NULL;
    SDL_IOStream *input = NULL;
    SDL_IOStream *output = NULL;
    const size_t maxlen = SDL_WIIU_IO_BUFFER_SIZE;
    size_t len;
    bool result = false;

    input = SDL_IOFromFile(oldpath, "rb");
    if (!input) {
        goto done;
    }

    output = SDL_IOFromFile(newpath, "wb");
    if (!output) {
        goto done;
    }

    buffer = (char *) SDL_aligned_alloc(SDL_WIIU_IO_ALIGNMENT, maxlen);
    if (!buffer) {
        goto done;
    }

    while ((len = SDL_ReadIO(input, buffer, maxlen)) > 0) {
        if (SDL_WriteIO(output, buffer, len) < len) {
            goto done;
        }
    }
    if (SDL_GetIOStatus(input) != SDL_IO_STATUS_EOF) {
        goto done;
    }

    SDL_CloseIO(input);
    input = NULL;

    if (!SDL_FlushIO(output)) {
        goto done;
    }

    result = SDL_CloseIO(output);
    output = NULL;

done:
    if (output) {
        SDL_CloseIO(output);
    }
    if (input) {
        SDL_CloseIO(input);
    }
    SDL_aligned_free(buffer);

    return result;
}

bool SDL_SYS_CreateDirectory(const char *path)
{
    return SDL_WIIU_CreateDirectory(path);
}

bool SDL_SYS_GetPathInfo(const char *path, SDL_PathInfo *info)
{
    return SDL_WIIU_GetPathInfo(path, info);
}

#endif // SDL_PLATFORM_WIIU
