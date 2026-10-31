// dm_webpad.h: Clean-room embedded HTTP + WebSocket gamepad server for local Wi-Fi.
// MIT License.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DM_WEBPAD_DEFAULT_PORT 2904

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 if web browser pads are enabled (DM_BROWSER_PADS=1 or dm_webpad_enable(1)).
int dm_webpad_enabled(void);

// Explicitly enable or disable webpad server before or during startup.
void dm_webpad_enable(int on);

// Starts the webpad TCP listener thread and Bonjour/mDNS service advertisement (_nxpad._tcp).
// Safe to call multiple times (idempotent).
void dm_webpad_start(void);

// Stops the webpad listener and disconnects clients.
void dm_webpad_stop(void);

// Synchronizes the webpad subsystem with the current slot mask.
void dm_webpad_poll(void);

// Returns active webpad port (e.g. 2904).
uint16_t dm_webpad_port(void);

// Copies primary local IPv4 address into buf (e.g. "192.168.1.38"), returns 0 on success.
int dm_webpad_get_ip(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
