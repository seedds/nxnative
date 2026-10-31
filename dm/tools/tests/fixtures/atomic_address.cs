// Evaluate the production Get40BitsAddress IR before global-to-storage lowering
// can cancel the address high word. This tests address arithmetic, not source text.
using System;
using System.Reflection;
using System.Runtime.Loader;

class AtomicAddress
{
    static object Property(object o, string name) => o.GetType().GetProperty(name).GetValue(o);
    static uint Eval(object operand, uint low, uint high)
    {
        string type = Property(operand, "Type").ToString();
        if (type == "Constant") return unchecked((uint)(int)Property(operand, "Value"));
        if (type == "Register") return ((int)Property(operand, "Value") & 0xffffff) == 2 ? low : high;
        object op = Property(operand, "AsgOp");
        uint Arg(int n) => Eval(op.GetType().GetMethod("GetSource").Invoke(op, new object[] { n }), low, high);
        return Property(op, "Inst").ToString() switch
        {
            "Add" => unchecked(Arg(0) + Arg(1)),
            "CompareLessU32" => Arg(0) < Arg(1) ? uint.MaxValue : 0,
            "ConditionalSelect" => Arg(0) != 0 ? Arg(1) : Arg(2),
            var name => throw new Exception("Unexpected address IR: " + name),
        };
    }
    static int Main(string[] args)
    {
        var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(args[0]);
        Type Context = assembly.GetType("Ryujinx.Graphics.Shader.Translation.EmitterContext");
        Type Register = assembly.GetType("Ryujinx.Graphics.Shader.Decoders.Register");
        Type RegisterType = assembly.GetType("Ryujinx.Graphics.Shader.Decoders.RegisterType");
        object reg = Activator.CreateInstance(Register, new object[] { 2, Enum.Parse(RegisterType, "Gpr") });
        var emit = assembly.GetType("Ryujinx.Graphics.Shader.Instructions.InstEmit").GetMethod("Get40BitsAddress", BindingFlags.NonPublic | BindingFlags.Static);
        // Literal expectations: no low-word borrow, borrow, positive carry and
        // non-extended wrap. A negative displacement must sign-extend to high.
        var cases = new (uint lo, uint hi, int offset, bool extended, uint wantLo, uint wantHi)[] {
            (8, 2, -4, true, 4, 2), (0, 2, -4, true, 0xfffffffc, 1),
            (0xfffffffc, 2, 4, true, 0, 3), (0xfffffffc, 2, 4, false, 0, 0),
            (8, 2, 4, true, 12, 2), (0, 2, -4, false, 0xfffffffc, 0),
        };
        int bad = 0;
        foreach (var c in cases)
        {
            object result = emit.Invoke(null, new object[] { Activator.CreateInstance(Context, true), reg, c.extended, c.offset });
            uint lo = Eval(result.GetType().GetField("Item1").GetValue(result), c.lo, c.hi);
            uint hi = Eval(result.GetType().GetField("Item2").GetValue(result), c.lo, c.hi);
            if (lo != c.wantLo || hi != c.wantHi) {
                Console.WriteLine($"{c.hi:x}:{c.lo:x} + {c.offset}, E={c.extended}: got {hi:x}:{lo:x}, want {c.wantHi:x}:{c.wantLo:x}"); bad++;
            }
        }
        return bad == 0 ? 0 : 1;
    }
}
