namespace Ryujinx.Graphics.Shader.IntermediateRepresentation
{
    enum IoVariable
    {
        Invalid,

        BackColorDiffuse,
        BackColorSpecular,
        BaseInstance,
        BaseVertex,
        ClipDistance,
        CtaId,
        DrawIndex,
        FogCoord,
        FragmentCoord,
        FragmentOutputColor,
        FragmentOutputDepth,
        FrontColorDiffuse,
        FrontColorSpecular,
        FrontFacing,
        GlobalId,
        InstanceId,
        InstanceIndex,
        InvocationId,
        Layer,
        PatchVertices,
        PointCoord,
        PointSize,
        Position,
        PrimitiveId,
        SampleId,
        SubgroupEqMask,
        SubgroupGeMask,
        SubgroupGtMask,
        SubgroupLaneId,
        SubgroupLeMask,
        SubgroupLtMask,
        TessellationCoord,
        TessellationLevelInner,
        TessellationLevelOuter,
        TextureCoord,
        ThreadId,
        ThreadKill,
        UserDefined,
        VertexId,
        VertexIndex,
        ViewportIndex,
        ViewportMask,
        HelperInvocation,   // nxnative: at the end, so the others keep their numbers
        BaryCoord,          // nxnative: the pixel's barycentric coordinates, perspective (a[0x3b4]) and screen-linear (a[0x3a8])
        BaryCoordNoPersp,
        // nxnative: a hull shader's view of the stage's circular buffer (system registers 21 and 22) and its reads of its own outputs (ISBERD with O): the
        // translator's own values, which DirectCbeWrites turns into outputs and constants before any code is generated
        DirectCbeAddressLow,
        DirectCbeAddressHigh,
        IsbeOutput,
    }
}
