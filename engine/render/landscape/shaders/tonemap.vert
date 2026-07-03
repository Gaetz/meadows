#version 460 core

// Fullscreen triangle from gl_VertexID; identity UV passthrough.
out vec2 vUv;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
