// Per-frame constants shared by every landscape shader. Must match
// render::FrameUniforms (engine/render/landscape/FrameUniforms.hpp) exactly.
layout(std140, binding = 0) uniform FrameUbo {
    mat4 uViewProj;
    vec4 uCameraPos; // xyz = camera world position
    vec4 uTime;      // x = seconds since scene start
};
