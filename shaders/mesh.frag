#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inWorldPos;

layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 3) uniform sampler2D shadowMap;
layout (set = 0, binding = 4) uniform samplerCube irradianceMap;
layout (set = 0, binding = 5) uniform samplerCube prefilterMap;
layout (set = 0, binding = 6) uniform sampler2D brdfLUT;

const float PI = 3.14159265359;

// https://learnopengl.com/PBR/Lighting

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / max(denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / max(denom, 0.0000001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness for ambient / indirect lighting
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

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

    // convert srgb texture to linear color space
    vec3 albedo = pow(texColor.rgb, vec3(2.2)) * inColor.rgb;

    // gltf specification: green = roughness, blue = metallic
    vec4 metalRough = texture(metalRoughTex, inUV);
    float roughness = clamp(metalRough.g * materialData.metalRoughFactors.y, 0.05, 1.0);
    float metallic = clamp(metalRough.b * materialData.metalRoughFactors.x, 0.0, 1.0);
    float ao = 1.0;

    vec3 N = normalize(inNormal);
    vec3 V = normalize(sceneData.cameraPosition.xyz - inWorldPos);
    vec3 R = reflect(-V, N);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    vec3 Lo = vec3(0.0);

    // directional sun light
    {
        vec3 L = normalize(sceneData.sunlightDirection.xyz);
        vec3 H = normalize(V + L);
        vec3 radiance = sceneData.sunlightColor.xyz * sceneData.sunlightDirection.w;

        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        float shadow = calculate_shadow(inWorldPos, N, L);

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * (1.0 - shadow);
    }

    // pointl lights 
    int numLights = int(sceneData.lightCount.x);
    for (int i = 0; i < numLights; ++i) {
        vec3 lightPos = sceneData.pointLights[i].position.xyz;
        vec3 lightColor = sceneData.pointLights[i].color.xyz * sceneData.pointLights[i].color.w;

        vec3 L = normalize(lightPos - inWorldPos);
        vec3 H = normalize(V + L);
        float distance = length(lightPos - inWorldPos);
        float attenuation = 1.0 / (distance * distance);
        vec3 radiance = lightColor * attenuation;

        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }

    // IBL
    vec3 F_ambient = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kS = F_ambient;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    // diffuse irradiance
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    // specular prefilter + 2d BRDF LUT
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBrdf = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefilteredColor * (F_ambient * envBrdf.x + envBrdf.y);

    vec3 ambient = (kD * diffuseIBL + specularIBL) * ao + (sceneData.ambientColor.xyz * albedo);
    vec3 color = ambient + Lo;

    // tone mapping and gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outFragColor = vec4(color, 1.0);
}