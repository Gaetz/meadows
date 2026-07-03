#version 460 core

layout(location = 0) in vec3 aPos;

layout(std140, binding = 1) uniform ShadowUbo { mat4 uLightViewProj; };

void main() {
    gl_Position = uLightViewProj * vec4(aPos, 1.0);
}
