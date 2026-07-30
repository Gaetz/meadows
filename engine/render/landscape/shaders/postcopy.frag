#version 460 core

// Plain 1:1 copy pass. Exists because vkCmdCopyImage between sampled
// targets costs ~1 ms in layout transitions on MoltenVK where a draw is
// ~0.1 ms — use this for same-size history/copy targets in post chains.
layout(binding = 0) uniform sampler2D uSource;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(uSource, vUv);
}
