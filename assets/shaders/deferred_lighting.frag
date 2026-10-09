#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outFragColor;

// G-Buffer textures
layout (set = 0, binding = 1) uniform sampler2D gPosition;
layout (set = 0, binding = 2) uniform sampler2D gNormal;
layout (set = 0, binding = 3) uniform sampler2D gAlbedo;
layout (set = 0, binding = 4) uniform sampler2D gMetalRough;

// Shadows & IBL
layout (set = 0, binding = 5) uniform sampler2D shadowMap;
layout (set = 0, binding = 6) uniform samplerCube irradianceMap;
layout (set = 0, binding = 7) uniform samplerCube prefilterMap;
layout (set = 0, binding = 8) uniform sampler2D brdfLUT;
layout (set = 0, binding = 9) uniform samplerCube pointShadowMap;
layout (set = 0, binding = 10) uniform sampler2D ssaoMap;
layout (set = 0, binding = 11) uniform sampler2D gEmissive;

layout (push_constant) uniform PushConstants {
    int enableSSAO;
} push;

const float PI = 3.14159265359;

// https://learnopengl.com/PBR/Lighting
// https://learnopengl.com/Advanced-Lighting/Shadows/Point-Shadows

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.0000001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness for ambient / indirect lighting
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float calculate_shadow(vec3 worldPos, vec3 normal, vec3 lightDir) {
    vec4 lightSpacePos = sceneData.sunlightViewproj * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    vec2 shadowUV = projCoords.xy * 0.5 + 0.5;
    float currentDepth = projCoords.z;

    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0 || currentDepth < 0.0 || currentDepth > 1.0) {
        return 0.0;
    }

    float bias = max(0.002 * (1.0 - max(dot(normal, lightDir), 0.0)), 0.0005);
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

// 20-sample spherical distribution for omnidirectional PCF 
const vec3 sampleOffsetDirections[20] = vec3[](
   vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1), 
   vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
   vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
   vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
   vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
);

float calculate_point_shadow(vec3 fragPos, vec3 lightPos, vec3 normal, float farPlane) {
    vec3 fragToLight = fragPos - lightPos;
    float currentDepth = length(fragToLight);

    // adaptive bias based on surface angle
    float bias = max(0.25 * (1.0 - max(dot(normal, normalize(-fragToLight)), 0.0)), 0.05);

    float viewDistance = length(sceneData.cameraPosition.xyz - fragPos);
    float diskRadius = (1.0 + (viewDistance / farPlane)) / 25.0;

    float shadow = 0.0;
    const int samples = 20;

    for (int i = 0; i < samples; ++i) {
        float closestDepth = texture(pointShadowMap, fragToLight + sampleOffsetDirections[i] * diskRadius).r;
        closestDepth *= farPlane; // undo mapping to [0, 1]
        if (currentDepth - bias > closestDepth) {
            shadow += 1.0;
        }
    }
    return shadow / float(samples);
}

void main() {
    vec4 posSample = texture(gPosition, inUV);
    // discard background pixels so the skybox can render over them
    if (posSample.w < 0.5) {
        discard;
    }

    vec3 inWorldPos = posSample.xyz;
    vec3 N = normalize(texture(gNormal, inUV).xyz);
    vec3 albedo = texture(gAlbedo, inUV).rgb; 

    vec4 mr = texture(gMetalRough, inUV);
    float metallic = mr.r;
    float roughness = mr.g;

    float materialAO = mr.b; // baked gltf ao

    float ssao = (push.enableSSAO != 0) ? texture(ssaoMap, inUV).r : 1.0;
    float totalAO = materialAO * ssao;

    vec3 V = normalize(sceneData.cameraPosition.xyz - inWorldPos);
    vec3 R = reflect(-V, N);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    // directional sunlight
    {
        vec3 L = normalize(sceneData.sunlightDirection.xyz);
        vec3 H = normalize(V + L);
        vec3 radiance = sceneData.sunlightColor.xyz * sceneData.sunlightDirection.w;

        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
        float NdotL = max(dot(N, L), 0.0);
        float shadow = calculate_shadow(inWorldPos, N, L);

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * (1.0 - shadow);
    }

    // point lights
    int numLights = int(sceneData.lightCount.x);
    int shadowLightIndex = int(sceneData.lightCount.y);

    for (int i = 0; i < numLights; ++i) {
        vec3 lightPos = sceneData.pointLights[i].position.xyz;
        float radius = sceneData.pointLights[i].position.w;
        float distance = length(lightPos - inWorldPos);
        if (distance >= radius) continue;

        vec3 L = normalize(lightPos - inWorldPos);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;

        bool castShadow = (sceneData.pointLights[i].params.x > 0.5);
        float shadow = 0.0;
        if (castShadow && shadowLightIndex == i) {
            shadow = calculate_point_shadow(inWorldPos, lightPos, N, radius);
        }

        float distOverRadius = distance / radius;
        float window = clamp(1.0 - distOverRadius * distOverRadius * distOverRadius * distOverRadius, 0.0, 1.0);
        float attenuation = (window * window) / (distance * distance + 1.0);

        vec3 lightColor = sceneData.pointLights[i].color.xyz * sceneData.pointLights[i].color.w;
        vec3 radiance = lightColor * attenuation;

        vec3 H = normalize(V + L);
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * (1.0 - shadow);
    }

    // IBL ambient
    vec3 F_ambient = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD_ambient = (vec3(1.0) - F_ambient) * (1.0 - metallic);

    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    const float MAX_REFLECTION_LOD = 4.0;
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBrdf = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefilteredColor * (F_ambient * envBrdf.x + envBrdf.y);

    vec3 ambient = (kD_ambient * diffuseIBL + specularIBL) * totalAO + (sceneData.ambientColor.xyz * albedo * totalAO);
    vec3 emissive = texture(gEmissive, inUV).rgb;

    vec3 color = ambient + Lo + emissive;

    // tone mapping and gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outFragColor = vec4(color, 1.0);
}