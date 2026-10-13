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
