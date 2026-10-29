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

#include <SDL3/SDL_hints.h>

#include "SDL_iostreamwiiu.h"
#include "../../SDL_hints_c.h"
#include "../../core/wiiu/SDL_wiiufs.h"

#include <coreinit/filesystem.h>

#define WIIU_REGION_CACHE_DEFAULT_BUFFERS 4
#define WIIU_REGION_CACHE_MIN_BUFFERS     2
#define WIIU_REGION_CACHE_MAX_BUFFERS     16
#define WIIU_REGION_CACHE_DEFAULT_SIZE    (64 * 1024)
#define WIIU_REGION_CACHE_MIN_SIZE        (4 * 1024)
#define WIIU_REGION_CACHE_MAX_SIZE        (256 * 1024)
#define WIIU_WRITEBEHIND_BUFFER_SIZE (64 * 1024)
#define WIIU_IO_BUFFER_ALIGNMENT     64
#define WIIU_IO_INVALID_REGION       SDL_MAX_UINT64

typedef enum SDL_WiiUIORegionState
{
    WIIU_IO_REGION_EMPTY,
    WIIU_IO_REGION_LOADING,
    WIIU_IO_REGION_READY
} SDL_WiiUIORegionState;

typedef struct SDL_WiiUIOCacheConfig
{
    int num_buffers;
    size_t region_size;
} SDL_WiiUIOCacheConfig;

typedef struct SDL_WiiUIOData SDL_WiiUIOData;

typedef struct SDL_WiiUIORegionSlot
{
    SDL_WiiUIOData *owner;
    Uint8 *buffer;
    Uint64 region;
    Uint64 last_used;
    size_t valid_bytes;
    FSCmdBlock cmd_block;
    FSAsyncData async_data;
    SDL_AtomicInt state;
} SDL_WiiUIORegionSlot;

struct SDL_WiiUIOData
{
    SDL_WIIUFile *file;
    SDL_Mutex *prefetch_lock;
    SDL_Condition *prefetch_condition;
    Uint8 *cache_memory;
    SDL_WiiUIORegionSlot *slots;
    Uint8 *write_buffer;
    Uint64 logical_position;
    Uint64 physical_position;
    Uint64 write_buffer_start;
    Uint64 access_counter;
    size_t write_buffer_pos;
    size_t region_size;
    int num_slots;
    bool physical_position_valid;
    bool readable;
    bool writable;
    bool append;
};

static SDL_InitState SDL_wiiu_io_cache_config_init;
static SDL_WiiUIOCacheConfig SDL_wiiu_io_cache_config = {
    WIIU_REGION_CACHE_DEFAULT_BUFFERS,
    WIIU_REGION_CACHE_DEFAULT_SIZE
};

static bool WIIU_IO_SeekPhysical(SDL_WiiUIOData *data, Uint64 position)
{
    if (!data) {
        return false;
    }

    if (position > SDL_MAX_SINT64) {
        return SDL_SetError("Seek offset out of range");
    }

    if (data->physical_position_valid && data->physical_position == position) {
        return true;
    }

    if (SDL_WIIU_FileSeek(data->file, (Sint64) position, SDL_IO_SEEK_SET) < 0) {
        return false;
    }

    data->physical_position = position;
    data->physical_position_valid = true;
    return true;
}

static size_t WIIU_IO_AlignUp(size_t value, size_t alignment)
{
    return (value + (alignment - 1)) & ~(alignment - 1);
}

static const SDL_WiiUIOCacheConfig *WIIU_IO_GetCacheConfig(void)
{
    if (SDL_ShouldInit(&SDL_wiiu_io_cache_config_init)) {
        const int num_buffers = SDL_clamp(SDL_GetStringInteger(SDL_GetHint(SDL_HINT_WIIU_FILE_REGION_CACHE_BUFFERS), WIIU_REGION_CACHE_DEFAULT_BUFFERS),
                                          WIIU_REGION_CACHE_MIN_BUFFERS,
                                          WIIU_REGION_CACHE_MAX_BUFFERS);
        const int region_size = SDL_clamp(SDL_GetStringInteger(SDL_GetHint(SDL_HINT_WIIU_FILE_REGION_CACHE_SIZE), WIIU_REGION_CACHE_DEFAULT_SIZE),
                                          WIIU_REGION_CACHE_MIN_SIZE,
                                          WIIU_REGION_CACHE_MAX_SIZE);

        SDL_wiiu_io_cache_config.num_buffers = num_buffers;
        SDL_wiiu_io_cache_config.region_size = WIIU_IO_AlignUp((size_t) region_size, WIIU_IO_BUFFER_ALIGNMENT);
        SDL_SetInitialized(&SDL_wiiu_io_cache_config_init, true);
    }
    return &SDL_wiiu_io_cache_config;
}

static void WIIU_IO_ResetSlot(SDL_WiiUIORegionSlot *slot)
{
    slot->region = WIIU_IO_INVALID_REGION;
    slot->last_used = 0;
    slot->valid_bytes = 0;
    SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_EMPTY);
}

static void WIIU_IO_PrefetchCallback(FSClient *client, FSCmdBlock *block, FSStatus result, uint32_t context)
{
    SDL_WiiUIORegionSlot *slot = (SDL_WiiUIORegionSlot *) (uintptr_t) context;
    SDL_WiiUIOData *data = slot->owner;
    (void) client;
    (void) block;

    SDL_LockMutex(data->prefetch_lock);
    if (result < 0 && result != FS_STATUS_END) {
        SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_EMPTY);
    } else {
        slot->valid_bytes = (result == FS_STATUS_END) ? 0 : (size_t) result;
        SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_READY);
    }
    SDL_BroadcastCondition(data->prefetch_condition);
    SDL_UnlockMutex(data->prefetch_lock);
}

static void WIIU_IO_CancelPendingPrefetch(SDL_WiiUIOData *data)
{
    int i;

    SDL_LockMutex(data->prefetch_lock);
    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        if (SDL_GetAtomicInt(&slot->state) == WIIU_IO_REGION_LOADING) {
            SDL_WIIU_CancelFileRead(&slot->cmd_block);
        }
    }

    // Cancellation is asynchronous; callbacks must finish before reuse or writes.
    for (i = 0; i < data->num_slots; ++i) {
        while (SDL_GetAtomicInt(&data->slots[i].state) == WIIU_IO_REGION_LOADING) {
            SDL_WaitCondition(data->prefetch_condition, data->prefetch_lock);
        }
    }
    SDL_UnlockMutex(data->prefetch_lock);
}

static void WIIU_IO_InvalidateReadCache(SDL_WiiUIOData *data)
{
    int i;

    WIIU_IO_CancelPendingPrefetch(data);
    for (i = 0; i < data->num_slots; ++i) {
        WIIU_IO_ResetSlot(&data->slots[i]);
    }
}

static SDL_WiiUIORegionSlot *WIIU_IO_FindReadySlot(SDL_WiiUIOData *data, Uint64 region)
{
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        const int state = SDL_GetAtomicInt(&slot->state);

        if (state == WIIU_IO_REGION_READY) {
            if (slot->region == region) {
                slot->last_used = ++data->access_counter;
                return slot;
            }
        }
    }

    return NULL;
}

static bool WIIU_IO_HasReadySlot(SDL_WiiUIOData *data, Uint64 region)
{
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        if (SDL_GetAtomicInt(&slot->state) == WIIU_IO_REGION_READY) {
            if (slot->region == region) {
                return true;
            }
        }
    }

    return false;
}

static bool WIIU_IO_IsRegionLoading(SDL_WiiUIOData *data, Uint64 region)
{
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        if ((SDL_GetAtomicInt(&slot->state) == WIIU_IO_REGION_LOADING) &&
            (slot->region == region)) {
            return true;
        }
    }

    return false;
}

static SDL_WiiUIORegionSlot *WIIU_IO_SelectVictimSlot(SDL_WiiUIOData *data, Uint64 pinned_region)
{
    SDL_WiiUIORegionSlot *victim = NULL;
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        const int state = SDL_GetAtomicInt(&slot->state);

        if (state == WIIU_IO_REGION_EMPTY) {
            return slot;
        }
        if (state == WIIU_IO_REGION_READY) {
            if (slot->region == pinned_region) {
                continue;
            }
            if (!victim || (slot->last_used < victim->last_used)) {
                victim = slot;
            }
        }
    }

    return victim;
}

static size_t WIIU_IO_ReadDirect(SDL_WiiUIOData *data, void *ptr, size_t size, SDL_IOStatus *status)
{
    const Uint64 position = data->logical_position;
    const size_t bytes = size;

    if (!WIIU_IO_SeekPhysical(data, position)) {
        if (status && *status == SDL_IO_STATUS_READY) {
            *status = SDL_IO_STATUS_ERROR;
        }
        return 0;
    }

    const size_t read = SDL_WIIU_FileRead(data->file, ptr, bytes, status);
    data->physical_position = position + read;
    data->physical_position_valid = true;
    return read;
}

static bool WIIU_IO_LoadRegionSync(SDL_WiiUIOData *data, SDL_WiiUIORegionSlot *slot, Uint64 region, SDL_IOStatus *status)
{
    const Uint64 position = region * data->region_size;
    size_t bytes;

    if (!WIIU_IO_SeekPhysical(data, position)) {
        if (status && *status == SDL_IO_STATUS_READY) {
            *status = SDL_IO_STATUS_ERROR;
        }
        return false;
    }

    bytes = SDL_WIIU_FileRead(data->file, slot->buffer, data->region_size, status);
    if (status && (*status == SDL_IO_STATUS_ERROR)) {
        WIIU_IO_ResetSlot(slot);
        return false;
    }

    slot->region = region;
    slot->valid_bytes = bytes;
    slot->last_used = ++data->access_counter;
    data->physical_position = position + bytes;
    data->physical_position_valid = true;
    SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_READY);
    return true;
}

static void WIIU_IO_EnsurePrefetch(SDL_WiiUIOData *data, Uint64 region)
{
    SDL_WiiUIORegionSlot *slot;
    Uint64 position;
    FSStatus status;

    if (WIIU_IO_HasReadySlot(data, region) || WIIU_IO_IsRegionLoading(data, region)) {
        return;
    }

    if (region > UINT32_MAX / data->region_size) {
        return;
    }
    position = region * data->region_size;

    slot = WIIU_IO_SelectVictimSlot(data, (region > 0) ? (region - 1) : WIIU_IO_INVALID_REGION);
    if (!slot) {
        return;
    }

    slot->region = region;
    slot->valid_bytes = 0;
    slot->last_used = ++data->access_counter;
    FSInitCmdBlock(&slot->cmd_block);
    SDL_zero(slot->async_data);
    slot->async_data.callback = WIIU_IO_PrefetchCallback;
    slot->async_data.param = (uint32_t) (uintptr_t) slot;
    SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_LOADING);

    // Positioned reads also advance the native handle. The next synchronous
    // transfer must seek after queued prefetch commands before using it again.
    data->physical_position_valid = false;
    status = SDL_WIIU_ReadFileAsync(data->file, &slot->cmd_block, slot->buffer,
                                    (uint32_t) data->region_size,
                                    (uint32_t) position, &slot->async_data);
    if (status != FS_STATUS_OK) {
        WIIU_IO_ResetSlot(slot);
    }
}

static bool WIIU_IO_FlushWriteBuffer(SDL_WiiUIOData *data, SDL_IOStatus *status)
{
    if (!data || data->write_buffer_pos == 0) {
        return true;
    }

    if (data->append) {
        const size_t written = SDL_WIIU_FileWrite(data->file, data->write_buffer, data->write_buffer_pos, status);
        if (written < data->write_buffer_pos) {
            if (written > 0) {
                SDL_memmove(data->write_buffer, data->write_buffer + written, data->write_buffer_pos - written);
                data->write_buffer_pos -= written;
            }
            data->physical_position_valid = false;
            return false;
        }

        data->write_buffer_pos = 0;
        data->physical_position_valid = false;
        return true;
    }

    if (!WIIU_IO_SeekPhysical(data, data->write_buffer_start)) {
        if (status && *status == SDL_IO_STATUS_READY) {
            *status = SDL_IO_STATUS_ERROR;
        }
        return false;
    }

    const size_t written = SDL_WIIU_FileWrite(data->file, data->write_buffer, data->write_buffer_pos, status);
    if (written < data->write_buffer_pos) {
        if (written > 0) {
            SDL_memmove(data->write_buffer, data->write_buffer + written, data->write_buffer_pos - written);
            data->write_buffer_start += written;
            data->write_buffer_pos -= written;
            data->physical_position = data->write_buffer_start;
            data->physical_position_valid = true;
        }
        return false;
    }

    data->physical_position = data->write_buffer_start + written;
    data->physical_position_valid = true;
    data->write_buffer_pos = 0;
    return true;
}

static Sint64 WIIU_IO_GetEffectiveSize(SDL_WiiUIOData *data)
{
    const Sint64 base_size = SDL_WIIU_FileSize(data->file);
    if (base_size < 0) {
        return -1;
    }

    if (!data->append && data->write_buffer_pos > 0) {
        const Uint64 buffered_end = data->write_buffer_start + data->write_buffer_pos;
        if (buffered_end > (Uint64) base_size) {
            if (buffered_end > SDL_MAX_SINT64) {
                return SDL_SetError("File size out of range");
            }
            return (Sint64) buffered_end;
        }
    }

    return base_size;
}

static Sint64 SDLCALL WIIU_IO_Size(void *userdata)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    return WIIU_IO_GetEffectiveSize(data);
}

static Sint64 SDLCALL WIIU_IO_Seek(void *userdata, Sint64 offset, SDL_IOWhence whence)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    Sint64 base = 0;
    Sint64 target;

    if (!data) {
        SDL_InvalidParamError("userdata");
        return -1;
    }

    if (!WIIU_IO_FlushWriteBuffer(data, NULL)) {
        return -1;
    }

    switch (whence) {
    case SDL_IO_SEEK_SET:
        base = 0;
        break;
    case SDL_IO_SEEK_CUR:
        if (data->logical_position > SDL_MAX_SINT64) {
            return SDL_SetError("Seek offset out of range");
        }
        base = (Sint64) data->logical_position;
        break;
    case SDL_IO_SEEK_END:
        base = WIIU_IO_GetEffectiveSize(data);
        if (base < 0) {
            return -1;
        }
        break;
    default:
        return SDL_SetError("Unknown value for 'whence'");
    }

    target = base + offset;
    if (target < 0) {
        return SDL_SetError("Seek offset out of range");
    }

    data->logical_position = (Uint64) target;
    return target;
}

static size_t SDLCALL WIIU_IO_Read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    Uint8 *dst = (Uint8 *) ptr;
    size_t total_need = size;
    size_t total_read = 0;

    if (!WIIU_IO_FlushWriteBuffer(data, status)) {
        return 0;
    }

    while (total_need > 0) {
        SDL_WiiUIORegionSlot *slot;
        const Uint64 region = data->logical_position / data->region_size;
        const size_t region_offset = (size_t) (data->logical_position % data->region_size);

        SDL_LockMutex(data->prefetch_lock);
        while (WIIU_IO_IsRegionLoading(data, region)) {
            SDL_WaitCondition(data->prefetch_condition, data->prefetch_lock);
        }
        slot = WIIU_IO_FindReadySlot(data, region);
        SDL_UnlockMutex(data->prefetch_lock);

        if (!slot && total_need < data->region_size) {
            slot = WIIU_IO_SelectVictimSlot(data, WIIU_IO_INVALID_REGION);
            if (slot && !WIIU_IO_LoadRegionSync(data, slot, region, status)) {
                break;
            }
        }
        if (!slot) {
            size_t chunk = total_need;
            int i;

            // Leave future cached or outstanding regions for the next iteration.
            for (i = 0; i < data->num_slots; ++i) {
                SDL_WiiUIORegionSlot *next = &data->slots[i];
                if (SDL_GetAtomicInt(&next->state) != WIIU_IO_REGION_EMPTY) {
                    const Uint64 next_position = next->region * data->region_size;
                    if (next_position > data->logical_position) {
                        chunk = (size_t) SDL_min((Uint64) chunk, next_position - data->logical_position);
                    }
                }
            }

            const size_t bytes = WIIU_IO_ReadDirect(data, dst, chunk, status);
            dst += bytes;
            total_need -= bytes;
            total_read += bytes;
            data->logical_position += bytes;
            if (bytes < chunk) {
                break;
            }
            continue;
        }

        const size_t available = (region_offset < slot->valid_bytes) ? (slot->valid_bytes - region_offset) : 0;

        if (available == 0) {
            if (status && *status == SDL_IO_STATUS_READY) {
                *status = SDL_IO_STATUS_EOF;
            }
            break;
        }

        const size_t chunk = SDL_min(total_need, available);
        SDL_memcpy(dst, slot->buffer + region_offset, chunk);
        dst += chunk;
        total_need -= chunk;
        total_read += chunk;
        data->logical_position += chunk;

        if (slot->valid_bytes == data->region_size && total_need < data->region_size) {
            WIIU_IO_EnsurePrefetch(data, region + 1);
        }

        if ((chunk == available) && (slot->valid_bytes < data->region_size)) {
            if (status && *status == SDL_IO_STATUS_READY) {
                *status = SDL_IO_STATUS_EOF;
            }
            break;
        }
    }

    return total_read;
}

static size_t SDLCALL WIIU_IO_Write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    const Uint8 *src = (const Uint8 *) ptr;
    size_t remaining = size;
    size_t total_written = 0;

    if (!data->writable) {
        if (status) {
            *status = SDL_IO_STATUS_READONLY;
        }
        return 0;
    }

    WIIU_IO_InvalidateReadCache(data);

    if (data->append) {
        if (!WIIU_IO_FlushWriteBuffer(data, status)) {
            return 0;
        }

        const Sint64 eof = SDL_WIIU_FileSize(data->file);
        if (eof < 0) {
            if (status && *status == SDL_IO_STATUS_READY) {
                *status = SDL_IO_STATUS_ERROR;
            }
            return 0;
        }

        const size_t bytes = SDL_WIIU_FileWrite(data->file, ptr, size, status);
        data->logical_position = (Uint64) eof + bytes;
        data->physical_position_valid = false;
        return bytes;
    }

    if (data->write_buffer_pos > 0 &&
        data->logical_position != (data->write_buffer_start + data->write_buffer_pos)) {
        if (!WIIU_IO_FlushWriteBuffer(data, status)) {
            return 0;
        }
    }

    while (remaining > 0) {
        if (data->write_buffer_pos == 0) {
            data->write_buffer_start = data->logical_position;
            if (remaining >= WIIU_WRITEBEHIND_BUFFER_SIZE) {
                if (!WIIU_IO_SeekPhysical(data, data->logical_position)) {
                    if (status && *status == SDL_IO_STATUS_READY) {
                        *status = SDL_IO_STATUS_ERROR;
                    }
                    break;
                }

                const size_t bytes = SDL_WIIU_FileWrite(data->file, src, remaining, status);
                data->logical_position += bytes;
                data->physical_position = data->logical_position;
                data->physical_position_valid = true;
                total_written += bytes;
                break;
            }
        }

        const size_t space = WIIU_WRITEBEHIND_BUFFER_SIZE - data->write_buffer_pos;
        const size_t chunk = SDL_min(remaining, space);

        SDL_memcpy(data->write_buffer + data->write_buffer_pos, src, chunk);
        data->write_buffer_pos += chunk;
        src += chunk;
        remaining -= chunk;
        total_written += chunk;
        data->logical_position += chunk;

        if (data->write_buffer_pos == WIIU_WRITEBEHIND_BUFFER_SIZE) {
            if (!WIIU_IO_FlushWriteBuffer(data, status)) {
                break;
            }
        }
    }

    return total_written;
}

static bool SDLCALL WIIU_IO_Flush(void *userdata, SDL_IOStatus *status)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    if (!WIIU_IO_FlushWriteBuffer(data, status)) {
        return false;
    }
    return SDL_WIIU_FileFlush(data->file, status);
}

static bool SDLCALL WIIU_IO_Close(void *userdata)
{
    SDL_WiiUIOData *data = (SDL_WiiUIOData *) userdata;
    bool result = true;

    if (data) {
        SDL_IOStatus status = SDL_IO_STATUS_READY;
        WIIU_IO_CancelPendingPrefetch(data);
        result = WIIU_IO_FlushWriteBuffer(data, &status);
        result = SDL_WIIU_FileClose(data->file) && result;
        SDL_aligned_free(data->cache_memory);
        SDL_DestroyCondition(data->prefetch_condition);
        SDL_DestroyMutex(data->prefetch_lock);
        SDL_free(data);
    }
    return result;
}

SDL_IOStream *SDL_WIIU_IOFromFile(const char *file, const char *mode)
{
    const SDL_WiiUIOCacheConfig *config = WIIU_IO_GetCacheConfig();
    SDL_WiiUIOData *data;
    size_t data_bytes;
    size_t cache_bytes;
    int i;
    SDL_IOStreamInterface iface;
    SDL_IOStream *stream;

    data_bytes = sizeof(*data) + ((size_t) config->num_buffers * sizeof(*data->slots));
    cache_bytes = config->region_size * (size_t) config->num_buffers;

    data = (SDL_WiiUIOData *) SDL_calloc(1, data_bytes);
    if (!data) {
        return NULL;
    }

    data->slots = (SDL_WiiUIORegionSlot *) (data + 1);

    data->file = SDL_WIIU_OpenFile(file, mode);
    if (!data->file) {
        SDL_free(data);
        return NULL;
    }

    data->readable = (SDL_strchr(mode, 'r') != NULL) || (SDL_strchr(mode, '+') != NULL);
    data->writable = (SDL_strchr(mode, 'w') != NULL) ||
                     (SDL_strchr(mode, 'a') != NULL) ||
                     (SDL_strchr(mode, '+') != NULL);
    data->append = (SDL_strchr(mode, 'a') != NULL);
    data->physical_position_valid = true;
    data->logical_position = 0;
    data->physical_position = 0;
    data->num_slots = config->num_buffers;
    data->region_size = config->region_size;

    data->prefetch_lock = SDL_CreateMutex();
    data->prefetch_condition = SDL_CreateCondition();
    data->cache_memory = (Uint8 *) SDL_aligned_alloc(WIIU_IO_BUFFER_ALIGNMENT, cache_bytes + WIIU_WRITEBEHIND_BUFFER_SIZE);
    if (!data->prefetch_lock || !data->prefetch_condition || !data->cache_memory) {
        WIIU_IO_Close(data);
        return NULL;
    }

    data->write_buffer = data->cache_memory + cache_bytes;
    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        slot->owner = data;
        slot->buffer = data->cache_memory + ((size_t) i * data->region_size);
        WIIU_IO_ResetSlot(slot);
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
