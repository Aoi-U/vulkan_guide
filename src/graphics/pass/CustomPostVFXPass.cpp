#include "CustomPostVFXPass.h"

#include <graphics/reflection/DescriptorBinder.h>
#include <graphics/reflection/PushConstantDispatcher.h>
#include <graphics/Renderer.h>
#include <resources/DefaultResources.h>
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

CustomPostVFXPass::CustomPostVFXPass(std::string passName, std::string fragShaderId, bool enabledByDefault)
	: RenderFeature(std::move(passName), enabledByDefault), _fragShaderId(std::move(fragShaderId)) {}

void CustomPostVFXPass::init(VulkanCore* core, ResourceManager* rm)
{
	_core = core;
	_rm = rm;

	auto vert = _rm->load<Shader>("deferred_lighting.vert", VK_SHADER_STAGE_VERTEX_BIT, "main");
	auto frag = _rm->load<Shader>(_fragShaderId, VK_SHADER_STAGE_FRAGMENT_BIT, "main");

	GraphicsPipelineDesc desc{
		.shaders = { vert, frag },
		.colorAttachmentFormats = { VK_FORMAT_R16G16B16A16_SFLOAT },
		.depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		.depthState = {.depthTest = false, .depthWrite = false },
		.rasterState = {.cullMode = VK_CULL_MODE_NONE, .polygonMode = VK_POLYGON_MODE_FILL }
	};
	_pipeline = GraphicsPipeline(_core->get_device(), _rm, desc);

	// set default push constant values from shader reflection
	for (const auto& [varName, var] : _pipeline.get_reflection().pushConstantsByName) {
		if (!_parameters.contains(varName)) {
			if (var.size == sizeof(float))
				set_float(varName, 1.0f, 0.0f, 1.0f);
			else if (var.size == sizeof(glm::vec4))
				set_vec4(varName, glm::vec4(1.0f));
		}
	}
}

void CustomPostVFXPass::cleanup()
{
	_customTextures.clear();
	_pipeline.cleanup();
	_core = nullptr;
	_rm = nullptr;
}

void CustomPostVFXPass::add_to_graph(RenderGraph& graph, RGBlackboard& blackboard, const RenderViewContext& view)
{
	if (!_pipeline.is_valid())
		return;

	auto& targets = blackboard.get<SceneRenderContext>();

	RGTextureDesc outDesc{
		.extent = { view.drawExtent.width, view.drawExtent.height, 1 },
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	};
	RGTextureHandle vfxOut = graph.create_texture(get_name() + "_color", outDesc);

	graph.add_pass<CustomPostVFXGraphParams>(get_name(),
		[&](RGPassBuilder& builder, CustomPostVFXGraphParams& params) {
			params.sceneColorIn = builder.read_texture(targets.sceneColor, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			if (shader_uses_binding("sceneDepth"))
				params.sceneDepth = builder.read_texture(targets.sceneDepth, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			if (shader_uses_binding("gPosition"))
				params.gPosition = builder.read_texture(targets.gPosition, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			if (shader_uses_binding("gNormal"))
				params.gNormal = builder.read_texture(targets.gNormal, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			if (shader_uses_binding("gAlbedo"))
				params.gAlbedo = builder.read_texture(targets.gAlbedo, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			if (shader_uses_binding("gEmissive"))
				params.gEmissive = builder.read_texture(targets.gEmissive, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			params.sceneColorOut = builder.set_color_attachment(vfxOut, VK_ATTACHMENT_LOAD_OP_CLEAR, { 0.0f, 0.0f, 0.0f, 1.0f });
			builder.set_extent(view.drawExtent);
		},
		[this, &view, &graph](const CustomPostVFXGraphParams& params, RenderContext& rc) {
			const auto& reflection = _pipeline.get_reflection();
			rc.bind_pipeline(_pipeline);

			if (reflection.sets.contains(0)) { // bind scene data set if shader uses it
				VkDescriptorSet vfxSet = view.frameData._frameDescriptors.allocate(_core->get_device(), _pipeline.get_descriptor_set_layout(0));
				VkSampler linear = DefaultResources::get().defaultSamplerLinear->get_sampler();
				VkSampler nearest = DefaultResources::get().defaultSamplerNearest->get_sampler();

				std::unordered_map<std::string, VkDescriptorBufferInfo> buffers = {
					{ "sceneData", { view.frameData.sceneDataBuffer->get_buffer(), 0, sizeof(GPUSceneData) }}
				};
				std::unordered_map<std::string, VkDescriptorImageInfo> images = {
					{ "sceneColor", { linear, graph.get_image_view(params.sceneColorIn), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }}
				};

				// bind additional textures if they are used in shader
				if (params.sceneDepth.is_valid())
					images["sceneDepth"] = { nearest, graph.get_image_view(params.sceneDepth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				if (params.gPosition.is_valid())
					images["gPosition"] = { nearest, graph.get_image_view(params.gPosition),  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				if (params.gNormal.is_valid())
					images["gNormal"] = { nearest, graph.get_image_view(params.gNormal),    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				if (params.gAlbedo.is_valid())
					images["gAlbedo"] = { nearest, graph.get_image_view(params.gAlbedo),    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				if (params.gEmissive.is_valid())
					images["gEmissive"] = { linear,  graph.get_image_view(params.gEmissive),  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

				// add custom textures
				for (const auto& [texName, texHandle] : _customTextures) {
					if (texHandle.is_valid()) {
						images[texName] = { texHandle->get_sampler(), texHandle->get_image_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
					}
				}

				DescriptorBinder::update_set(_core->get_device(), vfxSet, reflection.sets.at(0), buffers, images);
				rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 0, vfxSet);
			}

			if (reflection.sets.contains(1)) { // bind material set if shader uses bindless textures
				rc.bind_descriptor_set(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline.get_layout(), 1, view.drawCtx.bindlessMaterialSet);
			}

			if (!reflection.pushConstantRanges.empty()) { // set push constants if shader uses it
				PushConstantDispatcher pc(reflection);
				for (const auto& [name, value] : _parameters) {
					std::visit([&](const auto& val) {
						pc.set(name, val);
						}, value);
				}
				pc.flush(rc.get_cmd(), _pipeline.get_layout());
			}

			rc.set_cull_mode(VK_CULL_MODE_NONE);
			rc.draw(3, 1, 0, 0);
		});

	// chain output onto blackboard for subsequent passes
	targets.sceneColor = vfxOut;
}

void CustomPostVFXPass::on_imgui()
{
	for (auto& [name, value] : _parameters) {
		std::visit([&](auto& val) {
			using T = std::decay_t<decltype(val)>;

			// render appropriate imgui control based on parameter type
			if constexpr (std::is_same_v<T, float>) {
				auto rangeIt = _floatRanges.find(name);
				float minV = (rangeIt != _floatRanges.end()) ? rangeIt->second.minValue : 0.0f;
				float maxV = (rangeIt != _floatRanges.end()) ? rangeIt->second.maxValue : 1.0f;
				ImGui::SliderFloat(name.c_str(), &val, minV, maxV);
			}
			else if constexpr (std::is_same_v<T, int>) {
				ImGui::DragInt(name.c_str(), &val);
			}
			else if constexpr (std::is_same_v<T, glm::vec4>) {
				ImGui::ColorEdit4(name.c_str(), glm::value_ptr(val));
			}
		}, value);
	}
}

void CustomPostVFXPass::set_float(const std::string& name, float value, float minValue, float maxValue)
{
	_parameters[name] = value;
	_floatRanges[name] = { minValue, maxValue };
}

void CustomPostVFXPass::set_int(const std::string& name, int value)
{
	_parameters[name] = value;
}

void CustomPostVFXPass::set_vec4(const std::string& name, const glm::vec4& value)
{
	_parameters[name] = value;
}

void CustomPostVFXPass::set_custom_texture(const std::string& bindingName, ResourceHandle<Texture> texture)
{
	_customTextures[bindingName] = std::move(texture);
}

bool CustomPostVFXPass::shader_uses_binding(const std::string& name) const
{
	return _pipeline.get_reflection().bindingsByName.contains(name);
}
