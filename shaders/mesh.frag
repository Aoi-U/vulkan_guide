#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

void main()
{
  vec4 texColor = texture(colorTex, inUV);
  if (texColor.a < 0.5f) {
    discard;
  }

  vec3 N = normalize(inNormal);
  vec3 L = normalize(sceneData.sunlightDirection.xyz);
  float lightValue = max(dot(N, L), 0.1f);

  vec3 color = inColor.rgb * texColor.rgb;
  vec3 ambient = color * sceneData.ambientColor.xyz;
  
  outFragColor = vec4(color * lightValue * sceneData.sunlightColor.w + ambient, 1.0f);
}