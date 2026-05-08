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

#include "SDL_wiiulog.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SDL_WIIU_LOG_SERVER_PORT 4405
#define SDL_WIIU_MAX_PENDING_LOG_MESSAGES 128

typedef struct SDL_WiiULogMessage
{
    struct SDL_WiiULogMessage *next;
    char *message;
} SDL_WiiULogMessage;

static int SDL_wiiu_log_socket = -1;
static struct sockaddr_in SDL_wiiu_log_send_addr;
static SDL_SpinLock SDL_wiiu_log_lock;
static SDL_WiiULogMessage *SDL_wiiu_log_messages;
static SDL_WiiULogMessage *SDL_wiiu_log_messages_tail;
static Uint32 SDL_wiiu_log_message_count;
static Uint32 SDL_wiiu_log_dropped_messages;

static void SDL_WIIU_FreeLogMessage(SDL_WiiULogMessage *entry)
{
    if (!entry) {
        return;
    }

    SDL_free(entry->message);
    SDL_free(entry);
}

static bool SDL_WIIU_SendRawMessage(const char *message)
{
    char *packet;
    bool isstack;
    size_t length;

    if (!message || !*message) {
        return true;
    }

    length = SDL_strlen(message);
    packet = SDL_small_alloc(char, length + 2, &isstack);
    if (!packet) {
        return false;
    }

    SDL_memcpy(packet, message, length);
    packet[length] = '\n';
    packet[length + 1] = '\0';

    if (sendto(SDL_wiiu_log_socket,
               packet,
               length + 1,
               0,
               (const struct sockaddr *)&SDL_wiiu_log_send_addr,
               sizeof(SDL_wiiu_log_send_addr)) < 0) {
        SDL_small_free(packet, isstack);
        return false;
    }

    SDL_small_free(packet, isstack);
    return true;
}

static bool SDL_WIIU_QueueLogMessage(const char *message)
{
    SDL_WiiULogMessage *entry;

    entry = (SDL_WiiULogMessage *)SDL_malloc(sizeof(*entry));
    if (!entry) {
        SDL_LockSpinlock(&SDL_wiiu_log_lock);
        ++SDL_wiiu_log_dropped_messages;
        SDL_UnlockSpinlock(&SDL_wiiu_log_lock);
        return false;
    }

    entry->message = SDL_strdup(message);
    if (!entry->message) {
        SDL_free(entry);
        SDL_LockSpinlock(&SDL_wiiu_log_lock);
        ++SDL_wiiu_log_dropped_messages;
        SDL_UnlockSpinlock(&SDL_wiiu_log_lock);
        return false;
    }
    entry->next = NULL;

    SDL_LockSpinlock(&SDL_wiiu_log_lock);
    if (SDL_wiiu_log_message_count >= SDL_WIIU_MAX_PENDING_LOG_MESSAGES) {
        ++SDL_wiiu_log_dropped_messages;
        SDL_UnlockSpinlock(&SDL_wiiu_log_lock);
        SDL_WIIU_FreeLogMessage(entry);
        return false;
    }

    if (SDL_wiiu_log_messages_tail) {
        SDL_wiiu_log_messages_tail->next = entry;
    } else {
        SDL_wiiu_log_messages = entry;
    }
    SDL_wiiu_log_messages_tail = entry;
    ++SDL_wiiu_log_message_count;
    SDL_UnlockSpinlock(&SDL_wiiu_log_lock);
    return true;
}

static void SDL_WIIU_RequeueLogMessage(SDL_WiiULogMessage *entry)
{
    SDL_LockSpinlock(&SDL_wiiu_log_lock);
    entry->next = SDL_wiiu_log_messages;
    SDL_wiiu_log_messages = entry;
    if (!SDL_wiiu_log_messages_tail) {
        SDL_wiiu_log_messages_tail = entry;
    }
    ++SDL_wiiu_log_message_count;
    SDL_UnlockSpinlock(&SDL_wiiu_log_lock);
}

static bool SDL_WIIU_FlushPendingLogs(void)
{
    if (!SDL_WIIU_InitLog()) {
        return false;
    }

    for (;;) {
        SDL_WiiULogMessage *entry = NULL;
        Uint32 dropped_messages = 0;

        SDL_LockSpinlock(&SDL_wiiu_log_lock);
        if (SDL_wiiu_log_dropped_messages > 0) {
            dropped_messages = SDL_wiiu_log_dropped_messages;
            SDL_wiiu_log_dropped_messages = 0;
        } else if (SDL_wiiu_log_messages) {
            entry = SDL_wiiu_log_messages;
            SDL_wiiu_log_messages = entry->next;
            if (!SDL_wiiu_log_messages) {
                SDL_wiiu_log_messages_tail = NULL;
            }
            --SDL_wiiu_log_message_count;
            entry->next = NULL;
        }
        SDL_UnlockSpinlock(&SDL_wiiu_log_lock);

        if (dropped_messages > 0) {
            char warning[128];

            (void)SDL_snprintf(warning, sizeof(warning),
                               "[WARN][SYSTEM] Dropped %u SDL log messages while waiting for Wii U UDP logging",
                               (unsigned int)dropped_messages);
            if (!SDL_WIIU_SendRawMessage(warning)) {
                SDL_LockSpinlock(&SDL_wiiu_log_lock);
                SDL_wiiu_log_dropped_messages += dropped_messages;
                SDL_UnlockSpinlock(&SDL_wiiu_log_lock);
                return false;
            }
            continue;
        }

        if (!entry) {
            return true;
        }

        if (!SDL_WIIU_SendRawMessage(entry->message)) {
            SDL_WIIU_RequeueLogMessage(entry);
            return false;
        }

        SDL_WIIU_FreeLogMessage(entry);
    }
}

bool SDL_WIIU_InitLog(void)
{
    int broadcast_enable = 1;

    if (SDL_wiiu_log_socket >= 0) {
        return true;
    }

    SDL_wiiu_log_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (SDL_wiiu_log_socket < 0) {
        return false;
    }

    (void)setsockopt(SDL_wiiu_log_socket, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    SDL_zero(SDL_wiiu_log_send_addr);
    SDL_wiiu_log_send_addr.sin_family = AF_INET;
    SDL_wiiu_log_send_addr.sin_port = htons(SDL_WIIU_LOG_SERVER_PORT);
    SDL_wiiu_log_send_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    return true;
}

void SDL_WIIU_QuitLog(void)
{
    SDL_WiiULogMessage *entry;

    (void)SDL_WIIU_FlushPendingLogs();

    SDL_LockSpinlock(&SDL_wiiu_log_lock);
    entry = SDL_wiiu_log_messages;
    SDL_wiiu_log_messages = NULL;
    SDL_wiiu_log_messages_tail = NULL;
    SDL_wiiu_log_message_count = 0;
    SDL_wiiu_log_dropped_messages = 0;
    SDL_UnlockSpinlock(&SDL_wiiu_log_lock);

    while (entry) {
        SDL_WiiULogMessage *next = entry->next;
        SDL_WIIU_FreeLogMessage(entry);
        entry = next;
    }

    if (SDL_wiiu_log_socket >= 0) {
        shutdown(SDL_wiiu_log_socket, SHUT_WR);
        close(SDL_wiiu_log_socket);
        SDL_wiiu_log_socket = -1;
    }
}

void SDL_WIIU_LogOutput(const char *message)
{
    if (!message || !*message) {
        return;
    }

    if (!SDL_WIIU_FlushPendingLogs()) {
        (void)SDL_WIIU_QueueLogMessage(message);
        return;
    }

    if (!SDL_WIIU_SendRawMessage(message)) {
        (void)SDL_WIIU_QueueLogMessage(message);
    }
}

void SDL_WIIU_PumpLog(void)
{
    (void)SDL_WIIU_FlushPendingLogs();
}

#endif // SDL_PLATFORM_WIIU
