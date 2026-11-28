using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;

namespace Ryujinx.Graphics.Shader.Translation
{
    public enum RayTracingStage { RayGeneration = 0, ClosestHit = 7, Miss = 8 }

    // Addresses describe the caller's immutable linked input image. Logical IDs
    // belong to this backend, not to an RTCore build or a game's function names.
    public readonly record struct RayTracingEntry(
        ulong Address, int LogicalId, int OwnerId, RayTracingStage Stage, int FrameBytes);
    public readonly record struct RayTracingModuleRange(ulong Address, int Length);
    public readonly record struct RayTracingLogicalMove(ulong Address, ulong OriginalLowWord, int LogicalId);

    public sealed class RayTracingProgram
    {
        // Scene, dispatch parameters, fault report and immutable instance SBT
        // words. The accessor allocates native resources after these bindings.
        public const int ReservedBindings = 5; // Includes the translator's support buffer at0.
        public const int SceneBinding = 1, ParametersBinding = 2, ErrorsBinding = 3, InstanceSbtBinding = 4;
        public const int ImageClassBase = 16, ImageClassCount = 8, MembershipBase = ImageClassBase + ImageClassCount;
        public IReadOnlyList<RayTracingEntry> Entries { get; }
        public IReadOnlyList<RayTracingModuleRange> Modules { get; }
        public IReadOnlyDictionary<ulong, RayTracingLogicalMove> LogicalMoves { get; }
        public int LocalMemoryBytes { get; }
        public int LocalMemoryBias => 0x10000;
        public int CoreOffset { get; }
        public int DescriptorOffset { get; }

        public RayTracingProgram(IReadOnlyList<RayTracingEntry> entries,
                                 IReadOnlyList<RayTracingModuleRange> modules,
                                 IReadOnlyList<RayTracingLogicalMove> logicalMoves,
                                 int coreOffset = 0, int descriptorOffset = 0x140)
        {
            if (coreOffset < 0 || (coreOffset & 63) != 0 || descriptorOffset < 0 || (descriptorOffset & 15) != 0 ||
                (long)coreOffset + 0x140 > 0x10000 || (long)descriptorOffset + 0x1430 > 0x10000 ||
                (coreOffset < (long)descriptorOffset + 0x1430 && descriptorOffset < (long)coreOffset + 0x140))
                throw new ArgumentException("RT runtime resources need aligned, disjoint constant-bank extents.");
            CoreOffset = coreOffset; DescriptorOffset = descriptorOffset;
            if (entries == null || modules == null || logicalMoves == null || entries.Count == 0 || modules.Count == 0)
                throw new ArgumentException("A ray-tracing program needs entry points and bounded code modules.");
            RayTracingEntry[] functions = entries.ToArray();
            RayTracingModuleRange[] ranges = modules.OrderBy(x => x.Address).ToArray();
            Dictionary<int, RayTracingEntry> ids = new();
            HashSet<ulong> addresses = new();
            foreach (RayTracingModuleRange module in ranges)
            {
                if ((module.Address & 15) != 0 || module.Length <= 0 || (module.Length & 15) != 0 ||
                    module.Address > ulong.MaxValue - (ulong)module.Length)
                    throw new ArgumentException("Unaligned, empty or overflowing RT code module.");
            }
            for (int i = 1; i < ranges.Length; i++)
                if (ranges[i - 1].Address + (ulong)ranges[i - 1].Length > ranges[i].Address)
                    throw new ArgumentException("RT code modules overlap.");
            bool Inside(ulong address) => ranges.Any(x => address >= x.Address && address - x.Address < (ulong)x.Length);
            foreach (RayTracingEntry entry in functions)
            {
                if ((entry.Address & 15) != 0 || !Inside(entry.Address) ||
                    entry.LogicalId <= 0 || entry.LogicalId >= 0x8000 || !ids.TryAdd(entry.LogicalId, entry) ||
                    !addresses.Add(entry.Address) || entry.FrameBytes < 0 || entry.FrameBytes > 0x10000 ||
                    entry.Stage is not (RayTracingStage.RayGeneration or RayTracingStage.ClosestHit or RayTracingStage.Miss))
                    throw new ArgumentException("Invalid or duplicate RT entry point.");
            }
            foreach (RayTracingEntry entry in functions)
                if (!ids.TryGetValue(entry.OwnerId, out RayTracingEntry owner) || owner.OwnerId != owner.LogicalId ||
                    owner.Stage != entry.Stage || owner.FrameBytes != entry.FrameBytes ||
                    (entry.OwnerId != entry.LogicalId && entry.Stage != RayTracingStage.RayGeneration))
                    throw new ArgumentException("Invalid RT primary/continuation ownership.");
            if (!functions.Any(x => x.Stage == RayTracingStage.RayGeneration && x.OwnerId == x.LogicalId))
                throw new ArgumentException("An RT program has no ray-generation entry.");
            Dictionary<ulong, RayTracingLogicalMove> moves = new();
            foreach (RayTracingLogicalMove move in logicalMoves)
            {
                if (!Inside(move.Address) || (move.Address & 15) != 0 || !ids.ContainsKey(move.LogicalId) ||
                    move.OriginalLowWord >= 1ul << 32 || (move.OriginalLowWord & 0xfff) is not (0x802 or 0x882) ||
                    !moves.TryAdd(move.Address, move))
                    throw new ArgumentException("Unsupported or duplicate logical-ID MOV relocation.");
            }
            Entries = Array.AsReadOnly(functions);
            Modules = Array.AsReadOnly(ranges);
            LogicalMoves = new ReadOnlyDictionary<ulong, RayTracingLogicalMove>(moves);
            // Depth-one traversal: a raygen frame plus a closest/miss frame.
            // Metal owns traversal state; no NVIDIA traversal-helper frame is
            // silently charged to the guest's independently checked arena.
            int raygen = functions.Where(x => x.Stage == RayTracingStage.RayGeneration).Max(x => x.FrameBytes);
            int callback = functions.Where(x => x.Stage != RayTracingStage.RayGeneration).Select(x => x.FrameBytes).DefaultIfEmpty().Max();
            LocalMemoryBytes = checked((Math.Max(32, raygen + callback) + 31) & ~31);
            if (LocalMemoryBytes > 0x10000) throw new ArgumentException("RT continuation arena exceeds 64 KiB.");
        }
    }
}
