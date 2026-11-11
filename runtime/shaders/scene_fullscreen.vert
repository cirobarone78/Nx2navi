#version 450
// The scene grade reads pixel-center texture coordinates from location zero.
layout(location = 0) out vec4 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = vec4(p, 0.0, 1.0);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
