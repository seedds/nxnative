import Foundation

public struct EnginePatchItem: Identifiable, Equatable {
    public let id: String
    public let name: String
    public let summary: String
    public let relativePath: String
    public var isApplied: Bool
    
    public init(id: String, name: String, summary: String, relativePath: String, isApplied: Bool = false) {
        self.id = id
        self.name = name
        self.summary = summary
        self.relativePath = relativePath
        self.isApplied = isApplied
    }
}

public struct EnginePatchDefinition {
    public let id: String
    public let name: String
    public let summary: String
    public let relativePath: String
    public let checkToken: String
    public let applyTransform: (String) -> String?
    
    public init(
        id: String,
        name: String,
        summary: String,
        relativePath: String,
        checkToken: String,
        applyTransform: @escaping (String) -> String?
    ) {
        self.id = id
        self.name = name
        self.summary = summary
        self.relativePath = relativePath
        self.checkToken = checkToken
        self.applyTransform = applyTransform
    }
}

public enum EnginePatchRegistry {
    public static let patches: [EnginePatchDefinition] = [
        // 1. Dragon Quest VII v0 build definition in game.tsv
        EnginePatchDefinition(
            id: "dq7_game_tsv",
            name: "Dragon Quest VII v0 Metadata",
            summary: "Registers v1.0.0 (v0) package and hashes in game.tsv",
            relativePath: "dm/games/0100A9D01C446000/game.tsv",
            checkToken: "ab06020960edd8285b7ff5fad2e3224b",
            applyTransform: { content in
                let target = "build\tv131072\t1.2.0"
                let insert = "\nbuild\tv0\t1.0.0\tmain=ab06020960edd8285b7ff5fad2e3224b\tsubsdk0=92ef43cb1f9917ab206ac5c9e36240bc\tsdk=145dde94fb574acc9e09780a11995bd3"
                guard content.contains(target) else { return nil }
                return content.replacingOccurrences(of: target, with: target + insert)
            }
        ),
        
        // 2. Dragon Quest VII v0 allocator patches in patches.py
        EnginePatchDefinition(
            id: "dq7_patches_py",
            name: "Dragon Quest VII v0 Memory Allocator",
            summary: "Patches FMallocBinned3 constructor in patches.py",
            relativePath: "dm/games/0100A9D01C446000/patches.py",
            checkToken: "'ab06020960edd8285b7ff5fad2e3224b'",
            applyTransform: { content in
                var updated = content
                let cutsTarget = "CUTS = {"
                let cutsInsert = """
CUTS = {
  # DRAGON QUEST VII Reimagined 1.0.0 (v0) main
  'ab06020960edd8285b7ff5fad2e3224b': [
    (0x32755bc, '56704097', '1f2003d5', 'GCreateMalloc: bl 0x291714 (FMallocBinned3 constructor, 37 GB of address space) → nop; the host allocator\\'s vtable is put in at the gate after it (hle_uemalloc.c)'),
  ],
"""
                let obsTarget = "OBS = {"
                let obsInsert = """
OBS = {
  # DRAGON QUEST VII Reimagined 1.0.0 main
  'ab06020960edd8285b7ff5fad2e3224b': [
    (0x32755c4, 21, 'aa1303e0','GCreateMalloc: the allocator object is in x19, before it is used: the host puts its vtable in; orig mov x0, x19'),
  ],
"""
                guard updated.contains(cutsTarget) && updated.contains(obsTarget) else { return nil }
                updated = updated.replacingOccurrences(of: cutsTarget, with: cutsInsert)
                updated = updated.replacingOccurrences(of: obsTarget, with: obsInsert)
                return updated
            }
        ),
        
        // 3. Dragon Quest VII v0 UE allocator gate in hle_uemalloc.c
        EnginePatchDefinition(
            id: "dq7_uemalloc",
            name: "UE Allocator Gate (hle_uemalloc.c)",
            summary: "Hooks v0 allocator vtable at main+0x5e4c018",
            relativePath: "dm/host/hle_uemalloc.c",
            checkToken: "\"ab06020960edd8285b7ff5fad2e3224b\"",
            applyTransform: { content in
                let target = "{ \"f7ec03e4cb9de3bd8319d4e899d2e9cc\", \"DRAGON QUEST VII Reimagined 1.2.0\", UE4_27, 0, 0x5e4c0e8, 0x32758dc },"
                let insert = "\n    // DRAGON QUEST VII Reimagined 1.0.0 (v0)\n    { \"ab06020960edd8285b7ff5fad2e3224b\", \"DRAGON QUEST VII Reimagined 1.0.0\", UE4_27, 0, 0x5e4c018, 0x32755bc },"
                guard content.contains(target) else { return nil }
                return content.replacingOccurrences(of: target, with: target + insert)
            }
        ),
        
        // 4. Dragon Quest VII v0 60 FPS SyncInterval offset in hle_fpslock.c
        EnginePatchDefinition(
            id: "dq7_fpslock",
            name: "DQ VII 60 FPS Unlock (hle_fpslock.c)",
            summary: "Sets rhi.SyncInterval to correct offset 0x7dedf38 + 0x10",
            relativePath: "dm/host/hle_fpslock.c",
            checkToken: "{ \"ab06020960edd8285b7ff5fad2e3224b\", \"Unreal rhi.SyncInterval\", 0x7dedf38 + 0x10",
            applyTransform: { content in
                var text = content
                // If old erroneous offset exists, correct it in place
                let wrongOffset = "{ \"ab06020960edd8285b7ff5fad2e3224b\", \"Unreal rhi.SyncInterval\", 0x7ded858 + 0x10, 0, VSYNCS_I32, 2 }"
                let correctOffset = "{ \"ab06020960edd8285b7ff5fad2e3224b\", \"Unreal rhi.SyncInterval\", 0x7dedf38 + 0x10, 0, VSYNCS_I32, 2 }"
                if text.contains(wrongOffset) {
                    return text.replacingOccurrences(of: wrongOffset, with: correctOffset)
                }
                
                // If not present at all, append after v1.2.0 entry
                let anchor = "{ \"f7ec03e4cb9de3bd8319d4e899d2e9cc\", \"Unreal rhi.SyncInterval\", 0x7dedf38 + 0x10, 0, VSYNCS_I32, 2 },"
                let entries = """
\n    // DRAGON QUEST VII Reimagined 1.0.0 (v0): registered at main+0x4a61808 and main+0x36161b8
    { "ab06020960edd8285b7ff5fad2e3224b", "Unreal t.MaxFPS", 0x7f41908 + 0x10, 0, FPS_F32, 2 },
    { "ab06020960edd8285b7ff5fad2e3224b", "Unreal rhi.SyncInterval", 0x7dedf38 + 0x10, 0, VSYNCS_I32, 2 },
"""
                guard text.contains(anchor) else { return nil }
                return text.replacingOccurrences(of: anchor, with: anchor + entries)
            }
        ),
        
        // 5. Dynamic shader library fallback in nvn_mtl.m
        EnginePatchDefinition(
            id: "nvn_shader_fallback",
            name: "Dynamic Shader Chain Fallback (nvn_mtl.m)",
            summary: "Loads packaged metallib chains even if translation hash changed",
            relativePath: "dm/gfx/nvn/nvn_mtl.m",
            checkToken: "g_active_chain",
            applyTransform: { content in
                let target = "g_msl_dir = [NSString stringWithFormat:@\"%@/b-%s\", m ? @(m) : [NSString stringWithFormat:@\"%s%s/build/cache/metal_shader_cache\", getenv(\"DM_ROOT\") ?: getenv(\"HOME\"), getenv(\"DM_ROOT\") ? \"\" : \"/dm-work\"], DMSH_CHAIN];"
                let replacement = """
g_msl_dir = [NSString stringWithFormat:@"%@/b-%s", m ? @(m) : [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work"], DMSH_CHAIN];
    g_active_chain = @DMSH_CHAIN;
    if (![NSFileManager.defaultManager fileExistsAtPath:g_msl_dir] && m) {
        NSArray *sub = [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(m) error:nil];
        for (NSString *s in sub) {
            if ([s hasPrefix:@"b-"]) {
                NSString *candidate = [@(m) stringByAppendingPathComponent:s];
                BOOL isDir = NO;
                if ([NSFileManager.defaultManager fileExistsAtPath:candidate isDirectory:&isDir] && isDir) {
                    g_msl_dir = candidate;
                    g_active_chain = [s substringFromIndex:2];
                    MLOG("using packaged shader library directory %s (chain %s)", g_msl_dir.UTF8String, g_active_chain.UTF8String);
                    break;
                }
            }
        }
    }
"""
                let declTarget = "static NSString *g_shmap_path, *g_msl_dir;"
                let declRepl = "static NSString *g_shmap_path, *g_msl_dir, *g_active_chain;"
                
                guard content.contains(target) && content.contains(declTarget) else { return nil }
                var res = content.replacingOccurrences(of: declTarget, with: declRepl)
                res = res.replacingOccurrences(of: target, with: replacement)
                return res
            }
        ),
        
        // 6. Default Mac App naming in build_ipa_from_nsp.py
        EnginePatchDefinition(
            id: "mac_app_naming",
            name: "Mac App Output Naming (build_ipa_from_nsp.py)",
            summary: "Defaults Mac output to <Game>.app and optimizes buffer check",
            relativePath: "scripts/build_ipa_from_nsp.py",
            checkToken: "default_name = f'{mac_app_name(g.get(\"name\", g.id))}.app'",
            applyTransform: { content in
                let target = "out_ipa = Path(os.path.expanduser(args.out or OUT_IPA)).resolve() if (args.out or OUT_IPA) else nsp_dir / g.get('ipa')"
                let repl = """
if mac_pack:
        default_name = f'{mac_app_name(g.get("name", g.id))}.app'
        out_ipa = Path(os.path.expanduser(args.out or OUT_IPA)).resolve() if (args.out or OUT_IPA) else nsp_dir / default_name
    else:
        out_ipa = Path(os.path.expanduser(args.out or OUT_IPA)).resolve() if (args.out or OUT_IPA) else nsp_dir / (g.get('ipa') or f'{g.id}.ipa')
"""
                guard content.contains(target) else { return nil }
                return content.replacingOccurrences(of: target, with: repl)
            }
        )
    ]
}
