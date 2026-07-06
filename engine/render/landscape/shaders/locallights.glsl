// Local point lights (chantier 2 B5): the 16 nearest LightSource
// placements, filled per frame by the scene (flicker already applied).
// v1: point lights, smooth quadratic falloff, NO shadows (key-light
// shadows are a later brick). The landscape (terrain/grass) stays
// sun-only by decision — only meshes and characters include this.

layout(std140, binding = 5) uniform LightsUbo {
    vec4 uLightCount;                 // x = active lights
    vec4 uLightPositionRadius[16];    // xyz world, w radius (m)
    vec4 uLightColorIntensity[16];    // rgb premultiplied color*intensity
};

vec3 localLights(vec3 worldPos, vec3 n) {
    vec3 sum = vec3(0.0);
    int count = int(uLightCount.x + 0.5);
    for (int i = 0; i < count; ++i) {
        vec3 toLight = uLightPositionRadius[i].xyz - worldPos;
        float dist = length(toLight);
        float radius = uLightPositionRadius[i].w;
        if (dist >= radius) {
            continue;
        }
        float atten = 1.0 - dist / radius;
        atten *= atten;
        float diff = max(dot(n, toLight / max(dist, 1e-3)), 0.0);
        sum += uLightColorIntensity[i].rgb * (diff * atten);
    }
    return sum;
}
