#version 450

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outBrightColor;

layout (set = 0, binding = 0) uniform sampler2D sceneColor;

// https://learnopengl.com/Advanced-Lighting/Bloom

void main()
{
	vec3 color = texture(sceneColor, inUV).rgb;

	float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));

	if (brightness > 1.0) {
		outBrightColor = vec4(color.rgb, 1.0);
	}
	else {
		outBrightColor = vec4(0.0, 0.0, 0.0, 1.0);
	}
	
}