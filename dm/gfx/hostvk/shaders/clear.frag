// The clear triangle's fragment stage (docs/design/vulkan-backend.md 2.7): writes the clear value into colour attachment SLOT as the attachment's type (KIND 0 float, 1 unsigned
// integer, 2 signed integer); the pipeline's write mask keeps the other channels and attachments. gen_spirv.sh compiles it for SLOT 0-7 and KIND 0-2 (24 modules)
#version 450
layout(push_constant) uniform P { uvec4 v; float z; } p;
#if KIND == 1
layout(location = SLOT) out uvec4 o;
void main() { o = p.v; }
#elif KIND == 2
layout(location = SLOT) out ivec4 o;
void main() { o = ivec4(p.v); }
#else
layout(location = SLOT) out vec4 o;
void main() { o = uintBitsToFloat(p.v); }
#endif
