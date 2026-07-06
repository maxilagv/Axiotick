#include "network/socket_manager.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#error "SocketManager currently supports Windows only."
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define POLL_TIMEOUT_MS 5
#define READ_EVENTS (POLLIN | POLLRDNORM)

typedef struct {
    SOCKET socket;
    SocketCallback callback;
    void* context;
    bool is_listener;
    bool is_connecting;
} SocketEntry;

typedef struct {
    uint8_t* block;
    uint8_t* free_list[BUFFER_POOL_SIZE];
    size_t free_count;
    SRWLOCK lock;
} BufferPool;

struct SocketManager {
    WSAPOLLFD fds[MAX_EVENTS];
    SocketEntry entries[MAX_EVENTS];
    size_t count;
    volatile LONG running;
    BufferPool pool;
};

static void pool_init(BufferPool* pool) {
    pool->block = (uint8_t*)malloc(RECV_BUFFER_SIZE * BUFFER_POOL_SIZE);
    pool->free_count = 0;
    InitializeSRWLock(&pool->lock);
    if (!pool->block) return;

    for (size_t i = 0; i < BUFFER_POOL_SIZE; ++i) {
        pool->free_list[pool->free_count++] = pool->block + (i * RECV_BUFFER_SIZE);
    }
}

static void pool_destroy(BufferPool* pool) {
    if (pool->block) {
        free(pool->block);
        pool->block = NULL;
        pool->free_count = 0;
    }
}

static uint8_t* pool_acquire(BufferPool* pool) {
    uint8_t* buf = NULL;
    AcquireSRWLockExclusive(&pool->lock);
    if (pool->free_count > 0) {
        buf = pool->free_list[--pool->free_count];
    }
    ReleaseSRWLockExclusive(&pool->lock);
    return buf;
}

static void pool_release(BufferPool* pool, uint8_t* buf) {
    if (!buf) return;
    AcquireSRWLockExclusive(&pool->lock);
    if (pool->free_count < BUFFER_POOL_SIZE) {
        pool->free_list[pool->free_count++] = buf;
    }
    ReleaseSRWLockExclusive(&pool->lock);
}

static void sm_remove_at(SocketManager* sm, size_t index) {
    if (index >= sm->count) return;

    closesocket(sm->entries[index].socket);

    size_t last = sm->count - 1;
    if (index != last) {
        sm->fds[index] = sm->fds[last];
        sm->entries[index] = sm->entries[last];
    }
    sm->count--;
}

static int set_nonblocking(SOCKET s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode);
}

SocketManager* sm_create(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return NULL;
    }

    SocketManager* sm = (SocketManager*)calloc(1, sizeof(SocketManager));
    if (!sm) {
        WSACleanup();
        return NULL;
    }

    sm->running = 0;
    sm->count = 0;
    pool_init(&sm->pool);
    return sm;
}

void sm_destroy(SocketManager* sm) {
    if (!sm) return;
    for (size_t i = 0; i < sm->count; ++i) {
        closesocket(sm->entries[i].socket);
    }
    pool_destroy(&sm->pool);
    free(sm);
    WSACleanup();
}

int sm_listen(SocketManager* sm, uint16_t port, SocketCallback callback, void* context) {
    if (!sm || sm->count >= MAX_EVENTS) return -1;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    int exclusive = 1;
#ifdef SO_EXCLUSIVEADDRUSE
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
#endif
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    if (set_nonblocking(s) != 0) {
        closesocket(s);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }

    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }

    size_t idx = sm->count++;
    sm->fds[idx].fd = s;
    sm->fds[idx].events = READ_EVENTS | POLLERR;
    sm->fds[idx].revents = 0;
    sm->entries[idx].socket = s;
    sm->entries[idx].callback = callback;
    sm->entries[idx].context = context;
    sm->entries[idx].is_listener = true;
    sm->entries[idx].is_connecting = false;

    return 0;
}

int sm_connect(SocketManager* sm, const char* ip, uint16_t port, SocketCallback callback, void* context) {
    if (!sm || sm->count >= MAX_EVENTS) return -1;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    if (set_nonblocking(s) != 0) {
        closesocket(s);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        closesocket(s);
        return -1;
    }

    int rc = connect(s, (struct sockaddr*)&addr, sizeof(addr));
    bool connecting = false;
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            connecting = true;
        } else {
            closesocket(s);
            return -1;
        }
    }

    size_t idx = sm->count++;
    sm->fds[idx].fd = s;
    sm->fds[idx].events = READ_EVENTS | POLLWRNORM | POLLERR;
    sm->fds[idx].revents = 0;
    sm->entries[idx].socket = s;
    sm->entries[idx].callback = callback;
    sm->entries[idx].context = context;
    sm->entries[idx].is_listener = false;
    sm->entries[idx].is_connecting = connecting;

    if (!connecting && callback) {
        callback((intptr_t)s, EVENT_CONNECT, NULL, 0, context);
    }

    return 0;
}

static void handle_disconnect(SocketManager* sm, size_t index) {
    SocketEntry* entry = &sm->entries[index];
    if (entry->callback) {
        entry->callback((intptr_t)entry->socket, EVENT_DISCONNECT, NULL, 0, entry->context);
    }
    sm_remove_at(sm, index);
}

void sm_run(SocketManager* sm) {
    if (!sm) return;
    InterlockedExchange((volatile LONG*)&sm->running, 1);

    while (InterlockedCompareExchange((volatile LONG*)&sm->running, 1, 1) == 1) {
        if (sm->count == 0) {
            Sleep(POLL_TIMEOUT_MS);
            continue;
        }
        int ready = WSAPoll(sm->fds, (ULONG)sm->count, POLL_TIMEOUT_MS);
        if (ready == SOCKET_ERROR) {
            continue;
        }
        if (ready == 0) {
            continue;
        }

        for (size_t i = 0; i < sm->count; ++i) {
            if (sm->fds[i].revents == 0) continue;

            short revents = sm->fds[i].revents;
            sm->fds[i].revents = 0;

            SocketEntry* entry = &sm->entries[i];

            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                handle_disconnect(sm, i);
                i--;
                continue;
            }

            if (entry->is_listener && (revents & READ_EVENTS)) {
                for (;;) {
                    SOCKET client = accept(entry->socket, NULL, NULL);
                    if (client == INVALID_SOCKET) {
                        int err = WSAGetLastError();
                        if (err == WSAEWOULDBLOCK) break;
                        handle_disconnect(sm, i);
                        break;
                    }

                    if (set_nonblocking(client) != 0) {
                        closesocket(client);
                        continue;
                    }

                    if (sm->count >= MAX_EVENTS) {
                        closesocket(client);
                        continue;
                    }

                    size_t idx = sm->count++;
                    sm->fds[idx].fd = client;
                    sm->fds[idx].events = READ_EVENTS | POLLWRNORM | POLLERR;
                    sm->fds[idx].revents = 0;
                    sm->entries[idx].socket = client;
                    sm->entries[idx].callback = entry->callback;
                    sm->entries[idx].context = entry->context;
                    sm->entries[idx].is_listener = false;
                    sm->entries[idx].is_connecting = false;

                    if (entry->callback) {
                        entry->callback((intptr_t)client, EVENT_CONNECT, NULL, 0, entry->context);
                    }
                }
                continue;
            }

            if (entry->is_connecting && (revents & POLLWRNORM)) {
                int err = 0;
                int len = sizeof(err);
                if (getsockopt(entry->socket, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0) {
                    entry->is_connecting = false;
                    if (entry->callback) {
                        entry->callback((intptr_t)entry->socket, EVENT_CONNECT, NULL, 0, entry->context);
                    }
                } else {
                    handle_disconnect(sm, i);
                    i--;
                    continue;
                }
            }

            if (revents & READ_EVENTS) {
                uint8_t* buffer = pool_acquire(&sm->pool);
                uint8_t stack_buffer[RECV_BUFFER_SIZE];
                uint8_t* target = buffer ? buffer : stack_buffer;

                int received = recv(entry->socket, (char*)target, RECV_BUFFER_SIZE, 0);
                if (received > 0) {
                    if (entry->callback) {
                        entry->callback((intptr_t)entry->socket, EVENT_READ, target, (size_t)received, entry->context);
                    }
                } else if (received == 0) {
                    handle_disconnect(sm, i);
                    if (buffer) pool_release(&sm->pool, buffer);
                    i--;
                    continue;
                } else {
                    int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK) {
                        handle_disconnect(sm, i);
                        if (buffer) pool_release(&sm->pool, buffer);
                        i--;
                        continue;
                    }
                }

                if (buffer) pool_release(&sm->pool, buffer);
            }
        }
    }
}

void sm_stop(SocketManager* sm) {
    if (!sm) return;
    InterlockedExchange((volatile LONG*)&sm->running, 0);
}
