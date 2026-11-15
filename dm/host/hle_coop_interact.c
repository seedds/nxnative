// coop_interact: "Interact everywhere" (docs/coop-interact.md), an experimental opt-in for Link Play outside Palette Town, and "Story co-op"
// (docs/story-coop.md), the experimental step after it. MIT, our own code.
// The game keeps a session's permissions in a 36-bit mask (GameMgr+0x8c; its top bits are the online / host / visitor bits at +0x90) and rebuilds it
// whenever a session starts or the town changes. In a story area (stage type 1: Withered Wasteland and the other story towns) an online player always
// gets the Spectator pattern, whatever the host chose; Palette Town with Spectator Mode Off gets the normal online pattern. One site per supported main
// build swaps the first for the second while the session's own Spectator flag is clear. Each port reaches the site its own way (nxnative: an
// observation gate; Windows: nxrt interprets the function) and calls coop_interact_site with the registers there. Every player must have it on:
// coop_mods_add puts it in the mods fingerprint, so a session refuses a player whose setting differs.
#include "coop.h"
#include <stdlib.h>
#include <string.h>

// The mask builder's story-area online block. At the site (movk x13, #1, lsl #32) x13 holds the pattern's bits 0-31 (Spectator: bits 23-26 and 31)
// and the movk adds bit 32 (online); x22 is the session's Spectator flag, 0 in every story area. The normal online pattern sets bits 1-28 and 30.
// 2.0.1 main (76a489f3…); 1.0.5 main (816c5358…), where the builder is the same instruction for instruction
static const coop_site g_sites[] = {
    {"76a489f3bd3642fa5e18bd83d32c98ba", 0xb0f60, 0xb1030, 0xf2c0002du, COOP_SITE_MASK, "permission mask: story areas get Palette Town's Spectator-Off pattern"},
    {"816c5358e8178d802fcec4a7ce5a741a", 0xa61e0, 0xa62b0, 0xf2c0002du, COOP_SITE_MASK, "permission mask: story areas get Palette Town's Spectator-Off pattern"},
};

// Story co-op. The A button asks a chain of handlers (2.0.1 main+0x4c3ec8) what it does; the talk handler (2.0.1 main+0x4c024c) gives an online
// player who is not the host a visitor's reply (kinds 0xa/0xb, no conversation) unless the NPC is one of three every visitor may talk to. At the site
// (orr w8, w25, w0) w0 is IsHost's answer, so a visitor takes the host's path: the NPC's own conversation. 1.0.5's handler has the same site at the
// same offset, between the same calls (stage == 0x11, IsHost); its other differences are struct offsets
static const coop_site g_story[] = {
    {"76a489f3bd3642fa5e18bd83d32c98ba", 0x4c024c, 0x4c06d8, 0x2a000328u, COOP_SITE_TALK, "talk: a visitor gets the NPC's conversation, as the host"},
    {"816c5358e8178d802fcec4a7ce5a741a", 0x469424, 0x4698b0, 0x2a000328u, COOP_SITE_TALK, "talk: a visitor gets the NPC's conversation, as the host"},
};

static int sites_of(const coop_site *t, size_t nt, const char *main_build, const coop_site **out) {
    int n = 0;
    *out = NULL;
    if (!main_build) return 0;
    for (size_t i = 0; i < nt; i++)
        if (!strncmp(main_build, t[i].build, strlen(t[i].build))) { if (!n) *out = &t[i]; n++; }
    return n;
}
int coop_interact_sites(const char *main_build, const coop_site **out) { return sites_of(g_sites, sizeof g_sites / sizeof g_sites[0], main_build, out); }
int coop_story_sites(const char *main_build, const coop_site **out) { return sites_of(g_story, sizeof g_story / sizeof g_story[0], main_build, out); }

int coop_story_on(void) {
    const char *e = getenv("DM_COOP_STORY");
    return e && !strcmp(e, "1");
}

int coop_interact_on(void) {
    const char *e = getenv("DM_COOP_INTERACT");
    return (e && !strcmp(e, "1")) || coop_story_on();
}

void coop_interact_site(int id, uint64_t *x) {
    if (id == COOP_SITE_MASK && !(x[22] & 1)) x[13] = 0x5ffffffe;   // the movk that follows adds bit 32
    if (id == COOP_SITE_TALK && coop_story_on()) x[0] = 1;           // IsHost: yes
}

uint64_t coop_mods_add(uint64_t h, const char *name) {
    if (!h) h = 0xcbf29ce484222325ull;   // FNV-1a, as nxnative's cheat fingerprint (ldn_glue.c): each name, then 0xFF
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001b3ull; }
    h ^= 0xFF; h *= 0x100000001b3ull;
    return h;
}
