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

	ResourceHandle<Sampler> defaultSamplerLinear;
	ResourceHandle<Sampler> defaultSamplerNearest;
	ResourceHandle<Sampler> shadowSampler;

	ResourceHandle<MaterialTemplate> defaultPBRTemplate;

private:
	ResourceManager* _manager{ nullptr };
};