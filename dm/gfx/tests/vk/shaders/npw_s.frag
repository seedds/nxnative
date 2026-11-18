// t_vk_noperspective: npw.frag with the input interpolated the default way (with perspective correction): the control case
#version 450
layout(location = 3) in vec4 s;
layout(location = 0) out vec4 o;
void main() { o = s; }
