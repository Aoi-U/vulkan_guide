#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inWorldPos;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out vec4 outAlbedo;
layout (location = 3) out vec4 outMaterial;

void main()
{
	vec4 texColor = texture(colorTex, inUV);
	if (texColor.a < 0.5) {
		discard;
	}

	vec3 albedo = pow(texColor.rgb, vec3(2.2)) * inColor.rgb;
	vec4 metalRough = texture(metalRoughTex, inUV);

	float roughness = clamp(metalRough.g * materialData.metalRoughFactors.y, 0.05, 1.0);
	float metallic = clamp(metalRough.b * materialData.metalRoughFactors.x, 0.0, 1.0);
	float ao = 1.0; // no ambient occlusion texture yet

	outPosition = vec4(inWorldPos, 1.0);
	outNormal = vec4(normalize(inNormal), 1.0);
	outAlbedo = vec4(albedo, 1.0);
	outMaterial = vec4(metallic, roughness, ao, 1.0);
}