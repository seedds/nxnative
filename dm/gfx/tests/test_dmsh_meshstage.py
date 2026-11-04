"""Integer fragment pass-through coverage shared with task/mesh outputs."""

from pathlib import Path
import unittest


class IntegerPassThrough(unittest.TestCase):
    """dmsh_build.py fs_uint: a pixel shader that passes a flat input through to an integer target needs the integer entry point: Mario Kart World's dcb0504c844f1ecc, whose R8Uint output
    comes from its mesh stage's per-primitive attribute. A smooth input is not integer data"""

    SRC = """struct main0_out
{
    float4 m_19 [[color(0)]];
};

struct main0_in
{
    float4 m_16 [[user(locn0)%s]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _26 = in.m_16.x;
    out.m_19.x = %s;
    return out;
}
"""

    @classmethod
    def setUpClass(cls):
        import ast, re

        tools = Path(__file__).resolve().parents[1] / "tools"
        tree = ast.parse((tools / "dmsh_build.py").read_text())
        scope = {"re": re, "collections": __import__("collections")}
        exec(
            compile(
                ast.Module(
                    body=[
                        n
                        for n in tree.body
                        if isinstance(n, ast.FunctionDef)
                        and n.name in ("fs_uint", "int_outputs")
                    ],
                    type_ignores=[],
                ),
                "dmsh_build.py",
                "exec",
            ),
            scope,
        )
        cls.fs_uint = staticmethod(scope["fs_uint"])

    def test_flat_input_through_a_local(self):
        src, mask = self.fs_uint(self.SRC % (", flat", "_26"))
        self.assertIn("fragment main0_out_u main0_u(", src)
        self.assertEqual(mask, 0)

    def test_flat_input_directly(self):
        self.assertIsNotNone(self.fs_uint(self.SRC % (", flat", "in.m_16.x"))[0])

    # Kirby Star Allies' grass shader (c44819fbd37e33dc): normal and albedo reach their outputs as the bits of floats, its material word from integer locals, one a denormal literal after SPIRV-Cross
    GBUFFER = """struct main0_out
{
    float4 m_48 [[color(0)]];
    float4 m_50 [[color(1)]];
    float4 m_52 [[color(2)]];
    float4 m_54 [[color(3)]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _10 = in.m_1.x;
    int _20 = 0;
    int _21 = 0;
    if (_10 > 0.0)
    {
        _20 = as_type<int>(_10);
        _21 = 64;
    }
    int _30 = _20;
    int _31 = _21;
    int _32 = _31 << 1;
    out.m_48.x = as_type<float>(_30);
    out.m_50.x = as_type<float>(_30);
    out.m_52.x = as_type<float>(_31);
    out.m_52.y = as_type<float>(_32);
    out.m_52.z = %s;
    out.m_54.x = 0.0;
    return out;
}
"""

    def test_material_word_beside_float_bits(self):
        src, mask = self.fs_uint(self.GBUFFER % "8.96831e-44")
        self.assertEqual(mask, 1 << 2)
        self.assertIn(
            "uint4 m_52 [[color(2)]];\n    float4 m_54 [[color(3)]];\n};\n\nfragment main0_out_m main0_m(",
            src,
        )

    def test_float_bits_are_no_integers(
        self,
    ):  # every output the bits of a float: nothing to tell an integer target by, so the mask stays on the old rule, every output written through integer bits
        src, mask = self.fs_uint("""struct main0_out
{
    float4 m_48 [[color(0)]];
    float4 m_50 [[color(1)]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _10 = in.m_1.x;
    int _20 = as_type<int>(_10);
    out.m_48.x = as_type<float>(_20);
    out.m_50.x = _10;
    return out;
}
""")
        self.assertEqual(mask, 1)

    def test_smooth_input_gets_none(self):
        self.assertIsNone(self.fs_uint(self.SRC % ("", "_26"))[0])

    def test_computed_from_a_flat_input_gets_none(self):
        self.assertIsNone(self.fs_uint(self.SRC % (", flat", "_26 * 2.0"))[0])


if __name__ == "__main__":
    unittest.main()
