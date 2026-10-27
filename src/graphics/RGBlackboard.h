#pragma once

#include <graphics/RenderGraph.h>
#include <resources/DefaultResources.h>
#include <unordered_map>
#include <typeindex>
#include <memory>

struct SceneRenderContext
{
	// frame buffers
	RGBufferHandle gpuObjectBuffer;
	RGBufferHandle gpuInstanceBuffer;
	RGBufferHandle gpuIndirectBuffer;
	RGBufferHandle gpuCountBuffer;
	RGBufferHandle cullStatsBuffer;
	RGBufferHandle materialBuffer;
	RGBufferHandle gpuShadowIndirectBuffer;
	RGBufferHandle gpuShadowInstanceBuffer;

	// virtual texture buffers
	RGBufferHandle vtDescBuffer;
	RGBufferHandle vtFeedbackBuffer;

	// gbuffer
	RGTextureHandle gPosition;
	RGTextureHandle gNormal;
	RGTextureHandle gAlbedo;
	RGTextureHandle gMetalRough;
	RGTextureHandle gEmissive;
	
	// core targets
	RGTextureHandle sceneDepth;
	RGTextureHandle sceneColor; // hdr R16G16B16A16_SFLOAT
	RGTextureHandle sceneColorCopy; // optional for vfx
	RGTextureHandle finalColor; // ldr tonemapped R8G8B8A8_UNORM

	// environment
	RGTextureHandle envCubemap;
	RGTextureHandle irradianceMap;
	RGTextureHandle prefilterMap;
	RGTextureHandle brdfLut;

	// feature targets
	RGTextureHandle dirShadowMap;
	RGTextureHandle pointShadowMap;
	RGTextureHandle ssaoRaw;
	RGTextureHandle ssaoBlur;
	RGTextureHandle ssaoTexture;
	RGTextureHandle bloomTexture;

	// fallback handles
	RGTextureHandle defaultWhite;
	RGTextureHandle defaultBlack;
	RGTextureHandle defaultCubeWhite;
	RGTextureHandle defaultArrayWhite;
};

class RGBlackboard
{
public:
	template<typename T, typename... Args>
	T& add(Args&&... args)
	{
		auto ptr = std::make_shared<T>(std::forward<Args>(args)...);
		_storage[std::type_index(typeid(T))] = ptr;
		return *ptr;
	}

	template<typename T>
	T& get()
	{
		return *std::static_pointer_cast<T>(_storage.at(std::type_index(typeid(T))));
	}

	template<typename T>
	const T* try_get() const
	{
		auto it = _storage.find(std::type_index(typeid(T)));
		return it != _storage.end() ? static_cast<const T*>(it->second.get()) : nullptr;
	}

	void clear()
	{
		_storage.clear();
	}

private:
	std::unordered_map<std::type_index, std::shared_ptr<void>> _storage;
};