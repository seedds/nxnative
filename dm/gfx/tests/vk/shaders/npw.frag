// t_vk_noperspective: the input of location 3 as it arrives, interpolated without perspective (linearly in the window): with npw.vert's output it is the fragment's own place
// in the window, whatever the triangle's size and its vertices' w
#version 450
layout(location = 3) noperspective in vec4 s;
layout(location = 0) out vec4 o;
void main() { o = s; }
