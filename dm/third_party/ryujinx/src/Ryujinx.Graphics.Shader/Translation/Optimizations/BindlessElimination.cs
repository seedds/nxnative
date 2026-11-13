using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    class BindlessElimination
    {
        private const int NvnTextureBufferIndex = 2;   // nxnative: as BindlessToArray, see TryTableHandle
        private const int NvnSlotArrayLength = 32;      // nxnative: the arrays of GenerateBindlessAccess that are not indexed in the pools
        private const int NvnSeparateSamplerBase = 0x568;   // nxnative: c[2]'s separate sampler table (dmsh_abi.h DMSH_C2_SAMP), 128 slots of 8 bytes; a word there is a sampler handle, never a texture's
        private const int NvnSeparateSamplerEnd = NvnSeparateSamplerBase + 8 * 128;

        // nxnative: deferSeveral leaves the reads whose handle merges several constant buffer handles to a second sweep (see TryTableHandle)
        public static void RunPass(BasicBlock block, ResourceManager resourceManager, IGpuAccessor gpuAccessor, bool deferSeveral = false)
        {
            // We can turn a bindless into regular access by recognizing the pattern
            // produced by the compiler for separate texture and sampler.
            // We check for the following conditions:
            // - The handle is a constant buffer value.
            // - The handle is the result of a bitwise OR logical operation.
            // - Both sources of the OR operation comes from a constant buffer.
            LinkedListNode<INode> nextNode;

            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = nextNode)
            {
                nextNode = node.Next;

                if (node.Value is not TextureOperation texOp)
                {
                    continue;
                }

                if ((texOp.Flags & TextureFlags.Bindless) == 0)
                {
                    continue;
                }

                if (TryConvertBindless(block, resourceManager, gpuAccessor, texOp))
                {
                    continue;
                }

                TableHandle table = TryTableHandle(block, resourceManager, gpuAccessor, texOp, node, deferSeveral);

                if (table == TableHandle.Deferred)
                {
                    continue;
                }

                bool converted = table == TableHandle.Set && TryConvertBindless(block, resourceManager, gpuAccessor, texOp);
                if (!converted && texOp.Inst == Instruction.ImageAtomic && texOp.Flags.HasFlag(TextureFlags.AtomicMetal))
                {
                    throw new NotSupportedException("Metal image atomics require a statically resolved image handle.");
                }

                if (!converted &&
                    !GenerateBindlessAccess(block, resourceManager, gpuAccessor, texOp, node))
                {
                    // If we can't do bindless elimination, remove the texture operation.
                    // Set any destination variables to zero.

                    string typeName = texOp.Inst.IsImage()
                        ? texOp.Type.GetGlslImageTypeName(texOp.Format.GetComponentType())
                        : texOp.Type.GlslTextureTypeName;

                    gpuAccessor.Log($"Failed to find handle source for bindless access of type \"{typeName}\".");

                    if (Environment.GetEnvironmentVariable("DMSH_BINDLESS_TRACE") != null)   // nxnative: what the handle is made of, for a diagnosis (dmsh_xlate on one job)
                    {
                        Console.Error.WriteLine($"bindless {typeName} handle: {Describe(texOp.GetSource(0), 0)}");
                    }

                    for (int destIndex = 0; destIndex < texOp.DestsCount; destIndex++)
                    {
                        block.Operations.AddBefore(node, new Operation(Instruction.Copy, texOp.GetDest(destIndex), OperandHelper.Const(0)));
                    }

                    Utils.DeleteNode(node, texOp);
                }
            }
        }

        // nxnative: NVN keeps every texture and image handle in c[2]. When a handle reaches the instruction through phis whose other inputs are not made of c[2]
        // words (undefined, or another value the compiler kept in the same register on a path where the predicated read does not run: float arithmetic, texture
        // results, uniform words), the read can only name a texture those c[2] words give. Unreal's Niagara vertex factories read particle attributes this way, under a
        // predicate (float or half data): GenerateBindlessAccess refuses such a phi, so the read was removed and its result 0 (58,122 reads, 3,197 of FINAL
        // FANTASY RESONANCE's shaders), and its fire and smoke meshes collapsed to a point. Only those phis come here: one GenerateBindlessAccess takes keeps its
        // path. One such handle takes the phi's place. Several (the float and the half buffer reaching one read) give one read each, before this one, and the result is the
        // read whose handle equals the phi's value at run time (the host fills c[2] with the bound handles, or with distinct slot indices for dynamic handle arrays);
        // when none does, the path that got there does not use the result, and the last one is taken. The caller converts this operation as usual.
        // Every use of a handle shares one binding, typed by the instructions that read it, so a candidate that other reads use as a texture of another shape (a 2D array
        // where this read is 2D: two of Unreal's global pixel shaders in FINAL FANTASY RESONANCE) would make the read invalid and is left out. Those reads are converted in a
        // second sweep (deferSeveral), once every other read has registered its texture.
        // A game's own uniform buffer holds other data than handles, so a word of it alone is no handle here, but a texture word ORed with a sampler word of its bank or a constant sampler is one: Unreal
        // Engine 5.4 on the Switch 2 keeps its handles in c[3] and builds them under a guard (R = c[3][a]; R |= c[3][b]) for a read under a guard Utils.FindLastOperation cannot tie to it: !UP0
        // and !P0 with UP0 the vote of P0, P4 and P5 with P5 = P4 && !P1, and P6 and a branch over !P6. The phi then also takes the handle's first word and what the register held before.
        // With the OR unguarded the phi lies on its source, not on the handle.
        // A handle whose words the shader loads from memory on some path (a Load, or a global load GlobalToStorage served through a helper: IsLoaded) is a real
        // bindless access, also as a source of the OR, for a read GenerateBindlessAccess indexes the pools for (pooled: a sampled texture other than a buffer texture): that reads the
        // texture the handle names whatever its source. Other reads keep their candidates for those leaves (their slot arrays take c[2]'s handles only). PRAGMATA's
        // compute shader 02e1292114121308 reads its texture with (an ID from a material record, through the pool table) | constant sampler 0x104, under the guard that loads the ID; the
        // register held c[24] + 0x114 (a pushed buffer address's high word) on the other path, and that word alone was taken as the handle: the host had no slot for it and the dispatch ran
        // without its shader
        private enum TableHandle { None, Set, Deferred }

        // A global load GlobalToStorage served is a copy that keeps the load's storage kind, of its helper's result (or of 0 where it dropped the load): Copy.GlobalMemory(Call)
        private static bool IsLoaded(Operand leaf) => leaf.AsgOp is Operation op && (op.Inst is Instruction.Load or Instruction.Call || (op.Inst == Instruction.Copy && op.StorageKind != StorageKind.None));

        private static bool IsPooled(TextureOperation texOp) => !texOp.Inst.IsImage() && (texOp.Type & SamplerType.Mask) != SamplerType.TextureBuffer;   // as GenerateBindlessAccess

        private static TableHandle TryTableHandle(BasicBlock block, ResourceManager resourceManager, IGpuAccessor gpuAccessor, TextureOperation texOp, LinkedListNode<INode> node, bool deferSeveral)
        {
            Operand handle = texOp.GetSource(0);

            if (handle.AsgOp is not PhiNode and not Operation { Inst: Instruction.BitwiseOr } || IsBindlessHandleAllowed(handle))
            {
                return TableHandle.None;   // GenerateBindlessAccess takes it, as before (a choice among c[2] words: the dynamic handle arrays of tex_dyn_slots)
            }

            List<(Operand Texture, Operand Sampler)> found = new();

            foreach (Operand leaf in PhiLeaves(handle))
            {
                if (IsTableWord(leaf))
                {
                    AddCandidate(found, leaf, null);
                }
                else if (!texOp.Inst.IsImage() && leaf.AsgOp is Operation orOp && orOp.Inst == Instruction.BitwiseOr)
                {
                    List<Operand> textures = PhiLeaves(orOp.GetSource(0)), samplers = PhiLeaves(orOp.GetSource(1));

                    if (IsPooled(texOp) && (textures.Exists(IsLoaded) || samplers.Exists(IsLoaded)))
                    {
                        return TableHandle.None;   // loaded from memory on some path (above)
                    }

                    foreach (Operand texture in textures)
                    {
                        foreach (Operand sampler in samplers)
                        {
                            (Operand t, Operand sm) = IsSamplerWord(texture) && !IsSamplerWord(sampler) ? (sampler, texture) : (texture, sampler);   // either order, as TryConvertBindless

                            if (t.Type == OperandType.ConstantBuffer && !IsSamplerWord(t) && (IsSameBankWord(sm, t) || (sm.Type == OperandType.Constant && (sm.Value & 0xfffff) == 0)))
                            {
                                AddCandidate(found, t, sm);
                            }
                        }
                    }
                }
                else if (leaf.AsgOp is Operation { Inst: Instruction.Load } || (IsPooled(texOp) && IsLoaded(leaf)))
                {
                    return TableHandle.None;   // a handle loaded from memory or an input is a real bindless access (GenerateBindlessAccess)
                }
            }

            // A word alone that an ORed candidate also takes is that handle on its way (R = word; R |= other word), not a handle of its own
            found = found.FindAll(candidate => candidate.Sampler != null || !found.Exists(other => other.Sampler != null && (other.Texture.Value == candidate.Texture.Value || other.Sampler.Value == candidate.Texture.Value && other.Sampler.Type == OperandType.ConstantBuffer)));

            if (found.Count > 1)
            {
                if (texOp.Inst.IsImageStore())
                {
                    return TableHandle.None;   // one operation per candidate would store into every candidate
                }

                if (deferSeveral)
                {
                    return TableHandle.Deferred;
                }

                found = found.FindAll(candidate => resourceManager.GetTextureTypes(candidate.Texture.GetCbufSlot(), candidate.Texture.GetCbufOffset(), texOp.Inst.IsImage())
                    .TrueForAll(type => SameShape(type, texOp.Type)));
            }

            if (found.Count == 0)
            {
                return TableHandle.None;
            }

            int last = found.Count - 1;
            Operand[] matches = new Operand[last];
            Operand[][] results = new Operand[last][];

            for (int index = 0; index < last; index++)
            {
                Operand candidate = CandidateHandle(block, node, found[index]);
                Operand[] sources = new Operand[texOp.SourcesCount];
                results[index] = new Operand[texOp.DestsCount];

                for (int srcIndex = 0; srcIndex < sources.Length; srcIndex++)
                {
                    sources[srcIndex] = srcIndex == 0 ? candidate : texOp.GetSource(srcIndex);
                }

                for (int destIndex = 0; destIndex < results[index].Length; destIndex++)
                {
                    results[index][destIndex] = OperandHelper.Local();
                }

                TextureOperation read = new(texOp.Inst, texOp.Type, texOp.Format, texOp.Flags, texOp.Set, texOp.Binding, texOp.Index, results[index], sources);
                LinkedListNode<INode> readNode = block.Operations.AddBefore(node, read);

                if (!TryConvertBindless(block, resourceManager, gpuAccessor, read))
                {
                    for (int destIndex = 0; destIndex < results[index].Length; destIndex++)
                    {
                        block.Operations.AddBefore(readNode, new Operation(Instruction.Copy, results[index][destIndex], OperandHelper.Const(0)));
                    }

                    Utils.DeleteNode(readNode, read);
                }

                matches[index] = OperandHelper.Local();
                block.Operations.AddBefore(node, new Operation(Instruction.CompareEqual, matches[index], handle, candidate));
            }

            texOp.SetSource(0, CandidateHandle(block, node, found[last]));

            LinkedListNode<INode> after = node;

            for (int destIndex = 0; destIndex < texOp.DestsCount && last > 0; destIndex++)
            {
                Operand dest = texOp.GetDest(destIndex);
                Operand value = OperandHelper.Local();
                texOp.SetDest(destIndex, value);

                for (int index = last - 1; index >= 0; index--)
                {
                    Operand selected = index == 0 ? dest : OperandHelper.Local();
                    after = block.Operations.AddAfter(after, new Operation(Instruction.ConditionalSelect, selected, matches[index], results[index][destIndex], value));
                    value = selected;
                }
            }

            gpuAccessor.Log(found.Count == 1
                ? "Bindless handle taken from the one constant buffer handle among its phi inputs."
                : $"Bindless handle chosen at run time among {found.Count} constant buffer handles of its phi inputs.");

            if (Environment.GetEnvironmentVariable("DMSH_BINDLESS_TRACE") != null)   // nxnative: what the handle was made of, for a diagnosis (dmsh_xlate on one job)
            {
                Console.Error.WriteLine($"bindless handle from {found.Count} phi candidate(s): {Describe(handle, 0)}");
            }

            return TableHandle.Set;
        }

        // Whether a texture of the given type can be read as this instruction's (a 1D texture and a texture buffer are told apart only by the descriptor)
        private static bool SameShape(SamplerType type, SamplerType opType)
        {
            const SamplerType Shape = SamplerType.Mask | SamplerType.Array | SamplerType.Multisample;

            static SamplerType Of(SamplerType t)
            {
                t &= Shape;

                return t == SamplerType.TextureBuffer ? SamplerType.Texture1D : t;
            }

            return Of(type) == Of(opType);
        }

        private static Operand CandidateHandle(BasicBlock block, LinkedListNode<INode> node, (Operand Texture, Operand Sampler) candidate)
        {
            if (candidate.Sampler == null)
            {
                return candidate.Texture;
            }

            Operand combined = OperandHelper.Local();
            block.Operations.AddBefore(node, new Operation(Instruction.BitwiseOr, combined, candidate.Texture, candidate.Sampler));

            return combined;
        }

        private static bool IsTableWord(Operand operand)   // a c[2] word that can name a texture (not one of the separate sampler table: a sampler word alone, on a path the read does not take, is no candidate)
        {
            return operand.Type == OperandType.ConstantBuffer && operand.GetCbufSlot() == NvnTextureBufferIndex && !IsSamplerWord(operand);
        }

        private static bool IsSamplerWord(Operand operand)
        {
            if (operand.Type != OperandType.ConstantBuffer || operand.GetCbufSlot() != NvnTextureBufferIndex) return false;
            int offset = operand.GetCbufOffset() * 4;
            return offset >= NvnSeparateSamplerBase && offset < NvnSeparateSamplerEnd;
        }

        private static bool IsSameBankWord(Operand operand, Operand word)
        {
            return operand.Type == OperandType.ConstantBuffer && operand.GetCbufSlot() == word.GetCbufSlot();
        }

        private static void AddCandidate(List<(Operand Texture, Operand Sampler)> found, Operand texture, Operand sampler)
        {
            foreach ((Operand t, Operand s) in found)
            {
                if (t.Value == texture.Value && (s == null ? sampler == null : sampler != null && s.Type == sampler.Type && s.Value == sampler.Value))
                {
                    return;
                }
            }

            found.Add((texture, sampler));
        }

        // nxnative: an operand as text for DMSH_BINDLESS_TRACE: constant buffer words, constants, and operations with their sources, four levels deep
        private static string Describe(Operand o, int depth)
        {
            if (o == null) return "null";
            switch (o.Type)
            {
                case OperandType.ConstantBuffer: return $"c[{o.GetCbufSlot()}][0x{o.GetCbufOffset() * 4:x}]";
                case OperandType.Constant: return $"0x{o.Value:x}";
                case OperandType.Undefined: return "undefined";
                case OperandType.Register: return $"r{o.Value}";
            }
            if (depth > 4) return "...";
            if (o.AsgOp is PhiNode phi)
            {
                List<string> parts = new();
                for (int i = 0; i < phi.SourcesCount; i++) parts.Add(Describe(phi.GetSource(i), depth + 1));
                return "phi(" + string.Join(", ", parts) + ")";
            }
            if (o.AsgOp is Operation op)
            {
                List<string> parts = new();
                for (int i = 0; i < op.SourcesCount; i++) parts.Add(Describe(op.GetSource(i), depth + 1));
                string kind = op.StorageKind != StorageKind.None ? "." + op.StorageKind : "";
                return $"{op.Inst}{kind}(" + string.Join(", ", parts) + ")";
            }
            return o.Type.ToString();
        }

        // The values a phi can take, through the phis it merges (each visited once).
        private static List<Operand> PhiLeaves(Operand operand)
        {
            List<Operand> leaves = new();
            HashSet<PhiNode> visited = new();
            Stack<Operand> pending = new();
            pending.Push(operand);

            while (pending.Count > 0)
            {
                Operand current = pending.Pop();

                if (current.AsgOp is PhiNode phi)
                {
                    if (visited.Add(phi))
                    {
                        for (int index = 0; index < phi.SourcesCount; index++)
                        {
                            pending.Push(phi.GetSource(index));
                        }
                    }
                }
                else
                {
                    leaves.Add(current);
                }
            }

            return leaves;
        }

        private static bool GenerateBindlessAccess(
            BasicBlock block,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TextureOperation texOp,
            LinkedListNode<INode> node)
        {
            if (!gpuAccessor.QueryHostSupportsSeparateSampler())
            {
                // We depend on combining samplers and textures in the shader being supported for this.

                return false;
            }

            Operand bindlessHandle = texOp.GetSource(0);

            // nxnative: a sampled texture other than a buffer texture is indexed in the game's whole texture and sampler pools (the replay binds them by ID: dmsh_build.py pool_arrays,
            // nvn_mtl.m pool_gpu), so its arrays take the pool lengths. Buffer textures and images keep arrays of NvnSlotArrayLength, which the replay binds slot by slot (tex_dyn_slots)
            bool pooled = !texOp.Inst.IsImage() && (texOp.Type & SamplerType.Mask) != SamplerType.TextureBuffer;

            // nxnative: a pooled read takes its handle from any source. Its texture ID indexes the pool as the GPU's does, so where the handle comes from does not matter; a handle the
            // shader computes (RE Engine: a heap index from game memory or a uniform buffer, clamped to its range and ORed with a sampler ID) was deleted and read 0 before.
            // The slot arrays of images and buffer textures keep Ryujinx's limit: the replay binds them only for handles from c2's tables
            if (!pooled && !IsBindlessHandleAllowed(bindlessHandle))
            {
                return false;
            }

            Operand textureHandle = OperandHelper.Local();
            Operand samplerHandle = OperandHelper.Local();
            Operand textureIndex = OperandHelper.Local();

            block.Operations.AddBefore(node, new Operation(Instruction.BitwiseAnd, textureHandle, bindlessHandle, OperandHelper.Const(0xfffff)));
            block.Operations.AddBefore(node, new Operation(Instruction.ShiftRightU32, samplerHandle, bindlessHandle, OperandHelper.Const(20)));

            int texturePoolLength = pooled ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QueryTextureArrayLengthFromPool()) : NvnSlotArrayLength;

            block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, textureIndex, textureHandle, OperandHelper.Const(texturePoolLength - 1)));

            texOp.SetSource(0, textureIndex);

            bool hasSampler = !texOp.Inst.IsImage();

            SetBindingPair textureSetAndBinding = resourceManager.GetTextureOrImageBinding(
                texOp.Inst,
                texOp.Type,
                texOp.Format,
                texOp.Flags & ~TextureFlags.Bindless,
                0,
                TextureHandle.PackOffsets(0, 0, TextureHandleType.Direct),
                texturePoolLength,
                hasSampler);

            if (hasSampler)
            {
                Operand samplerIndex = OperandHelper.Local();

                int samplerPoolLength = pooled ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QuerySamplerArrayLengthFromPool()) : NvnSlotArrayLength;

                block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, samplerIndex, samplerHandle, OperandHelper.Const(samplerPoolLength - 1)));

                texOp.InsertSource(1, samplerIndex);

                SetBindingPair samplerSetAndBinding = resourceManager.GetTextureOrImageBinding(
                    texOp.Inst,
                    SamplerType.None,
                    texOp.Format,
                    TextureFlags.None,
                    0,
                    TextureHandle.PackOffsets(0, 0, TextureHandleType.Direct),
                    samplerPoolLength);

                texOp.TurnIntoArray(textureSetAndBinding, samplerSetAndBinding);
            }
            else
            {
                texOp.TurnIntoArray(textureSetAndBinding);
            }

            return true;
        }

        private static bool IsBindlessHandleAllowed(Operand bindlessHandle)
        {
            if (bindlessHandle.AsgOp is PhiNode phi)
            {
                for (int srcIndex = 0; srcIndex < phi.SourcesCount; srcIndex++)
                {
                    Operand phiSource = phi.GetSource(srcIndex);

                    if (phiSource.AsgOp is not PhiNode && !IsBindlessAccessAllowed(phiSource))
                    {
                        return false;
                    }
                }

                return true;
            }

            return IsBindlessAccessAllowed(bindlessHandle);
        }

        private static bool IsBindlessAccessAllowed(Operand bindlessHandle)
        {
            if (bindlessHandle.Type == OperandType.ConstantBuffer)
            {
                // Bindless access with handles from constant buffer is allowed.

                return true;
            }

            if (bindlessHandle.AsgOp is not Operation handleOp ||
                handleOp.Inst != Instruction.Load ||
                (handleOp.StorageKind != StorageKind.Input && handleOp.StorageKind != StorageKind.StorageBuffer))
            {
                // Right now, we only allow bindless access when the handle comes from a shader input or storage buffer.
                // This is an artificial limitation to prevent it from being used in cases where it
                // would have a large performance impact of loading all textures in the pool.
                // It might be removed in the future, if we can mitigate the performance impact.

                return false;
            }

            return true;
        }

        private static bool TryConvertBindless(BasicBlock block, ResourceManager resourceManager, IGpuAccessor gpuAccessor, TextureOperation texOp)
        {
            if (texOp.Inst == Instruction.TextureSample || texOp.Inst.IsTextureQuery())
            {
                Operand bindlessHandle = texOp.GetSource(0);

                // In some cases the compiler uses a shuffle operation to get the handle,
                // for some textureGrad implementations. In those cases, we can skip the shuffle.
                if (bindlessHandle.AsgOp is Operation shuffleOp && shuffleOp.Inst == Instruction.Shuffle)
                {
                    bindlessHandle = shuffleOp.GetSource(0);
                }

                bindlessHandle = Utils.FindLastOperation(bindlessHandle, block);

                // Some instructions do not encode an accurate sampler type:
                // - Most instructions uses the same type for 1D and Buffer.
                // - Query instructions may not have any type.
                // For those cases, we need to try getting the type from current GPU state,
                // as long bindless elimination is successful and we know where the texture descriptor is located.
                bool rewriteSamplerType =
                    texOp.Type == SamplerType.TextureBuffer ||
                    texOp.Inst == Instruction.TextureQuerySamples ||
                    texOp.Inst == Instruction.TextureQuerySize;

                if (bindlessHandle.Type == OperandType.ConstantBuffer)
                {
                    SetHandle(
                        resourceManager,
                        gpuAccessor,
                        texOp,
                        bindlessHandle.GetCbufOffset(),
                        bindlessHandle.GetCbufSlot(),
                        rewriteSamplerType,
                        isImage: false);

                    return true;
                }

                if (!TryGetOperation(bindlessHandle.AsgOp, out Operation handleCombineOp))
                {
                    return false;
                }

                if (handleCombineOp.Inst != Instruction.BitwiseOr)
                {
                    return false;
                }

                Operand src0 = Utils.FindLastOperation(handleCombineOp.GetSource(0), block);
                Operand src1 = Utils.FindLastOperation(handleCombineOp.GetSource(1), block);

                // For cases where we have a constant, ensure that the constant is always
                // the second operand.
                // Since this is a commutative operation, both are fine,
                // and having a "canonical" representation simplifies some checks below.
                if (src0.Type == OperandType.Constant && src1.Type != OperandType.Constant)
                {
                    (src0, src1) = (src1, src0);
                }

                // nxnative: the compiler ORs the two words in either order (Star Fox: the sampler word first in 62 pixel shaders, which named the sampler's word as the texture,
                // a binding with no slot). A word in c[2]'s separate sampler table is the sampler, the other the texture
                if (IsSamplerWord(src0) && !IsSamplerWord(src1))
                {
                    (src0, src1) = (src1, src0);
                }

                TextureHandleType handleType = TextureHandleType.SeparateSamplerHandle;

                // Try to match the following patterns:
                // Masked pattern:
                //  - samplerHandle = samplerHandle & 0xFFF00000;
                //  - textureHandle = textureHandle & 0xFFFFF;
                //  - combinedHandle = samplerHandle | textureHandle;
                //  Where samplerHandle and textureHandle comes from a constant buffer.
                // Shifted pattern:
                //  - samplerHandle = samplerId << 20;
                //  - combinedHandle = samplerHandle | textureHandle;
                //  Where samplerId and textureHandle comes from a constant buffer.
                // Constant pattern:
                //  - combinedHandle = samplerHandleConstant | textureHandle;
                //  Where samplerHandleConstant is a constant value, and textureHandle comes from a constant buffer.
                if (src0.AsgOp is Operation src0AsgOp)
                {
                    if (src1.AsgOp is Operation src1AsgOp &&
                        src0AsgOp.Inst == Instruction.BitwiseAnd &&
                        src1AsgOp.Inst == Instruction.BitwiseAnd)
                    {
                        src0 = GetSourceForMaskedHandle(src0AsgOp, 0xFFFFF);
                        src1 = GetSourceForMaskedHandle(src1AsgOp, 0xFFF00000);

                        // The OR operation is commutative, so we can also try to swap the operands to get a match.
                        if (src0 == null || src1 == null)
                        {
                            src0 = GetSourceForMaskedHandle(src1AsgOp, 0xFFFFF);
                            src1 = GetSourceForMaskedHandle(src0AsgOp, 0xFFF00000);
                        }

                        if (src0 == null || src1 == null)
                        {
                            return false;
                        }
                    }
                    else if (src0AsgOp.Inst == Instruction.ShiftLeft)
                    {
                        Operand shift = src0AsgOp.GetSource(1);

                        if (shift.Type == OperandType.Constant && shift.Value == 20)
                        {
                            src0 = src1;
                            src1 = src0AsgOp.GetSource(0);
                            handleType = TextureHandleType.SeparateSamplerId;
                        }
                    }
                }
                else if (src1.AsgOp is Operation src1AsgOp && src1AsgOp.Inst == Instruction.ShiftLeft)
                {
                    Operand shift = src1AsgOp.GetSource(1);

                    if (shift.Type == OperandType.Constant && shift.Value == 20)
                    {
                        src1 = src1AsgOp.GetSource(0);
                        handleType = TextureHandleType.SeparateSamplerId;
                    }
                }
                else if (src1.Type == OperandType.Constant && (src1.Value & 0xfffff) == 0)
                {
                    handleType = TextureHandleType.SeparateConstantSamplerHandle;
                }

                if (src0.Type != OperandType.ConstantBuffer)
                {
                    return false;
                }

                if (handleType == TextureHandleType.SeparateConstantSamplerHandle)
                {
                    SetHandle(
                        resourceManager,
                        gpuAccessor,
                        texOp,
                        TextureHandle.PackOffsets(src0.GetCbufOffset(), (src1.Value >> 20) & 0xfff, handleType),
                        TextureHandle.PackSlots(src0.GetCbufSlot(), 0),
                        rewriteSamplerType,
                        isImage: false);

                    return true;
                }
                else if (src1.Type == OperandType.ConstantBuffer)
                {
                    SetHandle(
                        resourceManager,
                        gpuAccessor,
                        texOp,
                        TextureHandle.PackOffsets(src0.GetCbufOffset(), src1.GetCbufOffset(), handleType),
                        TextureHandle.PackSlots(src0.GetCbufSlot(), src1.GetCbufSlot()),
                        rewriteSamplerType,
                        isImage: false);

                    return true;
                }
            }
            else if (texOp.Inst.IsImage())
            {
                Operand src0 = Utils.FindLastOperation(texOp.GetSource(0), block);

                if (src0.Type == OperandType.ConstantBuffer)
                {
                    int cbufOffset = src0.GetCbufOffset();
                    int cbufSlot = src0.GetCbufSlot();

                    if (texOp.Format == TextureFormat.Unknown)
                    {
                        if (texOp.Inst == Instruction.ImageAtomic)
                        {
                            texOp.Format = ShaderProperties.GetTextureFormatAtomic(gpuAccessor, cbufOffset, cbufSlot,
                                strict: texOp.Flags.HasFlag(TextureFlags.AtomicMetal));
                        }
                        else
                        {
                            texOp.Format = ShaderProperties.GetTextureFormat(gpuAccessor, cbufOffset, cbufSlot);
                        }
                    }

                    bool rewriteSamplerType = texOp.Type == SamplerType.TextureBuffer;

                    SetHandle(resourceManager, gpuAccessor, texOp, cbufOffset, cbufSlot, rewriteSamplerType, isImage: true);

                    return true;
                }
            }

            return false;
        }

        private static bool TryGetOperation(INode asgOp, out Operation outOperation)
        {
            if (asgOp is PhiNode phi)
            {
                // If we have a phi, let's check if all inputs are effectively the same value.
                // If so, we can "see through" the phi and pick any of the inputs (since they are all the same).
                // nxnative: the inputs are gathered through the phis they come from, as a loop makes (a handle kept in a loop variable
                // reaches the phi from the loop's own phi). Upstream compared the phi's direct inputs only and gave up on such handles.

                Operand firstSrc = null;
                HashSet<PhiNode> visited = new();
                Stack<PhiNode> pending = new();
                pending.Push(phi);

                while (pending.Count > 0)
                {
                    PhiNode current = pending.Pop();

                    if (!visited.Add(current))
                    {
                        continue;
                    }

                    for (int index = 0; index < current.SourcesCount; index++)
                    {
                        Operand src = current.GetSource(index);

                        if (src.AsgOp is PhiNode srcPhi)
                        {
                            pending.Push(srcPhi);
                        }
                        else if (firstSrc == null)
                        {
                            firstSrc = src;
                        }
                        else if (!IsSameOperand(firstSrc, src))
                        {
                            outOperation = null;

                            return false;
                        }
                    }
                }

                if (firstSrc == null)
                {
                    outOperation = null;

                    return false;
                }

                asgOp = firstSrc.AsgOp;
            }

            if (asgOp is Operation operation)
            {
                outOperation = operation;

                return true;
            }

            outOperation = null;

            return false;
        }

        private static bool IsSameOperand(Operand x, Operand y)
        {
            if (x == y)
            {
                return true;
            }

            if (x.Type == y.Type && x.Type == OperandType.LocalVariable)
            {
                // nxnative: the second source may also be the same constant (a constant sampler ORed into a texture handle).
                return x.AsgOp is Operation xOp &&
                    y.AsgOp is Operation yOp &&
                    xOp.Inst == Instruction.BitwiseOr &&
                    yOp.Inst == Instruction.BitwiseOr &&
                    AreBothEqualConstantBuffers(xOp.GetSource(0), yOp.GetSource(0)) &&
                    (AreBothEqualConstantBuffers(xOp.GetSource(1), yOp.GetSource(1)) || AreBothEqualConstants(xOp.GetSource(1), yOp.GetSource(1)));
            }

            return false;
        }

        private static bool AreBothEqualConstantBuffers(Operand x, Operand y)
        {
            return x.Type == y.Type && x.Value == y.Value && x.Type == OperandType.ConstantBuffer;
        }

        private static bool AreBothEqualConstants(Operand x, Operand y)
        {
            return x.Type == y.Type && x.Value == y.Value && x.Type == OperandType.Constant;
        }

        private static Operand GetSourceForMaskedHandle(Operation asgOp, uint mask)
        {
            // Assume it was already checked that the operation is bitwise AND.

            Operand src0 = asgOp.GetSource(0);
            Operand src1 = asgOp.GetSource(1);

            if (src0.Type == OperandType.ConstantBuffer && src1.Type == OperandType.ConstantBuffer)
            {
                // We can't check if the mask matches here as both operands are from a constant buffer.
                // Be optimistic and assume it matches. Avoid constant buffer 1 as official drivers
                // uses this one to store compiler constants.

                return src0.GetCbufSlot() == 1 ? src1 : src0;
            }
            else if (src0.Type == OperandType.ConstantBuffer && src1.Type == OperandType.Constant)
            {
                if ((uint)src1.Value == mask)
                {
                    return src0;
                }
            }
            else if (src0.Type == OperandType.Constant && src1.Type == OperandType.ConstantBuffer)
            {
                if ((uint)src0.Value == mask)
                {
                    return src1;
                }
            }

            return null;
        }

        private static void SetHandle(
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TextureOperation texOp,
            int cbufOffset,
            int cbufSlot,
            bool rewriteSamplerType,
            bool isImage)
        {
            if (rewriteSamplerType)
            {
                SamplerType newType = gpuAccessor.QuerySamplerType(cbufOffset, cbufSlot);

                if (texOp.Inst.IsTextureQuery())
                {
                    texOp.Type = newType;
                }
                else if (texOp.Type == SamplerType.TextureBuffer && newType == SamplerType.Texture1D)
                {
                    int coordsCount = 2;

                    if (InstEmit.Sample1DAs2D)
                    {
                        newType = SamplerType.Texture2D;
                        texOp.InsertSource(coordsCount++, OperandHelper.Const(0));
                    }

                    if (!isImage &&
                        (texOp.Flags & TextureFlags.IntCoords) != 0 &&
                        (texOp.Flags & TextureFlags.LodLevel) == 0)
                    {
                        // IntCoords textures must always have explicit LOD.
                        texOp.SetLodLevelFlag();
                        texOp.InsertSource(coordsCount, OperandHelper.Const(0));
                    }

                    texOp.Type = newType;
                }
            }

            SetBindingPair setAndBinding = resourceManager.GetTextureOrImageBinding(
                texOp.Inst,
                texOp.Type,
                texOp.Format,
                texOp.Flags & ~TextureFlags.Bindless,
                cbufSlot,
                cbufOffset);

            texOp.SetBinding(setAndBinding);
        }
    }
}
