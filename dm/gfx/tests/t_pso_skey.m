// Serializable pipeline key unit tests: the pso_skey hex round trip is byte-identical; tampering / wrong length / illegal characters are detected (expectations given directly by construction).
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdlib.h>
#include "../nvn/nvn_obj.h"
@interface NvShader : NSObject { @public int stage; char key[24]; } @end
@implementation NvShader @end
#include "../nvn/nvn_pso_skey.h"

static void fill(pso_key *k, unsigned seed) {
    memset(k, 0, sizeof *k);
    for (size_t i = 0; i < sizeof *k; i++) ((unsigned char *)k)[i] = (unsigned char)(seed + i * 13);
    k->vs = (void *)1; k->fs = (void *)2; k->fsfn = (void *)3;
}

int main(void) {
    for (unsigned seed = 1; seed <= 3; seed++) {
        pso_key k; fill(&k, seed); pso_key_norm(&k, seed & 1);   // keys are made normalized (pso_key_fill), and a parse normalizes
        NvShader *vs = [NvShader new], *fs = [NvShader new];
        vs->stage = (int)seed; fs->stage = (int)(seed + 1);
        snprintf(vs->key, sizeof vs->key, "vs-%u", seed); snprintf(fs->key, sizeof fs->key, "fs-%u", seed);
        pso_fz_desc fz = {.kind = 1, .fbmask = 0xa5};
        for (int i = 0; i < 32; i++) fz.fx[i] = (int8_t)(i - 7);
        pso_skey a, b; char hex[sizeof a * 2 + 1];
        pso_skey_from(&k, vs, fs, seed & 1, &fz, &a); pso_skey_hex(&a, hex);
        assert(pso_skey_parse(hex, &b) == 0); assert(memcmp(&a, &b, sizeof a) == 0);
        hex[0] = hex[0] == '0' ? '1' : '0'; assert(pso_skey_parse(hex, &b) == 0); assert(memcmp(&a, &b, sizeof a) != 0);
        assert(pso_skey_parse(hex + 1, &b) == -1); hex[0] = 'g'; assert(pso_skey_parse(hex, &b) == -1);
    }
    // A prewarm row written before pso_key.atest: the key without its last fields parses with the alpha test and the logic operation off
    {
        pso_key k; fill(&k, 9); k.atest = 1; k.logic = 1; pso_key_norm(&k, 0);
        NvShader *vs = [NvShader new]; snprintf(vs->key, sizeof vs->key, "vs-a");
        pso_skey a, b; char hex[sizeof a * 2 + 1];
        pso_skey_from(&k, vs, nil, 0, NULL, &a); pso_skey_hex(&a, hex);
        size_t old = strlen(hex) - 2 * (sizeof k.attr_hi + sizeof k.atest + sizeof k.logic + sizeof k.border + sizeof k.border_spare); hex[old] = 0;
        assert(pso_skey_parse(hex, &b) == 0); assert(b.k.atest == 0 && b.k.logic == 0);
        a.k.atest = a.k.logic = a.k.border = a.k.border_spare = 0; memset(a.k.attr_hi, 0, sizeof a.k.attr_hi); assert(memcmp(&a, &b, sizeof a) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): a pso_skey is its bytes (pso_skey_from zeroes it, pso_skey_hex writes them, the maps key on them)
    }
    // A row written before pso_key.attr_hi (NVN2's vertex attributes 16 to 31): they read as 0, the fields before them stay
    {
        pso_key k; fill(&k, 11); k.atest = 1; k.logic = 1; pso_key_norm(&k, 0);
        NvShader *vs = [NvShader new]; snprintf(vs->key, sizeof vs->key, "vs-h");
        pso_skey a, b; char hex[sizeof a * 2 + 1];
        pso_skey_from(&k, vs, nil, 0, NULL, &a); pso_skey_hex(&a, hex);
        hex[strlen(hex) - 2 * (sizeof k.attr_hi + sizeof k.border + sizeof k.border_spare)] = 0;
        assert(pso_skey_parse(hex, &b) == 0); assert(b.k.atest == 1 && b.k.logic == 1);
        a.k.border = a.k.border_spare = 0; memset(a.k.attr_hi, 0, sizeof a.k.attr_hi); assert(memcmp(&a, &b, sizeof a) == 0);   // NOLINT(bugprone-suspicious-memory-comparison): the key is defined byte by byte, padding included: pso_skey_from clears it and its hex form is every byte
    }
    // A row written before the border specialization keeps its high attributes and prewarms the no-correction pipeline. Live draws needing corrections have a different key.
    {
        pso_key k; fill(&k, 13); pso_key_norm(&k, 0);
        NvShader *vs = [NvShader new]; snprintf(vs->key, sizeof vs->key, "vs-b");
        pso_skey a, b; char hex[sizeof a * 2 + 1];
        pso_skey_from(&k, vs, nil, 0, NULL, &a); pso_skey_hex(&a, hex);
        hex[strlen(hex) - 2 * (sizeof k.border + sizeof k.border_spare)] = 0;
        assert(pso_skey_parse(hex, &b) == 0 && b.k.border == 0 && b.k.border_spare == 0);
        a.k.border = a.k.border_spare = 0; assert(memcmp(&a, &b, sizeof a) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): a pso_skey is its bytes (pso_skey_from zeroes it, pso_skey_hex writes them, the maps key on them)
        a.k.border = 1; assert(memcmp(&a, &b, sizeof a) != 0);  // NOLINT(bugprone-suspicious-memory-comparison): a pso_skey is its bytes (pso_skey_from zeroes it, pso_skey_hex writes them, the maps key on them)
    }
    // pso_key_attr: attribute 16 is the first of the high table
    {
        pso_key k; memset(&k, 0, sizeof k); k.attr[3].fmt = 7; k.attr_hi[0].fmt = 9;
        assert(pso_key_attr(&k, 3).fmt == 7 && pso_key_attr(&k, 16).fmt == 9 && pso_key_attr(&k, 17).fmt == 0);
    }
    // pso_key_norm: blend factors of a target that does not blend, and the mask / blend bit of a target a normal pass lacks, are not part of the key; a merged pass keeps
    // the mask and blend bits (they index logical targets through lmap)
    {
        NvShader *vs = [NvShader new]; snprintf(vs->key, sizeof vs->key, "vs-n");
        pso_key k1; memset(&k1, 0, sizeof k1); k1.cfmt[0] = 80; k1.cfmt[1] = 80; k1.blend_en = 1; k1.chmask = 0xff;
        k1.blend[0].src_rgb = 1; k1.blend[0].dst_rgb = 2;
        pso_key k2 = k1; k2.blend[1].src_rgb = 7; k2.blend[3].dst_a = 5; k2.blend_en |= 1u << 5; k2.chmask |= 0xfu << 20;
        pso_skey a, b; char ha[sizeof a * 2 + 1], hb[sizeof b * 2 + 1];
        pso_skey_from(&k1, vs, nil, 0, NULL, &a); pso_skey_from(&k2, vs, nil, 0, NULL, &b); assert(memcmp(&a, &b, sizeof a) != 0);
        pso_skey_hex(&a, ha); pso_skey_hex(&b, hb); assert(pso_skey_parse(ha, &a) == 0 && pso_skey_parse(hb, &b) == 0);
        assert(memcmp(&a, &b, sizeof a) == 0 && a.k.blend[0].src_rgb == 1 && a.k.blend[0].dst_rgb == 2 && a.k.chmask == 0xff && a.k.blend_en == 1);
        pso_skey_from(&k2, vs, nil, 1, NULL, &b); pso_skey_hex(&b, hb); assert(pso_skey_parse(hb, &b) == 0);
        assert(b.k.chmask == (0xffu | 0xfu << 20) && b.k.blend_en == (1u | 1u << 5) && b.k.blend[1].src_rgb == 0 && b.k.blend[3].dst_a == 0);
    }
    // Native meshes preserve their render state without serializing function pointers.
    {
        NvShader *ms = [NvShader new], *fs = [NvShader new];
        ms->stage = 4; fs->stage = 1; memcpy(ms->key, "mesh", 5); memcpy(fs->key, "frag", 5);
        pso_key k = {.cfmt = {80, 0, 92}, .dfmt = 252, .samples = 4, .blend_en = 1, .chmask = 0xf0f, .a2c = 1};
        k.blend[0].src_rgb = 1; k.blend[0].dst_rgb = 2;
        pso_key_norm(&k, 0);
        pso_skey stable, parsed; char hex[sizeof stable * 2 + 1], roundtrip[sizeof parsed * 2 + 1];
        pso_skey_from(&k, ms, fs, 0, NULL, &stable); pso_skey_hex(&stable, hex);
        assert(pso_mesh_skey_parse(hex, &parsed) == 0);
        pso_skey_hex(&parsed, roundtrip);
        assert(!strcmp(hex, roundtrip));
        stable.k.attr_hi[0].fmt = 7; pso_skey_hex(&stable, hex);
        assert(pso_mesh_skey_parse(hex, &parsed) == 0 && !parsed.k.attr_hi[0].fmt);
        stable.k.attr_hi[0].fmt = 0;
        stable.vs_stage = 0; pso_skey_hex(&stable, hex); assert(pso_mesh_skey_parse(hex, &parsed) < 0);
        stable.vs_stage = 4;
        stable.fs_stage = 3; pso_skey_hex(&stable, hex); assert(pso_mesh_skey_parse(hex, &parsed) < 0);
        stable.fs_stage = 1;
        stable.fz = 1; pso_skey_hex(&stable, hex); assert(pso_mesh_skey_parse(hex, &parsed) < 0);
        stable.fz = 0;
        stable.k.vs = (void *)1; pso_skey_hex(&stable, hex); assert(pso_mesh_skey_parse(hex, &parsed) < 0);
        stable.k.vs = NULL;
        stable.k.dfmt = 80; pso_skey_hex(&stable, hex); assert(pso_mesh_skey_parse(hex, &parsed) < 0);
        pso_skey_from(&k, ms, nil, 0, NULL, &stable); pso_skey_hex(&stable, hex);
        assert(pso_mesh_skey_parse(hex, &parsed) == 0 && !parsed.fs[0]);
        assert(pso_mesh_skey_parse(hex + 1, &parsed) < 0);
    }
    puts("PSO_SKEY OK render and native mesh keys round-trip; malformed and incompatible rows rejected");
    return 0;
}
