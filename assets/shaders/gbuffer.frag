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

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out vec4 outAlbedo;
layout (location = 3) out vec4 outMaterial;
layout (location = 4) out vec4 outEmissive;

struct MaterialConstants 
{
	vec4 colorFactors;
	vec4 metalRoughFactors;
	vec4 extraFactors; // x: occlusion strength, y: normal scale, z: isDoubleSided
	vec4 emissiveFactor; // xtz: emissiveFactor, w: emissiveStrength
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

layout(set = 1, binding = 0) uniform sampler2D globalTextures[]; // bindless texture array

void main()
{
	MaterialConstants mat = materialBuffer.materials[inMaterialIndex];

	// fetch albedo
	vec4 texColor = texture(globalTextures[nonuniformEXT(mat.albedoIndex)], inUV);
	if (texColor.a < 0.5) {
		discard;
	}
	vec3 albedo = texColor.rgb * mat.colorFactors.rgb;

	// fetch normal
	mat3 TBN = mat3(normalize(inTangent), normalize(inBitangent), normalize(inNormal));
	vec3 normalSample = texture(globalTextures[nonuniformEXT(mat.normalIndex)], inUV).rgb * 2.0 - 1.0;
	normalSample.xy *= mat.extraFactors.y;
	vec3 finalNormal = normalize(TBN * normalSample);
	if (mat.extraFactors.z > 0.5 && !gl_FrontFacing) {
		finalNormal = -finalNormal;
	}

	// fetch emissive
	//vec3 emissive = texture(globalTextures[nonuniformEXT(mat.emissiveIndex)], inUV).rgb * mat.emissiveFactor.rgb * mat.emissiveFactor.w;

	vec3 rawEmissive = texture(globalTextures[nonuniformEXT(mat.emissiveIndex)], inUV).rgb;
    rawEmissive = max(rawEmissive - vec3(0.02), vec3(0.0));
    vec3 emissive = rawEmissive * mat.emissiveFactor.rgb * mat.emissiveFactor.w;

	// fetch metalrough
	vec4 metalRough = texture(globalTextures[nonuniformEXT(mat.metalRoughIndex)], inUV);
	float roughness = clamp(metalRough.g * mat.metalRoughFactors.y, 0.0, 1.0);
	float metallic = clamp(metalRough.b * mat.metalRoughFactors.x, 0.0, 1.0);

	// fetch occlusion
	float rawAO = texture(globalTextures[nonuniformEXT(mat.occlusionIndex)], inUV).r;
	float occlusionStrength = mat.extraFactors.x > 0.0 ? mat.extraFactors.x : 1.0;
	float ao = mix(1.0, rawAO, occlusionStrength);

	outPosition = vec4(inWorldPos, 1.0);
	outNormal = vec4(finalNormal, 1.0);
	outAlbedo = vec4(albedo, 1.0);
	outMaterial = vec4(metallic, roughness, ao, 1.0);
	outEmissive = vec4(emissive, 1.0);
}