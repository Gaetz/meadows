#version 450

// SSAO Fragment Shader
// Based on Sascha Willems' SSAO implementation

layout (binding = 0) uniform sampler2D samplerPosition;  // World-space position
layout (binding = 1) uniform sampler2D samplerNormal;    // World-space normal
layout (binding = 2) uniform sampler2D samplerNoise;     // Noise texture

#define SSAO_KERNEL_SIZE 64

layout (binding = 3) uniform SSAOKernel {
    vec4 samples[SSAO_KERNEL_SIZE];
} ssaoKernel;

layout (binding = 4) uniform SSAOParams {
    mat4 projection;
    mat4 view;
    float radius;
    float bias;
    float intensity;
    int kernelSize;
} params;

layout (location = 0) in vec2 inUV;
layout (location = 0) out float outFragColor;

void main() {
    // Get G-Buffer values (world space position)
    vec3 fragPosWorld = texture(samplerPosition, inUV).rgb;
    vec3 normalWorld = normalize(texture(samplerNormal, inUV).rgb);

    // Skip background pixels (position is 0,0,0)
    if (length(fragPosWorld) < 0.001) {
        outFragColor = 1.0;
        return;
    }

    // Transform to view space
    vec3 fragPos = (params.view * vec4(fragPosWorld, 1.0)).xyz;
    vec3 normal = normalize((params.view * vec4(normalWorld, 0.0)).xyz);

    // Get a random vector using a noise lookup
    ivec2 texDim = textureSize(samplerPosition, 0);
    ivec2 noiseDim = textureSize(samplerNoise, 0);
    vec2 noiseUV = vec2(float(texDim.x) / float(noiseDim.x), float(texDim.y) / float(noiseDim.y)) * inUV;
    vec3 randomVec = normalize(texture(samplerNoise, noiseUV).xyz * 2.0 - 1.0);

    // Create TBN matrix (tangent space to view space)
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    // Calculate occlusion value
    float occlusion = 0.0;
    int actualKernelSize = min(params.kernelSize, SSAO_KERNEL_SIZE);

    for (int i = 0; i < actualKernelSize; i++) {
        // Get sample position in view space
        vec3 sampleDir = TBN * ssaoKernel.samples[i].xyz;
        vec3 samplePos = fragPos + sampleDir * params.radius;

        // Project sample position to clip space then to UV
        vec4 offset = params.projection * vec4(samplePos, 1.0);
        offset.xy /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;

        // Clamp to valid UV range
        offset.xy = clamp(offset.xy, 0.0, 1.0);

        // Get world position at sample UV and convert to view space
        vec3 sampleWorldPos = texture(samplerPosition, offset.xy).rgb;

        // Skip if we hit background
        if (length(sampleWorldPos) < 0.001) {
            continue;
        }

        vec3 sampleViewPos = (params.view * vec4(sampleWorldPos, 1.0)).xyz;
        float sampleDepth = sampleViewPos.z;

        // Range check - samples too far away should not contribute
        float rangeCheck = smoothstep(0.0, 1.0, params.radius / abs(fragPos.z - sampleDepth));

        // Depth comparison (in view space, looking down -Z, so more negative = further)
        occlusion += (sampleDepth >= samplePos.z + params.bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(actualKernelSize));
    outFragColor = pow(occlusion, params.intensity);
}

