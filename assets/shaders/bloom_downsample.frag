#version 450

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

layout (set = 0, binding = 0) uniform sampler2D srcTexture;

layout (push_constant) uniform PushConstants
{
	vec2 srcTexelSize;
	float threshold;
	float knee;
	int mipLevel;
} push;

// https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom

float luminance(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float karis_average(vec3 col)
{
	return 1.0 / (1.0 + luminance(col));
}

// quadratic soft knee threshold curve
vec3 prefilter_color(vec3 color)
{
	float brightness = max(color.r, max(color.g, color.b));
	float soft = brightness - push.threshold + push.knee;
	soft = clamp(soft, 0.0, 2.0 * push.knee);
	soft = soft * soft / ((4.0 * push.knee) + 0.00001);
	float contribution = max(soft, brightness - push.threshold);
	contribution /= max(brightness, 0.00001);
	return color * contribution;
}

void main()
{
	float x = push.srcTexelSize.x;
	float y = push.srcTexelSize.y;

	// Take 13 samples around current texel:
	// a - b - c
	// - j - k -
	// d - e - f
	// - l - m -
	// g - h - i
	// === ('e' is the current texel) ===
	vec3 a = texture(srcTexture, vec2(inUV.x - 2.0 * x, inUV.y + 2.0 * y)).rgb;
    vec3 b = texture(srcTexture, vec2(inUV.x,			inUV.y + 2.0 * y)).rgb;
    vec3 c = texture(srcTexture, vec2(inUV.x + 2.0 * x, inUV.y + 2.0 * y)).rgb;

    vec3 d = texture(srcTexture, vec2(inUV.x - 2.0 * x, inUV.y)).rgb;
    vec3 e = texture(srcTexture, vec2(inUV.x,			inUV.y)).rgb;
    vec3 f = texture(srcTexture, vec2(inUV.x + 2.0 * x, inUV.y)).rgb;

    vec3 g = texture(srcTexture, vec2(inUV.x - 2.0 * x, inUV.y - 2.0 * y)).rgb;
    vec3 h = texture(srcTexture, vec2(inUV.x,			inUV.y - 2.0 * y)).rgb;
    vec3 i = texture(srcTexture, vec2(inUV.x + 2.0 * x, inUV.y - 2.0 * y)).rgb;

    vec3 j = texture(srcTexture, vec2(inUV.x - x, inUV.y + y)).rgb;
    vec3 k = texture(srcTexture, vec2(inUV.x + x, inUV.y + y)).rgb;
    vec3 l = texture(srcTexture, vec2(inUV.x - x, inUV.y - y)).rgb;
    vec3 m = texture(srcTexture, vec2(inUV.x + x, inUV.y - y)).rgb;


	// Apply weighted distribution:
	// 0.5 + 0.125 + 0.125 + 0.125 + 0.125 = 1
	// a,b,d,e * 0.125
	// b,c,e,f * 0.125
	// d,e,g,h * 0.125
	// e,f,h,i * 0.125
	// j,k,l,m * 0.5
	// This shows 5 square areas that are being sampled. But some of them overlap,
	// so to have an energy preserving downsample we need to make some adjustments.
	// The weights are the distributed, so that the sum of j,k,l,m (e.g.)
	// contribute 0.5 to the final color output. The code below is written
	// to effectively yield this sum. We get:
	// 0.125*5 + 0.03125*4 + 0.0625*4 = 1

	// Check if we need to perform Karis average on each block of 4 samples
	vec3 downsample = vec3(0.0);
	if (push.mipLevel == 0) {
		vec3 g0 = (a + b + d + e) * (0.125 / 4.0);
        vec3 g1 = (b + c + e + f) * (0.125 / 4.0);
        vec3 g2 = (d + e + g + h) * (0.125 / 4.0);
        vec3 g3 = (e + f + h + i) * (0.125 / 4.0);
        vec3 g4 = (j + k + l + m) * (0.5 / 4.0);

        float kw0 = karis_average(g0);
        float kw1 = karis_average(g1);
        float kw2 = karis_average(g2);
        float kw3 = karis_average(g3);
        float kw4 = karis_average(g4);

        downsample = (g0 * kw0 + g1 * kw1 + g2 * kw2 + g3 * kw3 + g4 * kw4) / max(kw0 + kw1 + kw2 + kw3 + kw4, 0.0001);
        downsample = prefilter_color(downsample);
	}
	else {
		downsample = e * 0.125;
        downsample += (a + c + g + i) * 0.03125;
        downsample += (b + d + f + h) * 0.0625;
        downsample += (j + k + l + m) * 0.125;
	}

	outColor = vec4(max(downsample, vec3(0.0001)), 1.0);
}