// A shader's key, the one shader_map.tsv lists (column 4) and shader_split.py skey computes: the first 8 bytes of sha256(the control section's key bytes + the code area). hle_nvn.c keys_hash
// keys a program's stages with it at nvnProgramSetShaders, hle_glslc.c the stages of an output the game compiled with its own GLSLC, so both ask the shader map the same question.
// The control section's bytes in the key and the code area's length: NVN: +0x704 is the code area's length; +0x20 holds the section's own length: sections of at least 0x880 bytes (all of
// Mario's, Engage's, Dread's and 13 Sentinels') use the first 0x880; SDK 10's 3D World has 0x850-byte ones followed by other data, so only their own length is used (shader_split.py ctl_len).
// NVN2 (Switch 2, version 0xc9 at +4): the code area's length at +0x44; the key takes the first 0x48 bytes (through the shader's own hash at +0x20 and the code lengths), +0x30 hashed as zero:
// Donkey Kong Bananza's BNSH files hold the section's length there (0xd8, 0xd0 in effects), agl's BAHS 0, also in memory.
// Profile key shader_key_skip (DM_GAME_SHADER_KEY_SKIP_OFF / _LEN, game_profile.h): bytes of the control section hashed as zeros (shader_split.py key_ctl). Batman: Arkham Knight's engine shares
// one control section among shaders whose sections differ only in GLSLC's compile information (+0x778..+0x791, +0x7d0..+0x7e7): it hands a program the section of an earlier shader with the
// same rest, so the bytes there name another shader
#pragma once
#include <stdint.h>
#include <string.h>
#include <CommonCrypto/CommonDigest.h>

#define NVN2_KEY_CTL 0x48
static inline int nvn_shkey_nvn2(const void *control) {
    uint32_t magic, ver; memcpy(&magic, control, 4); memcpy(&ver, (const uint8_t *)control + 4, 4); return magic == 0x98761234u && ver == 0xc9;
}
static inline void nvn_shkey_lens(const void *control, uint32_t *nctl, uint32_t *ncode) {
    const uint8_t *c = control; uint32_t own;
    if (nvn_shkey_nvn2(c)) { memcpy(ncode, c + 0x44, 4); *nctl = NVN2_KEY_CTL; return; }
    memcpy(ncode, c + 0x704, 4);
    memcpy(&own, c + 0x20, 4);
    *nctl = own >= 0x760 && own < 0x880 ? own : 0x880;
}
// control: nctl bytes as nvn_shkey_lens gives them; code: ncode bytes, or NULL for a key without its code area (the code is hashed only when it is there)
static inline void nvn_shkey(const void *control, uint32_t nctl, const void *code, uint32_t ncode, uint8_t out[8]) {
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX hc; CC_SHA256_Init(&hc);
    if (nvn_shkey_nvn2(control)) {
        uint8_t ctl[NVN2_KEY_CTL]; memcpy(ctl, control, NVN2_KEY_CTL); memset(ctl + 0x30, 0, 4);
        CC_SHA256_Update(&hc, ctl, NVN2_KEY_CTL);
    } else if (DM_GAME_SHADER_KEY_SKIP_LEN && DM_GAME_SHADER_KEY_SKIP_OFF + DM_GAME_SHADER_KEY_SKIP_LEN <= nctl && nctl <= 0x880) {
        uint8_t ctl[0x880]; memcpy(ctl, control, nctl); memset(ctl + DM_GAME_SHADER_KEY_SKIP_OFF, 0, DM_GAME_SHADER_KEY_SKIP_LEN);
        CC_SHA256_Update(&hc, ctl, nctl);
    } else CC_SHA256_Update(&hc, control, nctl);
    if (code && ncode) CC_SHA256_Update(&hc, code, ncode);
    CC_SHA256_Final(md, &hc); memcpy(out, md, 8);
}
