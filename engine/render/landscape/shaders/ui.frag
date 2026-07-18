#version 460 core

// RmlUi surfaces: premultiplied texture x premultiplied vertex color,
// blended ONE / ONE_MINUS_SRC_ALPHA by the pipeline.

layout(binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(uTexture, vUv) * vColor;
}
