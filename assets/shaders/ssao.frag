#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec2 inUV;
layout (location = 0) out float outFragColor;

layout(set = 0, binding = 1) uniform SSAOKernel 
{
	vec4 samples[64];
} kernel;

layout (set = 0, binding = 2) uniform sampler2D gPosition;
layout (set = 0, binding = 3) uniform sampler2D gNormal;
layout (set = 0, binding = 4) uniform sampler2D texNoise;

layout (push_constant) uniform PushConstants
{
	float radius;
	float bias;
	float power;
	int kernelSize;
} push;

// https://learnopengl.com/Advanced-Lighting/SSAO

void main()
{
	vec4 posSample = texture(gPosition, inUV);
	if (posSample.w < 0.5) {
		outFragColor = 1.0;
		return;
	}

	// get input for SSAO algorithm
	vec3 fragPos = (sceneData.view * vec4(posSample.xyz, 1.0)).xyz;
	vec3 normal = normalize(mat3(sceneData.view) * texture(gNormal, inUV).xyz);
	vec2 noiseScale = vec2(textureSize(gPosition, 0)) / 4.0;
	vec3 randomVec = normalize(texture(texNoise, inUV * noiseScale).xyz);

	// create TBN change-of-basis matrix: from tangent-space to view-space
	vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
	vec3 bitangent = cross(normal, tangent);
	mat3 TBN = mat3(tangent, bitangent, normal);

	// iterate over the sample kernel and calculate occlusion factor
	float occlusion = 0.0;
	for (int i = 0; i < push.kernelSize; i++) {
		// get sample position
		vec3 samplePos = TBN * kernel.samples[i].xyz; // from tangent to view-space
		samplePos = fragPos + samplePos * push.radius;

		// project sample position (to sample texture) (to get position on screen/texture)
		vec4 offset = vec4(samplePos, 1.0);
		offset = sceneData.proj * offset; // from view to clip-space
		offset.xyz /= offset.w; // perspective divide
		offset.xyz = offset.xyz * 0.5 + 0.5; // transform to range 0.0 - 1.0

		// skip samples outside the screen
		if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0) {
            continue;
        }

		// get sample depth
		vec4 samplePosData = texture(gPosition, offset.xy);
        if (samplePosData.w < 0.5) {
            continue; // Sky never occludes
        }

		float sampleDepth = (sceneData.view * vec4(samplePosData.xyz, 1.0)).z; // get depth value of kernel space

		// range check & accumulate
		float rangeCheck = smoothstep(0.0, 1.0, push.radius / abs(fragPos.z - sampleDepth));
		occlusion += (sampleDepth >= samplePos.z + push.bias ? 1.0 : 0.0) * rangeCheck;
	}

	occlusion = 1.0 - (occlusion / float(push.kernelSize));
	outFragColor = clamp(pow(occlusion, push.power), 0.0, 1.0);
}