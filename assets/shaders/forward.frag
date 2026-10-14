#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inWorldPos;
layout (location = 3) in vec3 inTangent;
layout (location = 4) in vec3 inBitangent;
layout (location = 5) in flat uint inMaterialIndex;

layout (location = 0) out vec4 outFragColor;

struct MaterialConstants {
	vec4 colorFactors;
	vec4 metalRoughFactors;
	vec4 extraFactors;   // x: occlusionStrength, y: normalScale, z: isDoubleSided
	vec4 emissiveFactor; // xyz: emissiveFactor, w: emissiveStrength
	uint albedoIndex;
	uint normalIndex;
	uint metalRoughIndex;
	uint occlusionIndex;
	uint emissiveIndex;
	uint padding0, padding1, padding2;
};

layout(std430, set = 0, binding = 3) readonly buffer MaterialBuffer {
    MaterialConstants materials[];
} materialBuffer;

// forward VFX / transparency bindings (set 0)
layout(set = 0, binding = 4) uniform sampler2D sceneColorCopy;
layout(set = 0, binding = 5) uniform samplerCube irradianceMap;
layout(set = 0, binding = 6) uniform samplerCube prefilterMap;
layout(set = 0, binding = 7) uniform sampler2D brdfLUT;

// bindless texture array (set 1)
layout(set = 1, binding = 0) uniform sampler2D globalTextures[];

const float PI = 3.14159265359;

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    MaterialConstants mat = materialBuffer.materials[inMaterialIndex];

    vec4 baseSample = texture(globalTextures[nonuniformEXT(mat.albedoIndex)], inUV) * mat.colorFactors;
    float alpha = clamp(baseSample.a, 0.0, 1.0);
    if (alpha < 0.01) {
        discard;
    }

    // normal mapping
    mat3 TBN = mat3(normalize(inTangent), normalize(inBitangent), normalize(inNormal));
    vec3 normalSample = texture(globalTextures[nonuniformEXT(mat.normalIndex)], inUV).rgb * 2.0 - 1.0;
    normalSample.xy *= mat.extraFactors.y;
    vec3 N = normalize(TBN * normalSample);
    if (mat.extraFactors.z > 0.5 && !gl_FrontFacing) {
        N = -N;
    }

    vec4 metalRough = texture(globalTextures[nonuniformEXT(mat.metalRoughIndex)], inUV);
    float metallic  = clamp(metalRough.b * mat.metalRoughFactors.x, 0.0, 1.0);
    float roughness = clamp(metalRough.g * mat.metalRoughFactors.y, 0.04, 1.0);

    vec3 V = normalize(sceneData.cameraPosition.xyz - inWorldPos);
    vec3 R = reflect(-V, N);
    float NdotV = max(dot(N, V), 0.0);

    // fresnel rim to increase opacity at grazing angles
    vec3 F0 = mix(vec3(0.04), baseSample.rgb, metallic);
    vec3 F  = fresnelSchlickRoughness(NdotV, F0, roughness);

    // screenspace refraction using viewspace normal offset
    vec4 clipPos = sceneData.viewproj * vec4(inWorldPos, 1.0);
    vec2 screenUV = (clipPos.xy / clipPos.w) * 0.5 + 0.5;
    vec2 refractionOffset = (mat3(sceneData.view) * N).xy * 0.03 * (1.0 - roughness);
    vec3 refractedBg = texture(sceneColorCopy, clamp(screenUV + refractionOffset, 0.001, 0.999)).rgb;

    // specular & diffuse IBL
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBrdf = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = prefilteredColor * (F * envBrdf.x + envBrdf.y);
    vec3 diffuseIBL  = texture(irradianceMap, N).rgb * baseSample.rgb * (1.0 - metallic);

    // direct sun specular highlight
    vec3 L = normalize(sceneData.sunlightDirection.xyz);
    vec3 H = normalize(V + L);
    float specPower = exp2(10.0 * (1.0 - roughness) + 1.0);
    vec3 sunSpec = sceneData.sunlightColor.rgb * sceneData.sunlightDirection.w * pow(max(dot(N, H), 0.0), specPower) * F;

    // emissive
    vec3 emissive = texture(globalTextures[nonuniformEXT(mat.emissiveIndex)], inUV).rgb
                  * mat.emissiveFactor.rgb * mat.emissiveFactor.w;

    vec3 surfaceColor = mix(refractedBg * baseSample.rgb, diffuseIBL, alpha) + specularIBL + sunSpec + emissive;
    float finalAlpha  = clamp(alpha + dot(F, vec3(0.333)), 0.0, 1.0);

    outFragColor = vec4(surfaceColor, finalAlpha);
}