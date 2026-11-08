using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System.Collections.Generic;
using System.Numerics;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    // Ampere attribute, memory and texture instructions. Field positions: Mesa NAK sm70_encode.rs (OpIpa, OpALd, OpASt, OpLd, OpSt, OpAtom, OpTex ...).
    static partial class InstEmit
    {
        public static void RegisterAmpereMemory(AmpereRegistrar x)
        {
            const InstProps N = InstProps.None;

            x(AmpereOpcodes.Ipa, InstName.Ipa, AIpa, N);
            x(AmpereOpcodes.IpaIdx, InstName.Ipa, AIpa, N);
            x(AmpereOpcodes.Ald, InstName.Ald, AAld, N);
            x(AmpereOpcodes.Ast, InstName.Ast, AAst, N, writesRd: false);

            x(0x981, InstName.Ldg, ALdg, N);
            x(0x381, InstName.Ldg, ALdg, N);
            x(0xf8c, InstName.Atoms, AAtoms, N);
            x(0x986, InstName.Stg, AStg, N, writesRd: false);
            x(0x386, InstName.Stg, AStg, N, writesRd: false);
            x(0x98e, InstName.Red, ARed, N, writesRd: false);
            x(0x9a8, InstName.Atom, AAtomg, N);
            x(0x3a8, InstName.Atom, AAtomg, N);
            x(0x3a9, InstName.Atom, AAtomgCas, N);
            x(AmpereOpcodes.Ldl, InstName.Ldl, ALdLocalShared, N);
            x(0x984, InstName.Lds, ALdLocalShared, N);
            x(0x387, InstName.Stl, AStLocalShared, N, writesRd: false);
            x(0x987, InstName.Stl, AStLocalShared, N, writesRd: false);
            x(0x388, InstName.Sts, AStLocalShared, N, writesRd: false);
            x(0x988, InstName.Sts, AStLocalShared, N, writesRd: false);
            x(0x38c, InstName.Atoms, AAtoms, N);
            x(0x38d, InstName.Atoms, AAtomsCas, N);
            x(0x992, InstName.Membar, AMembar, N, writesRd: false);
            x(0x9ab, InstName.Membar, AErrbar, N, writesRd: false);

            x(0xb60, InstName.Tex, ATex, InstProps.Tex);
            x(0x361, InstName.Tex, ATex, InstProps.Tex);
            x(0x364, InstName.Tld4, ATex, InstProps.Tex);
            x(0x367, InstName.Tld, ATex, InstProps.Tex);
            x(0x36d, InstName.Txd, ATex, InstProps.Tex);   // TXD with the handle in a register (.B, bit 59; NAK sm70_encode.rs OpTxd)
            x(0xf60, InstName.Tex, ATex, InstProps.Tex);
            x(0xf63, InstName.Tld4, ATex, InstProps.Tex);
            x(0xf66, InstName.Tld, ATex, InstProps.Tex);   // TLD with the handle in a uniform register (bit 91): Unreal Engine 5.6's Switch 2 shaders (Orbitals)
            x(0xf6c, InstName.Txd, ATex, InstProps.Tex);
            x(0xf69, InstName.Tmml, ATex, InstProps.Tex);
            x(0x36a, InstName.Tmml, ATex, InstProps.Tex);
            x(0xb66, InstName.Tld, ATex, InstProps.Tex);
            x(0xb63, InstName.Tld4, ATex, InstProps.Tex);
            x(0xb69, InstName.Tmml, ATex, InstProps.Tex);
            x(0xb6c, InstName.Txd, ATex, InstProps.Tex);
            x(0xb6f, InstName.Txq, ATxq, InstProps.Tex);
            x(0x370, InstName.Txq, ATxq, InstProps.Tex);
            x(0xf6f, InstName.Txq, ATxq, InstProps.Tex);
            x(0xb97, InstName.Suld, ASurface, N);
            x(0xb99, InstName.Suld, ASurface, N);
            x(0xb9b, InstName.Sust, ASurface, N, writesRd: false);
            x(0xb9d, InstName.Sust, ASurface, N, writesRd: false);
            x(0x99c, InstName.SustB, ASustB, N, writesRd: false);
            x(0x593, InstName.Suatom, ASuatom, N);
            x(0x59f, InstName.Sured, ASured, N, writesRd: false);
            x(0x394, InstName.SuatomB, ASuatom, N);
            x(0xb1d, InstName.Bar, ABar, N, writesRd: false);
        }

        // ---- attributes ----

        // IPA Rd, a[addr] (0x326) or a[URa + addr] (0x526, the index a uniform register at 24..30): addr 64..72 in words, sample location 76..78 (default,
        // centroid, offset), frequency 78..80 (pass, constant, state). Like Maxwell's IPA.PASS, a perspective attribute is read divided by w: the shader
        // multiplies by 1 / a[0x7c] itself.
        public static void AIpa(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int addr = x.Bits(64, 72) << 2;

            if (x.Bits(76, 78) == 2)
            {
                throw AFail(context, "IPA at an offset");
            }

            Operand idxAddress = null;

            if (x.Opcode == AmpereOpcodes.IpaIdx)
            {
                idxAddress = context.IAdd(Register(x.Bits(24, 30), RegisterType.Uniform), Const(addr));
            }

            IpaOp ipaOp = x.Bits(78, 80) == 1 ? IpaOp.Constant : IpaOp.Pass;

            EmitIpa(context, idxAddress, addr, ipaOp, RegisterConsts.RegisterZeroIndex, false, x.Rd);
        }

        // Geometry and tessellation stages. OUT (opcode 0x124 in the ALU forms): Rd = the new output handle, Ra the old, the stream in the second source; 78..80 emit (1),
        // cut (2) or both, none being OUTFINAL. ISBERD Rd, [Ra]: the stage buffer entry of a vertex, which only ALD reads: Ra passes through, as on Maxwell. A geometry shader
        // that ends its output through an exit stub sets it up first: LEPC R, R + offset → 0x357 installs it as the address EXIT continues at, and the stub runs OUTFINAL
        // and 0x957 before its own EXIT. Per thread, with no output to finish, EXIT just ends the invocation: LEPC writes its own address (only the stub setup reads it),
        // and 0x357 and 0x957 do nothing (Donkey Kong Bananza's geometry shader 5d537f7630ec3671, which writes its primitives to memory with ATOMG / STG and emits none)
        public static void RegisterAmpereGeometry(AmpereAluRegistrar alu, AmpereRegistrar x)
        {
            const InstProps N = InstProps.None;

            alu(0x124, InstName.Out, AOut);
            x(0x923, InstName.Isberd, AIsberd, N);
            x(0x927, InstName.Ast, AStageOut, N, writesRd: false);
            x(0x34e, InstName.Lepc, ALepc, N);
            x(0x357, InstName.Nop, ANop, N, writesRd: false);
            x(0x957, InstName.Nop, ANop, N, writesRd: false);
        }

        public static void AOut(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int type = x.Bits(78, 80);

            if (type != 0)
            {
                EmitOut(context, (type & 1) != 0, (type & 2) != 0);
            }

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), AGpr(x.Ra));
            }
        }

        public static void AIsberd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int[] stage = context.TranslatorContext.GpuAccessor.QueryAmpereStageMemory();

            if (stage != null)
            {
                // A task or mesh shader's stage input (QueryAmpereStageMemory). Mode 0x78: its workgroup's number; 0x38: a word of the payload its task wrote, [Ra + offset];
                // 0xb8: a word it wrote itself with 0x927's mode 0xb8 (Ocarina of Time's task shader 5d3a096025e375a1 reads its payload back in a later thread)
                int mode = x.Bits(72, 80);

                if (mode != 0x78 && mode != 0x38 && mode != 0xb8)
                {
                    throw AFail(context, $"ISBERD mode {mode:x2} in a task or mesh shader");
                }

                Operand addr = mode == 0x78 ? Const(stage[0]) : context.IAdd(AGpr(x.Ra), Const(stage[mode == 0xb8 ? 2 : 1] + (int)x.SignedField(40, 64)));
                EmitLoad(context, StorageKind.SharedMemory, LsSize2.B32, addr, x.Rd, 0);
                return;
            }

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), AGpr(x.Ra));
            }
        }

        // Task and mesh shaders' stage output (0x927), [Ra + offset], Rb, mode 72..79: 0xb8 a word of the task's payload; 0xf8 a word of a mesh shader's attributes (each component
        // 0x80 bytes for 32 vertices or primitives); 0x88 the primitive count byte, 0x80 a primitive index byte. Bytes go through the shared memory's byte store, a compare-and-swap
        // on the word (SharedStoreSmallIntCas), since neighbouring threads write neighbouring bytes. A mesh workgroup of 32 threads is one warp on the console, all at the same
        // instruction, so the zero count each thread writes first (Mario Kart World: at its start) cannot pass the count one thread writes later; Metal's threads run in no such step,
        // and the stage memory starts at zero (dmsh_meshstage.py), so that store is left out
        public static void AStageOut(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int[] stage = context.TranslatorContext.GpuAccessor.QueryAmpereStageMemory();
            int mode = x.Bits(72, 80);

            if (stage == null || (mode != 0xb8 && mode != 0xf8 && mode != 0x88 && mode != 0x80))
            {
                throw AFail(context, $"stage output store mode {mode:x2} outside a task or mesh shader");
            }

            if (mode == 0x88 && x.Rb == RegisterConsts.RegisterZeroIndex)
            {
                return;
            }

            bool word = mode == 0xb8 || mode == 0xf8;
            Operand addr = context.IAdd(AGpr(x.Ra), Const((word ? stage[2] : stage[3]) + (int)x.SignedField(40, 64)));
            EmitStore(context, StorageKind.SharedMemory, word ? LsSize2.B32 : LsSize2.U8, addr, x.Rb, 0);
        }

        public static void ALepc(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), Const((int)context.CurrOp.Address));
            }

            if (x.Rd + 1 < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd + 1, RegisterType.Gpr), Const(0));
            }
        }

        // ALD Rd, a[Ra + addr], vtx: addr 40..50, vertex 32..40, count - 1 74..76, patch 76, physical (Ra the whole address) 77, output 79
        public static void AAld(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (!x.Bit(77) && x.Ra != RegisterConsts.RegisterZeroIndex)
            {
                throw AFail(context, "ALD with an offset register and an address");
            }

            EmitAld(context, x.Rd, x.Ra, x.Rb, x.Bits(40, 50), x.Bits(74, 76), x.Bit(76), x.Bit(79), x.Bit(77));
        }

        // AST a[Ra + addr], data (32..40), vtx (64..72)
        public static void AAst(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (!x.Bit(77) && x.Ra != RegisterConsts.RegisterZeroIndex)
            {
                throw AFail(context, "AST with an offset register and an address");
            }

            EmitAst(context, x.Ra, x.Rb, x.Bits(40, 50), x.Bits(74, 76), x.Bit(76), x.Bit(77));
        }

        // ---- global memory ----

        // The 64-bit address of a global access: Ra (a pair with bit raPairBit, else 32 bits) plus a uniform register pair at urLo (a pair with bit 72) in
        // the uniform form (bit 91), plus the signed offset (40..64). The low word stays (UR + Ra + offset), the form Ryujinx's global-to-storage pass
        // traces back to a storage buffer table entry in c[0].
        private static (Operand Low, Operand High) AGlobalAddress(EmitterContext context, AmpereOp x, int raPairBit, int urLo, int extraOffset, int descBit)
        {
            bool uniformForm = x.Bit(91);
            bool raPair = uniformForm ? x.Bit(raPairBit) : x.Bit(72);
            int offset = (int)x.SignedField(40, 64) + extraOffset;

            Operand lo = Register(x.Ra, RegisterType.Gpr);
            Operand hi = raPair && x.Ra != RegisterConsts.RegisterZeroIndex ? Register(x.Ra + 1, RegisterType.Gpr) : Const(0);

            // desc[URb][Ra.64] (descBit: 76 in LDG / STG, 71 in ATOMG / RED; nvdisasm): URb is a memory descriptor (the compiler's cache policy), not part of the address
            if (uniformForm && !x.Bit(descBit))
            {
                int ur = x.Bits(urLo, urLo + 6);

                if (ur != RegisterConsts.UniformZeroIndex)
                {
                    Operand uLo = Register(ur, RegisterType.Uniform);
                    Operand uHi = x.Bit(72) ? Register(ur + 1, RegisterType.Uniform) : Const(0);

                    Operand sum = context.IAdd(uLo, lo);
                    Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(sum, uLo), Const(1), Const(0));

                    hi = context.IAdd(context.IAdd(uHi, hi), carry);
                    lo = sum;
                }
            }

            if (offset != 0)
            {
                Operand sum = context.IAdd(lo, Const(offset));
                Operand carry = offset > 0
                    ? context.ConditionalSelect(context.ICompareLessUnsigned(sum, lo), Const(1), Const(0))
                    : context.ConditionalSelect(context.ICompareGreaterUnsigned(sum, lo), Const(-1), Const(0));

                hi = context.IAdd(hi, carry);
                lo = sum;
            }

            return (lo, hi);
        }

        // LDG Rd, [Ra + UR + offset]: size 73..76; the uniform form (0x981) loads only where its predicate (64..67, stored as 7 - P; 67 inverts) holds,
        // and is zero elsewhere
        public static void ALdg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            LsSize size = (LsSize)x.Bits(73, 76);
            int count = GetVectorCount(size);
            StorageKind storageKind = GetStorageKind(size);

            Operand pred = null;

            if (x.Opcode == 0x981)
            {
                int p = 7 - x.Bits(64, 67);

                if (p != RegisterConsts.PredicateTrueIndex || x.Bit(67))
                {
                    Operand pr = Register(p, RegisterType.Predicate);
                    pred = x.Bit(67) ? context.BitwiseNot(pr) : pr;
                }
            }

            // The address is read once, before any result is written, and that includes the zero fill of the predicated form below: the destination may be
            // the address register (LDG.128 R4, [R4+UR4], Orbitals' UE5 GPUScene reads). Zeroing first made the address UR + 0, so every such load read the
            // buffer's first words (the title's sky drew off screen, the GPUScene scatter kernels collapsed to entry 0)
            (Operand[] los, Operand[] his) = (new Operand[count], new Operand[count]);

            for (int index = 0; index < count; index++)
            {
                (Operand lo, Operand hi) = AGlobalAddress(context, x, 90, 32, index * 4, 76);
                (los[index], his[index]) = (context.Copy(lo), context.Copy(hi));   // copies: the destination may be the address register
            }

            Operand lblSkip = null;

            if (pred != null)
            {
                for (int index = 0; index < count && x.Rd + index < RegisterConsts.RegisterZeroIndex; index++)
                {
                    context.Copy(Register(x.Rd + index, RegisterType.Gpr), Const(0));
                }

                lblSkip = Label();
                context.BranchIfFalse(lblSkip, pred);
            }

            // The words of one LDG.64 / LDG.128 are marked as one access (Operation.GlobalVectorCount): an address the translator ties to no storage buffer then takes one pool
            // table lookup and one vector load for all of them, where each word took its own (Ocarina of Time's foliage mesh shaders: 4.6 ms of a 683x384 frame, 464 of 506 loads)
            bool vector = count > 1 && storageKind == StorageKind.GlobalMemory && x.Rd + count <= RegisterConsts.RegisterZeroIndex;
            int group = vector ? ++context.GlobalVectorGroups : 0;

            for (int index = 0; index < count; index++)
            {
                if (x.Rd + index >= RegisterConsts.RegisterZeroIndex)
                {
                    break;
                }

                Operand value = context.Load(storageKind, los[index], his[index]);

                if (vector && value.AsgOp is Operation load)
                {
                    load.GlobalVectorCount = count;
                    load.GlobalVectorWord = index;
                    load.GlobalVectorGroup = group;
                }

                context.Copy(Register(x.Rd + index, RegisterType.Gpr), value);
            }

            if (lblSkip != null)
            {
                context.MarkLabel(lblSkip);
            }
        }

        // STG [Ra + UR + offset], Rb: UR at 64 in the uniform form (0x986)
        public static void AStg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            LsSize size = (LsSize)x.Bits(73, 76);

            if (size > LsSize.B128)
            {
                throw AFail(context, $"STG size {size}");
            }

            int count = GetVectorCount(size);
            StorageKind storageKind = GetStorageKind(size);

            for (int index = 0; index < count; index++)
            {
                bool isRz = x.Rb + index >= RegisterConsts.RegisterZeroIndex;
                Operand value = isRz ? Const(0) : Register(x.Rb + index, RegisterType.Gpr);

                (Operand lo, Operand hi) = AGlobalAddress(context, x, 90, 64, index * 4, 76);

                context.Store(storageKind, lo, hi, value);
            }
        }

        // RED [Ra + UR + offset], Rb: type 73..76, operation 87..90
        public static void ARed(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand lo, Operand hi) = AGlobalAddress(context, x, 90, 64, 0, 71);
            AtomOp op = (AtomOp)x.Bits(87, 90);

            if (op is AtomOp.Inc or AtomOp.Dec && x.Bits(73, 75) is 0 or 1)
            {
                AGlobalIncDec(context, op, lo, hi, Register(x.Rb, RegisterType.Gpr));
                return;
            }

            EmitAtomicOp(context, StorageKind.GlobalMemory, op, (AtomSize)x.Bits(73, 76), lo, hi, Register(x.Rb, RegisterType.Gpr));
        }

        // ATOMG Rd, [Ra + UR + offset], Rb: operation 87..91; the uniform form keeps Ra's pair bit at 70
        public static void AAtomg(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand lo, Operand hi) = AGlobalAddress(context, x, 70, 64, 0, 71);
            AtomOp op = (AtomOp)x.Bits(87, 91);
            Operand value = x.Rb >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr);

            Operand res = op is AtomOp.Inc or AtomOp.Dec && x.Bits(73, 75) is 0 or 1
                ? AGlobalIncDec(context, op, lo, hi, value)
                : EmitAtomicOp(context, StorageKind.GlobalMemory, op, (AtomSize)x.Bits(73, 76), lo, hi, value);

            ASet(context, x, res);
        }

        // INC (3) / DEC (4) on global memory, CUDA's atomicInc / atomicDec: EmitAtomicOp has no case for them (the operation vanished, its result 0). A compare-and-swap loop, as ATOMS's:
        // INC: old >= b ? 0 : old + 1; DEC: old == 0 || old > b ? b : old - 1 (unsigned), retried until the word did not change meanwhile; the old value
        private static Operand AGlobalIncDec(EmitterContext context, AtomOp op, Operand lo, Operand hi, Operand value)
        {
            Operand loop = Label();
            context.MarkLabel(loop);
            Operand cur = context.Load(StorageKind.GlobalMemory, lo, hi);
            Operand next = op == AtomOp.Inc
                ? context.ConditionalSelect(context.ICompareGreaterOrEqualUnsigned(cur, value), Const(0), context.IAdd(cur, Const(1)))
                : context.ConditionalSelect(context.BitwiseOr(context.ICompareEqual(cur, Const(0)), context.ICompareGreaterUnsigned(cur, value)), value, context.IAdd(cur, Const(-1)));
            Operand prev = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, lo, hi, cur, next);
            context.BranchIfFalse(loop, context.ICompareEqual(prev, cur));

            return cur;
        }

        // ATOMG.CAS Rd, [Ra + offset], Rb (expected), Rc (new value, 64..71) (0x3a9; refused until now): PTX atom.cas, Rd the old value. 32 bits only (73..76 = 0)
        public static void AAtomgCas(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(73, 76) != 0)
            {
                throw AFail(context, $"ATOMG.CAS size {x.Bits(73, 76)}");
            }

            (Operand lo, Operand hi) = AGlobalAddress(context, x, 70, 64, 0, 71);
            Operand expected = context.Copy(x.Rb >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr));
            Operand value = x.Bits(64, 72) >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Bits(64, 72), RegisterType.Gpr);

            ASet(context, x, context.AtomicCompareAndSwap(StorageKind.GlobalMemory, lo, hi, expected, value));
        }

        // ATOMS.CAS Rd, [Ra + offset], Rb (expected), Rc (new value). nvdisasm names the forms by bit 87: clear, CAS, CUDA's atomicCAS, Rd the old value; set, CAST, and CAST.SPIN with
        // bit 88 too (the compiler's retry loop for a float add on shared memory, which repeats until Rd == 1: Fire Emblem Fortune's Weave's only use), Rd 1 when the swap happened.
        // Only the 32-bit size (73..76 = 0) is taken
        public static void AAtomsCas(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand ra = x.Ra >= RegisterConsts.RegisterZeroIndex ? Const(0) : AStride(context, x, Register(x.Ra, RegisterType.Gpr));
            Operand offset = context.ShiftRightU32(context.IAdd(ra, Const((int)x.SignedField(40, 64))), Const(2));
            Operand expected = context.Copy(x.Rb >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr));
            Operand value = x.Rc >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rc, RegisterType.Gpr);
            Operand old = context.AtomicCompareAndSwap(StorageKind.SharedMemory, Const(context.ResourceManager.SharedMemoryId), offset, expected, value);

            ASet(context, x, !x.Bit(87) ? old : context.ConditionalSelect(context.ICompareEqual(old, expected), Const(1), Const(0)));
        }

        // ---- local and shared memory ----

        // A shared or local address register's stride, bits 78..79: it counts elements of 1, 4, 8 or 16 bytes (Fire Emblem Fortune's Weave has 1 on all its 32-bit and 2 on all its 64-bit accesses with a
        // register, whose code bounds that register by the element count). The field is a scale, not the access size: Pokemon Pokopia has 3 on 3,312 shared accesses of 4, 8 and 16 bytes
        private static Operand AStride(EmitterContext context, AmpereOp x, Operand ra)
        {
            int stride = x.Bits(78, 80);

            return stride == 0 ? ra : context.ShiftLeft(ra, Const(stride + 1));
        }

        // The 32-bit address of a local / shared access: Ra by its stride, plus the uniform register (at urLo in the uniform form, bit 91)
        // The stride (78..79) scales a shared access's Ra only (PR #981: LDS / STS / ATOMS [Ra.X4 / X8 / X16]); local accesses (LDL / STL) do not take it. Scaling local
        // ones as well, as the Fire Emblem Fortune's Weave merge did, broke Donkey Kong Bananza's levels (blocky garbage over the whole picture)
        private static Operand ALocalAddress(EmitterContext context, AmpereOp x, int urLo, bool shared = false)
        {
            Operand ra = x.Ra == RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Ra, RegisterType.Gpr);
            Operand addr = x.Ra == RegisterConsts.RegisterZeroIndex || !shared ? ra : AStride(context, x, ra);

            if (x.Bit(91) && x.Bits(urLo, urLo + 6) != RegisterConsts.UniformZeroIndex)
            {
                addr = context.IAdd(addr, Register(x.Bits(urLo, urLo + 6), RegisterType.Uniform));
            }

            return addr;
        }

        // LDL / LDS Rd, [Ra + UR + offset]: UR at 32, size 73..76, offset 40..64
        public static void ALdLocalShared(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            StorageKind kind = x.Opcode == AmpereOpcodes.Ldl ? StorageKind.LocalMemory : StorageKind.SharedMemory;

            EmitLoad(context, kind, (LsSize2)x.Bits(73, 76), ALocalAddress(context, x, 32, kind == StorageKind.SharedMemory), x.Rd, (int)x.SignedField(40, 64));
        }

        // STL / STS [Ra + UR + offset], Rb: UR at 64
        public static void AStLocalShared(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            StorageKind kind = x.Opcode is 0x387 or 0x987 ? StorageKind.LocalMemory : StorageKind.SharedMemory;

            EmitStore(context, kind, (LsSize2)x.Bits(73, 76), ALocalAddress(context, x, 64, kind == StorageKind.SharedMemory), x.Rb, (int)x.SignedField(40, 64));
        }

        // ATOMS Rd, [Ra + offset], Rb: an atomic on shared memory, its operation (87..91) and size (73..76) as ATOMG's, its offset in bytes as STS's (Pokemon Pokopia's 51 compute shaders: ADD on
        // counters at the end of their shared memory, MAX; Splatoon Raiders has 6)
        public static void AAtoms(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool one = x.Opcode == 0xf8c;

            // 0xf8c, ATOMS.POPC.INC: an increment by 1 with no source register, the operation field 10 (the hardware sums the warp's lanes on one address; per lane an add of 1). Its one user is
            // Pokemon Pokopia's luminance histogram, hist[bin]++ (2118ab534fac88c6): the field for Rb names a register the shader never wrote on three of its four paths. Other encodings stop here
            if (one ? x.Bits(87, 91) != 10 || !x.Bit(91) || x.Rd != RegisterConsts.RegisterZeroIndex : x.Bit(91))
            {
                throw AFail(context, "ATOMS with a uniform register");
            }

            Operand offset = context.ShiftRightU32(context.IAdd(ALocalAddress(context, x, 64, shared: true), Const((int)x.SignedField(40, 64))), Const(2));
            Operand id = Const(context.ResourceManager.SharedMemoryId);

            if (one)
            {
                EmitAtomicOp(context, StorageKind.SharedMemory, AtomOp.Add, (AtomSize)x.Bits(73, 76), id, offset, Const(1));
                return;
            }

            Operand value = x.Rb >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr);
            AtomOp op = (AtomOp)x.Bits(87, 91);

            // INC (3) and DEC (4), CUDA's atomicInc / atomicDec: EmitAtomicOp has no case for them and gave 0 with no access. A compare-and-swap loop: the new value from the old one,
            // INC: old >= b ? 0 : old + 1; DEC: old == 0 || old > b ? b : old - 1 (unsigned), retried until the word did not change meanwhile; Rd the old value
            if (op is AtomOp.Inc or AtomOp.Dec && x.Bits(73, 75) is 0 or 1)
            {
                Operand loop = Label();
                context.MarkLabel(loop);
                Operand cur = context.Load(StorageKind.SharedMemory, id, offset);
                Operand next = op == AtomOp.Inc
                    ? context.ConditionalSelect(context.ICompareGreaterOrEqualUnsigned(cur, value), Const(0), context.IAdd(cur, Const(1)))
                    : context.ConditionalSelect(context.BitwiseOr(context.ICompareEqual(cur, Const(0)), context.ICompareGreaterUnsigned(cur, value)), value, context.IAdd(cur, Const(-1)));
                Operand prev = context.AtomicCompareAndSwap(StorageKind.SharedMemory, id, offset, cur, next);
                context.BranchIfFalse(loop, context.ICompareEqual(prev, cur));
                ASet(context, x, cur);
                return;
            }

            Operand res = EmitAtomicOp(context, StorageKind.SharedMemory, op, (AtomSize)x.Bits(73, 76), id, offset, value);

            ASet(context, x, res);
        }

        // ---- surfaces ----

        // SULD Rd, [Ra] (0xb97 formatted with a channel mask at 72..76, 0xb99 binary with a size at 73..76) and SUST [Ra], Rb (0xb9b, 0xb9d): the handle as
        // a texture's (40..59), the dimension at 61..64 (1D, 1D buffer, 1D array, 2D, 2D array, 3D: Maxwell's SuDim)
        public static void ASurface(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (int handle, int cbufSlot) = ATexHandle(context, x);
            bool formatted = x.Opcode is 0xb97 or 0xb9b;
            SuDim dim = (SuDim)x.Bits(61, 64);
            SuSize size = formatted ? 0 : (SuSize)x.Bits(73, 76);
            SuRgba mask = formatted ? (SuRgba)x.Bits(72, 76) : 0;

            if (x.Opcode is 0xb97 or 0xb99)
            {
                EmitSuld(context, CacheOpLd.Ca, dim, size, handle, mask, x.Ra, x.Rd, 0, formatted, false, false, cbufSlot);
            }
            else
            {
                EmitSust(context, CacheOpSt.Wb, dim, size, handle, mask, x.Ra, x.Rb, 0, formatted, false, false, cbufSlot);
            }
        }

        // SUATOM Rd, [Ra], Rb (0x593): an atomic on an image through a handle, laid out as SUST's (handle 40..59, dimension 61..64) and as ATOMG's (operation
        // 87..91, size 73..76, Pu 81..83). Donkey Kong Bananza's append compute shaders take a counter with it (SUATOM.ADD.U32 on a 1D image, the old value
        // shuffled to the warp) before their SUST. The handle may be in another bank, as SULD / SUST's: Orbitals' (Unreal Engine 5.6) compute shaders keep theirs in
        // the game's uniform buffer c[3] (SUATOM.ADD.U32 on a 1D buffer image, handle c[3] word 4), bound as an image from that buffer (dmsh_build.py decode_ubo_img).
        // A predicate result is not met in a game: refused.
        // 0x394 is the same with the handle in a register (64..72: bindless, as Maxwell's SUATOM.B; Eden ampere_isa_table.h 0x394 "SuAtom"). Orbitals:
        // 00321e00001ee008200000040c047394, SUATOM.D.1D_BUFFER.ADD.U32 R4, [R12], R4, R8.
        public static void ASuatom(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Opcode == 0x394)
            {
                if (x.Bits(81, 84) != 7)
                {
                    throw AFail(context, "SUATOM with a predicate result");
                }

                EmitSuatom(context, (SuDim)x.Bits(61, 64), 0, (SuatomOp)x.Bits(87, 91), (SuatomSize)x.Bits(73, 76), 0, x.Ra, x.Rb, x.Bits(64, 72), x.Rd, false, isBindless: true, compareAndSwap: false);
                return;
            }

            (int handle, int cbufSlot) = ATexHandle(context, x);

            if (x.Bits(81, 84) != 7)
            {
                throw AFail(context, "SUATOM with a predicate result");
            }

            EmitSuatom(context, (SuDim)x.Bits(61, 64), 0, (SuatomOp)x.Bits(87, 91), (SuatomSize)x.Bits(73, 76), handle, x.Ra, x.Rb, 0, x.Rd, false, isBindless: false, compareAndSwap: false, cbufSlot: cbufSlot);
        }

        // SURED [Ra], Rb (0x59f): SUATOM's reduction, no result: the same fields (handle 40..59, dimension 61..64, size 73..76, operation 87..91), Rd unused.
        // Orbitals' compute shaders: SURED.ADD / OR / MAX .U32 on 1D buffer, 2D and 3D images with the handle in c[3] (000fe2000010e00020c002060500759f:
        // SURED.ADD.U32 on a 1D buffer image, coordinate R5, value R6, handle c[3] word 2). Emitted as SUATOM to RZ (the atomic's result discarded).
        public static void ASured(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            // Bits 80..83 and 59..60 are 0 in every SURED met (Mario Kart World, Orbitals, PRAGMATA); another form is refused, as SUATOM's predicate result is
            if (x.Bits(80, 84) != 0 || x.Bits(59, 61) != 0)
            {
                throw AFail(context, $"SURED form {x.Bits(64, 96):x8}");
            }

            (int handle, int cbufSlot) = ATexHandle(context, x);
            SuatomOp op = (SuatomOp)x.Bits(87, 91);

            if (op > SuatomOp.Xor)
            {
                throw AFail(context, $"SURED operation {op}");
            }

            // 64-bit reductions (Nanite's visibility buffer, SURED.MAX.U64) are refused, not faked as 32-bit ones
            if ((SuatomSize)x.Bits(73, 76) is not (SuatomSize.U32 or SuatomSize.S32))
            {
                throw AFail(context, $"SURED form {x.Bits(64, 96):x8} (size {(SuatomSize)x.Bits(73, 76)})");
            }

            EmitSuatom(context, (SuDim)x.Bits(61, 64), 0, op, (SuatomSize)x.Bits(73, 76), handle, x.Ra, x.Rb, 0, RegisterConsts.RegisterZeroIndex, false, isBindless: false, compareAndSwap: false, cbufSlot: cbufSlot);
        }

        // SUST [Ra], Rb, Rc (0x99c): the formatted store with the handle in a register (64..72, bindless; NAK's only SUST form, sm70_encode.rs OpSuSt; Eden
        // ampere_isa_table.h 0x99c "SuSt"), the dimension at 61..64 and the channel mask at 72..76 as 0xb9b's. Orbitals: 0033de0000100f242000004c3800799c,
        // SUST.D.1D_BUFFER [R56], R76..R79 (mask 0xf), handle R36
        public static void ASustB(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            EmitSust(context, CacheOpSt.Wb, (SuDim)x.Bits(61, 64), 0, 0, (SuRgba)x.Bits(72, 76), x.Ra, x.Rb, x.Bits(64, 72), useComponents: true, false, isBindless: true);
        }

        // MEMBAR (0x992): scope at 76..79 (Eden lower_to_maxwell.h: 0 CTA, 2 GPU, 3 system, 5 VC): a workgroup memory barrier for CTA, a memory barrier for the rest
        public static void AMembar(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(76, 79) == 0)
            {
                context.GroupMemoryBarrier();
            }
            else
            {
                context.MemoryBarrier();
            }
        }

        // ERRBAR (0x9ab): waits for the earlier memory operations' errors; Eden lower_to_maxwell.h lowers it to MEMBAR.GL: a memory barrier
        public static void AErrbar(EmitterContext context)
        {
            context.MemoryBarrier();
        }

        // PIXLD Rd (0x925): a pixel value, 78..81 (NAK sm70_encode.rs OpPixLd, ir.rs PixVal: MSCOUNT 0, COVMASK 1, CENTROID_OFFSET 2, MY_INDEX 3, INNER_COVERAGE 4),
        // predicate output 81..84. Orbitals' pixel shaders read MY_INDEX (040e0200000ec0000000000000007925, PIXLD.MY_INDEX R0): the sample the fragment runs for,
        // as Maxwell's PIXLD.MYINDEX (InstEmitMove.Pixld). 0x925 is registered to APixelCovered (InstEmitAmpereAlu.cs), which hands MY_INDEX here; the other values are refused
        public static void APixld(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(78, 81) != 3 || x.Bits(81, 84) != RegisterConsts.PredicateTrueIndex || context.TranslatorContext.Definitions.Stage != ShaderStage.Fragment)
            {
                throw AFail(context, $"PIXLD value {x.Bits(78, 81)}");
            }

            ASet(context, x, context.Load(StorageKind.Input, IoVariable.SampleId));
        }

        // BAR.SYNC: the workgroup barrier
        public static void ABar(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(77, 80) != 0 && x.Bits(77, 80) != 4)
            {
                throw AFail(context, $"BAR mode {x.Bits(77, 80)}");
            }

            context.Barrier();
        }

        // ---- textures ----

        // Texture dimension (61..64) → Maxwell's TexDim
        private static TexDim ATexDim(EmitterContext context, int dim)
        {
            return dim switch
            {
                0 => TexDim._1d,
                4 => TexDim.Array1d,
                1 => TexDim._2d,
                5 => TexDim.Array2d,
                2 => TexDim._3d,
                3 => TexDim.Cube,
                7 => TexDim.ArrayCube,
                _ => throw AFail(context, $"texture dimension {dim}"),
            };
        }

        // The constant buffer word that holds the texture handle (40..59: word offset 0..14, bank 14..19). NVN keeps the handle table in c[2]
        // (0x20 + 8 slot), the texture buffer Ryujinx assumes; NVN2 compute's handles in c[0] are mapped there (AMapC0); a handle in another bank is passed
        // with its bank.
        private static (int Handle, int CbufSlot) ATexHandle(EmitterContext context, AmpereOp x)
        {
            (int bank, int offset) = AMapC0(context, x.Bits(54, 59), x.Bits(40, 54) * 4);

            return (offset >> 2, bank == 2 ? TextureOperation.DefaultCbufSlot : bank);
        }

        // The two source blocks of a texture instruction from Ra and Rb (n0 and n1 registers). With .SCR (60) the whole sequence was spread over the two
        // registers instead (4 → 2 + 2, 3 → 2 + 1, 2 → 1 + 1; NAK try_scalarize_tex).
        private static (List<Operand> A, List<Operand> B) ATexBlocks(EmitterContext context, AmpereOp x, int n0, int n1)
        {
            List<Operand> seq = [];

            void Take(int reg, int n)
            {
                for (int k = 0; k < n; k++)
                {
                    seq.Add(reg + k >= RegisterConsts.RegisterZeroIndex ? Const(0) : context.Copy(Register(reg + k, RegisterType.Gpr)));
                }
            }

            if (x.Bit(60))
            {
                int total = n0 + n1;

                if (total > 4)
                {
                    throw AFail(context, "texture .SCR with more than four sources");
                }

                int k0 = total >= 3 ? 2 : 1;

                Take(x.Ra, System.Math.Min(k0, total));
                Take(x.Rb, total - System.Math.Min(k0, total));
            }
            else
            {
                Take(x.Ra, n0);
                Take(x.Rb, n1);
            }

            return (seq.GetRange(0, n0), seq.GetRange(n0, seq.Count - n0));
        }

        // The results (mask 72..76) of a texture instruction to their registers: in order to Rd, Rd + 1, ...; with a second destination (64..72) the first
        // two to Rd and the others to it. .F16 (79): two results a register, packed as halves (the first two to Rd, the others to the second destination or
        // Rd + 1).
        private static void ATexResults(EmitterContext context, AmpereOp x, Operand[] results, bool f16)
        {
            int d0 = x.Rd;
            int d1 = x.Rc;
            bool split = d1 != RegisterConsts.RegisterZeroIndex;

            void Write(int reg, Operand value)
            {
                if (reg < RegisterConsts.RegisterZeroIndex)
                {
                    context.Copy(Register(reg, RegisterType.Gpr), value);
                }
            }

            if (d0 == RegisterConsts.RegisterZeroIndex)
            {
                return;
            }

            if (f16)
            {
                for (int pair = 0; pair * 2 < results.Length; pair++)
                {
                    Operand lo = results[pair * 2];
                    Operand hi = pair * 2 + 1 < results.Length ? results[pair * 2 + 1] : ConstF(0);
                    Write(pair == 0 ? d0 : split ? d1 : d0 + 1, context.PackHalf2x16(lo, hi));
                }

                return;
            }

            for (int i = 0; i < results.Length; i++)
            {
                Write(!split || i < 2 ? d0 + i : d1 + i - 2, results[i]);
            }
        }

        // TEX, TLD, TLD4, TMML, TXD. Source blocks (NAK nak_nir_lower_tex, SM70): A = array index, coordinates (TXD: coordinates, then the array index
        // with the offsets in its high half); B = LOD / bias, offset, depth reference, sample (TXD: the derivatives, x and y interleaved). The results are
        // read into locals, then written in Ampere's layout.
        // 0xf60 / 0xf66 / 0xf63 / 0xf6c: the same with the handle in uniform register 40..46 (bit 91 set) instead of a constant buffer word, which
        // Unreal Engine 5.6's Switch 2 shaders load with ULDC from the texture handle table (Orbitals: 17,440 of 23,432 shaders): taken as a bindless
        // handle, whose constant buffer source the bindless elimination finds as for Maxwell's TEX.B
        public static void ATex(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool urHandle = (x.Opcode & 0x400) != 0;
            bool regHandle = x.Opcode is 0x361 or 0x364 or 0x367 or 0x36a or 0x36d;
            int opcode = regHandle ? x.Opcode + 0x7ff : x.Opcode & ~0x400;   // 0x361 → TEX 0xb60, 0x364 → TLD4 0xb63, 0x367 → TLD 0xb66, 0x36a → TMML 0xb69, 0x36d → TXD 0xb6c (NAK sm70_encode.rs OpTld / OpTxd, .B bit 59)
            // 0xf69 and 0x36a are TMML the same way (0xb69's other fields; Kirby Air Riders: 4,776 pixel shaders)

            if (urHandle && (!x.Bit(91) || x.Bits(40, 46) >= RegisterConsts.UniformZeroIndex))
            {
                throw AFail(context, "texture handle in a uniform register without bit 91, or URZ");
            }

            if (regHandle && !x.Bit(59))
            {
                throw AFail(context, "texture handle in a register without .B (bit 59)");
            }

            // TEX through a handle in a register, not named by its offset: the compiler's form for a handle it read itself, usually from a table at a computed offset. BindlessElimination takes the
            // handle from there, as for Maxwell's TEX.B. The other fields are 0xb60's. 0xf60: a uniform register at 40..46. 0x361 (bit 59 set): a register, the first of the second source block,
            // before the LOD (a census of Fire Emblem Fortune's Weave's 3,327: of the sources that an LDC just wrote, always that place, with and without .SCR). Rc stays the second destination in both
            TexDim dim = ATexDim(context, x.Bits(61, 64));
            SamplerType type = ConvertSamplerType(dim);
            bool isArray = type.HasFlag(SamplerType.Array);
            int coords = type.Dimensions;
            int mask = x.Bits(72, 76);
            bool f16 = x.Bit(79) && opcode != 0xb6c;

            if (x.Bits(81, 84) != RegisterConsts.PredicateTrueIndex && opcode != 0xb69)
            {
                throw AFail(context, "texture residency predicate");
            }

            TextureFlags flags = TextureFlags.None;
            int lod = 0;
            bool offset = false, depth = false, ms = false, ptp = false;
            int n1 = 0;

            switch (opcode)
            {
                case 0xb60: // TEX: lod 87..90 (0 auto, 1 zero, 2 bias, 3 level), AOFFI 76, depth reference 78
                    lod = x.Bits(87, 90);
                    offset = x.Bit(76);
                    depth = x.Bit(78);
                    if (lod > 3)
                    {
                        throw AFail(context, $"TEX lod mode {lod}");
                    }

                    n1 = (lod >= 2 ? 1 : 0) + (offset ? 1 : 0) + (depth ? 1 : 0);
                    break;
                case 0xb66: // TLD: lod 87..90 (1 zero, 3 level), AOFFI 76, multisample 78
                    lod = x.Bits(87, 90);
                    offset = x.Bit(76);
                    ms = x.Bit(78);
                    if (lod is not 1 and not 3)
                    {
                        throw AFail(context, $"TLD lod mode {lod}");
                    }

                    flags |= TextureFlags.IntCoords;
                    n1 = (lod == 3 ? 1 : 0) + (offset ? 1 : 0) + (ms ? 1 : 0);
                    break;
                case 0xb63: // TLD4: offset mode 76..78 (1 AOFFI, 2 per texel), depth reference 78, component 87..89
                    int offMode = x.Bits(76, 78);
                    depth = x.Bit(78);
                    if (offMode == 3)
                    {
                        throw AFail(context, "TLD4 offset mode 3");
                    }

                    offset = offMode != 0;
                    ptp = offMode == 2;
                    flags |= TextureFlags.Gather;
                    n1 = (offMode == 2 ? 2 : offMode) + (depth ? 1 : 0);
                    break;
                case 0xb6c: // TXD: AOFFI 76
                    offset = x.Bit(76);
                    flags |= TextureFlags.Derivatives;
                    break;
            }

            (int handle, int cbufSlot) = urHandle || regHandle ? (0, TextureOperation.DefaultCbufSlot) : ATexHandle(context, x);
            List<Operand> sources = [];
            Operand arrayIndex = null;
            bool is1DTo2D = false;

            if (urHandle)
            {
                sources.Add(context.Copy(Register(x.Bits(40, 46), RegisterType.Uniform)));
                flags |= TextureFlags.Bindless;
            }

            if (opcode == 0xb6c)
            {
                // .B (0x36d): the handle is the first register of the FIRST block, before the coordinates (NAK nak_nir_lower_tex.c: TXD pushes tex_h to src0;
                // the other texture operations push it to src1)
                int h = regHandle ? 1 : 0;
                int n0 = h + coords + (isArray || offset ? 1 : 0);
                (List<Operand> a, List<Operand> b) = ATexBlocks(context, x, n0, 2 * coords);

                if (regHandle)
                {
                    sources.Add(a[0]);
                    flags |= TextureFlags.Bindless;
                }

                sources.AddRange(a.GetRange(h, coords));

                if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
                {
                    sources.Add(ConstF(0));
                    type = SamplerType.Texture2D | (type & SamplerType.Array);
                    is1DTo2D = true;
                }

                if (isArray)
                {
                    arrayIndex = context.BitwiseAnd(a[h + coords], Const(0xffff));
                }

                if (isArray)
                {
                    sources.Add(arrayIndex);
                }

                // The derivative registers are interleaved per coordinate (dPdx.x, dPdy.x, dPdx.y, dPdy.y, ...), as NAK writes them; the texture
                // operation takes all of dPdx, then all of dPdy
                for (int axis = 0; axis < 2; axis++)
                {
                    for (int i = 0; i < coords; i++)
                    {
                        sources.Add(b[i * 2 + axis]);
                    }
                }

                if (is1DTo2D)
                {
                    sources.Add(ConstF(0));
                    sources.Add(ConstF(0));
                }

                if (offset)
                {
                    for (int i = 0; i < coords; i++)
                    {
                        sources.Add(context.BitfieldExtractS32(a[h + coords], Const(16 + i * 4), Const(4)));
                    }

                    if (is1DTo2D)
                    {
                        sources.Add(Const(0));
                    }

                    flags |= TextureFlags.Offset;
                }
            }
            else
            {
                int n0 = coords + (isArray ? 1 : 0);
                // .B: the handle is the first register of the second block, before the LOD and the rest (NAK nak_nir_lower_tex: PUSH(src1, tex_h) first; Eden's
                // lowering of 0x361 copies it from Rb, or Rb + 1 after .SCR moved a coordinate there)
                (List<Operand> a, List<Operand> b) = ATexBlocks(context, x, n0, n1 + (regHandle ? 1 : 0));
                int ia = 0, ib = 0;

                if (regHandle)
                {
                    sources.Add(b[ib++]);
                    flags |= TextureFlags.Bindless;
                }

                if (isArray)
                {
                    arrayIndex = a[ia++];
                }

                for (int i = 0; i < coords; i++)
                {
                    sources.Add(a[ia++]);
                }

                // nxnative: as on Maxwell (InstEmitTexture.cs), the descriptor decides between a 1D texture and a texture buffer, and with no type answer (no game's NVN2 shaders have one)
                // a plain fetch reads 2D at (x & 0x3FFF, x >> 14): a 1D texture, one row on the host, reads (x, 0) as before, and the host binds a texture buffer as rows of 16384 texels over
                // the same memory (nvn_mtl.m tbuf_as_2d). Every other 1D form cannot be a texture buffer and reads row 0. Pokemon Pokopia's KTGL light lists read 0 before this, as a dummy
                if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
                {
                    bool plainFetch = opcode == 0xb66 && !isArray && lod == 1 && !offset && !ms;

                    if (!plainFetch || context.TranslatorContext.GpuAccessor.QuerySamplerType(handle) != SamplerType.TextureBuffer)
                    {
                        if (plainFetch)
                        {
                            Operand fetchX = sources[^1];
                            sources[^1] = context.BitwiseAnd(fetchX, Const(0x3FFF));
                            sources.Add(context.ShiftRightU32(fetchX, Const(14)));
                            context.TranslatorContext.GpuAccessor.RecordTexelFetch1D(handle);
                        }
                        else
                        {
                            sources.Add(opcode == 0xb66 ? Const(0) : ConstF(0));
                        }

                        type = SamplerType.Texture2D | (type & SamplerType.Array);
                        is1DTo2D = true;
                    }
                }

                if (isArray)
                {
                    sources.Add(arrayIndex);
                }

                Operand lodValue = null;

                if ((opcode == 0xb60 && lod >= 2) || (opcode == 0xb66 && lod == 3))
                {
                    lodValue = b[ib++];
                }

                Operand off0 = offset ? b[ib++] : null;
                Operand off1 = ptp ? b[ib++] : null;

                if (depth)
                {
                    sources.Add(b[ib++]);
                    type |= SamplerType.Shadow;
                }

                if (opcode == 0xb60 || opcode == 0xb66)
                {
                    if (!ms && (lod == 1 || lod == 3))
                    {
                        sources.Add(lodValue ?? (opcode == 0xb66 ? Const(0) : ConstF(0)));
                        flags |= TextureFlags.LodLevel;
                    }
                }

                if (offset && opcode != 0xb63)
                {
                    for (int i = 0; i < coords; i++)
                    {
                        sources.Add(context.BitfieldExtractS32(off0, Const(i * 4), Const(4)));
                    }

                    if (is1DTo2D)
                    {
                        sources.Add(Const(0));
                    }

                    flags |= TextureFlags.Offset;
                }
                else if (offset)
                {
                    int texels = ptp ? 4 : 1;

                    for (int i = 0; i < coords * texels; i++)
                    {
                        Operand packed = (i >> 2 & 1) == 0 ? off0 : off1;
                        sources.Add(context.BitfieldExtractS32(packed, Const((i & 3) * 8), Const(6)));
                    }

                    flags |= ptp ? TextureFlags.Offsets : TextureFlags.Offset;
                }

                if (opcode == 0xb60 && lod == 2)
                {
                    sources.Add(lodValue);
                    flags |= TextureFlags.LodBias;
                }

                if (ms)
                {
                    sources.Add(b[ib++]);
                    type |= SamplerType.Multisample;
                }

                if (opcode == 0xb63 && !depth)
                {
                    sources.Add(Const(x.Bits(87, 89)));
                }

                // A 1D integer fetch (TLD) is either a 1D texture or a texture buffer, decided by the bound descriptor. The
                // Maxwell path (EmitTexs, TldsTarget.Texture1DLodZero) handles both; Ampere (Switch 2) did not, so these fetches
                // stayed texture1d and a texture buffer could not be bound in the slot on Metal (a Metal 1D texture cannot be a
                // buffer view; Orbitals' UE manual-vertex-fetch background shaders read their vertex streams this way, from
                // bindless cb3 handles, so the UV read 0 and every such background rendered black). A non-bindless fetch answered
                // as a texture buffer becomes one (no LOD); every other 1D integer fetch, bindless or not, is read as 2D rows of
                // 16384 texels (x & 0x3FFF, x >> 14) as Sample1DAs2D does, so the host binds a 1D texture as one row and a texture
                // buffer as 16384-texel rows in the slot (nvn_mtl.m tbuf_as_2d; the reflection flags it fetch1d).
                if (opcode == 0xb66 && (lod == 1 || lod == 3) && type == SamplerType.Texture1D && !ms && !offset)
                {
                    if (!urHandle && !regHandle && lod == 1 &&
                        context.TranslatorContext.GpuAccessor.QuerySamplerType(handle) == SamplerType.TextureBuffer)
                    {
                        type = SamplerType.TextureBuffer;
                        sources.RemoveAt(sources.Count - 1);   // the LOD const a buffer fetch does not take
                        flags &= ~TextureFlags.LodLevel;
                    }
                    else if (Sample1DAs2D)
                    {
                        int ci = (urHandle || regHandle) ? 1 : 0;   // the texel-index coordinate, after the bindless handle if any
                        Operand xi = sources[ci];
                        sources[ci] = context.BitwiseAnd(xi, Const(0x3FFF));
                        sources.Insert(ci + 1, context.ShiftRightU32(xi, Const(14)));
                        context.TranslatorContext.GpuAccessor.RecordTexelFetch1D(handle);
                        type = SamplerType.Texture2D;
                    }
                }
            }

            int count = BitOperations.PopCount((uint)mask);
            Operand[] results = new Operand[count];

            for (int i = 0; i < count; i++)
            {
                results[i] = Local();
            }

            if (opcode == 0xb69)
            {
                ATmml(context, type, flags, handle, cbufSlot, mask, results, sources.ToArray(), urHandle || regHandle);
            }
            else
            {
                SetBindingPair setAndBinding = urHandle || regHandle ? default : context.ResourceManager.GetTextureOrImageBinding(
                    Instruction.TextureSample,
                    type,
                    TextureFormat.Unknown,
                    flags,
                    cbufSlot,
                    handle);

                context.TextureSample(type, flags, setAndBinding, mask, results, sources.ToArray());
            }

            ATexResults(context, x, results, f16);
        }

        // TMML: the LOD a sample would use, x and y in 8.8 fixed point (as Maxwell's)
        private static void ATmml(EmitterContext context, SamplerType type, TextureFlags flags, int handle, int cbufSlot, int mask, Operand[] results, Operand[] sources, bool bindless)
        {
            SetBindingPair setAndBinding = bindless ? default : context.ResourceManager.GetTextureOrImageBinding(Instruction.Lod, type, TextureFormat.Unknown, flags, cbufSlot, handle);
            int r = 0;

            for (int comp = 0; comp < 4; comp++)
            {
                if ((mask & (1 << comp)) == 0)
                {
                    continue;
                }

                if (comp >= 2)
                {
                    context.Copy(results[r++], Const(0));
                    continue;
                }

                Operand res = context.Lod(type, flags, setAndBinding, comp ^ 1, sources);
                context.Copy(results[r++], context.FP32ConvertToS32(context.FPMultiply(res, ConstF(256.0f))));
            }
        }

        // TXQ Rd, Ra: query 62..64 (dimensions with Ra the LOD, texture type); results as the other texture instructions
        public static void ATxq(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int query = x.Bits(62, 64);
            int mask = x.Bits(72, 76);
            // 0x370 is to 0xb6f as 0x361 to 0xb60: the handle in a register, Ra, with the LOD after it (Fire Emblem Fortune's Weave's lighting: MOV R1c, R64; MOV R1d, RZ; TXQ R1d, R1c). The shape
            // has no field here and the size query does not depend on it, so 2D, as Maxwell's bindless TXQ
            // 0xf6f: the handle in a uniform register at 40..46 (bit 91), as 0xf60's TEX, and the LOD in Ra (nvdisasm: TXQ RZ, R18, R18, TEX_HEADER_DIMENSION, UR6, 0x0, 0x1;
            // PRAGMATA)
            bool urHandle = x.Opcode == 0xf6f;

            if (urHandle && (!x.Bit(91) || x.Bits(40, 46) >= RegisterConsts.UniformZeroIndex))
            {
                throw AFail(context, "TXQ handle in a uniform register without bit 91, or URZ");
            }

            bool bindless = x.Opcode == 0x370 || urHandle;
            (int handle, int cbufSlot) = bindless ? (0, TextureOperation.DefaultCbufSlot) : ATexHandle(context, x);
            SamplerType type = bindless ? SamplerType.Texture2D : context.TranslatorContext.GpuAccessor.QuerySamplerType(handle);
            TextureFlags flags = bindless ? TextureFlags.Bindless : TextureFlags.None;

            int count = BitOperations.PopCount((uint)mask);
            Operand[] results = new Operand[count];

            for (int i = 0; i < count; i++)
            {
                results[i] = Local();
            }

            Operand Src(int reg) => reg >= RegisterConsts.RegisterZeroIndex ? Const(0) : context.Copy(Register(reg, RegisterType.Gpr));

            Operand[] sources = urHandle ? [context.Copy(Register(x.Bits(40, 46), RegisterType.Uniform)), Src(x.Ra)] : bindless ? [Src(x.Ra), Src(x.Ra + 1)] : [Src(x.Ra)];

            if (query == 0)
            {
                SetBindingPair setAndBinding = bindless ? default : context.ResourceManager.GetTextureOrImageBinding(
                    Instruction.TextureQuerySize, type, TextureFormat.Unknown, TextureFlags.None, cbufSlot, handle);
                int r = 0;

                for (int comp = 0; comp < 4; comp++)
                {
                    if ((mask & (1 << comp)) != 0)
                    {
                        context.Copy(results[r++], context.TextureQuerySize(type, flags, setAndBinding, comp, sources));
                    }
                }
            }
            else if (query == 1)
            {
                SetBindingPair setAndBinding = bindless ? default : context.ResourceManager.GetTextureOrImageBinding(
                    Instruction.TextureQuerySamples, type, TextureFormat.Unknown, TextureFlags.None, cbufSlot, handle);
                int r = 0;

                for (int comp = 0; comp < 4; comp++)
                {
                    if ((mask & (1 << comp)) != 0)
                    {
                        context.Copy(results[r++], comp == 2 ? context.TextureQuerySamples(type, flags, setAndBinding, sources) : Const(0));
                    }
                }
            }
            else
            {
                throw AFail(context, $"TXQ query {query}");
            }

            ATexResults(context, x, results, false);
        }
    }
}
