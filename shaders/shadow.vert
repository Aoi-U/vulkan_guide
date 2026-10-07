#version 450
#extension GL_EXT_buffer_reference : require

layout(push_constant) uniform PushConstants
{
	mat4 sunlightViewproj;
} push;

struct Vertex 
{
	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;
	vec4 tangent;
};

layout (buffer_reference, std430) readonly buffer VertexBuffer
{
	Vertex vertices[];
};

struct GPUObjectData
{
	mat4 worldMatrix;
	vec4 sphereBounds;
	VertexBuffer vertexBuffer;
	uint batchIndex;
	uint batchOffset;
};

layout (std430, set = 0, binding = 0) readonly buffer ObjectBuffer
{
	GPUObjectData objects[];
} objectBuffer;

layout(std430, set = 0, binding = 1) readonly buffer InstanceIndexBuffer
{
	uint objectIDs[];
} instanceBuffer;

void main()
{
	uint objectIndex = instanceBuffer.objectIDs[gl_InstanceIndex];
	GPUObjectData obj = objectBuffer.objects[objectIndex];
	Vertex v = obj.vertexBuffer.vertices[gl_VertexIndex];
	gl_Position = push.sunlightViewproj * obj.worldMatrix * vec4(v.position, 1.0);
}