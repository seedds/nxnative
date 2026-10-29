// t_vk_depth16_clear: the fragment depth written as given (the reference the clears must match)
#version 450
layout(location = 0) flat in float d;
void main() { gl_FragDepth = d; }
