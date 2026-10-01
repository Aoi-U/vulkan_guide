#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inWorldPos;

layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 3) uniform sampler2D shadowMap;
layout (set = 0, binding = 4) uniform samplerCube skyMap;

// https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping
float calculate_shadow(vec3 worldPos, vec3 normal, vec3 lightDir)
{
    vec4 lightSpacePos = sceneData.sunlightViewproj * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    vec2 shadowUV = projCoords.xy * 0.5 + 0.5;
    float currentDepth = projCoords.z;

    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0 || currentDepth < 0.0 || currentDepth > 1.0) {
        return 0.0;
    }

    float bias = max(0.003 * (1.0 - max(dot(normal, lightDir), 0.0)), 0.0008);
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, shadowUV + vec2(x, y) * texelSize).r;
            shadow += (currentDepth - bias > pcfDepth) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

void main()
{
    vec4 texColor = texture(colorTex, inUV);
    if (texColor.a < 0.5f) {
    discard;
    }
    vec4 metalRough = texture(metalRoughTex, inUV);
    float roughness = clamp(metalRough.g * materialData.metalRoughFactors.y, 0.04, 1.0);
    float metallic  = clamp(metalRough.b * materialData.metalRoughFactors.x, 0.0, 1.0);

    vec3 N = normalize(inNormal);
    vec3 V = normalize(sceneData.cameraPosition.xyz - inWorldPos);
    vec3 L = normalize(sceneData.sunlightDirection.xyz);
    vec3 H = normalize(L + V);
    
    vec3 albedo = inColor.rgb * texColor.rgb;
    vec3 sunLight = sceneData.sunlightColor.xyz * sceneData.sunlightColor.w; // color * intensity

    // direct lighting
    float NdotL = max(dot(N, L), 0.0);
    vec3 directDiffuse = albedo * sunLight * NdotL;

    float NdotH = max(dot(N, H), 0.0);
    float shininess = mix(128.0, 4.0, roughness);
    float spec = pow(NdotH, shininess) * NdotL;
    vec3 directSpecular = mix(vec3(0.04), albedo, metallic) * spec * sunLight;

    float shadow = calculate_shadow(inWorldPos, N, L);
    vec3 directLighting = (directDiffuse + directSpecular) * (1.0 - shadow);

    // environment lighting
    vec3 R = reflect(-V, N);
    vec3 envReflection = texture(skyMap, R).rgb;
    vec3 envDiffuse = texture(skyMap, N).rgb;

    // ambient lighting
    vec3 ambient = albedo * (sceneData.ambientColor.xyz + envDiffuse * 0.2);
    vec3 indirectSpecular = envReflection * mix(vec3(0.04), albedo, metallic) * (1.0 - roughness);

    outFragColor = vec4(ambient + directLighting + indirectSpecular, 1.0);
}
