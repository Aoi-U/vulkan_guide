#include "app/editor/EditorLayer.h"

#include <core/layer/LayerStack.h>
#include <core/Components.h>
#include <core/cvars.h>
#include <graphics/Renderer.h>
#include <app/editor/UIWidgets.h>
#include <resources/DefaultResources.h>
#include <resources/GlobalGeometryPool.h>
#include <resources/ModelSpawner.h>
#include <resources/AssetImporter.h>
#include <window/window.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <ImGuizmo.h>
#include <fmt/core.h>

void EditorLayer::on_attach(EngineContext& ctx)
{
	ILayer::on_attach(ctx);

	_guiRenderer.init(_ctx->window, _ctx->vulkanCore, _ctx->renderer->get_swapchain_format());

	_contentBrowserPanel.init("../../assets");

	_ctx->renderer->set_swapchain_recreate_callback([this]() {
		_guiRenderer.clear_image_descriptors();
		_sceneColorDescriptor = VK_NULL_HANDLE;
		});

	_ctx->renderer->set_overlay_render_callback([this](VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D swapchainExtent, VkImageView finalView) {
		if (finalView != VK_NULL_HANDLE && DefaultResources::get().defaultSamplerLinear.is_valid()) {
			_sceneColorDescriptor = _guiRenderer.get_image_descriptor(finalView, DefaultResources::get().defaultSamplerLinear->get_sampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

		if (_imguiFrameActive) {
			_guiRenderer.render(cmd, swapchainView, swapchainExtent);
			_imguiFrameActive = false;
		}
		});
}

void EditorLayer::on_detach()
{
	if (_ctx && _ctx->renderer) {
		_ctx->renderer->set_overlay_render_callback(nullptr);
		_ctx->renderer->set_swapchain_recreate_callback(nullptr);
	}

	_contentBrowserPanel.cleanup();
	_guiRenderer.clear_image_descriptors();
	_guiRenderer.cleanup();
	_sceneColorDescriptor = VK_NULL_HANDLE;
	_imguiFrameActive = false;

	ILayer::on_detach();
}

void EditorLayer::on_begin_frame(uint64_t frameNumber)
{
	if (!_ctx || !_ctx->window || _ctx->window->is_minimized()) {
		_imguiFrameActive = false;
		return;
	}
	_guiRenderer.new_frame();
	_imguiFrameActive = true;
}

void EditorLayer::on_update(float deltaTime)
{
	if (!_imguiFrameActive || !_ctx || !_ctx->layerStack)
		return;

	// call on_imgui_render() for all enabled layers
	_ctx->layerStack->render_imgui();
}

void EditorLayer::on_imgui_render()
{
	if (!_ctx)
		return;

	draw_dockspace_and_menubar();
	draw_viewport_panel();
	draw_hierarchy_panel();
	_inspectorPanel.on_imgui_render(*_ctx, _editorCtx);
	_contentBrowserPanel.on_imgui_render(*_ctx, _editorCtx, _guiRenderer);
	draw_renderer_and_metrics_panel();

	for (auto& cb : _panelCallbacks) {
		if (cb) {
			cb(*_ctx, _editorCtx);
		}
	}
}

void EditorLayer::on_event(EngineEvent & event)
{
	// forward raw sdl event to imgui
	ImGui_ImplSDL2_ProcessEvent(&event.sdlEvent);

	const ImGuiIO& io = ImGui::GetIO();

	event.wantCaptureMouse = io.WantCaptureMouse || ImGuizmo::IsUsing();
	event.wantCaptureKeyboard = io.WantCaptureKeyboard;

	// block mouse events from camera/scene layers when cursor is outside viewport
	if (event.is_mouse_event() && !_editorCtx.viewportHovered && event.wantCaptureMouse) {
		event.handled = true;
	}

	// block keyboard events from camera/scene layers when typing into imgui text widget
	if (event.is_keyboard_event() && !_editorCtx.viewportFocused && event.wantCaptureKeyboard) {
		event.handled = true;
	}
}

void EditorLayer::draw_dockspace_and_menubar()
{
	ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Save Scene (.scene)", "Ctrl+S")) {
				if (_ctx->activeRegistry) {
					SceneSerializer::save_scene("../../assets/scenes/default.scene", *_ctx->activeRegistry, _sceneEnvSettings);
				}
			}
			if (ImGui::MenuItem("Load Scene (.scene)", "Ctrl+O")) {
				if (_ctx->activeRegistry) {
					_editorCtx.clear_selection();
					SceneSerializer::load_scene(
						"../../assets/scenes/default.scene",
						*_ctx->activeRegistry,
						_sceneEnvSettings,
						_ctx->resourceManager,
						_ctx->renderer
					);
				}
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Assets")) {
			if (ImGui::MenuItem("Rescan & Cook Missing Assets")) {
				AssetRegistry::get().scan_directory("../../assets");
				AssetImporter::cook_missing_assets();
			}
			if (ImGui::MenuItem("Print Asset Manifest to Console")) {
				AssetRegistry::get().print_manifest();
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
}

void EditorLayer::draw_viewport_panel()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	if (ImGui::Begin("Viewport")) {
		_editorCtx.viewportFocused = ImGui::IsWindowFocused();
		_editorCtx.viewportHovered = ImGui::IsWindowHovered();

		ImVec2 avail = ImGui::GetContentRegionAvail();
		uint32_t w = std::max(1u, static_cast<uint32_t>(avail.x));
		uint32_t h = std::max(1u, static_cast<uint32_t>(avail.y));
		_editorCtx.viewportExtent = { w, h };

		if (_ctx->renderer) {
			_ctx->renderer->set_viewport_extent(_editorCtx.viewportExtent);
		}

		if (_sceneColorDescriptor != VK_NULL_HANDLE) {
			ImGui::Image((ImTextureID)_sceneColorDescriptor, avail);

			// allow dragging and dropping .gltf, .prefab, .gmesh, .hdr, or .scene directly into viewport
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_ASSET_UUID)) {
					if (payload->DataSize == sizeof(UUID)) {
						UUID droppedUUID = *static_cast<const UUID*>(payload->Data);
						if (const AssetMetadata* meta = AssetRegistry::get().get_metadata(droppedUUID)) {
							if ((meta->type == AssetType::Model || meta->type == AssetType::Prefab) && _ctx->activeRegistry && _ctx->resourceManager) {
								entt::entity root = spawn_prefab(*_ctx->activeRegistry, *_ctx->resourceManager, meta->virtualPath);
								_editorCtx.select_entity(root);
								if (_ctx->renderer) _ctx->renderer->mark_scene_dirty();
							}
							else if (meta->type == AssetType::EnvironmentHDR && _ctx->renderer) {
								_ctx->renderer->load_environment(meta->virtualPath);
							}
							else if (meta->type == AssetType::Scene && _ctx->activeRegistry) {
								_editorCtx.clear_selection();
								SceneSerializer::load_scene(meta->virtualPath, *_ctx->activeRegistry, _sceneEnvSettings, _ctx->resourceManager, _ctx->renderer);
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void EditorLayer::draw_hierarchy_panel()
{
	if (!ImGui::Begin("Scene Hierarchy")) {
		ImGui::End();
		return;
	}

	if (_ctx->activeRegistry) {
		entt::registry& registry = *_ctx->activeRegistry;

		if (ImGui::Button("+ Create Empty Entity", ImVec2(-1.0f, 0.0f))) {
			entt::entity e = registry.create();
			registry.emplace<IDComponent>(e, UUID::generate());
			registry.emplace<TagComponent>(e, "New Entity");
			registry.emplace<TransformComponent>(e);
			registry.emplace<HierarchyComponent>(e);
			_editorCtx.select_entity(e);
		}
		ImGui::Separator();

		auto view = registry.view<const TagComponent>();
		for (auto [entity, _] : view.each()) {
			const auto* hc = registry.try_get<HierarchyComponent>(entity);
			if (!hc || hc->parent == entt::null) {
				draw_entity_node(entity);
			}
		}
	}

	ImGui::End();
}

void EditorLayer::draw_entity_node(entt::entity entity)
{
	entt::registry& registry = *_ctx->activeRegistry;
	if (!registry.valid(entity)) return;

	const auto* tagComp = registry.try_get<TagComponent>(entity);
	const auto* hc = registry.try_get<HierarchyComponent>(entity);
	const char* name = (tagComp && !tagComp->tag.empty()) ? tagComp->tag.c_str() : "Unnamed Entity";

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (_editorCtx.inspectorMode == InspectorMode::Entity && _editorCtx.selectedEntity == entity) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}
	if (!hc || hc->children.empty()) {
		flags |= ImGuiTreeNodeFlags_Leaf;
	}

	bool opened = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entt::to_integral(entity))), flags, "%s", name);
	if (ImGui::IsItemClicked()) {
		_editorCtx.select_entity(entity);
	}

	if (opened) {
		if (hc) {
			for (entt::entity child : hc->children) {
				draw_entity_node(child);
			}
		}
		ImGui::TreePop();
	}
}

void EditorLayer::draw_renderer_and_metrics_panel()
{
	if (!ImGui::Begin("Renderer & Memory Metrics")) {
		ImGui::End();
		return;
	}

	if (_ctx->resourceManager) {
		if (ImGui::CollapsingHeader("GlobalGeometryPool & Staging Metrics", ImGuiTreeNodeFlags_DefaultOpen)) {
			GeometryPoolMetrics metrics = _ctx->resourceManager->get_geometry_pool().get_metrics();
			float vertUsedMB = static_cast<float>(metrics.usedVertexBytes) / (1024.0f * 1024.0f);
			float vertCapMB = static_cast<float>(metrics.totalVertexCapacityBytes) / (1024.0f * 1024.0f);
			float idxUsedMB = static_cast<float>(metrics.usedIndexBytes) / (1024.0f * 1024.0f);
			float idxCapMB = static_cast<float>(metrics.totalIndexCapacityBytes) / (1024.0f * 1024.0f);

			ImGui::Text("Active 64MB BDA Vertex Pages: %u", metrics.activeVertexPages);
			ImGui::ProgressBar(vertCapMB > 0.0f ? (vertUsedMB / vertCapMB) : 0.0f, ImVec2(-1.0f, 0.0f),
				fmt::format("Vertices: {:.2f} / {:.1f} MB", vertUsedMB, vertCapMB).c_str());

			ImGui::ProgressBar(idxCapMB > 0.0f ? (idxUsedMB / idxCapMB) : 0.0f, ImVec2(-1.0f, 0.0f),
				fmt::format("Indices: {:.2f} / {:.1f} MB", idxUsedMB, idxCapMB).c_str());

			auto& staging = _ctx->resourceManager->get_staging_uploader();
			float stageUsedMB = static_cast<float>(staging.get_used_bytes()) / (1024.0f * 1024.0f);
			float stageCapMB = static_cast<float>(staging.get_capacity_bytes()) / (1024.0f * 1024.0f);
			ImGui::ProgressBar(stageCapMB > 0.0f ? (stageUsedMB / stageCapMB) : 0.0f, ImVec2(-1.0f, 0.0f),
				fmt::format("Staging Ring: {:.2f} / {:.1f} MB", stageUsedMB, stageCapMB).c_str());
		}
	}

	if (_ctx->renderer && ImGui::CollapsingHeader("Render Graph Passes", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (auto& feature : _ctx->renderer->get_features()) {
			ImGui::PushID(feature->get_name().c_str());
			bool enabled = feature->is_enabled();
			if (ImGui::Checkbox(feature->get_name().c_str(), &enabled)) {
				feature->set_enabled(enabled);
			}
			if (enabled) {
				ImGui::Indent();
				feature->on_imgui();
				ImGui::Unindent();
			}
			ImGui::PopID();
		}
	}

	if (ImGui::CollapsingHeader("Console Variables (CVars)")) {
		CVarSystem::Get()->DrawCVarEditor();
	}

	ImGui::End();
}
