#version 450

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 0) uniform sampler2D sceneColor;
layout (set = 0, binding = 1) uniform sampler2D sceneDepth;
layout (set = 0, binding = 2) uniform sampler2D gNormal;

layout (push_constant) uniform PushConstants {
    float aberrationStrength;
    float edgeStrength;
    float vignetteIntensity;
} push;

void main() {
    vec2 centerOffset = inUV - vec2(0.5);
    float distFromCenter = length(centerOffset);

    // radial chromatic aberration
    vec2 uvOffset = centerOffset * push.aberrationStrength * distFromCenter;
    float r = texture(sceneColor, inUV + uvOffset).r;
    float g = texture(sceneColor, inUV).g;
    float b = texture(sceneColor, inUV - uvOffset).b;
    vec3 color = vec3(r, g, b);

    // normal + depth discontinuity edge detection (stylized / cyber Outline)
    vec2 texelSize = 1.0 / vec2(textureSize(sceneColor, 0));
    vec3 nCenter = texture(gNormal, inUV).xyz;
    vec3 nRight  = texture(gNormal, inUV + vec2(texelSize.x, 0.0)).xyz;
    vec3 nUp     = texture(gNormal, inUV + vec2(0.0, texelSize.y)).xyz;

    float dCenter = texture(sceneDepth, inUV).r;
    float dRight  = texture(sceneDepth, inUV + vec2(texelSize.x, 0.0)).r;
    float dUp     = texture(sceneDepth, inUV + vec2(0.0, texelSize.y)).r;

    float normalEdge = length(nCenter - nRight) + length(nCenter - nUp);
    float depthEdge  = (abs(dCenter - dRight) + abs(dCenter - dUp)) * 50.0;
    float edgeFactor = clamp((normalEdge + depthEdge) * push.edgeStrength, 0.0, 1.0);

    // sarken silhouettes and boost subtle cyan rim on strong geometric edges
    color = mix(color, color * 0.15, edgeFactor);

    // smooth vignette
    float vignette = smoothstep(0.9, 0.35, distFromCenter * (1.0 + push.vignetteIntensity));
    color *= vignette;

    outFragColor = vec4(color, 1.0);
}