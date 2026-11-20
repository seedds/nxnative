// Original bounded encoder/decoder for the public ldn_mitm wire format.
// No discovery/state implementation from another project is included here.
#include "hle_ldn.h"
#include <string.h>

static unsigned payload_size(unsigned type) {
    return type == DM_LDN_SCAN ? 0 : type == DM_LDN_CONNECT ? sizeof(dm_ldn_node) :
        type == DM_LDN_SCAN_RESPONSE || type == DM_LDN_SYNC ? sizeof(dm_ldn_network) : UINT32_MAX;
}
static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static void put16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
int dm_ldn_packet_size(const uint8_t *h) {
    if (!h || h[0] != 0 || h[1] != 0x14 || h[2] != 0x45 || h[3] != 0x11 || h[5] > 1 || h[10] || h[11]) return -1;
    unsigned expected = payload_size(h[4]), n = le16(h + 6), expanded = le16(h + 8);
    if (expected == UINT32_MAX || n > DM_LDN_PACKET_MAX - 12 || (h[5] ? expanded != expected || !n || !expanded : n != expected || expanded)) return -1;
    return (int)n + 12;
}
int dm_ldn_packet_encode(uint8_t *out, size_t cap, unsigned type, const void *payload, size_t size, int compress) {
    if (!out || size != payload_size(type) || (size && !payload) || cap < 12 || size > DM_LDN_PACKET_MAX - 12) return -1;
    uint8_t encoded[DM_LDN_PACKET_MAX]; size_t n = 0;
    const uint8_t *p = payload;
    if (compress && size) for (size_t i = 0; i < size;) {
        uint8_t b = p[i++]; encoded[n++] = b;
        if (!b) { unsigned zeros = 0; while (i < size && !p[i] && zeros < 255) { ++i; ++zeros; } encoded[n++] = (uint8_t)zeros; }
    }
    int compressed = compress && size && n < size;
    if (!compressed) n = size;
    if (n + 12 > cap) return -1;
    memset(out, 0, 12); out[1] = 0x14; out[2] = 0x45; out[3] = 0x11; out[4] = type; out[5] = compressed;
    put16(out + 6, (unsigned)n); put16(out + 8, compressed ? (unsigned)size : 0);
    if (n) memcpy(out + 12, compressed ? encoded : p, n);
    return (int)n + 12;
}
int dm_ldn_packet_decode(const uint8_t *packet, size_t size, unsigned *type, void *payload, size_t cap) {
    if (!packet || size < 12 || dm_ldn_packet_size(packet) != (int)size) return -1;
    unsigned expected = payload_size(packet[4]);
    if (expected > cap || (expected && !payload)) return -1;
    uint8_t *out = payload; size_t n = 0;
    if (!packet[5]) { if (expected) memcpy(out, packet + 12, expected); }
    else for (size_t i = 12; i < size;) {
        uint8_t b = packet[i++]; unsigned run = 1;
        if (!b) { if (i == size) return -1; run += packet[i++]; }
        if (run > expected - n) return -1;
        memset(out + n, b, run); n += run;
    }
    if (packet[5] && n != expected) return -1;
    if (type) *type = packet[4];
    return (int)expected;
}
int dm_ldn_network_valid(const dm_ldn_network *n) {
    if (!n || n->max_nodes < 1 || n->max_nodes > 8 || n->node_count < 1 || n->node_count > n->max_nodes ||
        n->advertise_size > 384 || n->common.ssid.len > 32 || n->common.type != 2 || !n->nodes[0].connected || !n->nodes[0].ip ||
        n->common.ssid.text[n->common.ssid.len]) return 0;
    unsigned connected = 0;
    for (int i = 0; i < 8; i++) if (n->nodes[i].connected) {
        if (n->nodes[i].connected != 1 || n->nodes[i].id != i || !n->nodes[i].ip || n->nodes[i].version < 0 || !memchr(n->nodes[i].name, 0, 33)) return 0;
        ++connected;
    }
    return connected == n->node_count;
}
int dm_ldn_filter_match(const dm_ldn_network *n, const dm_ldn_filter *f, int channel) {
    if (!dm_ldn_network_valid(n) || !f || f->flags & ~63u || ((f->flags & 16) && f->ssid.len > 32) || (channel && channel != n->common.channel)) return 0;
    if ((f->flags & 1) && f->id.intent.local_id != n->id.intent.local_id) return 0;
    if ((f->flags & 2) && memcmp(f->id.session, n->id.session, 16)) return 0;
    if ((f->flags & 4) && f->type != 3 && f->type != n->common.type) return 0;
    if ((f->flags & 8) && memcmp(f->mac, n->common.mac, 6)) return 0;
    if ((f->flags & 16) && (f->ssid.len != n->common.ssid.len || memcmp(f->ssid.text, n->common.ssid.text, f->ssid.len))) return 0;
    if ((f->flags & 32) && f->id.intent.scene != n->id.intent.scene) return 0;
    return 1;
}
