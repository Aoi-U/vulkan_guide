#include "ForwardTransparentPass.h"
#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/Renderer.h>
#include <graphics/utils/vk_utils.h>
#include <resources/GlobalGeometryPool.h>
#include <algorithm>
#include <glm/gtx/norm.hpp>
#include <imgui.h>
#include <algorithm>

void ForwardTransparentPass::init(VulkanCore* core, ResourceManager* rm)
{
	_core = core;
	_rm = rm;

	auto vert = _rm->load<Shader>("gbuffer.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = _rm->load<Shader>("forward.frag", VK_SHADER_STAGE_FRAGMENT_BIT, "main");
	if (vert.is_valid() && frag.is_valid()) {
		_forwardVfxTemplate = _rm->load<MaterialTemplate>("forward_vfx_template", vert, frag);
	}
	else {
		_forwardVfxTemplate = DefaultResources::get().defaultPBRTemplate;
	}
}

void ForwardTransparentPass::cleanup()
{
	_forwardVfxTemplate = {};
	_core = nullptr;
	_rm = nullptr;
}

void ForwardTransparentPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	if (view.drawCtx.transparentObjects.empty()) return;

	auto& targets = blackboard.get<SceneRenderContext>();

	// optional for vfx, copy the scene color to a separate texture specialized effects
	if (_captureSceneColorCopy) {

		RGTextureDesc copyDesc{
			.extent = { view.drawExtent.width, view.drawExtent.height, 1},
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
		};
		targets.sceneColorCopy = graph.create_texture("SceneColorCopy", copyDesc);

		struct CopyPassData
		{
			RGTextureHandle src;
			RGTextureHandle dst;
		};

		graph.add_pass<CopyPassData>("SceneColorCopy",
			[&](RGPassBuilder& builder, CopyPassData& data) {
				data.src = builder.read_texture(targets.sceneColor, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
				data.dst = builder.write_texture(targets.sceneColorCopy, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			},
			[&view, &graph](const CopyPassData& data, RenderContext& ctx) {
				vkutil::copy_image_to_image(ctx.get_cmd(), graph.get_image(data.src), graph.get_image(data.dst), view.drawExtent, view.drawExtent);
			});
	}
	else
	{
		targets.sceneColorCopy = targets.defaultBlack;
	}
	graph.add_pass<TransparentPassParams>("ForwardTransparentPass",
		[&](RGPassBuilder& builder, TransparentPassParams& params) {
			params.gpuObjectBuffer = builder.read_buffer(targets.gpuObjectBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.gpuInstanceBuffer = builder.read_buffer(targets.gpuInstanceBuffer, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.materialBuffer = builder.read_buffer(targets.materialBuffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
			params.sceneColorCopy = builder.read_texture(targets.sceneColorCopy, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.irradianceMap = builder.read_texture(targets.irradianceMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.prefilterMap = builder.read_texture(targets.prefilterMap, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			params.brdfLut = builder.read_texture(targets.brdfLut, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			bool gbufferActive = (targets.gPosition != targets.defaultBlack);
			VkAttachmentLoadOp depthLoadOp = gbufferActive ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;

			params.sceneColor = builder.set_color_attachment(targets.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD);
			params.sceneDepth = builder.set_depth_attachment(targets.sceneDepth, depthLoadOp, 0.0);
			builder.set_extent(view.drawExtent);
		},
		[this, &view, &graph](const TransparentPassParams& params, RenderContext& rc) {
			const DrawContext& drawCtx = view.drawCtx;
			FrameData& frameData = view.frameData;
			if (drawCtx.transparentObjects.empty() || !_forwardVfxTemplate.is_valid()) return;

			// sort transparent objects back to front relative to camera
			glm::vec3 camPos = glm::vec3(view.sceneData.cameraPosition);
			std::vector<RenderObject> sortedObjects(drawCtx.transparentObjects.begin(), drawCtx.transparentObjects.end());
			std::sort(sortedObjects.begin(), sortedObjects.end(), [&](const RenderObject& a, const RenderObject& b) {
				glm::vec3 posA = glm::vec3(a.transform * glm::vec4(a.bounds.origin, 1.0f));
				glm::vec3 posB = glm::vec3(b.transform * glm::vec4(b.bounds.origin, 1.0f));
				return glm::distance2(camPos, posA) > glm::distance2(camPos, posB);
				});

			// upload sorted transparent objects into tail of _gpuObjectBuffer and _gpuInstanceBuffer
			const uint32_t baseInstanceOffset = drawCtx.indirectDrawCount;
			const uint32_t transCount = static_cast<uint32_t>(sortedObjects.size());
			const uint32_t totalObjects = baseInstanceOffset + transCount;

			frameData._gpuObjectBuffer->resize(sizeof(GPUObjectData) * totalObjects, true);
			frameData._gpuInstanceBuffer->resize(sizeof(uint32_t) * totalObjects, true);

			auto* mappedObjects = static_cast<GPUObjectData*>(frameData._gpuObjectBuffer->get_mapped_data());
			auto* mappedInstances = static_cast<uint32_t*>(frameData._gpuInstanceBuffer->get_mapped_data());

			for (uint32_t i = 0; i < transCount; ++i) {
				const RenderObject& obj = sortedObjects[i];
				uint32_t globalIdx = baseInstanceOffset + i;
				mappedObjects[globalIdx] = GPUObjectData{
					.worldMatrix = obj.transform,
					.sphereBounds = glm::vec4(obj.bounds.origin, obj.bounds.sphereRadius),
					.vertexBufferAddress = obj.vertexBufferAddress,
					.batchIndex = 0,
					.batchOffset = 0,
					.materialIndex = obj.materialIndex
				};
				mappedInstances[globalIdx] = globalIdx;
			}


			// bind global descriptor set 0 and bindless texture set 1
			VkDescriptorSetLayout layout0 = _rm->get_pipeline_layout_cache().get_descriptor_set_layout(_forwardVfxTemplate->get_reflection().sets.at(0), false);
			VkDescriptorSet globalSet = frameData._frameDescriptors.allocate(_core->get_device(), layout0);

			DefaultResources& defaults = DefaultResources::get();
			std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
				{ "sceneData", { frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) } },
				{ "objectBuffer", { graph.get_buffer(params.gpuObjectBuffer), 0, sizeof(GPUObjectData) * totalObjects } },
				{ "instanceBuffer", { graph.get_buffer(params.gpuInstanceBuffer), 0, sizeof(uint32_t) * totalObjects } },
				{ "materialBuffer", { graph.get_buffer(params.materialBuffer), 0, frameData._materialDataBuffer->get_size() } }
			};
			std::unordered_map<std::string, VkDescriptorImageInfo> images = {
				{ "sceneColorCopy", { defaults.defaultSamplerLinear->get_sampler(), graph.get_image_view(params.sceneColorCopy), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
				{ "irradianceMap", { defaults.cubemapSampler->get_sampler(), graph.get_image_view(params.irradianceMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
				{ "prefilterMap", { defaults.cubemapSampler->get_sampler(), graph.get_image_view(params.prefilterMap), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
				{ "brdfLUT", { defaults.lutSampler->get_sampler(), graph.get_image_view(params.brdfLut), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } }
			};
			DescriptorBinder::update_set(_core->get_device(), globalSet, _forwardVfxTemplate->get_reflection().sets.at(0), buffers, images);

			MaterialPipelineKey key{
				.cullMode = VK_CULL_MODE_BACK_BIT,
				.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
				.polygonMode = VK_POLYGON_MODE_FILL,
				.blendMode = _defaultBlendMode,
				.depthTest = true,
				.depthWrite = false, // read only depth test against opaque geometry
				.depthComareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
				.colorAttachmentFormats = { VK_FORMAT_R16G16B16A16_SFLOAT },
				.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT
			};

			VkPipeline pipeline = _forwardVfxTemplate->get_pipeline(key);
			vkCmdBindPipeline(rc.get_cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _forwardVfxTemplate->get_layout(), 0, globalSet); // global descriptor set 0
			rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _forwardVfxTemplate->get_layout(), 1, drawCtx.bindlessMaterialSet); // bindless texture descriptor set 1
			rc.bind_index_buffer(_rm->get_geometry_pool().get_index_buffer(), 0, VK_INDEX_TYPE_UINT32); // bind global index buffer

			for (uint32_t i = 0; i < transCount; i++) {
				const RenderObject& obj = sortedObjects[i];
				rc.set_cull_mode(obj.isDoubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
				rc.draw_indexed(obj.indexCount, 1, obj.firstIndex, 0, baseInstanceOffset + i);
			}
		});
}

void ForwardTransparentPass::on_imgui()
{
	ImGui::Checkbox("Capture sceneColor copy", &_captureSceneColorCopy);
	const char* blendModes[] = { "None", "Additive", "AlphaBlend" };
	int currentBlendMode = static_cast<int>(_defaultBlendMode);
	if (ImGui::Combo("VFX blend mode", &currentBlendMode, blendModes, IM_ARRAYSIZE(blendModes))) {
		_defaultBlendMode = static_cast<BlendMode>(currentBlendMode);
	}
}
