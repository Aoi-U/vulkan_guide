#pragma once

#include <core/layer/ILayer.h>
#include <filesystem>

class ResourceLayer : public ILayer
{
public:
	explicit ResourceLayer(std::filesystem::path assetsRootPath = "../../assets", bool cookMissingAssetsOnBoot = true)
		: ILayer("ResourceLayer"), _assetsRootPath(std::move(assetsRootPath)), _cookOnBoot(cookMissingAssetsOnBoot) {}

	~ResourceLayer() override = default;

	void on_attach(EngineContext& ctx) override;
	void on_detach() override;

	void on_begin_frame(uint64_t frameNumber) override;

private:
	std::filesystem::path _assetsRootPath;
	bool _cookOnBoot{ true };
	bool _defaultResourcesInitialized{ false };
};