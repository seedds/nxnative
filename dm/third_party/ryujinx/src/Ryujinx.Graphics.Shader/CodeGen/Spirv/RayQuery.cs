using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;
using System;
using static Spv.Specification;
using SpvInstruction = Spv.Generator.Instruction;

namespace Ryujinx.Graphics.Shader.CodeGen.Spirv
{
    static partial class Instructions
    {
        // Sources: private hit-memory ID, Vulkan ray flags, mask, origin.xyz,
        // tmin, direction.xyz, tmax. The software scheduler admits only opaque
        // triangles; a procedural/nonopaque candidate is an explicit fault.
        // Context words: type,t,primitive,geometry,instance,customID,front,u,v,
        // inverse[12] (row-major 3x4), fault. No opaque BVH representation leaks.
        private static OperationResult GenerateRayQueryTrace(CodeGenContext context, AstOperation operation)
        {
            if (operation.SourcesCount != 11 || context.RayQueryScene == null ||
                operation.GetSource(1) is not AstOperand { Type: OperandType.Constant, Value: 1 } ||
                operation.GetSource(0) is not AstOperand { Type: OperandType.Constant } memory ||
                !context.Properties.LocalMemories.TryGetValue(memory.Value, out MemoryDefinition definition) ||
                definition.ArrayLength < 22 || definition.Type != (AggregateType.Array | AggregateType.U32))
            {
                throw new InvalidOperationException("RayQueryTrace requires a scene and a 22-word private hit context.");
            }

            SpvInstruction u32 = context.TypeU32(), f32 = context.TypeFP32(), boolean = context.TypeBool();
            SpvInstruction query = context.RayQueryVariable;
            SpvInstruction zero = context.Constant(u32, 0), one = context.Constant(u32, 1);
            SpvInstruction committed = context.Constant(context.TypeS32(), 1);
            SpvInstruction Src(int index, AggregateType type) => context.Get(type, operation.GetSource(index));
            void Store(int index, SpvInstruction value)
            {
                context.Store(context.AccessChain(context.TypePointer(StorageClass.Private, u32),
                    context.LocalMemories[memory.Value], context.Constant(context.TypeS32(), index)), value);
            }
            SpvInstruction Bits(SpvInstruction value) => context.Bitcast(u32, value);
            SpvInstruction Vector(int first) => context.CompositeConstruct(context.TypeVector(f32, 3),
                Src(first, AggregateType.FP32), Src(first + 1, AggregateType.FP32), Src(first + 2, AggregateType.FP32));

            for (int word = 0; word < 22; word++) Store(word, zero);
            context.RayQueryInitializeKHR(query, context.Load(context.TypeAccelerationStructureKHR(), context.RayQueryScene),
                Src(1, AggregateType.U32), Src(2, AggregateType.U32), Vector(3), Src(6, AggregateType.FP32),
                Vector(7), Src(10, AggregateType.FP32));

            SpvInstruction header = context.Label(), candidate = context.Label();
            SpvInstruction next = context.Label(), done = context.Label();
            context.Branch(header);
            context.AddLabel(header);
            SpvInstruction proceed = context.RayQueryProceedKHR(boolean, query);
            context.LoopMerge(done, next, LoopControlMask.MaskNone);
            context.BranchConditional(proceed, candidate, done);
            context.AddLabel(candidate);
            Store(21, one); // Do not silently treat unsupported candidates as misses.
            context.Branch(next);
            context.AddLabel(next);
            context.Branch(header);
            context.AddLabel(done);

            SpvInstruction type = context.RayQueryGetIntersectionTypeKHR(u32, query, committed);
            Store(0, type);
            SpvInstruction hit = context.Label(), merge = context.Label();
            SpvInstruction isTriangle = context.IEqual(boolean, type, one);
            context.SelectionMerge(merge, SelectionControlMask.MaskNone);
            context.BranchConditional(isTriangle, hit, merge);
            context.AddLabel(hit);
            Store(1, Bits(context.RayQueryGetIntersectionTKHR(f32, query, committed)));
            Store(2, context.RayQueryGetIntersectionPrimitiveIndexKHR(u32, query, committed));
            Store(3, context.RayQueryGetIntersectionGeometryIndexKHR(u32, query, committed));
            Store(4, context.RayQueryGetIntersectionInstanceIdKHR(u32, query, committed));
            Store(5, context.RayQueryGetIntersectionInstanceCustomIndexKHR(u32, query, committed));
            Store(6, context.Select(u32, context.RayQueryGetIntersectionFrontFaceKHR(boolean, query, committed), one, zero));
            SpvInstruction barycentric = context.RayQueryGetIntersectionBarycentricsKHR(context.TypeVector(f32, 2), query, committed);
            Store(7, Bits(context.CompositeExtract(f32, barycentric, 0)));
            Store(8, Bits(context.CompositeExtract(f32, barycentric, 1)));
            SpvInstruction inverse = context.RayQueryGetIntersectionWorldToObjectKHR(
                context.TypeMatrix(context.TypeVector(f32, 3), 4), query, committed);
            for (int row = 0; row < 3; row++)
            {
                for (int column = 0; column < 4; column++)
                {
                    Store(9 + row * 4 + column, Bits(context.CompositeExtract(f32, inverse, column, row)));
                }
            }
            context.Branch(merge);
            context.AddLabel(merge);
            return OperationResult.Invalid;
        }
    }
}
