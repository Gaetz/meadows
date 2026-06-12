#version 450

layout (location = 0) out vec2 outUV;

void main() {
    // Generate fullscreen triangle UVs (0 to 2)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // Generate fullscreen triangle positions (-1 to 3)
    gl_Position = vec4(outUV * 2.0f - 1.0f, 0.0f, 1.0f);
}
