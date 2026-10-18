#include "resources/ResourceLayer.h"

#include <resources/AssetRegistry.h>
#include <resources/AssetImporter.h>
#include <resources/DefaultResources.h>
#include <resources/ResourceManager.h>
#include <resources/GlobalGeometryPool.h>
#include <graphics/utils/StagingUploader.h>
#include <graphics/Renderer.h>
#include <graphics/VulkanCore.h>

void ResourceLayer::on_attach(EngineContext& ctx)
{
	ILayer::on_attach(ctx);

	// scan disk assets and populate uuids
	if (_ctx->assetRegistry) {
		_ctx->assetRegistry->scan_directory(_assetsRootPath);
	}
	else {
		AssetRegistry::get().scan_directory(_assetsRootPath);
	}

	// initialize default resources
	if (_ctx->resourceManager && !DefaultResources::get().whiteImage.is_valid()) {
		DefaultResources::get().init(_ctx->resourceManager);
		_defaultResourcesInitialized = true;
	}

	if (_cookOnBoot) {
		AssetImporter::cook_missing_assets();
	}

}

void ResourceLayer::on_detach()
{
	if (_defaultResourcesInitialized) {
		DefaultResources::get().cleanup();
		_defaultResourcesInitialized = false;
	}

	ILayer::on_detach();
}

void ResourceLayer::on_begin_frame(uint64_t frameNumber)
{
	if (!_ctx || !_ctx->resourceManager || !_ctx->vulkanCore)
		return;

	if (_ctx->window && _ctx->window->is_minimized()) {
		return;
	}

	if (_ctx->renderer) {
		_ctx->renderer->wait_for_current_frame_fence();
	}

	// flush deletion queue and reclaim completed staginguploader slices
	if (frameNumber >= Renderer::FRAME_OVERLAP) {
		uint64_t completedFrame = frameNumber - Renderer::FRAME_OVERLAP;
		_ctx->vulkanCore->get_deletion_queue().flush_retired(completedFrame);
		_ctx->resourceManager->get_staging_uploader().reclaim_completed_frames(completedFrame);
	}

	// reclaim global geometrypool allocations
	_ctx->resourceManager->get_geometry_pool().process_deferred_frees(frameNumber);

	_ctx->resourceManager->process_main_thread_tasks();

	// evict 0 reference resources
	_ctx->resourceManager->process_evictions(frameNumber);
}
