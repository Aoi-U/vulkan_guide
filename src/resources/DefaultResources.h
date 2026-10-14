#pragma once

#include <resources/ResourceManager.h>
#include <resources/Texture.h>
#include <resources/Sampler.h>
#include <resources/MaterialTemplate.h>

class DefaultResources
{
public:
	static DefaultResources& get()
	{
		static DefaultResources instance;
		return instance;
	}

	void init(ResourceManager* manager);
	void cleanup();

	ResourceHandle<Texture> whiteImage;
	ResourceHandle<Texture> blackImage;
	ResourceHandle<Texture> greyImage;
	ResourceHandle<Texture> errorCheckerboardImage;
	ResourceHandle<Texture> defaultNormalImage;
	ResourceHandle<Texture> whiteArrayImage;

	ResourceHandle<Sampler> defaultSamplerLinear;
	ResourceHandle<Sampler> defaultSamplerNearest;
	ResourceHandle<Sampler> shadowSampler;

	ResourceHandle<Texture> whiteCubeImage;
	ResourceHandle<Sampler> cubemapSampler;
	ResourceHandle<Sampler> lutSampler;
	ResourceHandle<Sampler> hdrSampler;

	ResourceHandle<MaterialTemplate> defaultPBRTemplate;

private:
	void init_environment_samplers();

	ResourceManager* _manager{ nullptr };
};