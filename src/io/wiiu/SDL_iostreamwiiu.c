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
#define WIIU_REGION_CACHE_DEFAULT_SIZE    (32 * 1024)
#define WIIU_REGION_CACHE_MIN_SIZE        (4 * 1024)
#define WIIU_REGION_CACHE_MAX_SIZE        (256 * 1024)
#define WIIU_WRITEBEHIND_BUFFER_SIZE (16 * 1024)
#define WIIU_IO_BUFFER_ALIGNMENT     64
#define WIIU_IO_PREFETCH_ERROR_MASK  FS_ERROR_FLAG_ALL
#define WIIU_IO_INVALID_REGION       SDL_MAX_UINT64

extern void FSCancelCommand(FSClient *client, FSCmdBlock *block);

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

typedef struct SDL_WiiUIPrefetchState
{
    SDL_InitState init;
    SDL_Mutex *lock;
    FSClient client;
    bool client_ready;
} SDL_WiiUIPrefetchState;

typedef struct SDL_WiiUIOData SDL_WiiUIOData;

typedef struct SDL_WiiUIORegionSlot
{
    SDL_WiiUIOData *owner;
    Uint8 *buffer;
    Uint64 region;
    Uint64 last_used;
    size_t valid_bytes;
    int generation;
    FSCmdBlock cmd_block;
    FSAsyncData async_data;
    SDL_AtomicInt state;
} SDL_WiiUIORegionSlot;

struct SDL_WiiUIOData
{
    SDL_WIIUFile *file;
    char *prefetch_path;
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
    SDL_AtomicInt cache_generation;
    bool physical_position_valid;
    bool readable;
    bool writable;
    bool append;
    bool prefetch_available;
    bool prefetch_failed;
    bool prefetch_handle_open;
    FSFileHandle prefetch_handle;
};

static SDL_InitState SDL_wiiu_io_cache_config_init;
static SDL_WiiUIOCacheConfig SDL_wiiu_io_cache_config = {
    WIIU_REGION_CACHE_DEFAULT_BUFFERS,
    WIIU_REGION_CACHE_DEFAULT_SIZE
};
static SDL_WiiUIPrefetchState SDL_wiiu_prefetch_state;

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

static bool WIIU_IO_InitPrefetchClient(void)
{
    if (SDL_ShouldInit(&SDL_wiiu_prefetch_state.init)) {
        FSStatus status;

        SDL_wiiu_prefetch_state.lock = SDL_CreateMutex();
        if (!SDL_wiiu_prefetch_state.lock) {
            return false;
        }

        FSInit();
        status = FSAddClient(&SDL_wiiu_prefetch_state.client, WIIU_IO_PREFETCH_ERROR_MASK);
        if (status != FS_STATUS_OK) {
            SDL_DestroyMutex(SDL_wiiu_prefetch_state.lock);
            SDL_wiiu_prefetch_state.lock = NULL;
            SDL_ClearError();
            return false;
        }

        SDL_wiiu_prefetch_state.client_ready = true;
        SDL_SetInitialized(&SDL_wiiu_prefetch_state.init, true);
    }

    return SDL_wiiu_prefetch_state.client_ready;
}

static int WIIU_IO_GetCurrentGeneration(SDL_WiiUIOData *data)
{
    return SDL_GetAtomicInt(&data->cache_generation);
}

static int WIIU_IO_AdvanceGeneration(SDL_WiiUIOData *data)
{
    return SDL_AddAtomicInt(&data->cache_generation, 1) + 1;
}

static void WIIU_IO_ResetSlot(SDL_WiiUIORegionSlot *slot)
{
    slot->region = WIIU_IO_INVALID_REGION;
    slot->last_used = 0;
    slot->valid_bytes = 0;
    slot->generation = 0;
    SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_EMPTY);
}

static bool WIIU_IO_GetRegionStart(const SDL_WiiUIOData *data, Uint64 region, Uint64 *position)
{
    if (region > (SDL_MAX_UINT64 / data->region_size)) {
        return SDL_SetError("Wii U file region exceeds supported range");
    }

    *position = region * data->region_size;
    return true;
}

static void WIIU_IO_PrefetchCallback(FSClient *client, FSCmdBlock *block, FSStatus result, uint32_t context)
{
    SDL_WiiUIORegionSlot *slot = (SDL_WiiUIORegionSlot *) (uintptr_t) context;
    SDL_WiiUIOData *data = slot->owner;
    const int current_generation = WIIU_IO_GetCurrentGeneration(data);
    (void) client;
    (void) block;

    if ((slot->generation != current_generation) || (result == FS_STATUS_CANCELLED)) {
        WIIU_IO_ResetSlot(slot);
        return;
    }

    if (result == FS_STATUS_END) {
        slot->valid_bytes = 0;
        SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_READY);
        return;
    }

    if (result < 0) {
        WIIU_IO_ResetSlot(slot);
        return;
    }

    slot->valid_bytes = (size_t) result;
    SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_READY);
}

static bool WIIU_IO_OpenPrefetchHandle(SDL_WiiUIOData *data, const char *file)
{
    FSCmdBlock cmd_block;
    char *resolved = NULL;
    FSStatus status;

    if (!data->readable) {
        return false;
    }

    resolved = SDL_WIIU_ResolveFSPath(file);
    if (!resolved) {
        SDL_ClearError();
        return false;
    }

    if (!WIIU_IO_InitPrefetchClient()) {
        SDL_free(resolved);
        SDL_ClearError();
        return false;
    }

    FSInitCmdBlock(&cmd_block);
    SDL_LockMutex(SDL_wiiu_prefetch_state.lock);
    status = FSOpenFile(&SDL_wiiu_prefetch_state.client,
                        &cmd_block,
                        resolved,
                        "r",
                        &data->prefetch_handle,
                        WIIU_IO_PREFETCH_ERROR_MASK);
    SDL_UnlockMutex(SDL_wiiu_prefetch_state.lock);
    SDL_free(resolved);
    if (status != FS_STATUS_OK) {
        SDL_ClearError();
        return false;
    }

    data->prefetch_handle_open = true;
    data->prefetch_available = true;
    return true;
}

static void WIIU_IO_CancelPendingPrefetch(SDL_WiiUIOData *data)
{
    int i;

    if (!data->prefetch_available) {
        return;
    }

    SDL_LockMutex(SDL_wiiu_prefetch_state.lock);
    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        if (SDL_GetAtomicInt(&slot->state) == WIIU_IO_REGION_LOADING) {
            FSCancelCommand(&SDL_wiiu_prefetch_state.client, &slot->cmd_block);
        }
    }
    SDL_UnlockMutex(SDL_wiiu_prefetch_state.lock);
}

static void WIIU_IO_InvalidateReadCache(SDL_WiiUIOData *data)
{
    const int generation = WIIU_IO_AdvanceGeneration(data);
    int i;

    WIIU_IO_CancelPendingPrefetch(data);

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        if (SDL_GetAtomicInt(&slot->state) == WIIU_IO_REGION_LOADING) {
            slot->generation = generation - 1;
            slot->region = WIIU_IO_INVALID_REGION;
            slot->valid_bytes = 0;
            slot->last_used = 0;
        } else {
            WIIU_IO_ResetSlot(slot);
        }
    }
}

static SDL_WiiUIORegionSlot *WIIU_IO_FindReadySlot(SDL_WiiUIOData *data, Uint64 region)
{
    const int generation = WIIU_IO_GetCurrentGeneration(data);
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        const int state = SDL_GetAtomicInt(&slot->state);

        if (state == WIIU_IO_REGION_READY) {
            if (slot->generation != generation) {
                WIIU_IO_ResetSlot(slot);
                continue;
            }
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
    const int generation = WIIU_IO_GetCurrentGeneration(data);
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        if (SDL_GetAtomicInt(&slot->state) == WIIU_IO_REGION_READY) {
            if (slot->generation != generation) {
                WIIU_IO_ResetSlot(slot);
                continue;
            }
            if (slot->region == region) {
                return true;
            }
        }
    }

    return false;
}

static bool WIIU_IO_IsRegionLoading(SDL_WiiUIOData *data, Uint64 region)
{
    const int generation = WIIU_IO_GetCurrentGeneration(data);
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        if ((SDL_GetAtomicInt(&slot->state) == WIIU_IO_REGION_LOADING) &&
            (slot->generation == generation) &&
            (slot->region == region)) {
            return true;
        }
    }

    return false;
}

static SDL_WiiUIORegionSlot *WIIU_IO_SelectVictimSlot(SDL_WiiUIOData *data, Uint64 pinned_region)
{
    const int generation = WIIU_IO_GetCurrentGeneration(data);
    SDL_WiiUIORegionSlot *victim = NULL;
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        SDL_WiiUIORegionSlot *slot = &data->slots[i];
        const int state = SDL_GetAtomicInt(&slot->state);

        if (state == WIIU_IO_REGION_EMPTY) {
            return slot;
        }
        if ((state == WIIU_IO_REGION_READY) && (slot->generation != generation)) {
            WIIU_IO_ResetSlot(slot);
            return slot;
        }
        if (state == WIIU_IO_REGION_READY) {
            if ((slot->generation == generation) && (slot->region == pinned_region)) {
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
    const int generation = WIIU_IO_GetCurrentGeneration(data);
    Uint64 position;
    size_t bytes;

    if (!WIIU_IO_GetRegionStart(data, region, &position)) {
        if (status && *status == SDL_IO_STATUS_READY) {
            *status = SDL_IO_STATUS_ERROR;
        }
        return false;
    }

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
    slot->generation = generation;
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
    const int generation = WIIU_IO_GetCurrentGeneration(data);

    if (data->prefetch_failed) {
        return;
    }

    if (WIIU_IO_HasReadySlot(data, region) || WIIU_IO_IsRegionLoading(data, region)) {
        return;
    }

    if (!WIIU_IO_GetRegionStart(data, region, &position)) {
        SDL_ClearError();
        return;
    }
    if ((position > UINT32_MAX) || (data->region_size > UINT32_MAX)) {
        return;
    }

    if (!data->prefetch_handle_open) {
        if (!data->prefetch_path || !WIIU_IO_OpenPrefetchHandle(data, data->prefetch_path)) {
            data->prefetch_failed = true;
            return;
        }
    }

    slot = WIIU_IO_SelectVictimSlot(data, (region > 0) ? (region - 1) : WIIU_IO_INVALID_REGION);
    if (!slot) {
        return;
    }

    slot->region = region;
    slot->generation = generation;
    slot->valid_bytes = 0;
    slot->last_used = ++data->access_counter;
    FSInitCmdBlock(&slot->cmd_block);
    SDL_zero(slot->async_data);
    slot->async_data.callback = WIIU_IO_PrefetchCallback;
    slot->async_data.param = (uint32_t) (uintptr_t) slot;
    SDL_SetAtomicInt(&slot->state, WIIU_IO_REGION_LOADING);

    SDL_LockMutex(SDL_wiiu_prefetch_state.lock);
    status = FSReadFileWithPosAsync(&SDL_wiiu_prefetch_state.client,
                                    &slot->cmd_block,
                                    slot->buffer,
                                    1,
                                    (uint32_t) data->region_size,
                                    (uint32_t) position,
                                    data->prefetch_handle,
                                    0,
                                    WIIU_IO_PREFETCH_ERROR_MASK,
                                    &slot->async_data);
    SDL_UnlockMutex(SDL_wiiu_prefetch_state.lock);
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

static bool WIIU_IO_HasPendingPrefetch(SDL_WiiUIOData *data)
{
    int i;

    for (i = 0; i < data->num_slots; ++i) {
        if (SDL_GetAtomicInt(&data->slots[i].state) == WIIU_IO_REGION_LOADING) {
            return true;
        }
    }

    return false;
}

static void WIIU_IO_ShutdownPrefetch(SDL_WiiUIOData *data)
{
    FSCmdBlock cmd_block;

    if (!data->prefetch_handle_open) {
        return;
    }

    WIIU_IO_CancelPendingPrefetch(data);
    while (WIIU_IO_HasPendingPrefetch(data)) {
        SDL_Delay(0);
    }

    if (data->prefetch_handle_open) {
        FSInitCmdBlock(&cmd_block);
        SDL_LockMutex(SDL_wiiu_prefetch_state.lock);
        (void) FSCloseFile(&SDL_wiiu_prefetch_state.client,
                           &cmd_block,
                           data->prefetch_handle,
                           WIIU_IO_PREFETCH_ERROR_MASK);
        SDL_UnlockMutex(SDL_wiiu_prefetch_state.lock);
        data->prefetch_handle_open = false;
    }

    data->prefetch_available = false;
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

        slot = WIIU_IO_FindReadySlot(data, region);
        if (!slot) {
            if (!WIIU_IO_IsRegionLoading(data, region)) {
                slot = WIIU_IO_SelectVictimSlot(data, WIIU_IO_INVALID_REGION);
                if (slot && !WIIU_IO_LoadRegionSync(data, slot, region, status)) {
                    break;
                }
            }
            if (!slot) {
                const size_t chunk = SDL_min(total_need, data->region_size - region_offset);
                const size_t bytes = WIIU_IO_ReadDirect(data, dst, chunk, status);
                if (bytes == 0) {
                    break;
                }
                dst += bytes;
                total_need -= bytes;
                total_read += bytes;
                data->logical_position += bytes;
                if (bytes < chunk) {
                    break;
                }
                continue;
            }
        }

        if (slot) {
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

            if (slot->valid_bytes == data->region_size) {
                WIIU_IO_EnsurePrefetch(data, region + 1);
            }

            if ((chunk == available) && (slot->valid_bytes < data->region_size)) {
                if (status && *status == SDL_IO_STATUS_READY) {
                    *status = SDL_IO_STATUS_EOF;
                }
                break;
            }
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

    if (size >= WIIU_WRITEBEHIND_BUFFER_SIZE && data->write_buffer_pos == 0) {
        if (!WIIU_IO_SeekPhysical(data, data->logical_position)) {
            if (status && *status == SDL_IO_STATUS_READY) {
                *status = SDL_IO_STATUS_ERROR;
            }
            return 0;
        }

        const size_t bytes = SDL_WIIU_FileWrite(data->file, ptr, size, status);
        data->logical_position += bytes;
        data->physical_position = data->logical_position;
        data->physical_position_valid = true;
        return bytes;
    }

    if (data->write_buffer_pos == 0) {
        data->write_buffer_start = data->logical_position;
    }

    while (remaining > 0) {
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
            data->write_buffer_start = data->logical_position;
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
        result = WIIU_IO_FlushWriteBuffer(data, &status);
        WIIU_IO_ShutdownPrefetch(data);
        result = SDL_WIIU_FileClose(data->file) && result;
        SDL_free(data->prefetch_path);
        SDL_aligned_free(data->cache_memory);
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

    if ((size_t) config->num_buffers > (SDL_SIZE_MAX / sizeof(*data->slots))) {
        return SDL_SetError("Wii U cache metadata exceeds SDL_SIZE_MAX"), (SDL_IOStream *) NULL;
    }
    if (config->region_size > (SDL_SIZE_MAX / (size_t) config->num_buffers)) {
        return SDL_SetError("Wii U cache allocation exceeds SDL_SIZE_MAX"), (SDL_IOStream *) NULL;
    }
    if ((size_t) config->num_buffers > ((SDL_SIZE_MAX - sizeof(*data) - WIIU_WRITEBEHIND_BUFFER_SIZE) / sizeof(*data->slots))) {
        return SDL_SetError("Wii U cache state exceeds SDL_SIZE_MAX"), (SDL_IOStream *) NULL;
    }

    data_bytes = sizeof(*data) + ((size_t) config->num_buffers * sizeof(*data->slots)) + WIIU_WRITEBEHIND_BUFFER_SIZE;
    cache_bytes = config->region_size * (size_t) config->num_buffers;

    data = (SDL_WiiUIOData *) SDL_calloc(1, data_bytes);
    if (!data) {
        return NULL;
    }

    data->slots = (SDL_WiiUIORegionSlot *) (data + 1);
    data->write_buffer = (Uint8 *) (data->slots + config->num_buffers);

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
    SDL_SetAtomicInt(&data->cache_generation, 1);
    data->prefetch_path = SDL_strdup(file);

    data->cache_memory = (Uint8 *) SDL_aligned_alloc(WIIU_IO_BUFFER_ALIGNMENT, cache_bytes);
    if (!data->cache_memory) {
        WIIU_IO_Close(data);
        return NULL;
    }

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
