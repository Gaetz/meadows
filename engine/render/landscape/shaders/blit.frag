#version 460 core

layout(binding = 0) uniform sampler2D uSceneColor;

in vec2 vUv;
out vec4 fragColor;

void main() {
    fragColor = texture(uSceneColor, vUv);
}
