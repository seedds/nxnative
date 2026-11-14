// Production DNS policy and TLS transport with ephemeral public test certificates.
#include "../nextendo_routes.h"
#include "../net_socket.h"
#include "../ssl_backend.h"
#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
static _Atomic unsigned waits;
#define CHECK(test, message) do { checks++; if (!(test)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); } } while (0)
void dm_guest_host_wait_begin(void) { atomic_fetch_add(&waits, 1); }
void dm_guest_host_wait_end(void) {}
typedef struct { uint8_t length, family; uint16_t port; uint32_t address; uint8_t padding[8]; } Address;
static void resolve_checks(int ready) {
    dm_net_addrinfo hint = {.family = 2, .type = 1, .protocol = 6, .flags = 2}; void *output = (void *)1;
    CHECK(dm_net_getaddrinfo("never-configured.nintendo.net", "443", &hint, &output) == 8 && output == NULL, "unconfigured Nintendo hostname is refused before native DNS");
    CHECK(dm_net_getaddrinfo("unconfigured.example.invalid", "443", &hint, &output) == 8 && output == NULL, "unconfigured third-party hostname is refused too");
    CHECK(dm_net_getaddrinfo("127.0.0.1", "1234", &hint, &output) == 0 && output, "numeric peer resolution survives missing/invalid service configuration");
    if (output) dm_net_freeaddrinfo(output);
    if (!ready) { CHECK(dm_net_getaddrinfo("api.console.invalid", "443", &hint, &output) == 8 && !output, "failed configuration publishes no partial route"); return; }
    CHECK(dm_net_getaddrinfo("API.CONSOLE.INVALID.", "443", &hint, &output) == 0 && output, "exact host matching normalizes DNS case and one final dot");
    if (output) { dm_net_addrinfo *info = output; Address address; memcpy(&address, info->addr, sizeof address);
        CHECK(info->family == 2 && info->type == 1 && address.address == htonl(0x7f000001) && address.port == htons(443) && !strcmp(info->canonname, "API.CONSOLE.INVALID."), "native resolver preserves guest canonical name and translates configured address"); dm_net_freeaddrinfo(output); }
    CHECK(dm_net_getaddrinfo_legacy("api.console.invalid", "443", &hint, &output) == 0 && output, "SDK4 routing shares the same resolver policy");
    if (output) { dm_net_addrinfo_legacy *info = output; CHECK(info->addr && info->canonname && !strcmp(info->canonname, "api.console.invalid"), "SDK4 pointer layout stays correct after canonical-name preservation"); dm_net_freeaddrinfo(output); }
    hint.type = 2; hint.protocol = 17;
    CHECK(dm_net_getaddrinfo("nncs2-%.n.n.srv.nintendo.net", "10025", &hint, &output) == 0 && output,
          "default socket resolver expands a retail NAT template before scoped routing");
    if (output) { Address address; memcpy(&address, ((dm_net_addrinfo *)output)->addr, sizeof address);
        CHECK(address.address == htonl(0x7f000001) && address.port == htons(10025), "NAT template retains its configured address and UDP port"); dm_net_freeaddrinfo(output); }
    CHECK(dm_net_getaddrinfo_legacy("nncs2-%.n.n.srv.nintendo.net", "10125", &hint, &output) == 0 && output,
          "legacy resolver applies the same default NSD expansion");
    if (output) dm_net_freeaddrinfo(output);
    CHECK(dm_net_getaddrinfo("unconfigured-%.example.invalid", "10025", &hint, &output) == 8 && !output,
          "NSD expansion cannot admit an unconfigured service");
    CHECK(dm_net_getaddrinfo("nncs2-%/.n.n.srv.nintendo.net", "10025", &hint, &output) == 8 && !output,
          "malformed service templates fail before native DNS");
    hint.type = 1; hint.protocol = 6;
    hint.flags = 4; CHECK(dm_net_getaddrinfo("api.console.invalid", "443", &hint, &output) == 8 && !output, "AI_NUMERICHOST does not turn a mapped DNS name into a numeric caller input");
    hint.flags = 1; CHECK(dm_net_getaddrinfo(NULL, "0", &hint, &output) == 0 && output, "passive null-host binding never requires a DNS service route"); if (output) dm_net_freeaddrinfo(output);
}
static void tls_checks(int ready, unsigned port, bool reject_pin) {
    char context[32] = {0}, connection[32] = {0}; CHECK(dm_ssl_context_create(context, 32) == 0 && dm_ssl_connection_create(connection, context) == 0, "actual native TLS registry prepared");
    CHECK(dm_ssl_set_hostname(connection, "never-configured.nintendo.net", 29) != 0, "unknown SNI cannot bypass fail-closed DNS through a literal socket");
    CHECK(dm_ssl_set_hostname(connection, "127.0.0.9", 9) != 0, "an unconfigured numeric address does not authorize TLS SNI");
    CHECK(dm_ssl_set_hostname(connection, "nncs2-lp1.n.n.srv.nintendo.net", (unsigned)strlen("nncs2-lp1.n.n.srv.nintendo.net")) != 0, "plaintext NAT route does not silently become a TLS route");
    if (!ready) { CHECK(dm_ssl_set_hostname(connection, "api.console.invalid", 19) != 0, "missing/invalid configuration cannot admit service TLS"); dm_ssl_connection_destroy(connection); dm_ssl_context_destroy(context); return; }
    CHECK(dm_ssl_set_hostname(connection, "api.console.invalid", 19) == 0, "exact TLS route admits its configured verification hostname");
    if (port) {
        dm_net_addrinfo hint = {.family=2,.type=1,.protocol=6}; void *answer = NULL; char service[16]; snprintf(service, sizeof service, "%u", port);
        CHECK(dm_net_getaddrinfo("api.console.invalid", service, &hint, &answer) == 0 && answer, "actual TLS socket obtains the routed resolver address");
        int fd = dm_net_socket(2, 1, 6); dm_net_addrinfo *info = answer;
        CHECK(info && fd >= 0 && dm_net_connect(fd, info->addr, info->addrlen) == 0 && dm_ssl_set_socket(connection, fd) == 0, "guest TCP descriptor is connected and borrowed safely"); if (answer) dm_net_freeaddrinfo(answer);
        if (reject_pin) {
            CHECK(dm_ssl_handshake(connection) != 0, "a different enrolled certificate rejects the actual TLS peer");
            int sent = 99; uint32_t verification = 0;
            CHECK(dm_ssl_last_error(connection, &verification, true) == 0 && verification &&
                  dm_ssl_write(connection, "PING", &sent, 4) != 0 && sent == 0,
                  "failed pin verification retains its error and sends no application data");
            dm_ssl_connection_destroy(connection); dm_ssl_context_destroy(context); return;
        }
        CHECK(dm_ssl_handshake(connection) == 0, "mapped SNI and only its scoped DER CA complete a real TLS handshake");
        char bytes[8] = {0}; int n = 0; CHECK(dm_ssl_read(connection, bytes, &n, 5, false) == 0 && n == 5 && !memcmp(bytes, "HELLO", 5), "routed TLS receives actual server plaintext");
        CHECK(dm_ssl_write(connection, "PING", &n, 4) == 0 && n == 4 && dm_ssl_read(connection, bytes, &n, 4, false) == 0 && n == 4 && !memcmp(bytes, "PONG", 4), "routed TLS carries actual bidirectional guest data");
    }
    dm_ssl_connection_destroy(connection); dm_ssl_context_destroy(context);
    if (port) {
        CHECK(dm_ssl_context_create(context, 32) == 0 && dm_ssl_connection_create(connection, context) == 0 && dm_ssl_set_hostname(connection, "wrong.console.invalid", 21) == 0, "negative mapped-host test uses a valid exact policy record");
        int fd = dm_net_socket(2,1,6); Address address = {16,2,htons((uint16_t)port),htonl(0x7f000001),{0}};
        CHECK(fd >= 0 && dm_net_connect(fd,&address,sizeof address) == 0 && dm_ssl_set_socket(connection,fd) == 0 && dm_ssl_handshake(connection) != 0, "a scoped trusted CA never bypasses mapped hostname validation");
        dm_ssl_connection_destroy(connection); dm_ssl_context_destroy(context);
    }
}
static void handoff_checks(unsigned port) {
    char context[32] = {0}, connection[32] = {0};
    CHECK(dm_ssl_context_create(context, 32) == 0 && dm_ssl_connection_create(connection, context) == 0,
          "numeric handoff uses an actual independent TLS connection");
    CHECK(dm_ssl_set_hostname(connection, "127.0.0.1", 9) == 0,
          "an explicitly paired numeric alias retains its enrolled service identity");
    int fd = dm_net_socket(2, 1, 6); Address address = {16, 2, htons((uint16_t)port), htonl(0x7f000001), {0}};
    CHECK(fd >= 0 && dm_net_connect(fd, &address, sizeof address) == 0 && dm_ssl_set_socket(connection, fd) == 0 && dm_ssl_handshake(connection) == 0,
          "numeric handoff verifies the exact enrolled certificate with canonical service SNI");
    char bytes[8] = {0}; int size = 0;
    CHECK(dm_ssl_read(connection, bytes, &size, 5, false) == 0 && size == 5 && !memcmp(bytes, "HELLO", 5) &&
          dm_ssl_write(connection, "PING", &size, 4) == 0 && size == 4 &&
          dm_ssl_read(connection, bytes, &size, 4, false) == 0 && size == 4 && !memcmp(bytes, "PONG", 4),
          "the paired address carries real bidirectional encrypted traffic");
    dm_ssl_connection_destroy(connection); dm_ssl_context_destroy(context);
}
static void v2_checks(void) {
    dm_net_addrinfo hint = {.family=2,.type=1,.protocol=6}; void *out = NULL;
    const char *names[] = {"deep.scope.invalid", "scope.invalid", "nat2.scope.invalid", "nat3.scope.invalid", "special.scope.invalid"};
    const uint32_t ips[] = {0x7f000002,0x7f000002,0x7f000003,0x7f000003,0x7f000004};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        CHECK(dm_net_getaddrinfo(names[i], "443", &hint, &out) == 0 && out, "V2 exact, suffix and label routes resolve");
        if (out) { Address address; memcpy(&address, ((dm_net_addrinfo *)out)->addr, sizeof address);
            CHECK(address.address == htonl(ips[i]), "V2 precedence is exact before label before longest suffix"); dm_net_freeaddrinfo(out); }
    }
    CHECK(dm_net_getaddrinfo("evilscope.invalid", "443", &hint, &out) == 8 && !out, "suffix matching respects a DNS label boundary");
    CHECK(dm_nextendo_routes_retry_unconfigured() == 0 && dm_nextendo_routes_status() == 0, "explicit setup retry cannot replace a published policy");
}
int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) return 2; setenv("DM_NETWORK", "nextendo", 1); if (strcmp(argv[1], "-")) setenv("DM_NEXTENDO_ROUTES", argv[1], 1); else unsetenv("DM_NEXTENDO_ROUTES");
    int expected = atoi(argv[2]), status = dm_nextendo_routes_initialize(); CHECK(status == expected && dm_nextendo_routes_status() == expected, "configuration reports its precise safe status");
    if (argc == 4 && argv[3][0] == '/') {
        setenv("DM_NEXTENDO_ROUTES", argv[3], 1);
        CHECK(dm_nextendo_routes_initialize() == expected, "a failed policy remains immutable without explicit setup");
        CHECK(dm_nextendo_routes_retry_unconfigured() == expected, "explicit setup reports the previous failed state");
        CHECK(dm_nextendo_routes_initialize() == 0 && dm_nextendo_routes_count() > 0, "explicit setup recovers after a complete profile is supplied");
        printf("Nextendo routes: %d checks, %d failures (recovery)\n",checks,failures); return failures ? 1 : 0;
    }
    CHECK((status == 0) == (dm_nextendo_routes_count() > 0), "only a fully validated table is published");
    setenv("DM_NEXTENDO_ROUTES", "/unconfigured/should-not-reload.tsv", 1); CHECK(dm_nextendo_routes_initialize() == expected, "routing configuration is immutable until the next process launch");
    CHECK(dm_bcat_online_enabled() == (status == 0), "BCAT capability requires an opted-in, fully published profile");
    resolve_checks(status == 0);
    setenv("DM_NETWORK", "auto", 1); resolve_checks(status == 0);
    tls_checks(status == 0, argc >= 4 ? (unsigned)strtoul(argv[3],NULL,10) : 0, argc == 5 && !strcmp(argv[4], "reject-pin"));
    if (argc == 5 && !strcmp(argv[4], "v2")) v2_checks();
    if (argc == 5 && !strcmp(argv[4], "handoff")) handoff_checks((unsigned)strtoul(argv[3], NULL, 10));
    unsetenv("DM_NEXTENDO_ROUTES"); setenv("DM_NETWORK", "auto", 1);
    dm_net_addrinfo local_hint = {.family=2,.type=1}; void *local_answer = NULL;
    CHECK(!dm_net_nextendo_enabled() && !dm_bcat_online_enabled() && !dm_net_getaddrinfo("localhost","0",&local_hint,&local_answer) && local_answer,
          "unconfigured AUTO retains ordinary local DNS and does not enable online services");
    if (local_answer) dm_net_freeaddrinfo(local_answer);
    setenv("DM_NEXTENDO_ROUTES", "invalid-relative-profile", 1);
    CHECK(dm_net_nextendo_enabled(), "an explicitly supplied invalid profile does not silently fall back to unrestricted AUTO");
    setenv("DM_NETWORK", "lan", 1); CHECK(!dm_bcat_online_enabled(), "BCAT capability stays off in local mode without any account read"); dm_net_addrinfo hint = {.family=2,.type=1}; void *answer = NULL; CHECK(dm_net_getaddrinfo("localhost", "0", &hint, &answer) == 0, "LAN resolver behavior remains independent of Nextendo policy"); if (answer) dm_net_freeaddrinfo(answer);
    printf("Nextendo routes: %d checks, %d failures (status %d)\n", checks, failures, status); return failures ? 1 : 0;
}
