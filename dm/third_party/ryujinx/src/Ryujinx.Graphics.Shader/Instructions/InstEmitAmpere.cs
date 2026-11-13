using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static class AmpereOpcodes
    {
        public const int Bra = 0x947;
        public const int Brx = 0x949;
        public const int Brxu = 0x958;   // BRX on a uniform register
        public const int Exit = 0x94d;
        public const int Kill = 0x95b;
        public const int Ret = 0x950;
        public const int Call = 0x944;
        public const int Ipa = 0x326;
        public const int IpaIdx = 0x526;
        public const int Ald = 0x321;
        public const int Ast = 0x322;
        public const int Ldc = 0xb82;
        public const int Ldl = 0x983;
    }

    // Ampere opcode → emitter. ALU instructions come in operand forms (bits 9..11, AmpereOp.Form) and are registered by their 9-bit opcode; the others by the
    // whole 12-bit opcode, which is looked up first.
    static class AmpereTable
    {
        private readonly struct Entry
        {
            public readonly InstName Name;
            public readonly InstEmitter Emitter;
            public readonly InstProps Props;
            public readonly bool UniformGuard;   // a uniform-datapath instruction: its guard (12..15) names a uniform predicate
            public readonly bool WritesRd;

            public Entry(InstName name, InstEmitter emitter, InstProps props, bool uniformGuard, bool writesRd)
            {
                Name = name;
                Emitter = emitter;
                Props = props;
                UniformGuard = uniformGuard;
                WritesRd = writesRd;
            }
        }

        private static readonly Dictionary<int, Entry> _exact = new();
        private static readonly Dictionary<int, Entry> _alu = new();

        static AmpereTable()
        {
            const InstProps B = InstProps.Bra;

            // Control flow. Convergence barriers (BSSY 0x945, BSYNC 0x941, BREAK 0x942), WARPSYNC 0x948, DEPBAR 0x91a and NOP 0x918 only order the warp's
            // threads: per thread they do nothing.
            X(AmpereOpcodes.Bra, InstName.Bra, InstEmit.ABra, B, writesRd: false);
            X(AmpereOpcodes.Brx, InstName.Brx, InstEmit.ABrx, B, writesRd: false);
            X(AmpereOpcodes.Brxu, InstName.Brx, InstEmit.ABrx, B, writesRd: false);
            X(AmpereOpcodes.Exit, InstName.Exit, InstEmit.AExit, B, writesRd: false);
            X(AmpereOpcodes.Kill, InstName.Kil, InstEmit.AKill, B, writesRd: false);
            X(AmpereOpcodes.Ret, InstName.Ret, InstEmit.ARet, B, writesRd: false);
            X(AmpereOpcodes.Call, InstName.Cal, InstEmit.ACall, B, writesRd: false);
            // ERRBAR 0x9ab, which follows a MEMBAR, only orders error reporting. YIELD 0x946 (CuAssembler's sm_86 records: 0x...7946 is "YIELD ;") only lets the scheduler
            // run another of the warp's paths: Resident Evil 4's 21 compute shaders with it have it after MEMBAR; ERRBAR, before a branch.
            foreach (int op in new[] { 0x918, 0x91a, 0x945, 0x941, 0x942, 0x946, 0x948, 0x9ab })
            {
                X(op, InstName.Nop, InstEmit.ANop, InstProps.None, writesRd: false);
            }

            // 0xf55 and 0xf56 move convergence barrier B15 to and from R26 around warp shuffles (BMOV): Hyrule Warriors: Age of Imprisonment's pixel shaders
            // 02e49c189b769074 and 49241cf469c76830 save it before each block of SHFL and restore it after, 16 times each. With the barriers themselves no-ops,
            // they are too: no other instruction reads R26 after them, and the general register next to the barrier field (R15) is read live by an FMUL.
            foreach (int op in new[] { 0xf55, 0xf56 })
            {
                X(op, InstName.Nop, InstEmit.ANop, InstProps.None, writesRd: false);
            }

            InstEmit.RegisterAmpereAlu(Alu, X);
            InstEmit.RegisterAmpereMemory(X);
            InstEmit.RegisterAmpereTensor(X);
            InstEmit.RegisterAmpereHalfCompare(Alu);
            InstEmit.RegisterAmpereGeometry(Alu, X);
        }

        private static void X(int opcode, InstName name, InstEmitter emitter, InstProps props, bool uniform = false, bool writesRd = true)
        {
            _exact[opcode] = new Entry(name, emitter, props, uniform, writesRd);
        }

        private static void Alu(int op9, InstName name, InstEmitter emitter, bool uniform = false, bool writesRd = true)
        {
            _alu[op9] = new Entry(name, emitter, InstProps.None, uniform, writesRd);
        }

        private static bool TryGet(int opcode, out Entry entry)
        {
            return _exact.TryGetValue(opcode, out entry) || _alu.TryGetValue(opcode & 0x1ff, out entry);
        }

        public static InstOp GetOp(ulong address, ulong lo, ulong hi)
        {
            int opcode = (int)(lo & 0xfff);

            if (TryGet(opcode, out Entry e))
            {
                return new InstOp(address, lo, hi, e.Name, e.Emitter, e.Props);
            }

            return new InstOp(address, lo, hi, InstName.Invalid, InstEmit.AUnsupported, InstProps.None);
        }

        public static bool HasUniformGuard(int opcode) => TryGet(opcode, out Entry e) && e.UniformGuard;

        // Whether the instruction reads a constant buffer word with its bank in bits 54..59: LDC, ULDC, or an ALU operation in form 3 or 5 (a uniform one has neither)
        public static bool ReadsConstantBank(AmpereOp x)
        {
            if (x.Opcode is AmpereOpcodes.Ldc or 0xab9 or 0xabb)
            {
                return true;
            }

            return !_exact.ContainsKey(x.Opcode) && _alu.TryGetValue(x.Opcode & 0x1ff, out Entry e) && !e.UniformGuard && x.Form is 3 or 5;
        }

        public static bool WritesRd(int opcode) => !TryGet(opcode, out Entry e) || e.WritesRd;
    }

    // An ALU source operand: a register, a uniform register, a 32-bit immediate or a constant buffer word, with the float or integer modifiers it carries.
    enum AKind
    {
        Reg,
        UReg,
        Imm,
        Cbuf,
        BindlessCbuf,   // cx[URb][offset]: Bank holds URb, the uniform register pair with the constant buffer's handle (ABindlessConstant)
    }

    readonly struct ASrc
    {
        public readonly AKind Kind;
        public readonly int Value;   // register number, immediate, or constant buffer byte offset
        public readonly int Bank;
        public readonly bool Neg;
        public readonly bool Abs;

        public ASrc(AKind kind, int value, int bank = 0, bool neg = false, bool abs = false)
        {
            Kind = kind;
            Value = value;
            Bank = bank;
            Neg = neg;
            Abs = abs;
        }

        public ASrc WithMods(bool neg, bool abs) => new(Kind, Value, Bank, neg, abs);

        public bool IsZeroReg => (Kind == AKind.Reg && Value == RegisterConsts.RegisterZeroIndex) || (Kind == AKind.UReg && Value == RegisterConsts.UniformZeroIndex);
    }

    static partial class InstEmit
    {
        public static void AUnsupported(EmitterContext context)
        {
            AmpereOp op = new(context.CurrOp);

            throw new NotSupportedException($"Ampere instruction {op.Opcode:x3} at 0x{context.CurrOp.Address:x} ({op.Hi:x16}{op.Lo:x16}) is not implemented");
        }

        public static void ANop(EmitterContext context)
        {
        }

        private static NotSupportedException AFail(EmitterContext context, string what)
        {
            AmpereOp op = new(context.CurrOp);

            return new NotSupportedException($"Ampere {op.Opcode:x3} at 0x{context.CurrOp.Address:x}: {what} ({op.Hi:x16}{op.Lo:x16})");
        }

        // ---- operands ----

        // A, B, C of an ALU instruction by its form. Uniform instructions name uniform registers (6 bits) in the register fields and take no constant buffer.
        // neg / abs: whether the operation has the float modifier bits (A 72 / 73, B 63 / 62, C 75 / 74); integer operations use some of them for other fields.
        private static (ASrc A, ASrc B, ASrc C) AAluSrcs(EmitterContext context, AmpereOp x, bool uniform = false, bool neg = true, bool abs = true)
        {
            AKind rk = uniform ? AKind.UReg : AKind.Reg;
            int rbits = uniform ? 6 : 8;

            ASrc Reg(int lo, int negBit, int absBit) => new(rk, x.Bits(lo, lo + rbits), 0, neg && x.Bit(negBit), abs && x.Bit(absBit));

            ASrc a = Reg(24, 72, 73);
            ASrc c64 = Reg(64, 75, 74);

            ASrc Slot1(AKind kind)
            {
                return kind switch
                {
                    AKind.Reg => Reg(32, 63, 62),
                    AKind.Imm => new ASrc(AKind.Imm, (int)x.Imm32),
                    AKind.Cbuf => ACbufOperand(context, x, neg && x.Bit(63), abs && x.Bit(62)),
                    _ => new ASrc(AKind.UReg, x.Bits(32, 38), 0, neg && x.Bit(63), abs && x.Bit(62)),
                };
            }

            int form = x.Form;

            if (uniform)
            {
                return form switch
                {
                    1 => (a, Slot1(AKind.Reg), c64),
                    2 => (a, c64, Slot1(AKind.Imm)),
                    4 => (a, Slot1(AKind.Imm), c64),
                    _ => throw AFail(context, $"uniform ALU form {form}"),
                };
            }

            return form switch
            {
                1 => (a, Slot1(AKind.Reg), c64),
                2 => (a, c64, Slot1(AKind.Imm)),
                3 => (a, c64, Slot1(AKind.Cbuf)),
                4 => (a, Slot1(AKind.Imm), c64),
                5 => (a, Slot1(AKind.Cbuf), c64),
                6 => (a, Slot1(AKind.UReg), c64),
                7 => (a, c64, Slot1(AKind.UReg)),
                _ => throw AFail(context, $"ALU form {form}"),
            };
        }

        // The second source of a two-source ALU operation: B in forms 1, 4, 5, 6; C in forms 2, 3, 7.
        private static (ASrc A, ASrc B) ASecond(EmitterContext context, AmpereOp x, bool uniform = false, bool neg = true, bool abs = true)
        {
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, uniform, neg, abs);

            return (a, x.Form is 1 or 4 or 5 or 6 ? b : c);
        }

        // The only source of a one-source operation (MOV, MUFU, F2I, ...): the B slot.
        private static ASrc AOne(EmitterContext context, AmpereOp x, bool uniform = false, bool neg = true, bool abs = true)
        {
            return AAluSrcs(context, x, uniform, neg, abs).B;
        }

        private static ASrc ACbufOperand(EmitterContext context, AmpereOp x, bool neg, bool abs)
        {
            if (x.Bit(91))
            {
                return new ASrc(AKind.BindlessCbuf, x.Bits(38, 54), x.Bits(32, 38), neg, abs);   // cx[URb][offset], URb at 32 where the register form has Rb
            }

            (int bank, int offset) = AMapC0(context, x.Bits(54, 59), x.Bits(38, 54));

            return new ASrc(AKind.Cbuf, offset, bank, neg, abs);
        }

        // NVN2 compute shaders lay out their driver constants in c[0] per shader (the control section counts them, QueryAmpereComputeC0Layout): from 0x10, the push
        // constants, then a 0x10-byte entry per uniform buffer slot, then one per storage buffer slot, then 8-byte handles: per texture slot, separate texture slot,
        // separate sampler slot and image slot, in that order (all 205 compute shaders of Donkey Kong Bananza read their c[0] that way; Star Fox's 1002069a331ceded has
        // 32 separate textures and 13 separate samplers before its 5 images, its sampler words at 0x258 and 0x260, its image at 0x288, and those counts make its size, 0x290).
        // They are mapped to the places the build's reflection and the host know: the push constants to c[24] (where the graphics stages read theirs), uniform slot r to
        // c[0] + 0x210 + 0x10 r and storage slot s to c[0] + 0x310 + 0x10 s (NVN's compute tables), texture slot t to c[2] + 0x20 + 8 t, separate texture r to
        // c[2] + 0x168 + 8 r, separate sampler r to c[2] + 0x568 + 8 r and image slot i to c[2] + 0x120 + 8 i (NVN's handle tables). Other offsets stay.
        // Push block 1 (control +0xaa words; PRAGMATA's buffer entries and handles) lies between push block 0 and the uniform entries, mapped to c[24] + 0x100.
        // NVN2 graphics stages read the buffer entries the game pushes for them from a bank of their own, c[25 + stage index] (QueryAmpereStageBank): mapped to their push block
        // in c[24] (AStageBlock). A bank past the push constants' (24) that is not the stage's is refused here: the binding table ends there, and it failed later as an
        // IndexOutOfRangeException (PRAGMATA: 18,198 shaders before the map)
        private static (int Bank, int Offset) AMapC0(EmitterContext context, int bank, int offset)
        {
            if (bank > 24)
            {
                if (offset < 0 || offset >= 0x100)
                {
                    throw AFail(context, $"stage buffer bank c[{bank}] read at 0x{offset:x}, past its push block");
                }

                return (24, AStageBlock(context, bank) * 0x100 + offset);
            }

            int[] layout = bank == 0 ? context.TranslatorContext.GpuAccessor.QueryAmpereComputeC0Layout() : null;

            if (layout == null || layout.Length < 5)
            {
                return (bank, offset);
            }

            int ubos = layout[0], ssbos = layout[1], textures = layout[2], size = layout[3], push = (layout[4] + 0xf) & ~0xf;
            // Push block 1 (control +0xaa: the driver's accessor reads +0xa9 + block, the words of each push block; dmsh_abi.h DMSH_PUSH_BLOCKS) follows push block 0,
            // before the uniform entries: PRAGMATA's c8f5a992ec204a44 reads {address, size} pairs at 0x20 / 0x28 and 0x30 / 0x38, right after its one push word, and
            // b12078fe3bce1273, with no push words, the three 8-byte handles it pushes at 0x10, 0x18 and 0x20, then its uniform entries
            int pushed = layout.Length > 7 ? (layout[7] + 0xf) & ~0xf : 0;
            int pb = 0x10 + push;
            int ub = pb + pushed;
            int sb = ub + 0x10 * ubos;
            int hb = sb + 0x10 * ssbos;
            int sepTextures = hb + 8 * textures;
            int sepSamplers = sepTextures + 8 * (layout.Length > 5 ? layout[5] : 0);
            int ib = sepSamplers + 8 * (layout.Length > 6 ? layout[6] : 0);

            if (offset >= 0x10 && offset < pb)
            {
                return (24, offset - 0x10);
            }

            if (offset >= ub && offset < sb)
            {
                // the host's table holds 16 uniform slots (dmsh_abi.h DMSH_C0_CS_UBO_SLOTS); past them lies the storage buffer table
                if (offset - ub >= 0x100)
                {
                    throw AFail(context, $"compute uniform slot {(offset - ub) >> 4}, past the host's 16");
                }

                return (0, 0x210 + (offset - ub));
            }

            // Push block 1, mapped to c[24] + 0x100. PRAGMATA's compute shaders load and store through the buffer addresses it holds (a persistent kernel spins on a work counter
            // behind one); taken for uniform slots, which the game never bound, or read at the wrong place, the GPU never finished its first 3D frame
            if (offset >= pb && offset < ub)
            {
                return (24, 0x100 + (offset - pb));
            }

            if (offset >= sb && offset < hb)
            {
                return (0, 0x310 + (offset - sb));
            }

            if (offset >= hb && offset < sepTextures)
            {
                return (2, 0x20 + (offset - hb));
            }

            if (offset >= sepTextures && offset < sepSamplers)
            {
                return (2, 0x168 + (offset - sepTextures));
            }

            if (offset >= sepSamplers && offset < ib)
            {
                return (2, 0x568 + (offset - sepSamplers));
            }

            if (offset >= ib && offset < size)
            {
                return (2, 0x120 + (offset - ib));
            }

            return (bank, offset);
        }

        // A constant bank past the push constants' (24) is an NVN2 graphics stage's buffer bank: stage i (vertex 0, hull or mesh 1, domain 2, geometry 3, pixel 4) reads c[25 + i]
        // (QueryAmpereStageBank), which holds what the game pushes into the graphics push block 1 + i with nvnCommandBufferPushGraphicsConstants: {address, size} entries, 16 bytes
        // each, or 8-byte handles, as compute reads its own from push block 1. PRAGMATA pushes the mesh stage's six entries into block 2 and the pixel stage's five into block 5 before
        // its visibility and material passes. The bank maps to that block of c[24] (dmsh_abi.h DMSH_PUSH_BLOCKS; an indexed read, LDC Ra / ULDC URa, is moved by the block's base
        // with it). Taken for the uniform slots the game binds, the mesh shader read three unbound slots and the material pass the wrong tables: every object drawn through the
        // visibility buffer came out black. Another stage's bank is refused.
        private static int AStageBlock(EmitterContext context, int bank)
        {
            if (bank != context.TranslatorContext.GpuAccessor.QueryAmpereStageBank())
            {
                throw AFail(context, $"constant bank c[{bank}], which is not this stage's buffer bank");
            }

            return bank - 24;
        }

        // The operand's 32-bit value, modifiers not applied.
        private static Operand AGet(EmitterContext context, ASrc s)
        {
            switch (s.Kind)
            {
                case AKind.Reg:
                    return Register(s.Value, RegisterType.Gpr);
                case AKind.UReg:
                    return Register(s.Value, RegisterType.Uniform);
                case AKind.Imm:
                    return Const(s.Value);
                case AKind.BindlessCbuf:
                    return ABindlessConstant(context, s.Bank, Const(0), s.Value);
                default:
                    if ((s.Value & 3) != 0)
                    {
                        throw AFail(context, "unaligned constant buffer operand");
                    }

                    return Cbuf(s.Bank, s.Value >> 2);
            }
        }

        // The upper word of a 64-bit operand pair.
        private static Operand AGetHigh(EmitterContext context, ASrc s)
        {
            return s.Kind switch
            {
                AKind.Reg => s.Value == RegisterConsts.RegisterZeroIndex ? Const(0) : Register(s.Value + 1, RegisterType.Gpr),
                AKind.UReg => s.Value == RegisterConsts.UniformZeroIndex ? Const(0) : Register(s.Value + 1, RegisterType.Uniform),
                AKind.Imm => Const(s.Value < 0 ? -1 : 0),
                AKind.BindlessCbuf => ABindlessConstant(context, s.Bank, Const(0), s.Value + 4),
                _ => Cbuf(s.Bank, (s.Value >> 2) + 1),
            };
        }

        // A bindless constant buffer read, cx[URb][index + offset]: URb and URb + 1 hold the buffer's 64-bit handle, the hardware's layout: bits 0..44 the address >> 4, 45..63 the
        // size >> 4 (as NVK's nvk_bindless_cbuf). NVN2 compute shaders take it from c[0]'s uniform entries, which the host fills in that layout (nvn_mtl.m,
        // DMSH_C0_CS_UBO) as NVN2's driver does (PRAGMATA's compute shader b12078fe3bce1273: ULDC.64 of c[0] + 0x220, uniform slot 1, then cx[UR][4]); the game
        // builds its own too: those it pushes (b12078fe3bce1273's at push block 1: 0x5a0083055d124, 32 bytes at 0x83055d1240) and those its pixel shaders load with LDG.64 from
        // its buffers. Read as a plain address, a game handle reached no memory pool: a dummy buffer, or 0 through the pool table. The word is read from global memory at the
        // address plus the offset: the storage pass traces the address (handle << 4) back to the handle's constant buffer word (GlobalToStorage.cs TryUnwrapHandle), and an
        // address loaded from memory goes through the memory pool table. An offset past the size reads what lies there, where the hardware reads 0 (open: not checked)
        private static Operand ABindlessConstant(EmitterContext context, int ur, Operand index, int offset)
        {
            if (ur == RegisterConsts.UniformZeroIndex)
            {
                return Const(0);
            }

            Operand lo = Register(ur, RegisterType.Uniform);
            Operand hi = Register(ur + 1, RegisterType.Uniform);

            // The address, 49 bits: (handle & (2^45 - 1)) << 4
            Operand baseLo = context.ShiftLeft(lo, Const(4));
            Operand baseHi = context.BitwiseAnd(context.BitwiseOr(context.ShiftLeft(hi, Const(4)), context.ShiftRightU32(lo, Const(28))), Const(0x1ffff));

            Operand add = context.IAdd(index, Const(offset));
            Operand sum = context.IAdd(baseLo, add);
            Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(sum, baseLo), Const(1), Const(0));

            return context.Load(StorageKind.GlobalMemory, sum, context.IAdd(baseHi, carry));
        }

        private static Operand AFloat(EmitterContext context, ASrc s, Instruction fpType = Instruction.FP32)
        {
            return context.FPAbsNeg(AGet(context, s), s.Abs, s.Neg, fpType);
        }

        private static Operand AInt(EmitterContext context, ASrc s)
        {
            if (s.Abs)
            {
                throw AFail(context, "integer absolute value modifier");
            }

            Operand v = AGet(context, s);

            return s.Neg ? context.INegate(v) : v;
        }

        private static Operand ADestReg(int rd, bool uniform)
        {
            return uniform ? Register(rd, RegisterType.Uniform) : Register(rd, RegisterType.Gpr);
        }

        // The destination register field: 16..24, or 16..22 on the uniform datapath.
        private static int ARd(AmpereOp x, bool uniform) => uniform ? x.Bits(16, 22) : x.Rd;

        private static void ASet(EmitterContext context, AmpereOp x, Operand value, bool uniform = false)
        {
            context.Copy(ADestReg(ARd(x, uniform), uniform), value);
        }

        private static Operand APredReg(int index, bool uniform)
        {
            return Register(index, uniform ? RegisterType.UniformPredicate : RegisterType.Predicate);
        }

        // A predicate source: 3 bits at lo, the not bit at notBit.
        private static Operand APred(EmitterContext context, AmpereOp x, int lo, int notBit, bool uniform = false)
        {
            Operand p = APredReg(x.Bits(lo, lo + 3), uniform);

            return x.Bit(notBit) ? context.BitwiseNot(p) : p;
        }

        private static void ASetPred(EmitterContext context, int index, Operand value, bool uniform = false)
        {
            if (index != RegisterConsts.PredicateTrueIndex)
            {
                context.Copy(APredReg(index, uniform), value);
            }
        }

        // ---- control flow ----

        // BRA [.U] target: taken where the guard (handled here, not by the caller) and the condition predicate (87..90, a uniform one with bit 91) both hold.
        public static void ABra(EmitterContext context)
        {
            InstOp currOp = context.CurrOp;
            AmpereOp op = new(currOp);
            ulong target = Decoder.AmpereTarget(currOp);

            if (target == currOp.Address + Decoder.AmpereInstSize)
            {
                return;
            }

            Operand label = context.GetLabel(target);
            Operand cond = AGuardAndCondition(context, op, op.Bit(91));

            if (cond == null)
            {
                context.Branch(label);
            }
            else
            {
                context.BranchIfTrue(label, cond);
            }
        }

        // The guard (12..15) and the condition predicate (87..90) of a branch-like instruction, ANDed; null when both are always true.
        private static Operand AGuardAndCondition(EmitterContext context, AmpereOp op, bool uniformCondition)
        {
            Operand result = null;

            if (op.Pred != RegisterConsts.PredicateTrueIndex || op.PredInv)
            {
                Operand g = APredReg(op.Pred, AmpereTable.HasUniformGuard(op.Opcode));
                result = op.PredInv ? context.BitwiseNot(g) : g;
            }

            if (op.Bits(87, 90) != RegisterConsts.PredicateTrueIndex || op.Bit(90))
            {
                Operand c = APred(context, op, 87, 90, uniformCondition);
                result = result == null ? c : context.BitwiseAnd(result, c);
            }

            return result;
        }

        // BRX Ra, imm: to the next instruction + Ra + imm; the decoder found the targets in the c1 jump table. Each target but the last is taken where the
        // address matches; the last one unconditionally (Ra holds one of the table's values).
        public static void ABrx(EmitterContext context)
        {
            InstOp currOp = context.CurrOp;
            AmpereOp op = new(currOp);
            int startIndex = context.CurrBlock.HasNext() ? 1 : 0;

            if (context.CurrBlock.Successors.Count <= startIndex)
            {
                throw AFail(context, "BRX with no target found in the c1 jump table");
            }

            long next = (long)currOp.Address + Decoder.AmpereInstSize + op.BranchOffset;
            Operand address = context.IAdd(Register(op.Ra, op.Opcode == AmpereOpcodes.Brxu ? RegisterType.Uniform : RegisterType.Gpr), Const((int)next));
            Block[] targets = context.CurrBlock.Successors.Skip(startIndex).OrderBy(b => b.Address).ToArray();

            for (int i = 0; i < targets.Length; i++)
            {
                Operand label = context.GetLabel(targets[i].Address);

                if (i == targets.Length - 1)
                {
                    context.Branch(label);
                }
                else
                {
                    context.BranchIfTrue(label, context.ICompareEqual(address, Const((int)targets[i].Address)));
                }
            }
        }

        public static void AExit(EmitterContext context)
        {
            AmpereOp op = new(context.CurrOp);

            if (context.IsNonMain)
            {
                throw AFail(context, "EXIT inside a called function");
            }

            Operand lblSkip = null;

            if (op.Bits(87, 90) != RegisterConsts.PredicateTrueIndex || op.Bit(90))
            {
                lblSkip = Label();
                context.BranchIfFalse(lblSkip, APred(context, op, 87, 90));
            }

            if (context.PrepareForReturn())
            {
                context.Return();
            }

            if (lblSkip != null)
            {
                context.MarkLabel(lblSkip);
            }
        }

        public static void AKill(EmitterContext context)
        {
            AmpereOp op = new(context.CurrOp);

            if (op.Bits(87, 90) != RegisterConsts.PredicateTrueIndex || op.Bit(90))
            {
                Operand lblSkip = Label();
                context.BranchIfFalse(lblSkip, APred(context, op, 87, 90));
                context.Discard();
                context.MarkLabel(lblSkip);
            }
            else
            {
                context.Discard();
            }
        }

        // CALL.REL.NOINC target: a function call; the callee returns to the next instruction (RET.REL.NODEC, through the return offset the caller left in a
        // register, which the call itself replaces).
        public static void ACall(EmitterContext context)
        {
            if (Decoder.IsAmpereJumpCall(context.Program, context.CurrOp))
            {
                context.Branch(context.GetLabel(Decoder.AmpereTarget(context.CurrOp)));
                return;
            }

            DecodedFunction function = context.Program.GetFunctionByAddress(Decoder.AmpereTarget(context.CurrOp));

            context.Call(function.Id, false);
        }

        public static void ARet(EmitterContext context)
        {
            if (!context.IsNonMain)
            {
                throw AFail(context, "RET in the main function");
            }

            context.Return();
        }
    }
}
