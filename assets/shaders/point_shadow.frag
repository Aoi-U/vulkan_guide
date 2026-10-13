#version 450

layout(location = 0) in vec3 inWorldPos;

layout(push_constant) uniform PushConstants
{
	mat4 lightViewproj;
	vec4 lightPos; // xyz: light position, w: far plane
} push;

void main()
{
	float lightDistance = length(inWorldPos - push.lightPos.xyz);
	// normalize to [0, 1]
	gl_FragDepth = lightDistance / push.lightPos.w;
}