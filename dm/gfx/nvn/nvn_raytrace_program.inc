// The universal offline RT library and the subset of modules a public NVN
// program owns. Both public entry points run under the submission mutex.
#include "nvn_raytrace.h"

@interface NvRayLibrary : NSObject { @public
    NvShader *shader;
    id<MTLComputePipelineState> pipeline;
    id<MTLBuffer> bank0;
    uint32_t parameterWords, localBytes, localBias, coreOffset, descriptorOffset;
    NSDictionary *moduleEntries, *moduleAsSlots, *catalogByKey;
} @end
@implementation NvRayLibrary @end

@interface NvRayProgram : NSObject { @public
    NvRayLibrary *library;
    NSData *membership;
    uint64_t tag;
    NSArray<NSDictionary *> *modules;
    int asSlot;
} @end
@implementation NvRayProgram @end

static int rtp_error(const char *format, ...) {
    char message[512]; va_list args; va_start(args, format); vsnprintf(message, sizeof message, format, args); va_end(args);
    MLOG("[RED] ray-tracing program: %s", message); return 0;
}
static int rtp_u32(id value, uint32_t *out) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return 0;
    double d = [value doubleValue]; uint64_t n = [value unsignedLongLongValue];
    if (!(d >= 0 && d <= UINT32_MAX) || d != (double)n) return 0;
    *out = (uint32_t)n; return 1;
}
static int rtp_hex(NSString *s, NSUInteger n) {
    if (![s isKindOfClass:NSString.class] || s.length != n) return 0;
    const char *p = s.UTF8String;
    for (NSUInteger i = 0; i < n; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    return 1;
}
static int rtp_read(uint64_t address, void *destination, size_t length) {
    if (!address || !length || address > UINT64_MAX - length) return 0;
    vm_size_t read = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)address, (vm_size_t)length,
                            (vm_address_t)destination, &read) == KERN_SUCCESS && read == length;
}
static NSString *rtp_metadata_sha(uint64_t address, size_t length) {
    if (!address || !length || address > UINT64_MAX - length) return nil;
    uint8_t block[4096], digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX context; CC_SHA256_Init(&context);
    while (length) {
        CC_LONG n = (CC_LONG)MIN(length, sizeof block);
        if (!rtp_read(address, block, n)) return nil;
        CC_SHA256_Update(&context, block, n); address += n; length -= n;
    }
    CC_SHA256_Final(digest, &context); char text[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (unsigned i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(text + 2 * i, 3, "%02x", digest[i]);
    return @(text);
}

static NvRayLibrary *rtp_library(void) {
    static NvRayLibrary *cached;
    shmap_load();
    NvShader *shader = shader_get(@"rt_library_b", NVN_RT_RESOURCE_STAGE);
    if (!shader) { rtp_error("the universal rt_library_b library did not load"); return nil; }
    if (cached && cached->shader == shader) return cached;
    NSDictionary *rt = shader->rtMetadata;
    if (![rt isKindOfClass:NSDictionary.class]) { rtp_error("rt_library_b has no runtime catalog"); return nil; }
    uint32_t version, imageBase, memberBase;
    NvRayLibrary *o = [NvRayLibrary new]; o->shader = shader;
    if (!rtp_u32(rt[@"version"], &version) || version != 1 ||
        !rtp_u32(rt[@"parameter_words"], &o->parameterWords) ||
        !rtp_u32(rt[@"local_bytes"], &o->localBytes) || !rtp_u32(rt[@"local_bias"], &o->localBias) ||
        !rtp_u32(rt[@"core_offset"], &o->coreOffset) || !rtp_u32(rt[@"descriptor_offset"], &o->descriptorOffset) ||
        !rtp_u32(rt[@"image_class_base"], &imageBase) || imageBase != DMSH_RT_IMAGE_CLASSES ||
        !rtp_u32(rt[@"membership_base"], &memberBase) || memberBase != DMSH_RT_MEMBERSHIP ||
        o->localBias != DMSH_RT_LOCAL_BIAS || !o->localBytes || o->localBytes > 0x10000 || (o->localBytes & 31) ||
        (o->coreOffset & 63) || o->coreOffset > 0x10000 - DMSH_RT_CORE_BYTES ||
        (o->descriptorOffset & 15) || o->descriptorOffset > 0x10000 - DMSH_RT_DESCRIPTOR_BYTES ||
        (o->coreOffset < o->descriptorOffset + DMSH_RT_DESCRIPTOR_BYTES && o->descriptorOffset < o->coreOffset + DMSH_RT_CORE_BYTES)) {
        rtp_error("rt_library_b runtime extents/layout are invalid"); return nil;
    }
    NSDictionary *catalog = rt[@"catalog"], *entries = rt[@"module_entries"], *asSlots = rt[@"module_as_slots"];
    if (![catalog isKindOfClass:NSDictionary.class]) { rtp_error("rt_library_b has no module catalog"); return nil; }
    NSArray *catalogModules = catalog[@"modules"];
    uint32_t catalogVersion;
    if (![catalog isKindOfClass:NSDictionary.class] || !rtp_u32(catalog[@"version"], &catalogVersion) || catalogVersion != 1 ||
        ![catalogModules isKindOfClass:NSArray.class] || !catalogModules.count ||
        ![entries isKindOfClass:NSDictionary.class] || ![asSlots isKindOfClass:NSDictionary.class] ||
        entries.count != catalogModules.count || asSlots.count != catalogModules.count) {
        rtp_error("rt_library_b module catalog or AS dependencies are missing/inconsistent"); return nil;
    }
    NSMutableDictionary *byKey = [NSMutableDictionary dictionary]; NSMutableDictionary<NSNumber *, NSDictionary *> *byId = [NSMutableDictionary dictionary];
    uint32_t maxId = 0;
    for (id value in catalogModules) {
        if (![value isKindOfClass:NSDictionary.class]) { rtp_error("a catalog module is not a record"); return nil; }
        NSDictionary *module = value; NSString *key = module[@"key"]; uint32_t bytes;
        if (!rtp_hex(key, 16)) { rtp_error("a catalog module has an invalid key"); return nil; }
        NSArray *functions = entries[key], *slots = asSlots[key];
        if (!rtp_hex(key, 16) || byKey[key] || !rtp_hex(module[@"metadata_sha256"], 64) ||
            !rtp_u32(module[@"length"], &bytes) || !bytes || bytes >= (1u << 24) ||
            ![functions isKindOfClass:NSArray.class] || !functions.count ||
            ![functions isEqual:module[@"entries"]] || ![slots isKindOfClass:NSArray.class]) {
            rtp_error("catalog module %s has invalid identity/entries", [key description].UTF8String); return nil;
        }
        NSMutableSet *slotSet = [NSMutableSet set];
        for (id slot in slots) { uint32_t index; if (!rtp_u32(slot, &index) || index >= 32 || [slotSet containsObject:slot]) { rtp_error("module %s has invalid AS dependencies", key.UTF8String); return nil; } [slotSet addObject:slot]; }
        NSMutableDictionary<NSNumber *, NSNumber *> *owners = [NSMutableDictionary dictionary];
        for (id function in functions) {
            if (![function isKindOfClass:NSDictionary.class]) { rtp_error("module %s entry is not a record", key.UTF8String); return nil; }
            uint32_t logical, owner, kind, frame, entryIndex, regionIndex;
            if (!rtp_u32(function[@"id"], &logical) || !logical || logical > INT16_MAX || byId[@(logical)] ||
                !rtp_u32(function[@"owner"], &owner) || !owner || owner > INT16_MAX ||
                !rtp_u32(function[@"kind"], &kind) || (kind != 0 && kind != 7 && kind != 8) ||
                !rtp_u32(function[@"frame"], &frame) || frame > o->localBytes ||
                !rtp_u32(function[@"entry_index"], &entryIndex) || !rtp_u32(function[@"region_index"], &regionIndex) ||
                (regionIndex == 0 ? (owner != logical || owners[@(entryIndex)] != nil) :
                                   (![owners[@(entryIndex)] isEqual:@(owner)] || kind != 0))) {
                rtp_error("module %s contains an invalid logical entry", key.UTF8String); return nil;
            }
            if (regionIndex == 0) owners[@(entryIndex)] = @(owner);
            byId[@(logical)] = function; maxId = MAX(maxId, logical);
        }
        byKey[key] = module;
    }
    if (o->parameterWords != ((DMSH_RT_MEMBERSHIP + maxId / 32 + 1 + 3) & ~3u)) { rtp_error("membership parameter extent disagrees with logical IDs"); return nil; }
    NSArray *resources = catalog[@"resources"];
    NSMutableSet *resourceNames = [NSMutableSet set];
    if (![resources isKindOfClass:NSArray.class] || resources.count != 2) { rtp_error("named RT resource layout is missing"); return nil; }
    for (id value in resources) {
        if (![value isKindOfClass:NSDictionary.class]) { rtp_error("named RT resource is not a record"); return nil; }
        NSString *name = value[@"name"]; uint32_t kind, size, offset, alignment;
        uint32_t expectedSize = [name isEqual:@"coreParams"] ? DMSH_RT_CORE_BYTES : [name isEqual:@"descriptorBaseArray"] ? DMSH_RT_DESCRIPTOR_BYTES : 0;
        uint32_t expectedOffset = [name isEqual:@"coreParams"] ? o->coreOffset : o->descriptorOffset;
        if (!expectedSize || [resourceNames containsObject:name] || !rtp_u32(value[@"kind"], &kind) || kind != 3 ||
            !rtp_u32(value[@"size"], &size) || size != expectedSize || !rtp_u32(value[@"offset"], &offset) || offset != expectedOffset ||
            !rtp_u32(value[@"alignment"], &alignment) || !alignment || (alignment & (alignment - 1)) || (offset & (alignment - 1)) ||
            ![value[@"data"] isEqual:@""]) { rtp_error("unsupported named resource layout or initialized RT resource"); return nil; }
        [resourceNames addObject:name];
    }
    o->moduleEntries = [entries copy]; o->moduleAsSlots = [asSlots copy]; o->catalogByKey = [byKey copy];
    NSError *error = nil; o->pipeline = cps_new(shader->fn, &error);
    if (!o->pipeline) { rtp_error("rt_library_b pipeline failed: %s", error.localizedDescription.UTF8String); return nil; }
    o->bank0 = [g_dev newBufferWithLength:0x10000 options:MTLResourceStorageModeShared];
    if (!o->bank0) { rtp_error("could not allocate the readonly RT bank0"); return nil; }
    memset(o->bank0.contents, 0, o->bank0.length);
    uint32_t softwareK = o->localBias + o->localBytes; memcpy((uint8_t *)o->bank0.contents + 0x28, &softwareK, sizeof softwareK);
    res_add(o->bank0); cached = o; return o;
}

int mtl_rt_program_set(prog_t *program, int count, const NVNshaderData *modules,
                       uint32_t traceDepth, uint32_t callableDepth, uint32_t flags) {
    @autoreleasepool {
    if (!program || program->magic != MAGIC('P','R','O','G') || program->serial || program->nstage)
        return rtp_error("initialization requires a fresh program");
    if (count <= 0 || !modules) return rtp_error("initialization requires a nonempty module array");
    if ((uint64_t)count > (UINT64_MAX - (uintptr_t)modules) / sizeof *modules) return rtp_error("module descriptor array overflows its address range");
    if (traceDepth != 1 || callableDepth || flags) return rtp_error("unsupported trace/callable depth or flags (%u/%u/%#x)", traceDepth, callableDepth, flags);
    NvRayLibrary *library = rtp_library(); if (!library) return 0;
    NSMutableArray *snapshots = [NSMutableArray array]; NSMutableSet<NSNumber *> *asSlots = [NSMutableSet set];
    NSMutableData *membership = [NSMutableData dataWithLength:(library->parameterWords - DMSH_RT_MEMBERSHIP) * sizeof(uint32_t)];
    for (int i = 0; i < count; i++) {
        NVNshaderData input; uint8_t header[0x68];
        if (!rtp_read((uint64_t)(uintptr_t)modules + (size_t)i * sizeof input, &input, sizeof input) ||
            !rtp_read((uint64_t)(uintptr_t)input.control, header, sizeof header)) return rtp_error("module %d descriptor/control is unreadable", i);
        uint32_t magic, version, metaBytes, metaOffset, codeBytes, keyCodeBytes;
        memcpy(&magic, header, 4); memcpy(&version, header + 4, 4); memcpy(&keyCodeBytes, header + 0x44, 4);
        memcpy(&metaBytes, header + 0x5c, 4); memcpy(&metaOffset, header + 0x60, 4); memcpy(&codeBytes, header + 0x64, 4);
        if (magic != 0x98761234u || version != 0xc9 || header[0x56] != 8 || !metaBytes || metaOffset < sizeof header || (metaOffset & 7) ||
            !codeBytes || codeBytes >= (1u << 24) || codeBytes != keyCodeBytes || metaOffset > UINT32_MAX - metaBytes)
            return rtp_error("module %d is not a supported bounded NVN2 RT control", i);
        // Hash metadata through bounded reads, without allocating a guest-sized
        // control buffer. Only catalog identity/entries survive initialization.
        NSMutableData *code = [NSMutableData dataWithLength:codeBytes];
        if (!rtp_read(input.data, code.mutableBytes, code.length)) return rtp_error("module %d code is unreadable", i);
        NVNshaderData snapshot = {(uint64_t)(uintptr_t)code.bytes, header}; uint8_t digest[8];
        if (!nvn_shader_key(&snapshot, digest)) return rtp_error("module %d has no standard shader key", i);
        char text[17]; for (unsigned j = 0; j < 8; j++) snprintf(text + 2 * j, 3, "%02x", digest[j]); NSString *key = @(text);
        NSDictionary *catalog = library->catalogByKey[key];
        if ((uintptr_t)input.control > UINT64_MAX - metaOffset) return rtp_error("module %d metadata address overflows", i);
        NSString *metadataSha = catalog ? rtp_metadata_sha((uintptr_t)input.control + metaOffset, metaBytes) : nil;
        if (!catalog || [catalog[@"length"] unsignedIntValue] != codeBytes || ![catalog[@"metadata_sha256"] isEqual:metadataSha])
            return rtp_error("module %d key %s or metadata is absent/different in rt_library_b", i, text);
        NSArray *entries = library->moduleEntries[key];
        uint32_t *mask = membership.mutableBytes;
        for (NSDictionary *entry in entries) { unsigned logical = [entry[@"id"] unsignedIntValue]; mask[logical / 32] |= 1u << (logical & 31); }
        [asSlots addObjectsFromArray:library->moduleAsSlots[key]];
        [snapshots addObject:@{@"key":key, @"entries":entries, @"metadata_sha256":metadataSha}];
    }
    if (asSlots.count != 1) return rtp_error("program requires %lu AS slots; the current kernel supports exactly one", (unsigned long)asSlots.count);
    NvRayProgram *rt = [NvRayProgram new]; rt->library = library; rt->membership = [membership copy]; rt->modules = [snapshots copy];
    rt->asSlot = asSlots.anyObject.intValue;
    NvProg *object = [NvProg new]; object->raytracing = rt;
    uint64_t serial = nvn_new_serial(); rt->tag = serial;
    serial_put(serial, object); program->serial = serial; program->nstage = 0;
    return 1;
    }
}

void mtl_rt_group_handle(const prog_t *program, const int32_t indices[3], void *out16) {
    if (!program || !indices || !out16 || !program->serial) MDIE("[RED] shader-group handle requires an initialized RT program and output");
    NvProg *object = serial_get(program->serial); NvRayProgram *rt = [object isKindOfClass:NvProg.class] ? object->raytracing : nil;
    if (!rt || indices[0] < 0 || (NSUInteger)indices[0] >= rt->modules.count || indices[1] != -1 || indices[2] != -1)
        MDIE("[RED] unsupported RT shader-group tuple or program");
    NSArray *entries = rt->modules[(NSUInteger)indices[0]][@"entries"]; NSDictionary *primary = nil;
    for (NSDictionary *entry in entries) if ([entry[@"region_index"] unsignedIntValue] == 0) {
        if (primary) MDIE("[RED] shader-group module contains multiple primary entries; tuple selection is unsupported");
        primary = entry;
    }
    if (!primary) MDIE("[RED] shader-group module has no primary entry");
    uint32_t id_ = [primary[@"id"] unsignedIntValue], kind = [primary[@"kind"] unsignedIntValue];
    uint32_t words[4] = {id_ | kind << 16, 0, (uint32_t)rt->tag, (uint32_t)(rt->tag >> 32)};
    memcpy(out16, words, sizeof words);   // Device integer 0x7a is 32; native writer owns only this 16-byte prefix.
}
