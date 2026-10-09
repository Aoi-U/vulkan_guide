#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "input_structures.glsl"

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outWorldPos;
layout (location = 3) out vec3 outTangent;
layout (location = 4) out vec3 outBitangent;
layout (location = 5) out flat uint outMaterialIndex;

struct Vertex {
    vec3 position;
    float uv_x;
    vec3 normal;
    float uv_y;
    vec4 color;
    vec4 tangent;
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
    uint materialIndex;
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

    vec3 normalWS = normalize((obj.worldMatrix * vec4(v.normal, 0.0)).xyz);
    vec3 tangentWS = normalize((obj.worldMatrix * vec4(v.tangent.xyz, 0.0)).xyz);
    vec3 bitangentWS = cross(normalWS, tangentWS) * v.tangent.w;

    outNormal = normalWS;
    outTangent = tangentWS;
    outBitangent = bitangentWS;
    outUV = vec2(v.uv_x, v.uv_y);
    outWorldPos = worldPosition.xyz;
    outMaterialIndex = obj.materialIndex;
}