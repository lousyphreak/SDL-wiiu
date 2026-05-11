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

#include "SDL_iostreamwiiu.h"
#include "../../core/wiiu/SDL_wiiufs.h"

typedef struct SDL_WiiUIOData
{
    SDL_WIIUFile *file;
} SDL_WiiUIOData;

static Sint64 SDLCALL WIIU_IO_Size(void *userdata)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    return SDL_WIIU_FileSize(data->file);
}

static Sint64 SDLCALL WIIU_IO_Seek(void *userdata, Sint64 offset, SDL_IOWhence whence)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    return SDL_WIIU_FileSeek(data->file, offset, whence);
}

static size_t SDLCALL WIIU_IO_Read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    return SDL_WIIU_FileRead(data->file, ptr, size, status);
}

static size_t SDLCALL WIIU_IO_Write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    return SDL_WIIU_FileWrite(data->file, ptr, size, status);
}

static bool SDLCALL WIIU_IO_Flush(void *userdata, SDL_IOStatus *status)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    return SDL_WIIU_FileFlush(data->file, status);
}

static bool SDLCALL WIIU_IO_Close(void *userdata)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    bool result = true;

    if (data) {
        result = SDL_WIIU_FileClose(data->file);
        SDL_free(data);
    }
    return result;
}

SDL_IOStream *SDL_WIIU_IOFromFile(const char *file, const char *mode)
{
    SDL_WiiUIOData *data;
    SDL_IOStreamInterface iface;
    SDL_IOStream *stream;

    data = (SDL_WiiUIOData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        return NULL;
    }

    data->file = SDL_WIIU_OpenFile(file, mode);
    if (!data->file) {
        SDL_free(data);
        return NULL;
    }

    SDL_INIT_INTERFACE(&iface);
    iface.size = WIIU_IO_Size;
    iface.seek = WIIU_IO_Seek;
    iface.read = WIIU_IO_Read;
    iface.write = WIIU_IO_Write;
    iface.flush = WIIU_IO_Flush;
    iface.close = WIIU_IO_Close;

    stream = SDL_OpenIO(&iface, data);
    if (!stream) {
        WIIU_IO_Close(data);
    }
    return stream;
}

#endif // SDL_PLATFORM_WIIU
