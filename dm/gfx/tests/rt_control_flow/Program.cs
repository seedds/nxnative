// Synthetic instructions and IR only; no game code or fixed module addresses.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Program
{
    sealed class Accessor(byte[] code) : IGpuAccessor
    {
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => address >= (ulong)code.Length ?
            default : MemoryMarshal.Cast<byte, ulong>(code.AsSpan((int)address));
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, index);
        public SetBindingPair CreateImageBinding(int count, bool buffer) => new(0, 16);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, index);
        public SetBindingPair CreateTextureBinding(int count, bool buffer) => new(0, 32);
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int index) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;
    }

    static int checks;
    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new InvalidOperationException(message);
    }
    static void Reject(Action action, string message)
    {
        try { action(); }
        catch (InvalidOperationException) { checks++; return; }
        throw new InvalidOperationException("Accepted " + message);
    }
    static UInt128 Base(int opcode, int guard = 7) => (UInt128)(uint)opcode |
        ((UInt128)(uint)guard << 12) | ((UInt128)7 << 87);
    static UInt128 Ret() => Base(AmpereOpcodes.Ret) | ((UInt128)4 << 24) | ((UInt128)3 << 85);
    static UInt128 Transfer(int opcode, long delta, int guard = 7) => Base(opcode, guard) |
        (((UInt128)(ulong)delta & (((UInt128)1 << 50) - 1)) << 32) |
        (opcode == AmpereOpcodes.Call ? (UInt128)1 << 86 : 0);
    static UInt128 Move(int destination, int value) => Base(0x802) |
        ((UInt128)(uint)destination << 16) | ((UInt128)(uint)value << 32);
    static InstOp InstructionAt(UInt128 word, ulong address = 0) =>
        AmpereTable.GetOp(address, (ulong)word, (ulong)(word >> 64));
    static byte[] Code(params (int Offset, UInt128 Word)[] instructions)
    {
        byte[] code = new byte[instructions.Max(x => x.Offset) + 16];
        foreach ((int offset, UInt128 word) in instructions)
        {
            BitConverter.GetBytes((ulong)word).CopyTo(code, offset);
            BitConverter.GetBytes((ulong)(word >> 64)).CopyTo(code, offset + 8);
        }
        return code;
    }
    static DecodedProgram Decode(byte[] code, ulong[] entries, bool rt = true) =>
        Decoder.DecodeAmpere(new ShaderDefinitions(ShaderStage.Compute, 1, 1, 1), new Accessor(code), 0, entries, rt);
    static RayTracingProgram Manifest(int length, ulong[] addresses, params RayTracingModuleRange[] modules) =>
        new(addresses.Select((address, index) => new RayTracingEntry(address, index + 1, 1,
            RayTracingStage.RayGeneration, 0)).ToArray(), modules.Length == 0 ? [new(0, length)] : modules, []);

    static void LogicalMoves()
    {
        UInt128 general = Move(2, 0);
        UInt128 uniform = Base(0x882) | ((UInt128)3 << 16);
        UInt128 immediate = Base(0x882) | ((UInt128)4 << 16) | ((UInt128)0x1234 << 32);
        UInt128 register = Base(0xc82) | ((UInt128)5 << 16) | ((UInt128)9 << 32);
        byte[] code = Code((0, general), (16, uniform), (32, immediate), (48, register), (64, Ret()));
        Accessor accessor = new(code);
        ShaderDefinitions definitions = new(ShaderStage.Compute, 1, 1, 1);
        TranslationOptions options = new(TargetLanguage.Spirv, TargetApi.Vulkan,
            TranslationFlags.Ampere | TranslationFlags.Compute);
        DecodedProgram decoded = Decode(code, [0, 64]);
        RayTracingProgram program = new(
            [new(0, 7, 7, RayTracingStage.RayGeneration, 0), new(64, 13, 13, RayTracingStage.RayGeneration, 0)],
            [new(0, code.Length)], [new(0, (uint)general, 7), new(16, (uint)uniform, 13)]);
        TranslatorContext context = new(0, code.Length, 0, definitions, accessor, options, decoded, program);
        ResourceManager resources = new(ShaderStage.Compute, accessor, new ResourceReservations(false, false));
        Operation Emit(UInt128 word, ulong address)
        {
            EmitterContext emission = new(context, resources, decoded, false, false)
            {
                CurrOp = InstructionAt(word, address),
            };
            emission.CurrOp.Emitter(emission);
            return emission.GetOperations().Single();
        }
        void ConstantMove(UInt128 word, ulong address, int destination, RegisterType type, int value)
        {
            Operation operation = Emit(word, address);
            Register register = operation.GetDest(0).GetRegister();
            Check(operation.Inst == Instruction.Copy && register.Index == destination && register.Type == type,
                "A logical move must retain its architectural destination class.");
            Check(operation.GetSource(0).Type == OperandType.Constant && operation.GetSource(0).Value == value,
                "Both MOV and UMOV must apply their assigned logical IDs without changing ordinary immediates.");
        }
        ConstantMove(general, 0, 2, RegisterType.Gpr, 7);
        ConstantMove(uniform, 16, 3, RegisterType.Uniform, 13);
        ConstantMove(immediate, 32, 4, RegisterType.Uniform, 0x1234);
        Operation ordinary = Emit(register, 48);
        Check(ordinary.GetDest(0).GetRegister().Type == RegisterType.Uniform &&
            ordinary.GetDest(0).GetRegister().Index == 5 && ordinary.GetSource(0).Type == OperandType.Register &&
            ordinary.GetSource(0).GetRegister().Type == RegisterType.Uniform && ordinary.GetSource(0).GetRegister().Index == 9,
            "An unrelocated UMOV register source must remain an ordinary uniform-register move.");
        foreach ((UInt128 word, ulong address) in new[] { (general, 0ul), (uniform, 16ul) })
        {
            try { Emit(word ^ ((UInt128)1 << 16), address); }
            catch (NotSupportedException) { checks++; continue; }
            throw new InvalidOperationException("A logical MOV or UMOV accepted an instruction template mismatch.");
        }
    }

    static int Main()
    {
        try
        {
            LogicalMoves();
            byte[] code = Code((0, Move(6, 11)), (16, Transfer(AmpereOpcodes.Call, 48, 0)),
                (32, Move(7, 22)), (48, Ret()), (80, Move(9, 33)), (96, Ret()));
            DecodedProgram decoded = Decode(code, [0, 80]);
            RayTracingControlFlow.ValidateDecoded(decoded, Manifest(code.Length, [0, 80]));
            DecodedFunction caller = decoded.MainFunction;
            Block call = caller.Blocks.Single(x => x.OpCodes.Any(i => i.Address == 16));
            Check(call.Successors.Count == 2 && call.Successors[0].Address == 32 && call.Successors[1].Address == 80,
                "Predicated driver call must retain fallthrough and enter its target CFG.");
            Check(decoded.GetFunctionByAddress(80) != null && caller.Blocks.Any(x => x.Address == 80),
                "A separately declared metadata entry must also be flattened in its caller.");
            Check(caller.Blocks.Single(x => x.OpCodes.Any(i => i.Address == 96)).Successors.Count == 0,
                "The tail helper RET must not resume at the instruction after CALL.");

            Accessor accessor = new(code);
            ShaderDefinitions definitions = new(ShaderStage.Compute, 1, 1, 1);
            TranslationOptions options = new(TargetLanguage.Spirv, TargetApi.Vulkan,
                TranslationFlags.Ampere | TranslationFlags.Compute);
            TranslatorContext context = new(0, code.Length, 0, definitions, accessor, options, decoded,
                Manifest(code.Length, [0, 80]));
            ResourceManager resources = new(ShaderStage.Compute, accessor, new ResourceReservations(false, false));
            EmitterContext emission = new(context, resources, decoded, false, false)
            {
                CurrOp = call.OpCodes.Single(x => x.Address == 16),
                CurrBlock = call,
            };
            InstEmit.ACall(emission);
            Check(emission.GetOperations().Length == 1 && emission.GetOperations()[0].Inst == Instruction.Branch,
                "A driver call to a declared entry must emit a branch, not an inferred Call.");
            emission.CurrOp = InstructionAt(Ret());
            InstEmit.ARet(emission);
            Check(emission.GetOperations()[1].Inst == Instruction.Return,
                "A validated driver RET is allowed in the first native entry too.");
            EmitterContext badRpc = new(context, resources, decoded, false, false) { CurrOp = InstructionAt(Move(4, 1)) };
            badRpc.CurrOp.Emitter(badRpc);
            Reject(() => RayTracingControlFlow.ValidateOperations(badRpc.GetOperations()), "emitted RPC overwrite");
            DecodedProgram offsetEntry = Decode(code, [80]);
            RayTracingControlFlow.ValidateDecoded(offsetEntry, Manifest(code.Length, [80]));
            Check(offsetEntry.MainFunction.Address == 80 && offsetEntry.Count() == 1,
                "RT decode must not invent an entry at image address0.");

            byte[] unconditional = Code((0, Transfer(AmpereOpcodes.Call, 16)), (16, Move(7, 99)),
                (32, Move(9, 33)), (48, Ret()));
            DecodedProgram tail = Decode(unconditional, [0, 32]);
            RayTracingControlFlow.ValidateDecoded(tail, Manifest(unconditional.Length, [0, 32]));
            Check(!tail.MainFunction.Blocks.SelectMany(x => x.OpCodes).Any(x => x.Address == 16),
                "An unconditional driver call has no fallthrough.");
            byte[] earlyReturn = Code((0, Ret() & ~((UInt128)7 << 12)), (16, Move(9, 33)), (32, Ret()));
            DecodedProgram conditional = Decode(earlyReturn, [0]);
            RayTracingControlFlow.ValidateDecoded(conditional, Manifest(earlyReturn.Length, [0]));
            Check(conditional.MainFunction.Blocks[0].Successors.Single().Address == 16,
                "A predicated driver RET must retain its untaken fallthrough.");

            // The ordinary path still models an ordinary returning native call.
            DecodedProgram ordinary = Decode(code, [0], rt: false);
            Check(ordinary.GetFunctionByAddress(80) != null &&
                !ordinary.MainFunction.Blocks.Any(x => x.Address == 80) &&
                ordinary.MainFunction.Blocks.Any(x => x.Address == 32), "Ordinary call decoding changed.");

            Reject(() => Decode(Code((0, Transfer(AmpereOpcodes.Call, 0) & ~((UInt128)1 << 86)), (16, Ret())), [0]), "CALL.INC");
            Reject(() => Decode(Code((0, Transfer(AmpereOpcodes.Call, 0) & ~((UInt128)7 << 87)), (16, Ret())), [0]), "secondary call predicate");
            foreach (UInt128 bad in new[] { Ret() & ~((UInt128)1 << 85), Ret() & ~((UInt128)1 << 86),
                (Ret() & ~((UInt128)255 << 24)) | ((UInt128)6 << 24), Ret() | ((UInt128)4 << 32),
                Ret() & ~((UInt128)7 << 87), Ret() | ((UInt128)1 << 91) })
                Reject(() => Decode(Code((0, bad)), [0]), "unsupported RET form");
            foreach (int opcode in new[] { AmpereOpcodes.Exit, AmpereOpcodes.Kill, AmpereOpcodes.Brx, AmpereOpcodes.Brxu, 0x343, 0xf55, 0xf56 })
                Reject(() => Decode(Code((0, Base(opcode)), (16, Ret())), [0]), "unsupported transfer");
            Reject(() => Decode(Code((0, Transfer(AmpereOpcodes.Bra, 4)), (16, Ret())), [0]), "unaligned target");
            Reject(() => Decode(Code((0, Transfer(AmpereOpcodes.Bra, -32))), [0]), "negative target wrap");
            Reject(() => RayTracingControlFlow.DirectTarget(InstructionAt(Transfer(AmpereOpcodes.Bra, 16), ulong.MaxValue - 31)), "positive target wrap");
            Reject(() => Decode(Code((0, Move(6, 11))), [0]), "fallthrough beyond available code");
            Reject(() => Decode(new byte[8], [0]), "truncated instruction");
            Reject(() => RayTracingControlFlow.ValidateDecoded(Decode(code, [0]),
                Manifest(code.Length, [0, 80])), "missing declared entry");
            Reject(() => RayTracingControlFlow.ValidateDecoded(decoded,
                Manifest(code.Length, [0, 80], new(0, 64), new(80, 32))), "cross-module tail target");
            byte[] crossFallthrough = Code((0, Move(6, 11)), (16, Ret()));
            Reject(() => RayTracingControlFlow.ValidateDecoded(Decode(crossFallthrough, [0]),
                Manifest(crossFallthrough.Length, [0], new(0, 16), new(16, 16))), "cross-module fallthrough");

            foreach (int rpc in new[] { 4, 5 })
            {
                Reject(() => RayTracingControlFlow.ValidateOperations([new Operation(Instruction.Copy,
                    Register(6, RegisterType.Gpr), Register(rpc, RegisterType.Gpr))]), "RPC data read");
                Reject(() => RayTracingControlFlow.ValidateOperations([new Operation(Instruction.Copy,
                    Register(rpc, RegisterType.Gpr), Const(1))]), "RPC overwrite");
            }
            Reject(() => RayTracingControlFlow.ValidateOperations([new Operation(Instruction.Call, null, Const(1))]), "remaining inferred call");
            Reject(() => RayTracingControlFlow.ValidateOperations([new Operation(Instruction.Discard, null)]), "discard IR");
            RayTracingControlFlow.ValidateOperations([new Operation(Instruction.Copy, Register(4, RegisterType.Uniform),
                Register(5, RegisterType.Uniform)), new Operation(Instruction.Return, null)]);
            Check(true, "Uniform R4/R5 indices are distinct from driver GPR RPC.");
            Console.WriteLine($"RT control flow: {checks} checks passed");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
