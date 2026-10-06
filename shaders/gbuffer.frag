#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inWorldPos;
layout (location = 4) in vec3 inTangent;
layout (location = 5) in vec3 inBitangent;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out vec4 outAlbedo;
layout (location = 3) out vec4 outMaterial;
layout (location = 4) out vec4 outEmissive;

void main()
{
	vec4 texColor = texture(colorTex, inUV);
	if (texColor.a < 0.5) {
		discard;
	}

	vec3 albedo = texColor.rgb * inColor.rgb;

	mat3 TBN = mat3(normalize(inTangent), normalize(inBitangent), normalize(inNormal));
	vec3 normalSample = texture(normalTex, inUV).rgb * 2.0 - 1.0;
	normalSample.xy *= materialData.extraFactors.y;
	vec3 finalNormal = normalize(TBN * normalSample);

	if (materialData.extraFactors.z > 0.5 && !gl_FrontFacing) {
		finalNormal = -finalNormal;
	}

	vec3 emissive = texture(emissiveTex, inUV).rgb * materialData.emissiveFactor.rgb * materialData.emissiveFactor.w;

	vec4 metalRough = texture(metalRoughTex, inUV);
	float roughness = clamp(metalRough.g * materialData.metalRoughFactors.y, 0.05, 1.0);
	float metallic = clamp(metalRough.b * materialData.metalRoughFactors.x, 0.0, 1.0);

	float rawAO = texture(occlusionTex, inUV).r;
	float occlusionStrength = materialData.extraFactors.x > 0.0 ? materialData.extraFactors.x : 1.0;
	float ao = mix(1.0, rawAO, occlusionStrength);

	outPosition = vec4(inWorldPos, 1.0);
	outNormal = vec4(finalNormal, 1.0);
	outAlbedo = vec4(albedo, 1.0);
	outMaterial = vec4(metallic, roughness, ao, 1.0);
	outEmissive = vec4(emissive, 1.0);
}