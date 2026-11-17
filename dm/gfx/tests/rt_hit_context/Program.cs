// Hand-built ordinary SSA. No game metadata, shader bytes or native runtime.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using Hit = Ryujinx.Graphics.Shader.Translation.RayTracingHitContext;

static class Program
{
    const int State = 3, HitMemory = 4;
    static int _checks;
    static void Check(bool value, string message)
    {
        _checks++;
        if (!value) throw new Exception(message);
    }

    sealed class Fixture
    {
        public readonly BasicBlock Block = new(0);
        public BasicBlock[] Blocks => [Block];
        public readonly Operand[] Imports = new Operand[NativeFunctionState.Words];
        public Fixture()
        {
            for (int slot = 0; slot < Imports.Length; slot++)
                Imports[slot] = Add(Instruction.Load, StorageKind.LocalMemory, Const(State), Const(slot));
        }
        public Operand Add(Instruction inst, params Operand[] sources) => Add(inst, StorageKind.None, sources);
        public Operand Add(Instruction inst, StorageKind storage, params Operand[] sources)
        {
            Operand result = Local();
            Block.Operations.AddLast(new Operation(inst, storage, result, sources));
            return result;
        }
        public void Effect(Instruction inst, StorageKind storage, params Operand[] sources) =>
            Block.Operations.AddLast(new Operation(inst, storage, null, sources));
        public (Operand Low, Operand High) PairAdd((Operand Low, Operand High) a, (Operand Low, Operand High) b)
        {
            Operand low = Add(Instruction.Add, a.Low, b.Low);
            Operand carry = Add(Instruction.ConditionalSelect, Add(Instruction.CompareLessU32, low, a.Low), Const(1), Const(0));
            return (low, Add(Instruction.Add, Add(Instruction.Add, a.High, b.High), carry));
        }
        public (Operand Low, Operand High) Plus((Operand Low, Operand High) a, int value) => PairAdd(a, (Const(value), Const(0)));
        public (Operand Low, Operand High) Scale(Operand value, bool signed = false) =>
            (Add(Instruction.Multiply, value, Const(64)), Add(signed ? Instruction.MultiplyHighS32 : Instruction.MultiplyHighU32, value, Const(64)));
        public (Operand Low, Operand High) Record(Operand index = null) =>
            PairAdd((Imports[18], Imports[19]), Plus(Scale(index ?? Imports[40], true), 0x180));
        public Operand Load((Operand Low, Operand High) address) => Add(Instruction.Load, StorageKind.GlobalMemory, address.Low, address.High);
        public void Sink(Operand value) => Effect(Instruction.Store, StorageKind.StorageBuffer, Const(8), Const(0), value);
        public void Boundary(int slot, Operand value) => Effect(Instruction.Store, StorageKind.LocalMemory, Const(State), Const(slot), value);
        public Operand Phi(params Operand[] sources)
        {
            Operand value = Local(); PhiNode phi = new(value);
            foreach (Operand source in sources) phi.AddSource(new BasicBlock(), source);
            Block.Operations.AddLast(phi); return value;
        }
    }

    static Hit.CompleteSuccessorSet Successors(Fixture successor = null)
    {
        successor ??= new Fixture();
        Hit.FunctionCfg function = new(1, successor.Blocks);
        return new([function], new Dictionary<int, Hit.FunctionCfg> { [1] = function });
    }
    static Hit.Result Lower(Fixture fixture, Hit.CompleteSuccessorSet successors = null) =>
        Hit.Lower(fixture.Blocks, State, HitMemory, new(18, 19, 40), successors ?? Successors());
    static void Reject(Fixture fixture, string reason, Hit.CompleteSuccessorSet successors = null)
    {
        INode[] before = fixture.Block.Operations.ToArray();
        try { Lower(fixture, successors); throw new Exception("Accepted " + reason); }
        catch (InvalidOperationException error)
        {
            Check(error.Message.StartsWith("Unsupported native current-hit access:"), reason + " has explicit diagnostic");
            Check(before.SequenceEqual(fixture.Block.Operations), reason + " leaves the owned graph unchanged");
        }
    }

    static void Synthetic()
    {
        Fixture id = new(); id.Sink(id.Load(id.Plus(id.Record(), 8)));
        Check(Lower(id).InstanceIdLoads == 1, "ID accessor is intrinsic");
        Check(id.Block.Operations.OfType<Operation>().Any(x => x.StorageKind == StorageKind.LocalMemory &&
            x.Inst == Instruction.Load && x.GetSource(0).Value == HitMemory && x.GetSource(1).Value == 5), "ID uses hit word5");

        Fixture matrix = new(); var record = matrix.Record();
        Operand delta = matrix.Load(matrix.Plus(record, 12)); var inverse = matrix.PairAdd(record, matrix.Scale(delta));
        for (int row = 0; row < 3; row++) for (int column = 0; column < 4; column++) matrix.Sink(matrix.Load(matrix.Plus(inverse, 16 + row * 16 + column * 4)));
        var matrixResult = Lower(matrix);
        Check(matrixResult.MatrixLoads == 12, "all inverse matrix components lower");
        Check(!matrix.Block.Operations.OfType<Operation>().Any(x => x.StorageKind == StorageKind.GlobalMemory), "delta and address cone disappear without invented values");
        Check(matrix.Block.Operations.OfType<Operation>().Where(x => x.Inst == Instruction.Load && x.StorageKind == StorageKind.LocalMemory &&
            x.GetSource(0).Value == HitMemory).Select(x => x.GetSource(1).Value).Order().SequenceEqual(Enumerable.Range(9, 12)), "matrix maps row-major words9..20");

        Fixture aliases = new(); var a = aliases.Record();
        Operand lo = aliases.Add(Instruction.Copy, a.Low), hi = aliases.Add(Instruction.Copy, a.High);
        var aliasPair = (aliases.Phi(lo, aliases.Add(Instruction.Copy, lo)), aliases.Phi(hi, aliases.Add(Instruction.Copy, hi)));
        aliases.Sink(aliases.Load(aliases.Plus(aliasPair, 8))); Check(Lower(aliases).InstanceIdLoads == 1, "copy and identical phi aliases");

        Fixture unknown = new(); unknown.Sink(unknown.Load(unknown.Plus(unknown.Record(), 16))); Reject(unknown, "unknown field");
        Fixture other = new(); other.Sink(other.Load(other.Plus(other.Record(other.Add(Instruction.Add, other.Imports[40], Const(1))), 8))); Reject(other, "noncurrent instance");
        Fixture badCarry = new(); var badAddress = badCarry.Plus(badCarry.Record(), 8); badCarry.Sink(badCarry.Load((badAddress.Low, badCarry.Imports[19]))); Reject(badCarry, "unproved high limb");
        Fixture predicateCarry = new(); var predicateRecord = predicateCarry.Record();
        Operand predicateLow = predicateCarry.Add(Instruction.Add, predicateRecord.Low, Const(8));
        Operand maskCarry = predicateCarry.Add(Instruction.CompareLessU32, predicateLow, predicateRecord.Low);
        Operand predicateHigh = predicateCarry.Add(Instruction.Add, predicateRecord.High, maskCarry);
        predicateCarry.Sink(predicateCarry.Load((predicateLow, predicateHigh)));
        Reject(predicateCarry, "all-bits boolean is not numeric carry1");
        Fixture badPhi = new(); var p0 = badPhi.Plus(badPhi.Record(), 8); var p1 = badPhi.Plus(badPhi.Record(), 16);
        badPhi.Sink(badPhi.Load((badPhi.Phi(p0.Low, p1.Low), badPhi.Phi(p0.High, p1.High)))); Reject(badPhi, "different phi pointers");

        Fixture ordinary = new(); Operand custom = ordinary.Load(ordinary.Plus(ordinary.Record(), 8));
        Operand indexed = ordinary.Add(Instruction.Add, Const(0x12340), ordinary.Add(Instruction.Multiply, custom, Const(192)));
        Operand fetched = ordinary.Load((indexed, Const(0))); Operation fetch = (Operation)fetched.AsgOp; ordinary.Sink(fetched);
        Check(Lower(ordinary).InstanceIdLoads == 1 && ordinary.Block.Operations.Contains(fetch), "ordinary geometry fetch remains the same operation");
        Fixture coincidence = new(); coincidence.Sink(coincidence.Load((Const(0x188), Const(0))));
        Check(Lower(coincidence).InstanceIdLoads == 0, "numeric address coincidence selects no intrinsic");
        Fixture numericDelta = new(); numericDelta.Sink(numericDelta.Load(numericDelta.Plus(numericDelta.Record(), 12))); Reject(numericDelta, "numeric inverse-index escape");

        Fixture escaping = new(); var escaped = escaping.Record(); escaping.Sink(escaping.Load(escaping.Plus(escaped, 8))); escaping.Boundary(18, escaped.Low); escaping.Boundary(19, escaped.High);
        Check(Lower(escaping).RemovedBoundarySlots.SequenceEqual(new[] { 18, 19 }), "proven dead scratch boundary stores disappear");
        Fixture live = new(); var liveRecord = live.Record(); live.Sink(live.Load(live.Plus(liveRecord, 8))); live.Boundary(18, liveRecord.Low);
        Fixture reader = new(); reader.Sink(reader.Imports[18]); Reject(live, "live pointer output", Successors(reader));
        const int FaultMemory = 5;
        Fixture faultExit = new();
        faultExit.Effect(Instruction.Store, StorageKind.LocalMemory, Const(FaultMemory), Const(0), Const(1));
        faultExit.Boundary(18, faultExit.Imports[18]); faultExit.Effect(Instruction.Return, StorageKind.None);
        INode[] faultBefore = faultExit.Block.Operations.ToArray();
        Hit.FunctionCfg faultFunction = new(1, faultExit.Blocks);
        Hit.CompleteSuccessorSet faultSuccessors = new([faultFunction], new Dictionary<int, Hit.FunctionCfg> { [1] = faultFunction }, FaultMemory);
        Fixture faultScratch = new(); faultScratch.Boundary(18, faultScratch.Record().Low);
        Check(Lower(faultScratch, faultSuccessors).RemovedBoundarySlots.SequenceEqual(new[] { 18 }), "terminal fault flush cannot reach a resumed shader");
        Check(faultBefore.SequenceEqual(faultExit.Block.Operations), "fault liveness preserves every real guard, bank store and return");
        Fixture noContract = new(); noContract.Boundary(18, noContract.Record().Low);
        Reject(noContract, "fault bank cannot be assumed", Successors(faultExit));
        Fixture zeroFault = new();
        zeroFault.Effect(Instruction.Store, StorageKind.LocalMemory, Const(FaultMemory), Const(0), Const(0));
        zeroFault.Boundary(18, zeroFault.Imports[18]); zeroFault.Effect(Instruction.Return, StorageKind.None);
        Hit.FunctionCfg zeroFunction = new(1, zeroFault.Blocks);
        Fixture zeroScratch = new(); zeroScratch.Boundary(18, zeroScratch.Record().Low);
        Reject(zeroScratch, "zero fault is a live normal return", new([zeroFunction], new Dictionary<int, Hit.FunctionCfg> { [1] = zeroFunction }, FaultMemory));
        Fixture observableFault = new();
        observableFault.Effect(Instruction.Store, StorageKind.LocalMemory, Const(FaultMemory), Const(0), Const(1));
        observableFault.Sink(observableFault.Imports[18]); observableFault.Boundary(18, observableFault.Imports[18]); observableFault.Effect(Instruction.Return, StorageKind.None);
        Hit.FunctionCfg observableFunction = new(1, observableFault.Blocks);
        Fixture observableScratch = new(); observableScratch.Boundary(18, observableScratch.Record().Low);
        Reject(observableScratch, "fault paths still retain observable effects", new([observableFunction], new Dictionary<int, Hit.FunctionCfg> { [1] = observableFunction }, FaultMemory));
        Fixture branchReader = new(); branchReader.Block.Operations.AddLast(new Operation(Instruction.BranchIfTrue, Label(), branchReader.Imports[18]));
        Fixture branchLive = new(); branchLive.Boundary(18, branchLive.Record().Low);
        Reject(branchLive, "branch-label destination still consumes its predicate", Successors(branchReader));
        Fixture branch = new(); branch.Block.Operations.AddLast(new Operation(Instruction.BranchIfTrue, Label(), branch.Record().Low));
        Reject(branch, "AS-derived branch predicate");
        Fixture store = new(); var storeAddress = store.Record(); store.Effect(Instruction.Store, StorageKind.GlobalMemory, storeAddress.Low, storeAddress.High, Const(1)); Reject(store, "AS-derived store");
        Fixture dead = new(); dead.Load(dead.Plus(dead.Record(), 444)); Lower(dead);
        Check(!dead.Block.Operations.OfType<Operation>().Any(x => x.StorageKind == StorageKind.GlobalMemory), "dead unknown address loads disappear as dead SSA");
        Fixture cycle = new(); Operand cyclic = Local(); PhiNode cyclicPhi = new(cyclic);
        cycle.Block.Operations.AddLast(cyclicPhi);
        Operand cyclicLoad = cycle.Load((cycle.Add(Instruction.Add, cycle.Imports[18], cyclic), cycle.Imports[19]));
        cyclicPhi.AddSource(new BasicBlock(), cyclicLoad); cyclicPhi.AddSource(new BasicBlock(), Const(0)); Lower(cycle);
        Check(!cycle.Block.Operations.OfType<Operation>().Any(x => x.StorageKind == StorageKind.GlobalMemory), "dead AS-derived phi cycles leave no raw metadata load");

        Fixture caller = new(); caller.Effect(Instruction.Call, StorageKind.None, Const(2));
        Fixture target = new(); var successor = new Hit.FunctionCfg(1, caller.Blocks); var helper = new Hit.FunctionCfg(2, target.Blocks);
        var complete = new Hit.CompleteSuccessorSet([successor], new Dictionary<int, Hit.FunctionCfg> { [1] = successor, [2] = helper });
        Fixture across = new(); var acrossRecord = across.Record(); across.Boundary(18, acrossRecord.Low);
        Reject(across, "implicit unflushed call state", complete);
        Fixture flushing = new(); flushing.Boundary(18, Const(7)); flushing.Boundary(19, Const(0)); flushing.Effect(Instruction.Call, StorageKind.None, Const(2));
        Fixture consuming = new(); consuming.Sink(consuming.Imports[18]);
        var flushingCfg = new Hit.FunctionCfg(1, flushing.Blocks); var consumingCfg = new Hit.FunctionCfg(2, consuming.Blocks);
        Fixture overwritten = new(); overwritten.Boundary(18, overwritten.Record().Low);
        Check(Lower(overwritten, new([flushingCfg], new Dictionary<int, Hit.FunctionCfg> { [1] = flushingCfg, [2] = consumingCfg })).RemovedBoundarySlots.SequenceEqual(new[] { 18 }),
            "helper reads the flushed value, not its caller's overwritten entry import");
        Fixture missing = new(); missing.Sink(missing.Load(missing.Plus(missing.Record(), 8)));
        Reject(missing, "missing helper target", new([successor], new Dictionary<int, Hit.FunctionCfg> { [1] = successor }));
        Fixture closestCall = new(); closestCall.Effect(Instruction.Call, StorageKind.None, Const(2)); Reject(closestCall, "unproved closest call transfer");
        Fixture malformed = new(); malformed.Effect(Instruction.Return, StorageKind.None, Const(1));
        Fixture valid = new(); valid.Sink(valid.Load(valid.Plus(valid.Record(), 8))); Reject(valid, "valued native successor return", Successors(malformed));
        Fixture dynamicBank = new(); dynamicBank.Sink(dynamicBank.Add(Instruction.Load, StorageKind.LocalMemory, Const(State), dynamicBank.Imports[40]));
        Fixture dynamicConsumer = new(); dynamicConsumer.Boundary(18, dynamicConsumer.Record().Low);
        Reject(dynamicConsumer, "dynamic architectural-bank read cannot prove a scratch slot dead", Successors(dynamicBank));
    }

    // Optional external oracle: ordinary SSA JSON produced by a caller-owned
    // diagnostic translator. No proprietary fixture is stored in this project.
    static BasicBlock[] ReadGraph(string path, int stateMemory)
    {
        JsonObject graph = JsonNode.Parse(File.ReadAllText(path)).AsObject();
        Operand[] operands = graph["operands"].AsArray().Select(x =>
            new Operand(Enum.Parse<OperandType>((string)x["type"]), (int)x["value"])).ToArray();
        Dictionary<int, BasicBlock> blocks = new();
        foreach (var n in graph["nodes"].AsArray())
        {
            int index = (int)n["block"]; if (!blocks.TryGetValue(index, out BasicBlock block)) blocks.Add(index, block = new(index));
            Operand[] sources = n["sources"].AsArray().Select(x => operands[(int)x]).ToArray();
            Operand[] dests = n["dests"].AsArray().Select(x => operands[(int)x]).ToArray();
            if ((string)n["inst"] == "Phi")
            {
                PhiNode phi = new(dests[0]); foreach (Operand source in sources) phi.AddSource(new BasicBlock(), source);
                block.Operations.AddLast(phi);
            }
            else
            {
                Instruction inst = Enum.Parse<Instruction>((string)n["inst"]); StorageKind storage = Enum.Parse<StorageKind>((string)n["storage"]);
                if (storage == StorageKind.LocalMemory && sources.Length >= 2 && sources[0].Type == OperandType.Constant &&
                    sources[0].Value == (int)graph["state_memory"]) sources[0] = Const(stateMemory);
                if (dests.Length > 1 && storage != StorageKind.None) throw new Exception("Oracle has an unsupported multidestination memory operation");
                block.Operations.AddLast(dests.Length > 1 ? new Operation(inst, 0, dests, sources) :
                    new Operation(inst, storage, dests.FirstOrDefault(), sources));
            }
        }
        return blocks.Values.ToArray();
    }

    static void Oracle(string path)
    {
        foreach (var item in JsonNode.Parse(File.ReadAllText(path))["cases"].AsArray())
        {
            Dictionary<int, Hit.FunctionCfg> functions = new();
            int stateMemory = (int)item["state_memory"];
            foreach (var f in item["functions"].AsArray()) functions.Add((int)f["id"], new((int)f["id"], ReadGraph((string)f["graph"], stateMemory)));
            var entries = item["successors"].AsArray().Select(x => functions[(int)x]).ToArray();
            var result = Hit.Lower(ReadGraph((string)item["closest"], stateMemory), stateMemory, 99, new(18, 19, 40), new(entries, functions));
            Check(result.InstanceIdLoads == (int)item["ids"] && result.MatrixLoads == (int)item["matrices"], "external semantic oracle counts");
            Check(result.RemovedBoundarySlots.SequenceEqual(item["removed"].AsArray().Select(x => (int)x)), "external successor-liveness oracle");
            Console.WriteLine($"External SSA oracle: {result.InstanceIdLoads} ID, {result.MatrixLoads} matrix, scratch [{string.Join(',', result.RemovedBoundarySlots)}]");
        }
    }

    static int Main(string[] args)
    {
        try
        {
            Synthetic();
            if (args.Length != 0) Oracle(args[0]);
            Console.WriteLine($"Typed current-hit SSA: {_checks} checks passed"); return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
