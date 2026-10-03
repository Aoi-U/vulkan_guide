#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inViewRay;
layout (location = 0) out vec4 outFragColor;

layout (set = 0, binding = 4) uniform samplerCube skyMap;

void main()
{
	vec3 color = texture(skyMap, normalize(inViewRay)).rgb;
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    outFragColor = vec4(color, 1.0);
}