namespace Ryujinx.Graphics.Shader.Decoders
{
    static class RegisterConsts
    {
        public const int GprsCount = 255;
        public const int PredsCount = 7;
        public const int FlagsCount = 4;
        public const int UniformsCount = 63;
        public const int UniformPredsCount = 7;
        public const int ShadowsCount = 256;
        // nxnative: one shadow register past the deferred writes' range (Translator.DeferAmpereWrites hands out 0 .. ShadowsCount - 1), which no frontend writes: BarrierExits' "the thread has exited" flag
        public const int BarrierExitedIndex = ShadowsCount;
        public const int TotalCount = GprsCount + PredsCount + FlagsCount + UniformsCount + UniformPredsCount + ShadowsCount + 1;

        public const int RegisterZeroIndex = GprsCount;
        public const int PredicateTrueIndex = PredsCount;
        public const int UniformZeroIndex = UniformsCount;
        public const int UniformPredicateTrueIndex = UniformPredsCount;
    }
}
