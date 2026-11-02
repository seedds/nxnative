// Opt-in, bounded TLS state metadata. Never pass names, pointers or stream data.
#pragma once
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    DM_TLS_HOSTNAME, DM_TLS_SOCKET, DM_TLS_VALUE, DM_TLS_OPTION,
    DM_TLS_PREPARE, DM_TLS_VERIFY, DM_TLS_HANDSHAKE, DM_TLS_CERTIFICATES,
    DM_TLS_READ, DM_TLS_WRITE
} dm_tls_stage;

static inline void dm_tls_trace(dm_tls_stage stage, uint64_t session, uint32_t result,
                                int32_t native, uint64_t detail, unsigned flags) {
    int saved = errno;
    const char *flag = getenv("DM_SSL_TRACE");
    static _Atomic unsigned events;
    if (flag && !strcmp(flag, "1")) {
        unsigned used = atomic_load(&events);
        while (used < 128) {
            if (atomic_compare_exchange_weak(&events, &used, used + 1)) {
                fprintf(stderr, "[ssl-trace] stage=%u session=%" PRIu64 " result=0x%08" PRIx32
                        " native=%" PRId32 " detail=%" PRIu64 " flags=0x%02x\n",
                        (unsigned)stage, session, result, native, detail, flags);
                break;
            }
        }
    }
    errno = saved;
}
