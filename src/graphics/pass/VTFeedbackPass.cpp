#include "VTFeedbackPass.h"

#include <graphics/Renderer.h>
#include <graphics/vt/VirtualTextureSystem.h>
#include <resources/ResourceManager.h>
#include <imgui.h>

void VTFeedbackClearPass::init(VulkanCore* core, ResourceManager* rm)
{
	_core = core;
	_rm = rm;
}

void VTFeedbackClearPass::cleanup()
{
	_core = nullptr;
	_rm = nullptr;
}

void VTFeedbackClearPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	if (!_rm)
		return;

	auto& targets = blackboard.get<SceneRenderContext>();
	VirtualTextureSystem& vtSys = _rm->get_vtsystem();

	if (vtSys.get_feedback_buffer() == VK_NULL_HANDLE)
		return;

	targets.vtDescBuffer = graph.import_buffer("VTDescBuffer", vtSys.get_vt_desc_buffer(), vtSys.get_vt_desc_buffer_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

	targets.vtFeedbackBuffer = graph.import_buffer("VTFeedbackBuffer", vtSys.get_feedback_buffer(), vtSys.get_gpu_feedback_buffer_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

	graph.add_pass<VTFeedbackClearParams>("VTFeedbackClear",
		[&](RGPassBuilder& builder, VTFeedbackClearParams& params)
		{
			params.vtFeedbackBuffer = builder.write_buffer(targets.vtFeedbackBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			builder.set_side_effect(true);
		},
		[&graph](const VTFeedbackClearParams& params, RenderContext& ctx)
		{
			VkBuffer feedbackBuf = graph.get_buffer(params.vtFeedbackBuffer);
			if (feedbackBuf != VK_NULL_HANDLE)
			{
				// reset GPUVTFeedbackHeader to 0 before gbufferpass writes feedback requests
				vkCmdFillBuffer(ctx.get_cmd(), feedbackBuf, 0, sizeof(GPUVTFeedbackHeader), 0);
			}
		});
}

void VTFeedbackPass::init(VulkanCore* core, ResourceManager* rm)
{
	_core = core;
	_rm = rm;
}

void VTFeedbackPass::cleanup()
{
	_core = nullptr;
	_rm = nullptr;
}

void VTFeedbackPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	if (!_rm)
		return;

	auto& targets = blackboard.get<SceneRenderContext>();
	if (!targets.vtFeedbackBuffer.is_valid())
		return;

	VirtualTextureSystem& vtSys = _rm->get_vtsystem();
	const uint32_t frameSlot = view.frameNumber % Renderer::FRAME_OVERLAP;
	VkBuffer readbackBuf = vtSys.get_feedback_readback_buffer(frameSlot);
	if (readbackBuf == VK_NULL_HANDLE)
		return;

	RGBufferHandle vtReadbackHandle = graph.import_buffer("VTFeedbackReadbackBuffer", readbackBuf, vtSys.get_gpu_feedback_buffer_size(), VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

	graph.add_pass<VTFeedbackReadbackParams>("VTFeedbackReadback",
		[&](RGPassBuilder& builder, VTFeedbackReadbackParams& params)
		{
			params.vtFeedbackBuffer = builder.read_buffer(targets.vtFeedbackBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			params.vtReadbackBuffer = builder.write_buffer(vtReadbackHandle, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			builder.set_side_effect(true);
		},
		[&graph](const VTFeedbackReadbackParams& params, RenderContext& ctx)
		{
			VkBuffer srcBuf = graph.get_buffer(params.vtFeedbackBuffer);
			VkBuffer dstBuf = graph.get_buffer(params.vtReadbackBuffer);
			if (srcBuf != VK_NULL_HANDLE && dstBuf != VK_NULL_HANDLE)
			{
				VkBufferCopy copyRegion{
					.srcOffset = 0,
					.dstOffset = 0,
					.size = VT_FEEDBACK_BUFFER_SIZE_BYTES
				};
				vkCmdCopyBuffer(ctx.get_cmd(), srcBuf, dstBuf, 1, &copyRegion);
			}
		});
}

void VTFeedbackPass::on_imgui()
{
	if (!_rm)
		return;

	const char* debugModes[] = {
		"0: None (Standard PBR)",
		"1: Virtual Mip Level Heatmap",
		"2: Exact vs Fallback Mip Residency",
		"3: Physical Atlas Tile Grid"
	};
	int currentDebugMode = CVarSystem::Get()->GetIntCVar("r.vt.debugMode");
	if (ImGui::Combo("VT Debug View", &currentDebugMode, debugModes, IM_ARRAYSIZE(debugModes)))
	{
		CVarSystem::Get()->SetIntCVar("r.vt.debugMode", currentDebugMode);
	}

	int subsample = CVarSystem::Get()->GetIntCVar("r.vt.feedbackSubsample");
	if (ImGui::SliderInt("Feedback Subsample (NxN)", &subsample, 1, 32))
	{
		CVarSystem::Get()->SetIntCVar("r.vt.feedbackSubsample", subsample);
	}

	int maxUploads = CVarSystem::Get()->GetIntCVar("r.vt.maxTileUploadsPerFrame");
	if (ImGui::SliderInt("Max Tile Uploads / Frame", &maxUploads, 1, 64))
	{
		CVarSystem::Get()->SetIntCVar("r.vt.maxTileUploadsPerFrame", maxUploads);
	}

	float mipBias = static_cast<float>(CVarSystem::Get()->GetFloatCVar("r.vt.mipBias"));
	if (ImGui::SliderFloat("VT Mip Bias", &mipBias, -2.0f, 4.0f, "%.2f"))
	{
		CVarSystem::Get()->SetFloatCVar("r.vt.mipBias", mipBias);
	}

	ImGui::Separator();
	VirtualTextureMetrics m = _rm->get_vtsystem().get_metrics();
	ImGui::Text("Registered Virtual Textures: %u", m.registeredVirtualTextures);
	ImGui::Text("Physical Atlas Slots: %u / %u (%u pinned)",
		m.usedPhysicalSlots, m.totalPhysicalSlots, m.pinnedPhysicalSlots);
	ImGui::Text("Feedback Raw / Unique Pages: %u / %u",
		m.lastFrameRawFeedbackCount, m.lastFrameUniquePagesRequested);
	ImGui::Text("Tiles Queued / In-Flight / Total Streamed: %u / %u / %llu",
		m.lastFrameTilesQueued, m.inFlightTileRequests, static_cast<unsigned long long>(m.totalTilesStreamed));
}
