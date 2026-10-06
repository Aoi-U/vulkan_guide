struct PointLight
{
	vec4 position;
	vec4 color;
	vec4 params; // x: cast shadow, yzw padding
};

const int MAX_POINT_LIGHTS = 256;

layout (set = 0, binding = 0) uniform SceneData
{
  mat4 view;
  mat4 proj;
  mat4 viewproj;
  mat4 sunlightViewproj;
  vec4 cameraPosition;
  vec4 ambientColor;
  vec4 sunlightDirection; // w for sun power
  vec4 sunlightColor;
  PointLight pointLights[MAX_POINT_LIGHTS];
  vec4 lightCount;
} sceneData;

layout (set = 1, binding = 0) uniform GLTFMaterialData
{
  vec4 colorFactors;
  vec4 metalRoughFactors;
  vec4 extraFactors; // x: occlusion strength, y: normal scale, z: isDoubleSided
  vec4 emissiveFactor; // xtz: emissiveFactor, w: emissiveStrength
} materialData;

layout (set = 1, binding = 1) uniform sampler2D colorTex;
layout (set = 1, binding = 2) uniform sampler2D metalRoughTex;
layout (set = 1, binding = 3) uniform sampler2D occlusionTex;
layout (set = 1, binding = 4) uniform sampler2D normalTex;
layout (set = 1, binding = 5) uniform sampler2D emissiveTex;