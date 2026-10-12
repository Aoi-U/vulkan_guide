#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inViewRay;
layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 4) uniform samplerCube skyMap;

void main()
{
	vec3 color = textureLod(skyMap, normalize(inViewRay), 0.0).rgb;

    outFragColor = vec4(color, 1.0);
}