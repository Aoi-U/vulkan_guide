#version 450

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

layout(push_constant) uniform PushConstants 
{
	float exposure;
} push;

vec3 ACESFilm(vec3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

void main()
{
    vec3 color = texture(sceneColor, inUV).rgb;

    color *= push.exposure; // exposure adjustment
    color = ACESFilm(color); // hdr to ldr tone mapping
    color = pow(color, vec3(1.0 / 2.2)); // gamma correction

    outFragColor = vec4(color, 1.0);
}