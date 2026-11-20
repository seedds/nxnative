// The Nextendo provider as it is offline, for the host unit tests that compile a service file without the network code (t_friends, t_account, t_account_save):
// DM_NETWORK is offline, so no Nextendo call is reached, and each fails if one is. Weak, so a test's own definition wins.
#include "../hle.h"
#include "../net_socket.h"
#include "../nextendo_account.h"
#include "../nextendo_social.h"
#include "../friends_history.h"
__attribute__((weak)) dm_net_mode_t dm_net_mode(void) { return DM_NET_OFFLINE; }
__attribute__((weak)) uint64_t hle_system_event_binding(void *g) { (void)g; return 0; }
__attribute__((weak)) void *guest_sym(const char *name) { (void)name; return NULL; }
__attribute__((weak)) void dm_hle_wait_enter(void) { abort(); }   // the HLE_FORCE_WAIT gate's entry (entry.S); no wait hook is called here
__attribute__((weak)) uint32_t dm_friends_history_key(void *out, const void *uid, unsigned flag) { (void)out; (void)uid; (void)flag; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_async_attach(void *o, uint64_t l, uint64_t e) { (void)o; (void)l; (void)e; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_async_cancel(void *o) { (void)o; return RES_FAIL; }
__attribute__((weak)) void dm_nextendo_async_construct(void *o) { (void)o; }
__attribute__((weak)) void dm_nextendo_async_destroy(void *o) { (void)o; }
__attribute__((weak)) uint32_t dm_nextendo_async_done(void *o, uint8_t *out) { (void)o; (void)out; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_async_execute(void *o, dm_nextendo_async_operation op, const void *c) { (void)o; (void)op; (void)c; return RES_FAIL; }
__attribute__((weak)) uint64_t dm_nextendo_async_lifetime(void *o) { (void)o; return 0; }
__attribute__((weak)) void *dm_nextendo_async_move(void *o, void *s) { (void)s; return o; }
__attribute__((weak)) uint32_t dm_nextendo_async_result(void *o) { (void)o; return RES_FAIL; }
__attribute__((weak)) void dm_nextendo_async_swap(void *o, void *s) { (void)o; (void)s; }
__attribute__((weak)) bool dm_nextendo_async_valid(void *o) { (void)o; return false; }
__attribute__((weak)) uint32_t dm_nextendo_friends_available(uint8_t *a, const void *u) { (void)a; (void)u; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_friends_list(int *n, void *o, const void *u, int off, int cap, const void *f, bool ids) { (void)n; (void)o; (void)u; (void)off; (void)cap; (void)f; (void)ids; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_friends_update(void *o, const void *u, const uint64_t *ids, int n) { (void)o; (void)u; (void)ids; (void)n; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_friends_ensure(const void *u) { (void)u; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_presence_commit(const void *o) { (void)o; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_presence_describe(void *o, const char *d) { (void)o; (void)d; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_presence_initialize(void *o, const void *u) { (void)o; (void)u; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_social_publish(unsigned st, const uint8_t *f, size_t n) { (void)st; (void)f; (void)n; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_account_available(uint8_t *out) { if (!out) return RES_FAIL; *out = 0; return RES_OK; }   // as hle_nextendo_apple.m answers offline
__attribute__((weak)) uint32_t dm_nextendo_account_birthday(char output[11]) { (void)output; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_account_country(char output[3]) { (void)output; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_account_ensure(void) { return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_account_id(uint64_t *out) { (void)out; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_account_load_token(uint64_t *size, char *buffer, uint64_t capacity) { (void)size; (void)buffer; (void)capacity; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_account_username(char output[33]) { (void)output; return RES_FAIL; }
__attribute__((weak)) uint32_t dm_nextendo_async_ensure(void *o) { (void)o; return RES_FAIL; }
__attribute__((weak)) void dm_guest_host_wait_begin(void) {}
__attribute__((weak)) void dm_guest_host_wait_end(void) {}
__attribute__((weak)) void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
__attribute__((weak)) void hle_signal_system_event(void *g) { (void)g; }
__attribute__((weak)) int dm_account_uid_exists(const void *uid) { (void)uid; return 0; }
