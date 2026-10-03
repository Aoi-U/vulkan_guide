#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "input_structures.glsl"

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec4 outColor;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec3 outWorldPos;

struct Vertex {
    vec3 position;
    float uv_x;
    vec3 normal;
    float uv_y;
    vec4 color;
};

layout (buffer_reference, std430) readonly buffer VertexBuffer {
    Vertex vertices[];
};

struct GPUObjectData {
    mat4 worldMatrix;
    vec4 sphereBounds;
    VertexBuffer vertexBuffer;
    uint batchIndex;
    uint batchOffset;
};

layout(std430, set = 0, binding = 1) readonly buffer ObjectBuffer {
    GPUObjectData objects[];
} objectBuffer;

layout(std430, set = 0, binding = 2) readonly buffer InstanceIndexBuffer {
    uint objectIDs[];
} instanceBuffer;

void main()
{
    uint objectIndex = instanceBuffer.objectIDs[gl_InstanceIndex];
    GPUObjectData obj = objectBuffer.objects[objectIndex];
    Vertex v = obj.vertexBuffer.vertices[gl_VertexIndex];

    vec4 worldPosition = obj.worldMatrix * vec4(v.position, 1.0);
    gl_Position = sceneData.viewproj * worldPosition;

    outNormal = normalize((obj.worldMatrix * vec4(v.normal, 0.0)).xyz);
    outColor = v.color * materialData.colorFactors;
    outUV = vec2(v.uv_x, v.uv_y);
    outWorldPos = worldPosition.xyz;
}