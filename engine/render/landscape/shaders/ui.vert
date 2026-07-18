#version 460 core

// RmlUi geometry (H4): pixel-space positions -> NDC, top-left origin.

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor; // premultiplied
layout(location = 2) in vec2 aUv;

layout(std140, binding = 0) uniform UiUbo {
    vec4 uTransform; // xy = translation (px), zw = viewport size (px)
};

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;

void main() {
    vec2 pos = aPos + uTransform.xy;
    vec2 ndc = vec2(pos.x / uTransform.z * 2.0 - 1.0,
                    1.0 - pos.y / uTransform.w * 2.0);
    vColor = aColor;
    vUv = aUv;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
