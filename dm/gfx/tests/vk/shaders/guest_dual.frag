// t_vk_guest: the second blend source written as an ordinary output at location 1, without an index, as Super Mario Galaxy's GX shaders write it (the guest layer makes it
// location 0, index 1 for a pipeline that blends with it); the first source is the colour the vertex stage gives (flat)
#version 450
layout(location = 0) flat in vec4 col;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main() { o0 = col; o1 = vec4(0.25, 0.5, 0.75, 1.0); }
