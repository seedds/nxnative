// Pure, synthetic protocol fixtures: no sockets, LAN, game files or devices.
// This checks eligibility for supplemental routing, not reachability or replies.
#include "../discovery_protocol.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); abort(); } } while (0)

enum { PIA_PORT = 30000, LDN_PORT = 11452, PIA_OVERHEAD = 0x12f, PIA_MAX = 1364 };
static const unsigned char ldn_query[12] = {0, 0x14, 0x45, 0x11, 0, 0, 0, 0, 0, 0, 0, 0};

static void be32(unsigned char *p, uint32_t n) {
    p[0] = (unsigned char)(n >> 24); p[1] = (unsigned char)(n >> 16);
    p[2] = (unsigned char)(n >> 8); p[3] = (unsigned char)n;
}

// The bodies deliberately contain arbitrary data. Only the verified query
// envelope determines eligibility; no title ID, payload magic or reply nonce
// may be inferred here. The counter belongs to the client sending the query.
static size_t pia_query(unsigned char *p, unsigned payload, unsigned flags) {
    size_t size = payload + PIA_OVERHEAD;
    for (size_t i = 0; i < size; ++i) p[i] = (unsigned char)(i * 197u + payload * 17u);
    p[0] = 0; be32(p + 1, payload);
    p[payload + 5] = 1; p[payload + 6] = (unsigned char)flags;
    memset(p + payload + 7, 0, 8); p[payload + 14] = 1;
    return size;
}

static void valid_pia_lengths(void) {
    unsigned char packet[PIA_MAX + 1];
    // Both sides of the 255/256 boundary catch a little-endian length decoder;
    // 1061 bytes is the largest payload whose complete envelope fits in 1364.
    for (unsigned payload = 0; payload <= PIA_MAX - PIA_OVERHEAD; ++payload) {
        for (unsigned flags = 0; flags <= 1; ++flags) {
            size_t size = pia_query(packet, payload, flags);
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == DM_DISCOVERY_PIA);
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size + 1) == 0);
        }
    }
    size_t size = pia_query(packet, 0, 0);
    for (unsigned byte = 0; byte < 8; ++byte) {
        const unsigned values[] = {1, 0x80, 0xff};
        for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
            memset(packet + 7, 0, 8); packet[7 + byte] = (unsigned char)values[i];
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == DM_DISCOVERY_PIA);
        }
    }
    memset(packet + 7, 0, 8);
    CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
}

static void malformed_pia(void) {
    unsigned char packet[PIA_MAX + 1];
    const unsigned payloads[] = {0, 1, 2, 255, 256, 1060, 1061};
    for (unsigned i = 0; i < sizeof payloads / sizeof payloads[0]; ++i) {
        unsigned payload = payloads[i];
        size_t size = pia_query(packet, payload, 0);
        // Test every truncation with an allocation ending at the advertised
        // size so ASan can catch reads into an incomplete header or footer.
        for (size_t cut = 0; cut < size; ++cut) {
            unsigned char *short_packet = malloc(cut ? cut : 1); CHECK(short_packet != NULL);
            memcpy(short_packet, packet, cut);
            CHECK(dm_discovery_query_protocol(PIA_PORT, short_packet, cut) == 0);
            free(short_packet);
        }
        for (unsigned type = 1; type <= 255; ++type) {
            packet[0] = (unsigned char)type;
            // Type 1 is a response, even if all other fields match a query.
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
        }
        packet[0] = 0;
        for (unsigned version = 0; version <= 255; ++version) {
            if (version == 1) continue;
            packet[payload + 5] = (unsigned char)version;
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
        }
        packet[payload + 5] = 1;
        for (unsigned flags = 2; flags <= 255; ++flags) {
            packet[payload + 6] = (unsigned char)flags;
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
        }
        packet[payload + 6] = 0;
        memset(packet + payload + 7, 0, 8);
        CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
    }
    size_t size = pia_query(packet, 256, 1);
    const uint32_t wrong_lengths[] = {0, 1, 255, 257, 1061, 1062, 0x10000, 0x01000000, 0x7fffffff, UINT32_MAX};
    for (unsigned i = 0; i < sizeof wrong_lengths / sizeof wrong_lengths[0]; ++i) {
        be32(packet + 1, wrong_lengths[i]);
        CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
    }
    // The length itself is plausible in little endian, but not on the wire.
    packet[1] = 0; packet[2] = 1; packet[3] = 0; packet[4] = 0;
    CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
    size = pia_query(packet, 1061, 0);
    CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == DM_DISCOVERY_PIA);
    packet[PIA_MAX] = 0;
    CHECK(dm_discovery_query_protocol(PIA_PORT, packet, sizeof packet) == 0);
    // Oversize must be rejected before indexing with the untrusted length.
    CHECK(dm_discovery_query_protocol(PIA_PORT, packet, SIZE_MAX) == 0);
}

static void exact_ldn(void) {
    unsigned char packet[32];
    CHECK(dm_discovery_query_protocol(LDN_PORT, ldn_query, sizeof ldn_query) == DM_DISCOVERY_LDN);
    for (unsigned byte = 0; byte < sizeof ldn_query; ++byte) {
        for (unsigned value = 0; value <= 255; ++value) {
            if (value == ldn_query[byte]) continue;
            memcpy(packet, ldn_query, sizeof ldn_query); packet[byte] = (unsigned char)value;
            CHECK(dm_discovery_query_protocol(LDN_PORT, packet, sizeof ldn_query) == 0);
        }
    }
    memset(packet, 0, sizeof packet); memcpy(packet, ldn_query, sizeof ldn_query);
    for (size_t size = 0; size <= sizeof packet; ++size) {
        unsigned char *copy = malloc(size ? size : 1); CHECK(copy != NULL);
        memcpy(copy, packet, size);
        CHECK(dm_discovery_query_protocol(LDN_PORT, copy, size) == (size == sizeof ldn_query ? DM_DISCOVERY_LDN : 0));
        free(copy);
    }
    CHECK(dm_discovery_query_protocol(LDN_PORT, packet, SIZE_MAX) == 0);
}

static void routing_scope(void) {
    unsigned char packet[PIA_MAX];
    size_t size = pia_query(packet, 17, 1);
    // The input is a host-order destination port, including on little-endian
    // hosts. A verified query on another port must not start supplemental work.
    for (unsigned port = 0; port <= 65535; ++port) {
        CHECK(dm_discovery_query_protocol(port, packet, size) == (port == PIA_PORT ? DM_DISCOVERY_PIA : 0));
        CHECK(dm_discovery_query_protocol(port, ldn_query, sizeof ldn_query) == (port == LDN_PORT ? DM_DISCOVERY_LDN : 0));
    }
    CHECK(dm_discovery_query_protocol(65536, packet, size) == 0);
    CHECK(dm_discovery_query_protocol(UINT_MAX, packet, size) == 0);
    CHECK(dm_discovery_query_protocol(65536, ldn_query, sizeof ldn_query) == 0);
    CHECK(dm_discovery_query_protocol(UINT_MAX, ldn_query, sizeof ldn_query) == 0);
    const unsigned ports[] = {0, PIA_PORT, LDN_PORT, 65535, UINT_MAX};
    const size_t sizes[] = {0, 1, 5, 12, PIA_OVERHEAD, PIA_MAX, SIZE_MAX};
    for (unsigned p = 0; p < sizeof ports / sizeof ports[0]; ++p)
        for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s)
            CHECK(dm_discovery_query_protocol(ports[p], NULL, sizes[s]) == 0);
}

static uint32_t random_word(uint32_t *state) {
    uint32_t n = *state; n ^= n << 13; n ^= n >> 17; n ^= n << 5; *state = n; return n;
}

static void fuzz_bounds(void) {
    uint32_t state = UINT32_C(0x930ef217);
    for (unsigned run = 0; run < 50000; ++run) {
        unsigned payload = random_word(&state) % (PIA_MAX - PIA_OVERHEAD + 1);
        size_t size = run % 3 ? random_word(&state) % 1501 : payload + PIA_OVERHEAD;
        // A varying unaligned start plus an exact trailing allocation catches
        // alignment assumptions and reads past even one-byte input buffers.
        size_t prefix = run % 8;
        unsigned char *allocation = malloc(prefix + (size ? size : 1)); CHECK(allocation != NULL);
        unsigned char *packet = allocation + prefix;
        for (size_t i = 0; i < size; ++i) packet[i] = (unsigned char)random_word(&state);
        if (!(run % 3)) {
            // Reach the footer checks on unaligned valid frames as well as
            // testing random data that normally fails at the header.
            pia_query(packet, payload, random_word(&state) & 1u);
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == DM_DISCOVERY_PIA);
            switch ((run / 3) % 6) {
            case 0: packet[0] = 1; break;
            case 1: memset(packet + payload + 7, 0, 8); break;
            case 2: packet[payload + 5] = 2; break;
            case 3: packet[payload + 6] = 2; break;
            case 4: be32(packet + 1, UINT32_MAX); break;
            case 5: --size; break;
            }
            CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
        }
        const unsigned ports[] = {PIA_PORT, LDN_PORT, 0, 65535, UINT_MAX};
        for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; ++i) {
            unsigned result = dm_discovery_query_protocol(ports[i], packet, size);
            CHECK(result == 0 || result == DM_DISCOVERY_PIA || result == DM_DISCOVERY_LDN);
            CHECK(!result || (result == DM_DISCOVERY_PIA ? ports[i] == PIA_PORT : ports[i] == LDN_PORT));
        }
        // A guaranteed invalid type makes this an independent rejection test
        // even if later random generation happens to resemble a valid envelope.
        if (size) packet[0] = 0xff;
        CHECK(dm_discovery_query_protocol(PIA_PORT, packet, size) == 0);
        CHECK(dm_discovery_query_protocol(LDN_PORT, packet, size) == 0);
        free(allocation);
    }
}

int main(void) {
    valid_pia_lengths(); malformed_pia(); exact_ldn(); routing_scope(); fuzz_bounds();
    printf("discovery protocol: %u checks passed (synthetic Pia/LDN queries, scope, malformed input and fuzz bounds).\n", checks);
    return 0;
}
