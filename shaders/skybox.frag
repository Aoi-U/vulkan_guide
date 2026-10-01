#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inViewRay;
layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 4) uniform samplerCube skyMap;

void main()
{
	outFragColor = vec4(texture(skyMap, normalize(inViewRay)).rgb, 1.0);
}