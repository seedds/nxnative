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

// Returns the 4-digit pairing PIN as a null-terminated string (e.g. "4819").
const char *dm_webpad_pin(void);

// Sets a custom 4-digit pairing PIN. Pass NULL to generate a random PIN.
void dm_webpad_set_pin(const char *pin);

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

// Callback signature for host UI notifications (e.g. PIN request overlay)
// Pass pin = NULL when no clients are pending authentication to dismiss the overlay
typedef void (*dm_webpad_pin_prompt_cb)(const char *pin, const char *client_ip);

// Sets the callback to be notified when an unauthenticated client needs pairing
void dm_webpad_set_pin_prompt_cb(dm_webpad_pin_prompt_cb cb);

// Triggered by the server when a client connects and needs authentication
void dm_webpad_trigger_pin_prompt(const char *client_ip);

// Checks if any client is currently pending authentication, and dismisses overlay if none
void dm_webpad_check_pending_auth(void);

#ifdef __cplusplus
}
#endif
