#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) out vec3 outViewRay;

void main()
{
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	vec4 clip = vec4(uv * 2.0 - 1.0, 0.0, 1.0); 
	gl_Position = clip;

	mat4 invView = inverse(sceneData.view);
	mat4 invProj = inverse(sceneData.proj);
	vec4 unprojected = invView * invProj * vec4(clip.xy, 1.0, 1.0);
	outViewRay = unprojected.xyz / unprojected.w - sceneData.cameraPosition.xyz;
}