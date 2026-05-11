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

#include "../SDL_sysasyncio.h"
#include "../../SDL_utils_c.h"
#include "../../core/wiiu/SDL_wiiufs.h"

#include <stdint.h>
#include <coreinit/filesystem.h>

#define SDL_WIIU_ASYNCIO_ALIGNMENT 64
#define SDL_WIIU_ASYNCIO_ERROR_MASK FS_ERROR_FLAG_ALL

extern void FSCancelCommand(FSClient *client, FSCmdBlock *block);

typedef struct SDL_WiiUAsyncIOQueueData
{
    SDL_Mutex *lock;
    SDL_Condition *condition;
    SDL_AsyncIOTask completed_tasks;
} SDL_WiiUAsyncIOQueueData;

typedef struct SDL_WiiUAsyncIOData
{
    SDL_Mutex *lock;
    FSClient client;
    FSFileHandle handle;
    bool readonly;
} SDL_WiiUAsyncIOData;

typedef enum SDL_WiiUAsyncIOStage
{
    SDL_WIIU_ASYNCIO_STAGE_IO,
    SDL_WIIU_ASYNCIO_STAGE_FLUSH,
    SDL_WIIU_ASYNCIO_STAGE_CLOSE
} SDL_WiiUAsyncIOStage;

typedef struct SDL_WiiUAsyncIOTaskData
{
    SDL_AsyncIOTask *task;
    SDL_WiiUAsyncIOData *filedata;
    FSCmdBlock cmd_block;
    FSAsyncData async_data;
    Uint8 *bounce_buffer;
    Uint8 *io_buffer;
    FSStatus staged_result;
    SDL_WiiUAsyncIOStage stage;
} SDL_WiiUAsyncIOTaskData;

static SDL_InitState SDL_wiiu_asyncio_init;

static void WIIU_AsyncIOComplete(SDL_AsyncIOTask *task)
{
    SDL_WiiUAsyncIOQueueData *queuedata = (SDL_WiiUAsyncIOQueueData *) task->queue->userdata;
    SDL_LockMutex(queuedata->lock);
    LINKED_LIST_PREPEND(task, queuedata->completed_tasks, queue);
    SDL_SignalCondition(queuedata->condition);
    SDL_UnlockMutex(queuedata->lock);
}

static bool WIIU_InitializeAsyncIO(void)
{
    if (SDL_ShouldInit(&SDL_wiiu_asyncio_init)) {
        SDL_DebugLogBackend("asyncio", "wiiu");
        SDL_SetInitialized(&SDL_wiiu_asyncio_init, true);
    }
    return true;
}

static size_t WIIU_GetAlignedBufferSize(Uint64 requested)
{
    size_t size = (size_t) SDL_max(requested, 1);
    return (size + (SDL_WIIU_ASYNCIO_ALIGNMENT - 1)) & ~(SDL_WIIU_ASYNCIO_ALIGNMENT - 1);
}

static bool WIIU_AsyncIOTaskNeedsBounceBuffer(const void *buffer)
{
    return ((((uintptr_t) buffer) & (SDL_WIIU_ASYNCIO_ALIGNMENT - 1)) != 0);
}

static void WIIU_DestroyAsyncTaskData(SDL_WiiUAsyncIOTaskData *taskdata)
{
    if (!taskdata) {
        return;
    }
    SDL_aligned_free(taskdata->bounce_buffer);
    if (taskdata->task) {
        taskdata->task->backenddata = NULL;
    }
    SDL_free(taskdata);
}

static void WIIU_FinalizeAsyncTask(SDL_WiiUAsyncIOTaskData *taskdata, FSStatus result)
{
    SDL_AsyncIOTask *task = taskdata->task;

    if (task->type == SDL_ASYNCIO_TASK_CLOSE) {
        const FSStatus effective = (taskdata->staged_result != FS_STATUS_OK) ? taskdata->staged_result : result;
        if (effective == FS_STATUS_OK) {
            task->result = SDL_ASYNCIO_COMPLETE;
        } else if (effective == FS_STATUS_CANCELLED) {
            task->result = SDL_ASYNCIO_CANCELED;
        } else {
            task->result = SDL_ASYNCIO_FAILURE;
        }
    } else if ((task->type == SDL_ASYNCIO_TASK_READ) && (result == FS_STATUS_END)) {
        task->result_size = 0;
        task->result = SDL_ASYNCIO_COMPLETE;
    } else if (result >= 0) {
        task->result_size = (Uint64) result;
        if (task->type == SDL_ASYNCIO_TASK_READ) {
            if (taskdata->bounce_buffer && task->result_size > 0) {
                SDL_memcpy(task->buffer, taskdata->bounce_buffer, (size_t) task->result_size);
            }
            task->result = SDL_ASYNCIO_COMPLETE;
        } else {
            task->result = (task->result_size == task->requested_size) ? SDL_ASYNCIO_COMPLETE : SDL_ASYNCIO_FAILURE;
        }
    } else if (result == FS_STATUS_CANCELLED) {
        task->result = SDL_ASYNCIO_CANCELED;
    } else {
        task->result = SDL_ASYNCIO_FAILURE;
    }

    WIIU_DestroyAsyncTaskData(taskdata);
    WIIU_AsyncIOComplete(task);
}

static void WIIU_AsyncIOCallback(FSClient *client, FSCmdBlock *block, FSStatus result, uint32_t context)
{
    SDL_WiiUAsyncIOTaskData *taskdata = (SDL_WiiUAsyncIOTaskData *) (uintptr_t) context;
    SDL_AsyncIOTask *task = taskdata->task;
    SDL_WiiUAsyncIOData *filedata = taskdata->filedata;
    (void) client;
    (void) block;

    if ((task->type == SDL_ASYNCIO_TASK_CLOSE) && (taskdata->stage == SDL_WIIU_ASYNCIO_STAGE_FLUSH)) {
        FSStatus issue_status;

        if (result != FS_STATUS_OK) {
            taskdata->staged_result = result;
        }

        taskdata->stage = SDL_WIIU_ASYNCIO_STAGE_CLOSE;
        SDL_LockMutex(filedata->lock);
        issue_status = FSCloseFileAsync(&filedata->client,
                                        &taskdata->cmd_block,
                                        filedata->handle,
                                        SDL_WIIU_ASYNCIO_ERROR_MASK,
                                        &taskdata->async_data);
        SDL_UnlockMutex(filedata->lock);
        if (issue_status == FS_STATUS_OK) {
            return;
        }

        if (taskdata->staged_result == FS_STATUS_OK) {
            taskdata->staged_result = issue_status;
        }
        WIIU_FinalizeAsyncTask(taskdata, issue_status);
        return;
    }

    WIIU_FinalizeAsyncTask(taskdata, result);
}

static bool WIIU_PrepareIOTaskBuffer(SDL_WiiUAsyncIOTaskData *taskdata, bool writing)
{
    SDL_AsyncIOTask *task = taskdata->task;

    if (task->requested_size > SDL_SIZE_MAX) {
        return SDL_SetError("Wii U async I/O task exceeds SDL_SIZE_MAX");
    }

    if (!WIIU_AsyncIOTaskNeedsBounceBuffer(task->buffer)) {
        taskdata->io_buffer = (Uint8 *) task->buffer;
        return true;
    }

    taskdata->bounce_buffer = (Uint8 *) SDL_aligned_alloc(SDL_WIIU_ASYNCIO_ALIGNMENT, WIIU_GetAlignedBufferSize(task->requested_size));
    if (!taskdata->bounce_buffer) {
        return false;
    }

    if (writing && task->requested_size > 0) {
        SDL_memcpy(taskdata->bounce_buffer, task->buffer, (size_t) task->requested_size);
    }
    taskdata->io_buffer = taskdata->bounce_buffer;
    return true;
}

static SDL_WiiUAsyncIOTaskData *WIIU_CreateAsyncTaskData(SDL_AsyncIOTask *task)
{
    SDL_WiiUAsyncIOTaskData *taskdata = (SDL_WiiUAsyncIOTaskData *) SDL_calloc(1, sizeof(*taskdata));
    if (!taskdata) {
        return NULL;
    }

    taskdata->task = task;
    taskdata->filedata = (SDL_WiiUAsyncIOData *) task->asyncio->userdata;
    taskdata->staged_result = FS_STATUS_OK;
    taskdata->stage = SDL_WIIU_ASYNCIO_STAGE_IO;
    FSInitCmdBlock(&taskdata->cmd_block);
    SDL_zero(taskdata->async_data);
    taskdata->async_data.callback = WIIU_AsyncIOCallback;
    taskdata->async_data.param = (uint32_t) (uintptr_t) taskdata;
    task->backenddata = taskdata;
    return taskdata;
}

static bool WIIU_FailAsyncTaskData(SDL_WiiUAsyncIOTaskData *taskdata, bool value)
{
    WIIU_DestroyAsyncTaskData(taskdata);
    return value;
}

static bool WIIU_QueueImmediateTask(SDL_AsyncIOTask *task)
{
    task->result = SDL_ASYNCIO_COMPLETE;
    task->result_size = 0;
    WIIU_AsyncIOComplete(task);
    return true;
}

static bool WIIU_IssueReadTask(SDL_WiiUAsyncIOTaskData *taskdata)
{
    SDL_AsyncIOTask *task = taskdata->task;
    SDL_WiiUAsyncIOData *filedata = taskdata->filedata;
    FSStatus status;

    if ((task->offset > UINT32_MAX) || (task->requested_size > UINT32_MAX)) {
        return WIIU_FailAsyncTaskData(taskdata, SDL_SetError("Wii U async read exceeds supported range"));
    }
    if (!WIIU_PrepareIOTaskBuffer(taskdata, false)) {
        return WIIU_FailAsyncTaskData(taskdata, false);
    }

    SDL_LockMutex(filedata->lock);
    /* Cafe FS reports completed byte counts as element counts; size=1 makes those counts match SDL byte counts. */
    status = FSReadFileWithPosAsync(&filedata->client,
                                    &taskdata->cmd_block,
                                    taskdata->io_buffer,
                                    1,
                                    (uint32_t) task->requested_size,
                                    (uint32_t) task->offset,
                                    filedata->handle,
                                    0,
                                    SDL_WIIU_ASYNCIO_ERROR_MASK,
                                    &taskdata->async_data);
    SDL_UnlockMutex(filedata->lock);
    if (status == FS_STATUS_OK) {
        return true;
    }

    return WIIU_FailAsyncTaskData(taskdata, SDL_WIIU_SetFSError("FSReadFileWithPosAsync", status));
}

static bool WIIU_IssueWriteTask(SDL_WiiUAsyncIOTaskData *taskdata)
{
    SDL_AsyncIOTask *task = taskdata->task;
    SDL_WiiUAsyncIOData *filedata = taskdata->filedata;
    FSStatus status;

    if ((task->offset > UINT32_MAX) || (task->requested_size > UINT32_MAX)) {
        return WIIU_FailAsyncTaskData(taskdata, SDL_SetError("Wii U async write exceeds supported range"));
    }
    if (!WIIU_PrepareIOTaskBuffer(taskdata, true)) {
        return WIIU_FailAsyncTaskData(taskdata, false);
    }

    SDL_LockMutex(filedata->lock);
    /* Cafe FS reports completed byte counts as element counts; size=1 makes those counts match SDL byte counts. */
    status = FSWriteFileWithPosAsync(&filedata->client,
                                     &taskdata->cmd_block,
                                     taskdata->io_buffer,
                                     1,
                                     (uint32_t) task->requested_size,
                                     (uint32_t) task->offset,
                                     filedata->handle,
                                     0,
                                     SDL_WIIU_ASYNCIO_ERROR_MASK,
                                     &taskdata->async_data);
    SDL_UnlockMutex(filedata->lock);
    if (status == FS_STATUS_OK) {
        return true;
    }

    return WIIU_FailAsyncTaskData(taskdata, SDL_WIIU_SetFSError("FSWriteFileWithPosAsync", status));
}

static bool WIIU_IssueCloseTask(SDL_WiiUAsyncIOTaskData *taskdata)
{
    SDL_AsyncIOTask *task = taskdata->task;
    SDL_WiiUAsyncIOData *filedata = taskdata->filedata;
    FSStatus status;
    const bool do_flush = task->flush && !filedata->readonly;

    taskdata->stage = do_flush ? SDL_WIIU_ASYNCIO_STAGE_FLUSH : SDL_WIIU_ASYNCIO_STAGE_CLOSE;

    SDL_LockMutex(filedata->lock);
    status = do_flush ? FSFlushFileAsync(&filedata->client,
                                         &taskdata->cmd_block,
                                         filedata->handle,
                                         SDL_WIIU_ASYNCIO_ERROR_MASK,
                                         &taskdata->async_data)
                      : FSCloseFileAsync(&filedata->client,
                                         &taskdata->cmd_block,
                                         filedata->handle,
                                         SDL_WIIU_ASYNCIO_ERROR_MASK,
                                         &taskdata->async_data);
    SDL_UnlockMutex(filedata->lock);
    if (status == FS_STATUS_OK) {
        return true;
    }

    return WIIU_FailAsyncTaskData(taskdata, SDL_WIIU_SetFSError(do_flush ? "FSFlushFileAsync" : "FSCloseFileAsync", status));
}

static Sint64 wiiu_asyncio_size(void *userdata)
{
    SDL_WiiUAsyncIOData *data = (SDL_WiiUAsyncIOData *) userdata;
    FSCmdBlock cmd_block;
    FSStat stat;
    FSStatus status;

    FSInitCmdBlock(&cmd_block);
    SDL_LockMutex(data->lock);
    status = FSGetStatFile(&data->client,
                           &cmd_block,
                           data->handle,
                           &stat,
                           SDL_WIIU_ASYNCIO_ERROR_MASK);
    SDL_UnlockMutex(data->lock);
    if (status != FS_STATUS_OK) {
        SDL_WIIU_SetFSError("FSGetStatFile", status);
        return -1;
    }

    return (Sint64) stat.size;
}

static bool wiiu_asyncio_read(void *userdata, SDL_AsyncIOTask *task)
{
    SDL_WiiUAsyncIOTaskData *taskdata;
    (void) userdata;

    if (task->requested_size == 0) {
        return WIIU_QueueImmediateTask(task);
    }

    taskdata = WIIU_CreateAsyncTaskData(task);
    if (!taskdata) {
        return false;
    }
    return WIIU_IssueReadTask(taskdata);
}

static bool wiiu_asyncio_write(void *userdata, SDL_AsyncIOTask *task)
{
    SDL_WiiUAsyncIOTaskData *taskdata;
    (void) userdata;

    if (task->requested_size == 0) {
        return WIIU_QueueImmediateTask(task);
    }

    taskdata = WIIU_CreateAsyncTaskData(task);
    if (!taskdata) {
        return false;
    }
    return WIIU_IssueWriteTask(taskdata);
}

static bool wiiu_asyncio_close(void *userdata, SDL_AsyncIOTask *task)
{
    SDL_WiiUAsyncIOTaskData *taskdata = WIIU_CreateAsyncTaskData(task);
    (void) userdata;

    if (!taskdata) {
        return false;
    }
    return WIIU_IssueCloseTask(taskdata);
}

static void wiiu_asyncio_destroy(void *userdata)
{
    SDL_WiiUAsyncIOData *data = (SDL_WiiUAsyncIOData *) userdata;
    if (!data) {
        return;
    }

    FSDelClient(&data->client, SDL_WIIU_ASYNCIO_ERROR_MASK);
    SDL_DestroyMutex(data->lock);
    SDL_free(data);
}

static bool wiiu_asyncioqueue_queue_task(void *userdata, SDL_AsyncIOTask *task)
{
    (void) userdata;
    (void) task;
    return true;
}

static void wiiu_asyncioqueue_cancel_task(void *userdata, SDL_AsyncIOTask *task)
{
    SDL_WiiUAsyncIOTaskData *taskdata = (SDL_WiiUAsyncIOTaskData *) task->backenddata;
    SDL_WiiUAsyncIOData *filedata;
    (void) userdata;

    if (!taskdata) {
        return;
    }

    filedata = taskdata->filedata;
    SDL_LockMutex(filedata->lock);
    FSCancelCommand(&filedata->client, &taskdata->cmd_block);
    SDL_UnlockMutex(filedata->lock);
}

static SDL_AsyncIOTask *wiiu_asyncioqueue_get_results(void *userdata)
{
    SDL_WiiUAsyncIOQueueData *queuedata = (SDL_WiiUAsyncIOQueueData *) userdata;
    SDL_AsyncIOTask *task;

    SDL_LockMutex(queuedata->lock);
    task = LINKED_LIST_START(queuedata->completed_tasks, queue);
    if (task) {
        LINKED_LIST_UNLINK(task, queue);
    }
    SDL_UnlockMutex(queuedata->lock);
    return task;
}

static SDL_AsyncIOTask *wiiu_asyncioqueue_wait_results(void *userdata, Sint32 timeoutMS)
{
    SDL_WiiUAsyncIOQueueData *queuedata = (SDL_WiiUAsyncIOQueueData *) userdata;
    SDL_AsyncIOTask *task;

    SDL_LockMutex(queuedata->lock);
    task = LINKED_LIST_START(queuedata->completed_tasks, queue);
    if (!task) {
        SDL_WaitConditionTimeout(queuedata->condition, queuedata->lock, timeoutMS);
        task = LINKED_LIST_START(queuedata->completed_tasks, queue);
    }
    if (task) {
        LINKED_LIST_UNLINK(task, queue);
    }
    SDL_UnlockMutex(queuedata->lock);
    return task;
}

static void wiiu_asyncioqueue_signal(void *userdata)
{
    SDL_WiiUAsyncIOQueueData *queuedata = (SDL_WiiUAsyncIOQueueData *) userdata;
    SDL_LockMutex(queuedata->lock);
    SDL_BroadcastCondition(queuedata->condition);
    SDL_UnlockMutex(queuedata->lock);
}

static void wiiu_asyncioqueue_destroy(void *userdata)
{
    SDL_WiiUAsyncIOQueueData *queuedata = (SDL_WiiUAsyncIOQueueData *) userdata;
    SDL_DestroyMutex(queuedata->lock);
    SDL_DestroyCondition(queuedata->condition);
    SDL_free(queuedata);
}

bool SDL_SYS_CreateAsyncIOQueue(SDL_AsyncIOQueue *queue)
{
    SDL_WiiUAsyncIOQueueData *queuedata;
    static const SDL_AsyncIOQueueInterface SDL_WiiUAsyncIOQueueInterface = {
        wiiu_asyncioqueue_queue_task,
        wiiu_asyncioqueue_cancel_task,
        wiiu_asyncioqueue_get_results,
        wiiu_asyncioqueue_wait_results,
        wiiu_asyncioqueue_signal,
        wiiu_asyncioqueue_destroy
    };

    if (!WIIU_InitializeAsyncIO()) {
        return false;
    }

    queuedata = (SDL_WiiUAsyncIOQueueData *) SDL_calloc(1, sizeof(*queuedata));
    if (!queuedata) {
        return false;
    }

    queuedata->lock = SDL_CreateMutex();
    if (!queuedata->lock) {
        SDL_free(queuedata);
        return false;
    }

    queuedata->condition = SDL_CreateCondition();
    if (!queuedata->condition) {
        SDL_DestroyMutex(queuedata->lock);
        SDL_free(queuedata);
        return false;
    }

    SDL_copyp(&queue->iface, &SDL_WiiUAsyncIOQueueInterface);
    queue->userdata = queuedata;
    return true;
}

bool SDL_SYS_AsyncIOFromFile(const char *file, const char *mode, SDL_AsyncIO *asyncio)
{
    SDL_WiiUAsyncIOData *data;
    FSCmdBlock cmd_block;
    const char *fs_mode = NULL;
    char *resolved = NULL;
    FSStatus status;
    static const SDL_AsyncIOInterface SDL_WiiUAsyncIOInterface = {
        wiiu_asyncio_size,
        wiiu_asyncio_read,
        wiiu_asyncio_write,
        wiiu_asyncio_close,
        wiiu_asyncio_destroy
    };

    if (!WIIU_InitializeAsyncIO()) {
        return false;
    }

    if (SDL_strcmp(mode, "rb") == 0) {
        fs_mode = "r";
    } else if (SDL_strcmp(mode, "wb") == 0) {
        fs_mode = "w";
    } else if (SDL_strcmp(mode, "r+b") == 0) {
        fs_mode = "r+";
    } else if (SDL_strcmp(mode, "w+b") == 0) {
        fs_mode = "w+";
    } else {
        return SDL_SetError("Unsupported Wii U async file mode");
    }

    data = (SDL_WiiUAsyncIOData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        return false;
    }

    data->readonly = asyncio->readonly;
    data->lock = SDL_CreateMutex();
    if (!data->lock) {
        SDL_free(data);
        return false;
    }

    FSInit();
    status = FSAddClient(&data->client, SDL_WIIU_ASYNCIO_ERROR_MASK);
    if (status != FS_STATUS_OK) {
        SDL_DestroyMutex(data->lock);
        SDL_free(data);
        return SDL_WIIU_SetFSError("FSAddClient", status);
    }

    resolved = SDL_WIIU_ResolveFSPath(file);
    if (!resolved) {
        FSDelClient(&data->client, SDL_WIIU_ASYNCIO_ERROR_MASK);
        SDL_DestroyMutex(data->lock);
        SDL_free(data);
        return false;
    }

    FSInitCmdBlock(&cmd_block);
    SDL_LockMutex(data->lock);
    status = FSOpenFile(&data->client,
                        &cmd_block,
                        resolved,
                        fs_mode,
                        &data->handle,
                        SDL_WIIU_ASYNCIO_ERROR_MASK);
    SDL_UnlockMutex(data->lock);
    SDL_free(resolved);
    if (status != FS_STATUS_OK) {
        FSDelClient(&data->client, SDL_WIIU_ASYNCIO_ERROR_MASK);
        SDL_DestroyMutex(data->lock);
        SDL_free(data);
        return SDL_WIIU_SetFSError("FSOpenFile", status);
    }

    SDL_copyp(&asyncio->iface, &SDL_WiiUAsyncIOInterface);
    asyncio->userdata = data;
    return true;
}

void SDL_SYS_QuitAsyncIO(void)
{
    if (SDL_ShouldQuit(&SDL_wiiu_asyncio_init)) {
        SDL_SetInitialized(&SDL_wiiu_asyncio_init, false);
    }
}

#endif // SDL_PLATFORM_WIIU
