#ifndef ARGENTUM_NETWORK_SOCKET_MANAGER_H
#define ARGENTUM_NETWORK_SOCKET_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Configuration ---
#define MAX_EVENTS 1024
#define RECV_BUFFER_SIZE 4096
#define BUFFER_POOL_SIZE 1024

// --- Types ---
typedef struct SocketManager SocketManager;

typedef enum {
    EVENT_CONNECT,
    EVENT_DISCONNECT,
    EVENT_READ,
    EVENT_WRITE
} SocketEventType;

// Callback function pointer for handling socket events
typedef void (*SocketCallback)(intptr_t fd, SocketEventType type, void* data, size_t len, void* context);

// --- Interface ---

/**
 * @brief Creates a new high-performance socket manager (Windows WSAPoll-based).
 */
SocketManager* sm_create(void);

/**
 * @brief Destroys the socket manager.
 */
void sm_destroy(SocketManager* sm);

/**
 * @brief Starts listening on a specific port (TCP).
 * @return 0 on success, -1 on error.
 */
int sm_listen(SocketManager* sm, uint16_t port, SocketCallback callback, void* context);

/**
 * @brief Connects to a remote server.
 */
int sm_connect(SocketManager* sm, const char* ip, uint16_t port, SocketCallback callback, void* context);

/**
 * @brief Runs the event loop. This blocks the calling thread.
 */
void sm_run(SocketManager* sm);

/**
 * @brief Requests the event loop to stop.
 */
void sm_stop(SocketManager* sm);

#ifdef __cplusplus
}
#endif

#endif // ARGENTUM_NETWORK_SOCKET_MANAGER_H
