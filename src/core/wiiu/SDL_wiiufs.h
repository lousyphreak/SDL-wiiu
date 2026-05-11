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
#ifndef SDL_wiiufs_h_
#define SDL_wiiufs_h_

#include "SDL_internal.h"

#ifdef SDL_PLATFORM_WIIU

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_WIIUFile SDL_WIIUFile;

extern char *SDL_WIIU_GetBasePath(void);
extern char *SDL_WIIU_GetCurrentDirectory(void);
extern char *SDL_WIIU_GetPrefPath(const char *org, const char *app);
extern bool SDL_WIIU_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback cb, void *userdata);
extern bool SDL_WIIU_GetPathInfo(const char *path, SDL_PathInfo *info);
extern bool SDL_WIIU_CreateDirectory(const char *path);
extern bool SDL_WIIU_RemovePath(const char *path);
extern bool SDL_WIIU_RenamePath(const char *oldpath, const char *newpath);
extern SDL_WIIUFile *SDL_WIIU_OpenFile(const char *path, const char *mode);
extern Sint64 SDL_WIIU_FileSize(SDL_WIIUFile *file);
extern Sint64 SDL_WIIU_FileSeek(SDL_WIIUFile *file, Sint64 offset, SDL_IOWhence whence);
extern size_t SDL_WIIU_FileRead(SDL_WIIUFile *file, void *ptr, size_t size, SDL_IOStatus *status);
extern size_t SDL_WIIU_FileWrite(SDL_WIIUFile *file, const void *ptr, size_t size, SDL_IOStatus *status);
extern bool SDL_WIIU_FileFlush(SDL_WIIUFile *file, SDL_IOStatus *status);
extern bool SDL_WIIU_FileClose(SDL_WIIUFile *file);
extern bool SDL_WIIU_FlushUserSave(void);
extern Uint64 SDL_WIIU_GetUserSaveSpaceRemaining(void);

#ifdef __cplusplus
}
#endif

#endif // SDL_PLATFORM_WIIU

#endif // SDL_wiiufs_h_
