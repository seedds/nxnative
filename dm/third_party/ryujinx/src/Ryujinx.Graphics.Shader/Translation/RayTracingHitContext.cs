using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using System;
using System.Collections.Generic;
using System.Linq;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // Recognize native current-hit accessors from their SSA provenance. Native
    // AS storage is opaque: no GPU address, address range, shader name or code
    // offset selects an intrinsic. The caller owns the CFGs for this operation.
    static class RayTracingHitContext
    {
        // Native current-instance accessors use B=root+0x180+64*index,
        // B+8 for ID, B+c for a relative64-byte inverse-record index, and
        // inverse-record+0x10 for three rows of four float components.
        private const ulong InstanceIdAddress = 0x180 + 8;
        private const ulong InverseIndexAddress = 0x180 + 12;
        private const ulong InverseMatrixAddress = 0x180 + 16;
        private const int InstanceRecordBytes = 64;
        private const int HitIdWord = 5, HitInverseMatrixWord = 9;

        public readonly record struct ClosestEntrySeed(int RootLowSlot, int RootHighSlot, int InstanceIndexSlot);
        public sealed record FunctionCfg(int Id, BasicBlock[] Blocks);

        // The caller supplies every possible resumed entry and the complete
        // direct-call target map. Missing targets cannot establish dead state.
        public sealed record CompleteSuccessorSet(
            IReadOnlyList<FunctionCfg> Entries,
            IReadOnlyDictionary<int, FunctionCfg> Functions,
            int FaultMemoryId = -1);

        public readonly record struct Result(int InstanceIdLoads, int MatrixLoads, int[] RemovedBoundarySlots);

        private static InvalidOperationException Unsupported(string reason) =>
            new("Unsupported native current-hit access: " + reason);

        private sealed class Graph
        {
            public readonly List<(BasicBlock Block, LinkedListNode<INode> Node)> Nodes = new();
            public readonly Dictionary<Operand, INode> Definitions = new();
            public readonly Dictionary<int, Operand> Imports = new();
            private readonly HashSet<INode> _terminalFaultFlushes = new();
            private readonly List<(INode Value, Instruction Inst, Operand[] Sources, Operand[] Dests)> _snapshot = new();
            private readonly Dictionary<BasicBlock, LinkedListNode<INode>[]> _blockOrder = new();

            public Graph(BasicBlock[] blocks, int stateMemory, int faultMemory)
            {
                if (blocks == null || blocks.Length == 0) throw Unsupported("missing SSA CFG");
                HashSet<INode> owned = new();
                bool prefix = true;
                foreach (BasicBlock block in blocks)
                {
                    if (block == null || _blockOrder.ContainsKey(block)) throw Unsupported("missing or repeated basic block");
                    List<LinkedListNode<INode>> order = new();
                    for (var node = block.Operations.First; node != null; node = node.Next)
                    {
                        order.Add(node);
                        INode value = node.Value;
                        if (!owned.Add(value)) throw Unsupported("a node has multiple owners");
                        Nodes.Add((block, node));
                        Operand[] sources = Sources(value), dests = Dests(value);
                        if (value is Operation { StorageKind: StorageKind.LocalMemory } memory &&
                            sources.Length > 0 && IsConstant(sources[0], stateMemory) &&
                            (memory.Inst is not (Instruction.Load or Instruction.Store) ||
                             sources.Length != (memory.Inst == Instruction.Load ? 2 : 3) ||
                             sources[1].Type != OperandType.Constant || (uint)sources[1].Value >= NativeFunctionState.Words))
                            throw Unsupported("architectural bank access is not a constant slot load/store");
                        foreach (Operand dest in dests)
                        {
                            if (dest.Type == OperandType.Label && value is Operation branch &&
                                branch.Inst is Instruction.Branch or Instruction.BranchIfTrue or Instruction.BranchIfFalse) continue;
                            if (dest.Type != OperandType.LocalVariable || dest.AsgOp != value ||
                                !Definitions.TryAdd(dest, value)) throw Unsupported("CFG is not ordinary renamed SSA");
                        }
                        _snapshot.Add((value, value is Operation op ? op.Inst : 0, sources, dests));
                        if (prefix && value is Operation { Inst: Instruction.Load, StorageKind: StorageKind.LocalMemory } load &&
                            sources.Length == 2 && IsConstant(sources[0], stateMemory) &&
                            sources[1].Type == OperandType.Constant && (uint)sources[1].Value < NativeFunctionState.Words && dests.Length == 1)
                        {
                            if (!Imports.TryAdd(sources[1].Value, dests[0])) throw Unsupported("duplicate entry import");
                        }
                        else prefix = false;
                    }
                    _blockOrder.Add(block, order.ToArray());
                    // Checked native memory and formatted stores fault-return
                    // before the driver can resume any shader. Only the exact
                    // private-bank flush tail is unobservable on that path;
                    // retain all real stores and runtime fault/exit guards.
                    if (faultMemory >= 0 && block.GetLastOp() is Operation { Inst: Instruction.Return, SourcesCount: 0, DestsCount: 0 })
                    {
                        List<INode> flushes = new();
                        var prior = block.Operations.Last.Previous;
                        while (prior?.Value is Operation { Inst: Instruction.Store, StorageKind: StorageKind.LocalMemory } store &&
                               TryStateSlot(store, stateMemory, out _))
                        {
                            flushes.Add(prior.Value); prior = prior.Previous;
                        }
                        if (prior?.Value is Operation { Inst: Instruction.Store, StorageKind: StorageKind.LocalMemory, SourcesCount: 3 } fault &&
                            IsConstant(fault.GetSource(0), faultMemory) && IsConstant(fault.GetSource(1), 0) &&
                            fault.GetSource(2).Type == OperandType.Constant && fault.GetSource(2).Value != 0)
                            _terminalFaultFlushes.UnionWith(flushes);
                    }
                }
                foreach (var (_, node) in Nodes)
                {
                    foreach (Operand source in Sources(node.Value))
                    {
                        if (source.Type == OperandType.LocalVariable && !Definitions.ContainsKey(source))
                            throw Unsupported("SSA source belongs to another function or has no definition");
                    }
                }
            }

            public void ValidateUnchanged()
            {
                foreach (var (block, order) in _blockOrder)
                {
                    var current = block.Operations.First;
                    foreach (var expected in order)
                    {
                        if (current != expected) throw Unsupported("SSA operation order changed during analysis");
                        current = current.Next;
                    }
                    if (current != null) throw Unsupported("SSA operation inserted during analysis");
                }
                for (int i = 0; i < Nodes.Count; i++)
                {
                    var (block, node) = Nodes[i];
                    var (value, inst, sources, dests) = _snapshot[i];
                    if (node.List != block.Operations || node.Value != value ||
                        (node.Value is Operation op ? op.Inst : 0) != inst ||
                        !Sources(node.Value).SequenceEqual(sources) || !Dests(node.Value).SequenceEqual(dests))
                        throw Unsupported("SSA graph changed during analysis");
                }
            }

            public HashSet<Operand> Needed(HashSet<INode> deleted = null, HashSet<INode> replaced = null)
            {
                HashSet<Operand> needed = new();
                Stack<Operand> work = new();
                foreach (var (_, node) in Nodes)
                {
                    if (deleted?.Contains(node.Value) != true && !_terminalFaultFlushes.Contains(node.Value) && IsObservable(node.Value))
                        foreach (Operand source in Sources(node.Value)) work.Push(source);
                }
                while (work.TryPop(out Operand value))
                {
                    if (!needed.Add(value) || !Definitions.TryGetValue(value, out INode definition) ||
                        deleted?.Contains(definition) == true || replaced?.Contains(definition) == true) continue;
                    foreach (Operand source in Sources(definition)) work.Push(source);
                }
                return needed;
            }

            public HashSet<int> LiveInputs(int stateMemory)
            {
                if (Imports.Count != NativeFunctionState.Words) throw Unsupported("incomplete native entry import bank");
                HashSet<Operand> needed = Needed();
                foreach (var (block, node) in Nodes)
                {
                    if (node.Value is not Operation { Inst: Instruction.Call }) continue;
                    HashSet<int> flushed = new();
                    for (var prior = node.Previous; prior != null; prior = prior.Previous)
                    {
                        if (prior.Value is not Operation { Inst: Instruction.Store, StorageKind: StorageKind.LocalMemory } store) break;
                        if (TryStateSlot(store, stateMemory, out int slot)) flushed.Add(slot);
                    }
                    // A call both consumes and preserves shared state. Even a
                    // callee which never reads a slot may leave it for its
                    // caller's reimport. All unflushed slots remain live here.
                    foreach (var (slot, value) in Imports) if (!flushed.Contains(slot)) needed.Add(value);
                }
                return Imports.Where(x => needed.Contains(x.Value)).Select(x => x.Key).ToHashSet();
            }
        }

        private static Operand[] Sources(INode node) => Enumerable.Range(0, node.SourcesCount).Select(node.GetSource).ToArray();
        private static Operand[] Dests(INode node) => Enumerable.Range(0, node.DestsCount).Select(node.GetDest).Where(x => x != null).ToArray();
        private static bool IsConstant(Operand operand, int value) => operand.Type == OperandType.Constant && operand.Value == value;
        private static bool IsObservable(INode node) => node.DestsCount == 0 || node is Operation op &&
            (op.Inst & Instruction.Mask) is Instruction.Branch or Instruction.BranchIfTrue or Instruction.BranchIfFalse or
            Instruction.Return or Instruction.Discard or Instruction.Call or Instruction.ImageAtomic or
            Instruction.AtomicAdd or Instruction.AtomicAnd or Instruction.AtomicCompareAndSwap or
            Instruction.AtomicMaxS32 or Instruction.AtomicMaxU32 or Instruction.AtomicMinS32 or
            Instruction.AtomicMinU32 or Instruction.AtomicOr or Instruction.AtomicSwap or Instruction.AtomicXor;

        private static bool IsGlobal(StorageKind storage) => storage is StorageKind.GlobalMemory or
            StorageKind.GlobalMemoryS8 or StorageKind.GlobalMemoryS16 or StorageKind.GlobalMemoryU8 or
            StorageKind.GlobalMemoryU16 or StorageKind.GlobalMemoryVector;

        private static bool TryStateSlot(Operation operation, int stateMemory, out int slot)
        {
            slot = -1;
            if (operation.SourcesCount != 3 || !IsConstant(operation.GetSource(0), stateMemory) ||
                operation.GetSource(1).Type != OperandType.Constant) return false;
            slot = operation.GetSource(1).Value;
            return (uint)slot < NativeFunctionState.Words;
        }

        private enum ExprKind { Constant, Value, Add, Multiply, Carry, PredicateCarry, Truth01, Compare, HighMultiply, Phi, Cycle, And, Or }
        private sealed record Expr(ExprKind Kind, uint Value, int[] Args);

        // Interned bit-vector expressions keep equality structural and avoid
        // exponential expression trees. Addition and carry are modulo32; a
        // recognized pair explicitly proves the corresponding modulo64 value.
        private sealed class Expressions
        {
            private readonly Dictionary<(ExprKind, uint, string), int> _intern = new();
            private readonly Dictionary<Operand, int> _memo = new(), _ids = new();
            private readonly HashSet<Operand> _busy = new();
            private readonly List<Expr> _expressions = new();
            public Expr this[int index] => _expressions[index];
            private int Id(Operand value)
            {
                if (!_ids.TryGetValue(value, out int id)) _ids.Add(value, id = _ids.Count);
                return id;
            }
            public int Intern(ExprKind kind, uint value, params int[] args)
            {
                var key = (kind, value, string.Join(',', args));
                if (!_intern.TryGetValue(key, out int id))
                {
                    _intern.Add(key, id = _expressions.Count);
                    _expressions.Add(new Expr(kind, value, args));
                }
                return id;
            }
            public int Constant(uint value) => Intern(ExprKind.Constant, value);
            public uint? ConstantValue(int expression) => this[expression].Kind == ExprKind.Constant ? this[expression].Value : null;
            public int Add(params int[] values)
            {
                List<int> terms = new(); uint constant = 0;
                foreach (int value in values)
                {
                    foreach (int term in this[value].Kind == ExprKind.Add ? this[value].Args : [value])
                    {
                        if (ConstantValue(term) is uint c) constant = unchecked(constant + c);
                        else terms.Add(term);
                    }
                }
                if (constant != 0) terms.Add(Constant(constant));
                if (terms.Count == 0) return Constant(0);
                if (terms.Count == 1) return terms[0];
                terms.Sort(); return Intern(ExprKind.Add, 0, terms.ToArray());
            }
            public int Carry(int a, int b)
            {
                uint? ca = ConstantValue(a), cb = ConstantValue(b);
                if (ca == 0 || cb == 0) return Constant(0);
                if (ca.HasValue && cb.HasValue) return Constant((uint)(((ulong)ca.Value + cb.Value) >> 32));
                return Intern(ExprKind.Carry, 0, Math.Min(a, b), Math.Max(a, b));
            }
            private int? Remainder(int total, int part)
            {
                List<int> a = new(this[total].Kind == ExprKind.Add ? this[total].Args : [total]);
                int[] b = this[part].Kind == ExprKind.Add ? this[part].Args : [part];
                uint constant = 0;
                foreach (int term in a) if (ConstantValue(term) is uint c) constant = unchecked(constant + c);
                a.RemoveAll(x => ConstantValue(x).HasValue);
                foreach (int term in b)
                {
                    if (ConstantValue(term) is uint c) constant = unchecked(constant - c);
                    else if (!a.Remove(term)) return null;
                }
                a.Add(Constant(constant)); return Add(a.ToArray());
            }
            private int Predicate(int expression)
            {
                if (ConstantValue(expression) is uint value) return Constant(value != 0 ? uint.MaxValue : 0);
                if (this[expression].Kind is ExprKind.PredicateCarry or ExprKind.Compare) return expression;
                if (this[expression].Kind == ExprKind.Truth01) return Predicate(this[expression].Args[0]);
                if (this[expression].Kind == ExprKind.Carry) return Intern(ExprKind.PredicateCarry, 0, expression);
                return Intern(ExprKind.Compare, (uint)Instruction.CompareNotEqual, expression, Constant(0));
            }
            private int Truth01(int predicate)
            {
                if (ConstantValue(predicate) is uint value) return Constant(value != 0 ? 1u : 0u);
                if (this[predicate].Kind == ExprKind.PredicateCarry) return this[predicate].Args[0];
                if (this[predicate].Kind is ExprKind.Carry or ExprKind.Truth01) return predicate;
                return Intern(ExprKind.Truth01, 0, Predicate(predicate));
            }

            public int Get(Operand value)
            {
                if (_memo.TryGetValue(value, out int cached)) return cached;
                if (!_busy.Add(value)) return Intern(ExprKind.Cycle, (uint)Id(value));
                int result;
                if (value.Type == OperandType.Constant) result = Constant(unchecked((uint)value.Value));
                else if (value.AsgOp is PhiNode phi)
                {
                    int[] sources = Sources(phi).Select(Get).ToArray();
                    result = sources.Length > 0 && sources.All(x => x == sources[0]) ? sources[0] : Intern(ExprKind.Phi, (uint)Id(value));
                }
                else if (value.AsgOp is Operation op)
                {
                    // Loads are opaque scalar values. In particular, a custom
                    // ID used for an SSBO address does not become an AS pointer.
                    int[] s = op.Inst is Instruction.Copy or Instruction.Add or Instruction.Multiply or
                        Instruction.MultiplyHighS32 or Instruction.MultiplyHighU32 or Instruction.BitwiseNot or
                        Instruction.BitwiseAnd or Instruction.BitwiseOr or Instruction.CompareLessU32 or
                        Instruction.CompareNotEqual or Instruction.ConditionalSelect ? Sources(op).Select(Get).ToArray() : [];
                    uint? a = s.Length > 0 ? ConstantValue(s[0]) : null;
                    uint? b = s.Length > 1 ? ConstantValue(s[1]) : null;
                    result = Intern(ExprKind.Value, (uint)Id(value));
                    switch (op.Inst)
                    {
                        case Instruction.Copy: result = s[0]; break;
                        case Instruction.Add: result = Add(s); break;
                        case Instruction.Multiply:
                            result = a == 0 || b == 0 ? Constant(0) : a == 1 ? s[1] : b == 1 ? s[0] :
                                a.HasValue && b.HasValue ? Constant(unchecked(a.Value * b.Value)) :
                                Intern(ExprKind.Multiply, 0, Math.Min(s[0], s[1]), Math.Max(s[0], s[1])); break;
                        case Instruction.MultiplyHighS32:
                        case Instruction.MultiplyHighU32:
                            result = Intern(ExprKind.HighMultiply, (uint)op.Inst, s); break;
                        case Instruction.BitwiseNot when a.HasValue: result = Constant(~a.Value); break;
                        case Instruction.BitwiseAnd:
                        case Instruction.BitwiseOr:
                            result = a.HasValue && b.HasValue ? Constant(op.Inst == Instruction.BitwiseAnd ? a.Value & b.Value : a.Value | b.Value) :
                                op.Inst == Instruction.BitwiseOr && a == 0 ? s[1] : op.Inst == Instruction.BitwiseOr && b == 0 ? s[0] :
                                op.Inst == Instruction.BitwiseAnd && a == uint.MaxValue ? s[1] : op.Inst == Instruction.BitwiseAnd && b == uint.MaxValue ? s[0] :
                                Intern(op.Inst == Instruction.BitwiseAnd ? ExprKind.And : ExprKind.Or, 0, Math.Min(s[0], s[1]), Math.Max(s[0], s[1])); break;
                        case Instruction.CompareLessU32:
                            result = s[0] == s[1] ? Constant(0) : a.HasValue && b.HasValue ? Constant(a.Value < b.Value ? uint.MaxValue : 0) :
                                Remainder(s[0], s[1]) is int rest ? Predicate(Carry(s[1], rest)) : Intern(ExprKind.Compare, (uint)op.Inst, s); break;
                        case Instruction.CompareNotEqual when b == 0 &&
                            this[s[0]].Kind is ExprKind.Carry or ExprKind.Truth01 or ExprKind.PredicateCarry or ExprKind.Compare:
                            result = Predicate(s[0]); break;
                        // IR bool->integer reinterpretation uses 0xffffffff,
                        // whereas an address carry is exactly0/1. Do not erase
                        // the explicit select which establishes that contract.
                        case Instruction.ConditionalSelect when ConstantValue(s[1]) == 1 && ConstantValue(s[2]) == 0:
                            result = Truth01(s[0]); break;
                        case Instruction.ConditionalSelect when a.HasValue: result = a.Value != 0 ? s[1] : s[2]; break;
                    }
                }
                else result = Intern(ExprKind.Value, (uint)Id(value));
                _busy.Remove(value); _memo.Add(value, result); return result;
            }
        }

        private readonly record struct Wide(int Root, int Instance, Operation Delta, int DeltaScale, ulong Constant)
        {
            public bool TryAdd(Wide other, out Wide result)
            {
                result = default;
                if (Delta != null && other.Delta != null && Delta != other.Delta) return false;
                long root = (long)Root + other.Root;
                long instance = (long)Instance + other.Instance, deltaScale = (long)DeltaScale + other.DeltaScale;
                if (root is < 0 or > 1 || instance is < int.MinValue or > int.MaxValue ||
                    deltaScale is < int.MinValue or > int.MaxValue) return false;
                result = new Wide((int)root, (int)instance, Delta ?? other.Delta,
                    (int)deltaScale, unchecked(Constant + other.Constant));
                return true;
            }
        }

        private sealed class Analysis
        {
            private readonly Graph _graph;
            private readonly Expressions _expressions = new();
            private readonly Dictionary<(int, int), Wide> _pairs = new();
            private readonly Dictionary<int, List<(int High, Wide Value)>> _byLow = new();
            private readonly Dictionary<int, Operation> _deltas = new();
            private readonly Operand _rootLow, _rootHigh;
            private readonly int _instance;
            private bool _changed;
            public readonly Dictionary<INode, int> Replacements = new();
            public readonly HashSet<INode> Deleted = new(), DeadCone = new();
            public readonly SortedSet<int> RemovedSlots = new();

            public Analysis(Graph graph, ClosestEntrySeed seed)
            {
                _graph = graph;
                if (!graph.Imports.TryGetValue(seed.RootLowSlot, out _rootLow) ||
                    !graph.Imports.TryGetValue(seed.RootHighSlot, out _rootHigh) ||
                    !graph.Imports.TryGetValue(seed.InstanceIndexSlot, out Operand instance) ||
                    new[] { seed.RootLowSlot, seed.RootHighSlot, seed.InstanceIndexSlot }.Distinct().Count() != 3)
                    throw Unsupported("missing or aliased closest entry seeds");
                _instance = _expressions.Get(instance);
                Put(_expressions.Get(_rootLow), _expressions.Get(_rootHigh), new Wide(1, 0, null, 0, 0));
            }
            private void Put(int low, int high, Wide value)
            {
                if (_pairs.TryGetValue((low, high), out Wide old))
                {
                    if (old != value) throw Unsupported("ambiguous typed pointer pair");
                    return;
                }
                _pairs.Add((low, high), value);
                if (!_byLow.TryGetValue(low, out var list)) _byLow.Add(low, list = new());
                list.Add((high, value)); _changed = true;
            }
            private (int High, Wide Value)[] Options(int low)
            {
                List<(int, Wide)> result = _byLow.TryGetValue(low, out var list) ? new(list) : new();
                if (_expressions.ConstantValue(low) is uint c) result.Add((_expressions.Constant(0), new Wide(0, 0, null, 0, c)));
                return result.ToArray();
            }
            private void Discover()
            {
                Expressions e = _expressions;
                foreach (var (_, node) in _graph.Nodes)
                {
                    if (node.Value is not Operation op || op.Inst is not (Instruction.MultiplyHighS32 or Instruction.MultiplyHighU32)) continue;
                    int a = e.Get(op.GetSource(0)), b = e.Get(op.GetSource(1));
                    foreach (var (scalar, scale) in new[] { (a, b), (b, a) })
                    {
                        if (e.ConstantValue(scale) != InstanceRecordBytes) continue;
                        Wide? value = scalar == _instance && op.Inst == Instruction.MultiplyHighS32 ? new Wide(0, InstanceRecordBytes, null, 0, 0) :
                            _deltas.TryGetValue(scalar, out Operation delta) && op.Inst == Instruction.MultiplyHighU32 ? new Wide(0, 0, delta, InstanceRecordBytes, 0) : null;
                        if (value.HasValue) Put(e.Intern(ExprKind.Multiply, 0, Math.Min(a, b), Math.Max(a, b)), e.Get(op.Dest), value.Value);
                    }
                }
                for (int iteration = 0; iteration <= _graph.Nodes.Count; iteration++)
                {
                    _changed = false;
                    foreach (var (_, node) in _graph.Nodes)
                    {
                        if (node.Value is not Operation { Inst: Instruction.Add } add) continue;
                        int a = e.Get(add.GetSource(0)), b = e.Get(add.GetSource(1));
                        foreach (var (ha, va) in Options(a)) foreach (var (hb, vb) in Options(b))
                            if (va.TryAdd(vb, out Wide result)) Put(e.Get(add.Dest), e.Add(ha, hb, e.Carry(a, b)), result);
                    }
                    if (!_changed) return;
                }
                throw Unsupported("pointer analysis did not converge");
            }
            private Wide? Address(Operation operation) => operation.SourcesCount >= 2 &&
                _pairs.TryGetValue((_expressions.Get(operation.GetSource(0)), _expressions.Get(operation.GetSource(1))), out Wide value) ? value : null;

            public void Run(int stateMemory, HashSet<int> liveSuccessorInputs)
            {
                // Native calls use the shared bank implicitly. Propagating an
                // AS pointer through another callable body requires an explicit
                // interprocedural pointer contract, not an assumed C ABI.
                if (_graph.Nodes.Any(x => x.Node.Value is Operation { Inst: Instruction.Call }))
                    throw Unsupported("closest body transfers through a native call");
                Discover();
                foreach (var (_, node) in _graph.Nodes)
                {
                    if (node.Value is Operation { Inst: Instruction.Load, StorageKind: StorageKind.GlobalMemory } load &&
                        Address(load) is Wide { Root: 1, Instance: InstanceRecordBytes, Delta: null, Constant: InverseIndexAddress })
                        _deltas.Add(_expressions.Get(load.Dest), load);
                }
                Discover();
                foreach (var (_, node) in _graph.Nodes)
                {
                    if (node.Value is not Operation { Inst: Instruction.Load, StorageKind: StorageKind.GlobalMemory } load || Address(load) is not Wide address) continue;
                    if (address is { Root: 1, Instance: InstanceRecordBytes, Delta: null, Constant: InstanceIdAddress }) Replacements.Add(load, HitIdWord);
                    else if (address is { Root: 1, Instance: InstanceRecordBytes, DeltaScale: InstanceRecordBytes } && address.Delta != null &&
                        address.Constant >= InverseMatrixAddress && address.Constant < InverseMatrixAddress + 48 && (address.Constant & 3) == 0)
                        Replacements.Add(load, HitInverseMatrixWord + (int)((address.Constant - InverseMatrixAddress) / 4));
                }
                HashSet<Operand> taint = new() { _rootLow, _rootHigh };
                bool changed;
                do
                {
                    changed = false;
                    foreach (var (_, node) in _graph.Nodes)
                    {
                        if (Replacements.ContainsKey(node.Value) || !Sources(node.Value).Any(taint.Contains)) continue;
                        foreach (Operand dest in Dests(node.Value)) changed |= taint.Add(dest);
                    }
                } while (changed);
                foreach (var (_, node) in _graph.Nodes)
                {
                    if (node.Value is Operation { Inst: Instruction.Store, StorageKind: StorageKind.LocalMemory } store &&
                        store.SourcesCount == 3 && taint.Contains(store.GetSource(2)) && TryStateSlot(store, stateMemory, out int slot))
                    {
                        if (liveSuccessorInputs.Contains(slot)) throw Unsupported($"AS-derived boundary slot {slot} is live in a successor");
                        Deleted.Add(store); RemovedSlots.Add(slot);
                    }
                }
                HashSet<Operand> needed = _graph.Needed(Deleted, Replacements.Keys.ToHashSet());
                foreach (var (_, node) in _graph.Nodes)
                {
                    INode value = node.Value;
                    if (Replacements.ContainsKey(value) || Deleted.Contains(value) || !Sources(value).Any(taint.Contains)) continue;
                    if (IsObservable(value) || value is Operation { Inst: Instruction.Load } load &&
                        IsGlobal(load.StorageKind) && Dests(value).Any(needed.Contains))
                        throw Unsupported("unrecognized AS-derived observable operation");
                    if (Dests(value).All(x => !needed.Contains(x))) DeadCone.Add(value);
                }
            }
        }

        public static Result Lower(BasicBlock[] closest, int stateMemoryId, int hitMemoryId,
            ClosestEntrySeed seed, CompleteSuccessorSet successors)
        {
            // CFG exits have already been validated by NativeFunctionState;
            // all graphs are renamed SSA from the same ResourceManager. The
            // hit memory is the caller's private U32[22] query-result context.
            if (stateMemoryId < 0 || hitMemoryId < 0 || stateMemoryId == hitMemoryId ||
                successors?.Entries == null || successors.Functions == null || successors.Entries.Count == 0 ||
                successors.FaultMemoryId >= 0 && (successors.FaultMemoryId == stateMemoryId || successors.FaultMemoryId == hitMemoryId))
                throw Unsupported("missing state, hit context or complete successor set");
            Dictionary<BasicBlock[], Graph> graphs = new();
            Dictionary<INode, Graph> owners = new();
            Graph GetGraph(BasicBlock[] blocks)
            {
                if (blocks == null) throw Unsupported("missing function CFG");
                if (graphs.TryGetValue(blocks, out Graph known)) return known;
                Graph graph = new(blocks, stateMemoryId, successors.FaultMemoryId);
                foreach (var (_, node) in graph.Nodes)
                    if (!owners.TryAdd(node.Value, graph)) throw Unsupported("SSA nodes shared by different functions");
                graphs.Add(blocks, graph); return graph;
            }
            HashSet<int> visited = new();
            void ValidateClosure(FunctionCfg function)
            {
                if (function == null || function.Id <= 0 ||
                    !successors.Functions.TryGetValue(function.Id, out FunctionCfg target) || target == null ||
                    target.Id != function.Id || target.Blocks != function.Blocks)
                    throw Unsupported("missing or inconsistent native function target");
                if (!visited.Add(function.Id)) return;
                Graph graph = GetGraph(function.Blocks);
                _ = graph.LiveInputs(stateMemoryId); // Check every reachable helper's import bank too.
                foreach (var (_, node) in graph.Nodes)
                {
                    if (node.Value is Operation transfer && (transfer.Inst == Instruction.Discard ||
                        transfer.Inst == Instruction.Return && (transfer.SourcesCount != 0 || transfer.DestsCount != 0)))
                        throw Unsupported("unsupported native successor transfer");
                    if (node.Value is not Operation { Inst: Instruction.Call } call) continue;
                    if (call.DestsCount != 0 || call.SourcesCount != 1 || call.GetSource(0).Type != OperandType.Constant ||
                        !successors.Functions.TryGetValue(call.GetSource(0).Value, out FunctionCfg callee) ||
                        callee == null || callee.Id != call.GetSource(0).Value)
                        throw Unsupported("missing or malformed direct native call target");
                    ValidateClosure(callee);
                }
            }
            HashSet<int> live = new();
            foreach (FunctionCfg successor in successors.Entries)
            {
                ValidateClosure(successor);
                live.UnionWith(GetGraph(successor.Blocks).LiveInputs(stateMemoryId));
            }
            Graph entry = GetGraph(closest);
            if (entry.Imports.Count != NativeFunctionState.Words) throw Unsupported("incomplete closest entry import bank");
            Analysis analysis = new(entry, seed);
            analysis.Run(stateMemoryId, live);
            // No mutation before all proofs succeed. The private analysis and
            // node snapshots cannot be applied to a later/different SSA graph.
            foreach (Graph graph in graphs.Values) graph.ValidateUnchanged();
            int ids = analysis.Replacements.Values.Count(x => x == HitIdWord);
            int matrices = analysis.Replacements.Count - ids;
            foreach (var (block, node) in entry.Nodes)
            {
                if (analysis.Deleted.Contains(node.Value)) Optimizer.RemoveNode(block, node);
                else if (analysis.Replacements.TryGetValue(node.Value, out int word))
                {
                    Operation old = (Operation)node.Value;
                    foreach (Operand source in Sources(old)) if (source.Type == OperandType.LocalVariable) source.UseOps.Remove(old);
                    node.Value = new Operation(Instruction.Load, StorageKind.LocalMemory, old.Dest, Const(hitMemoryId), Const(word));
                }
            }
            // Remove only the proved-dead AS address cone, including cycles of
            // pure phis. In particular B+c has no synthetic numeric replacement.
            foreach (var (block, node) in entry.Nodes)
            {
                if (node.List == block.Operations && analysis.DeadCone.Contains(node.Value)) Optimizer.RemoveNode(block, node);
            }
            return new Result(ids, matrices, analysis.RemovedSlots.ToArray());
        }
    }
}
