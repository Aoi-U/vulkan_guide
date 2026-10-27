#include "app/editor/EditorLayer.h"

#include <core/layer/LayerStack.h>
#include <core/Components.h>
#include <core/cvars.h>
#include <core/Profiler.h>
#include <graphics/Renderer.h>
#include <graphics/pass/FrustumCullPass.h>
#include <graphics/vt/VirtualTextureSystem.h>
#include <app/editor/UIWidgets.h>
#include <resources/AssetImporter.h>
#include <resources/DefaultResources.h>
#include <resources/GlobalGeometryPool.h>
#include <resources/ModelSpawner.h>
#include <scene/SceneLayer.h>
#include <window/window.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <fmt/core.h>
#include <limits>
#include <cmath>
#include <algorithm>

namespace
{
	// ray sphere intersection for fast bounding volume prerejection
	bool intersect_ray_sphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& sphereCenter, float sphereRadius, float& outT)
	{
		glm::vec3 oc = rayOrigin - sphereCenter;
		float b = glm::dot(oc, rayDir);
		float c = glm::dot(oc, oc) - sphereRadius * sphereRadius;
		float discriminant = b * b - c;
		if (discriminant < 0.0f) return false;

		float sqrtDisc = std::sqrt(discriminant);
		float t0 = -b - sqrtDisc;
		float t1 = -b + sqrtDisc;
		if (t1 < 0.0f) return false;

		outT = (t0 >= 0.0f) ? t0 : t1;
		return true;
	}

	// ray triangle intersection for mesh picking
	bool intersect_ray_triangle(const glm::vec3& orig, const glm::vec3& dir, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, float& outT)
	{
		constexpr float EPSILON = 1e-6f;
		glm::vec3 edge1 = v1 - v0;
		glm::vec3 edge2 = v2 - v0;
		glm::vec3 pvec = glm::cross(dir, edge2);
		float det = glm::dot(edge1, pvec);

		if (std::abs(det) < EPSILON) return false;
		float invDet = 1.0f / det;

		glm::vec3 tvec = orig - v0;
		float u = glm::dot(tvec, pvec) * invDet;
		if (u < 0.0f || u > 1.0f) return false;

		glm::vec3 qvec = glm::cross(tvec, edge1);
		float v = glm::dot(dir, qvec) * invDet;
		if (v < 0.0f || u + v > 1.0f) return false;

		float t = glm::dot(edge2, qvec) * invDet;
		if (t > EPSILON) {
			outT = t;
			return true;
		}
		return false;
	}

	// recursively marks entity and its childrens TransformComponents dirty after gizmo manipulation
	void mark_subtree_transforms_dirty(entt::registry& reg, entt::entity entity, const glm::mat4& parentWorld)
	{
		if (!reg.valid(entity) || !reg.all_of<TransformComponent>(entity)) return;

		auto& tc = reg.get<TransformComponent>(entity);
		tc.worldTransform = parentWorld * tc.get_transform();
		tc.isDirty = true;

		if (const auto* hc = reg.try_get<HierarchyComponent>(entity)) {
			for (entt::entity child : hc->children) {
				mark_subtree_transforms_dirty(reg, child, tc.worldTransform);
			}
		}
	}
}

void EditorLayer::on_attach(EngineContext& ctx)
{
	ILayer::on_attach(ctx);

	_editorCtx.gizmoOperation = ImGuizmo::TRANSLATE;
	_editorCtx.gizmoMode = ImGuizmo::WORLD;

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

	if (_ctx->eventDispatcher)
	{
		_ctx->eventDispatcher->sink<Events::SceneSaveAsEvent>().connect<&EditorLayer::on_save_as>(this);
	}
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

	if (_ctx && _ctx->eventDispatcher)
	{
		_ctx->eventDispatcher->sink<Events::SceneSaveAsEvent>().disconnect(this);
	}

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

	_frameTimeHistory.push_back(deltaTime * 1000.0f);
	if (_frameTimeHistory.size() > MAX_FRAME_HISTORY) {
		_frameTimeHistory.pop_front();
	}

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
	draw_render_target_viewer_panel();
	draw_performance_panel();
	draw_renderer_settings_panel();

	for (auto& cb : _panelCallbacks) {
		if (cb) {
			cb(*_ctx, _editorCtx);
		}
	}
}

void EditorLayer::on_event(EngineEvent& event)
{
	ImGui_ImplSDL2_ProcessEvent(&event.sdlEvent);

	const ImGuiIO& io = ImGui::GetIO();
	event.wantCaptureMouse = io.WantCaptureMouse || ImGuizmo::IsUsing();
	event.wantCaptureKeyboard = io.WantCaptureKeyboard;

	// Handle Editor keyboard shortcuts when not typing in a text box and not flying camera (RMB held)
	if (event.sdlEvent.type == SDL_KEYDOWN && !event.sdlEvent.key.repeat && !io.WantTextInput) {
		const Uint32 mouseButtons = SDL_GetMouseState(nullptr, nullptr);
		bool rightMouseDown = (mouseButtons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;

		if (!rightMouseDown) {
			switch (event.sdlEvent.key.keysym.sym) {
			case SDLK_w:
				_editorCtx.gizmoOperation = ImGuizmo::TRANSLATE;
				break;
			case SDLK_e:
				_editorCtx.gizmoOperation = ImGuizmo::ROTATE;
				break;
			case SDLK_r:
				_editorCtx.gizmoOperation = ImGuizmo::SCALE;
				break;
			case SDLK_DELETE:
				if (_editorCtx.inspectorMode == InspectorMode::Entity && _editorCtx.selectedEntity != entt::null) {
					delete_entity_recursive(_editorCtx.selectedEntity, true);
					event.handled = true;
				}
				break;
			default:
				break;
			}
		}
	}

	if (event.is_mouse_event() && !_editorCtx.viewportHovered && event.wantCaptureMouse) {
		event.handled = true;
	}

	if (event.is_keyboard_event() && !_editorCtx.viewportFocused && event.wantCaptureKeyboard) {
		event.handled = true;
	}
}

void EditorLayer::draw_dockspace_and_menubar()
{
	ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
	SceneLayer* sceneLayer = _ctx->layerStack ? _ctx->layerStack->get_layer<SceneLayer>() : nullptr;

	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Empty Scene", "Ctrl+N") && sceneLayer)
			{
				_editorCtx.clear_selection();
				sceneLayer->new_scene("Untitled Scene");
			}

			// open scene submenu 
			if (ImGui::BeginMenu("Open Scene"))
			{
				auto sceneAssets = AssetRegistry::get().get_assets_by_type(AssetType::Scene, false);
				if (sceneAssets.empty())
				{
					ImGui::TextDisabled("No .scene files found in assets/");
				}
				else
				{
					for (const auto& sMeta : sceneAssets)
					{
						bool isCurrent = sceneLayer && (sceneLayer->get_scene().get_file_path().filename() == sMeta.virtualPath.
							filename());
						std::string itemLabel = fmt::format("{} ({})", sMeta.name, sMeta.virtualPath.filename().string());
						if (ImGui::MenuItem(itemLabel.c_str(), nullptr, isCurrent) && sceneLayer)
						{
							_editorCtx.clear_selection();
							sceneLayer->load_scene(sMeta.virtualPath);
						}
					}
				}
				ImGui::EndMenu();
			}

			ImGui::Separator();

			std::string currentFileName = (sceneLayer && !sceneLayer->get_scene().get_file_path().empty())
				                              ? sceneLayer->get_scene().get_file_path().filename().string()
				                              : "default.scene";
			std::string saveLabel = fmt::format("Save '{}'", currentFileName);

			if (ImGui::MenuItem(saveLabel.c_str(), "Ctrl+S") && sceneLayer)
			{
				sceneLayer->save_scene();
			}

			if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
			{
				open_save_scene_as_popup();
			}

			ImGui::Separator();

			if (ImGui::MenuItem("Reload Current Scene", "Ctrl+O") && sceneLayer)
			{
				_editorCtx.clear_selection();
				sceneLayer->load_scene(sceneLayer->get_scene().get_file_path());
			}

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Assets"))
		{
			if (ImGui::MenuItem("Rescan & Cook Missing Assets"))
			{
				AssetRegistry::get().scan_directory("../../assets");
				AssetImporter::cook_missing_assets();
			}
			if (ImGui::MenuItem("Print Asset Manifest to Console"))
			{
				AssetRegistry::get().print_manifest();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Environment"))
		{
			if (ImGui::MenuItem("Reset to Procedural Sky") && _ctx->renderer)
			{
				_ctx->renderer->remove_environment();
				if (sceneLayer) sceneLayer->sync_environment_from_renderer();
			}
			if (ImGui::BeginMenu("Load HDR Environment"))
			{
				auto hdrAssets = AssetRegistry::get().get_assets_by_type(AssetType::EnvironmentHDR, false);
				for (const auto& hdr : hdrAssets) {
					if (ImGui::MenuItem(hdr.name.c_str()) && _ctx->renderer) {
						_ctx->renderer->load_environment(hdr.virtualPath, hdr.uuid);
						if (sceneLayer) sceneLayer->sync_environment_from_renderer();
					}
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}

		// play/stop button
		if (sceneLayer)
		{
			ImGui::Separator();
			if (!sceneLayer->is_playing())
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1.0f));
				if (ImGui::SmallButton("  Play  "))
				{
					sceneLayer->begin_play();
				}
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.22f, 0.22f, 1.0f));
				if (ImGui::SmallButton("  Stop  "))
				{
					_editorCtx.clear_selection();
					sceneLayer->end_play();
				}
				ImGui::PopStyleColor();
			}

			// active scene name and save destination indicator 
			ImGui::Separator();
			const auto& env = sceneLayer->get_scene().get_environment_settings();
			const auto& activePath = sceneLayer->get_scene().get_file_path();
			bool existsOnDisk = !activePath.empty() && std::filesystem::exists(activePath);

			ImGui::TextColored(
				existsOnDisk ? ImVec4(0.45f, 0.85f, 1.0f, 1.0f) : ImVec4(0.95f, 0.78f, 0.30f, 1.0f),
				"Scene: %s [%s]",
				env.sceneName.c_str(),
				activePath.empty() ? "Unsaved" : activePath.filename().string().c_str()
			);

			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::Text("Scene Name: %s", env.sceneName.c_str());
				ImGui::Text("Scene UUID: %s", env.sceneUUID.to_string().c_str());
				ImGui::Text("Save Target: %s", activePath.generic_string().c_str());
				ImGui::Text("Status: %s", existsOnDisk ? "Saved on disk" : "New / Unsaved file");
				ImGui::Text("Sky Mode: %s", (_ctx->renderer && _ctx->renderer->is_using_hdr_environment())
					                            ? fmt::format(
						                            "HDR ({})", _ctx->renderer->get_environment_path().filename().string()).
					                            c_str()
					                            : "Procedural");
				ImGui::EndTooltip();
			}
		}

		ImGui::EndMainMenuBar();
	}

	draw_save_scene_as_popup();
}

void EditorLayer::draw_viewport_panel()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	if (ImGui::Begin("Viewport")) {
		_editorCtx.viewportFocused = ImGui::IsWindowFocused();
		_editorCtx.viewportHovered = ImGui::IsWindowHovered();

		// top overlay toolbar for gizmo
		ImGui::SetCursorPos(ImVec2(12.0f, 32.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.16f, 0.85f));
		ImGui::BeginChild("GizmoToolbar", ImVec2(440.0f, 32.0f), true, ImGuiWindowFlags_NoScrollbar);

		auto draw_op_btn = [&](const char* label, int op) {
			bool active = (_editorCtx.gizmoOperation == op);
			if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.52f, 0.88f, 1.0f));
			if (ImGui::SmallButton(label)) _editorCtx.gizmoOperation = op;
			if (active) ImGui::PopStyleColor();
			ImGui::SameLine();
			};

		draw_op_btn("Translate (W)", ImGuizmo::TRANSLATE);
		draw_op_btn("Rotate (E)", ImGuizmo::ROTATE);
		draw_op_btn("Scale (R)", ImGuizmo::SCALE);

		ImGui::TextDisabled("|");
		ImGui::SameLine();
		if (ImGui::SmallButton(_editorCtx.gizmoMode == ImGuizmo::LOCAL ? "Local" : "World")) {
			_editorCtx.gizmoMode = (_editorCtx.gizmoMode == ImGuizmo::LOCAL) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
		}
		ImGui::SameLine();
		ImGui::Checkbox("Snap", &_editorCtx.useSnap);
		ImGui::EndChild();
		ImGui::PopStyleColor();

		// reset cursor to top left of content region to draw scene image
		ImGui::SetCursorPos(ImGui::GetWindowContentRegionMin());
		ImVec2 avail = ImGui::GetContentRegionAvail();
		uint32_t w = std::max(1u, static_cast<uint32_t>(avail.x));
		uint32_t h = std::max(1u, static_cast<uint32_t>(avail.y));
		_editorCtx.viewportExtent = { w, h };

		if (_ctx->renderer) {
			_ctx->renderer->set_viewport_extent(_editorCtx.viewportExtent);
		}

		ImVec2 imageMin = ImGui::GetCursorScreenPos();
		glm::vec2 viewportPos(imageMin.x, imageMin.y);
		glm::vec2 viewportSize(avail.x, avail.y);

		if (_sceneColorDescriptor != VK_NULL_HANDLE) {
			ImGui::Image((ImTextureID)_sceneColorDescriptor, avail);

			// drag and drop asset spawning into Viewport
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_ASSET_UUID)) {
					if (payload->DataSize == sizeof(UUID)) {
						UUID droppedUUID = *static_cast<const UUID*>(payload->Data);
						if (const AssetMetadata* meta = AssetRegistry::get().get_metadata(droppedUUID)) {
							if ((meta->type == AssetType::Model || meta->type == AssetType::Prefab) && _ctx->activeRegistry && _ctx->resourceManager) {
								entt::entity root = spawn_prefab(*_ctx->activeRegistry, *_ctx->resourceManager, meta->virtualPath);
								_editorCtx.select_entity(root);
								if (_ctx->renderer) 
									_ctx->renderer->mark_scene_dirty();
							}
							else if (meta->type == AssetType::EnvironmentHDR && _ctx->renderer) {
								_ctx->renderer->load_environment(meta->virtualPath, meta->uuid);
								SceneLayer* sceneLayer = _ctx->layerStack ? _ctx->layerStack->get_layer<SceneLayer>() : nullptr;
								if (sceneLayer) 
									sceneLayer->sync_environment_from_renderer();
							}
							else if (meta->type == AssetType::Scene) {
								SceneLayer* sceneLayer = _ctx->layerStack ? _ctx->layerStack->get_layer<SceneLayer>() : nullptr;
								if (sceneLayer) {
									_editorCtx.clear_selection();
									sceneLayer->load_scene(meta->virtualPath);
								}
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			// draw ImGuizmo over selected entity
			draw_gizmos(viewportPos, viewportSize);

			// mouse clicking (only when not hovering/dragging gizmo or toolbar)
			if (_editorCtx.viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
				ImVec2 mPos = ImGui::GetMousePos();
				entt::entity hit = pick_entity_at_mouse({ mPos.x, mPos.y }, viewportPos, viewportSize);
				if (hit != entt::null) {
					_editorCtx.select_entity(hit);
				}
				else {
					_editorCtx.clear_selection();
				}
			}
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void EditorLayer::draw_gizmos(const glm::vec2& viewportPos, const glm::vec2& viewportSize)
{
	if (!_ctx->activeRegistry || !_ctx->activeSceneData)
		return;
	if (_editorCtx.inspectorMode != InspectorMode::Entity || _editorCtx.selectedEntity == entt::null)
		return;

	entt::registry& registry = *_ctx->activeRegistry;
	entt::entity entity = _editorCtx.selectedEntity;
	if (!registry.valid(entity) || !registry.all_of<TransformComponent>(entity))
		return;

	auto& tc = registry.get<TransformComponent>(entity);

	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(viewportPos.x, viewportPos.y, viewportSize.x, viewportSize.y);

	glm::mat4 cameraView = _ctx->activeSceneData->view;
	glm::mat4 cameraProj = _ctx->activeSceneData->proj;
	// unflip vulkan inverted y projection axis for ImGuizmo 
	cameraProj[1][1] *= -1.0f;

	glm::mat4 transformMatrix = tc.worldTransform;

	float snapValues[3] = { _editorCtx.snapTranslation, _editorCtx.snapTranslation, _editorCtx.snapTranslation };
	if (_editorCtx.gizmoOperation == ImGuizmo::ROTATE) {
		snapValues[0] = snapValues[1] = snapValues[2] = _editorCtx.snapRotation;
	}
	else if (_editorCtx.gizmoOperation == ImGuizmo::SCALE) {
		snapValues[0] = snapValues[1] = snapValues[2] = _editorCtx.snapScale;
	}

	ImGuizmo::OPERATION op = static_cast<ImGuizmo::OPERATION>(_editorCtx.gizmoOperation);
	ImGuizmo::MODE mode = (op == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : static_cast<ImGuizmo::MODE>(_editorCtx.gizmoMode);

	if (ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraProj), op, mode, glm::value_ptr(transformMatrix), nullptr, _editorCtx.useSnap ? snapValues : nullptr)) {
		// convert manipulated world transform back to parent local space if entity has a parent
		glm::mat4 parentWorld(1.0f);
		if (const auto* hc = registry.try_get<HierarchyComponent>(entity)) {
			if (hc->parent != entt::null && registry.valid(hc->parent) && registry.all_of<TransformComponent>(hc->parent)) {
				parentWorld = registry.get<TransformComponent>(hc->parent).worldTransform;
			}
		}

		glm::mat4 localMatrix = glm::inverse(parentWorld) * transformMatrix;

		float matrixTranslation[3], matrixRotation[3], matrixScale[3];
		ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(localMatrix), matrixTranslation, matrixRotation, matrixScale);

		tc.position = glm::make_vec3(matrixTranslation);
		tc.rotation = glm::make_vec3(matrixRotation);
		tc.scale = glm::make_vec3(matrixScale);

		mark_subtree_transforms_dirty(registry, entity, parentWorld);
		if (_ctx->renderer) {
			_ctx->renderer->mark_scene_dirty();
		}
	}
}

void EditorLayer::draw_hierarchy_panel()
{
	if (!ImGui::Begin("Scene Hierarchy")) {
		ImGui::End();
		return;
	}

	if (_ctx->activeScene) {
		entt::registry& registry = _ctx->activeScene->get_registry();

		if (ImGui::Button("+ Create Empty Entity", ImVec2(-1.0f, 0.0f))) {
			entt::entity e = _ctx->activeScene->create_entity("New Entity");
			_editorCtx.select_entity(e);
			if (_ctx->renderer) _ctx->renderer->mark_scene_dirty();
		}
		ImGui::Separator();

		auto view = registry.view<const TagComponent>();
		for (auto [entity, _] : view.each()) {
			const auto* hc = registry.try_get<HierarchyComponent>(entity);
			if (!hc || hc->parent == entt::null || !registry.valid(hc->parent)) {
				draw_entity_node(entity);
			}
		}

		if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered()) {
			_editorCtx.clear_selection();
		}
	}

	ImGui::End();
}

void EditorLayer::draw_entity_node(entt::entity entity)
{
	if (!_ctx->activeScene) 
		return;
	entt::registry& registry = _ctx->activeScene->get_registry();
	if (!registry.valid(entity)) 
		return;

	const auto* tagComp = registry.try_get<TagComponent>(entity);
	const auto* hc = registry.try_get<HierarchyComponent>(entity);
	const char* name = (tagComp && !tagComp->tag.empty()) ? tagComp->tag.c_str() : "Unnamed Entity";

	// setup ImGui tree node flags based on selection and children
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (_editorCtx.inspectorMode == InspectorMode::Entity && _editorCtx.selectedEntity == entity) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}
	if (!hc || hc->children.empty()) {
		flags |= ImGuiTreeNodeFlags_Leaf;
	}

	// draw tree node
	bool opened = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entt::to_integral(entity))), flags, "%s", name);

	// left click select
	if (ImGui::IsItemClicked()) {
		_editorCtx.select_entity(entity);
	}

	// right click context menu
	bool entityDeleted = false;
	if (ImGui::BeginPopupContextItem()) {
		if (ImGui::MenuItem("Duplicate Entity")) {
			entt::entity copy = _ctx->activeScene->duplicate_entity(entity);
			_editorCtx.select_entity(copy);
			if (_ctx->renderer) _ctx->renderer->mark_scene_dirty();
		}
		if (ImGui::MenuItem("Delete Entity")) {
			if (_editorCtx.selectedEntity == entity) {
				_editorCtx.clear_selection();
			}
			_ctx->activeScene->destroy_entity(entity);
			if (_ctx->renderer) _ctx->renderer->mark_scene_dirty();
			entityDeleted = true;
		}
		ImGui::EndPopup();
	}

	if (opened) {
		if (!entityDeleted && hc) {
			std::vector<entt::entity> children = hc->children;
			for (entt::entity child : children) {
				draw_entity_node(child);
			}
		}
		ImGui::TreePop();
	}
}

void EditorLayer::draw_render_target_viewer_panel()
{
	if (!ImGui::Begin("Render Target Viewer")) {
		ImGui::End();
		return;
	}

	if (!_ctx->renderer) {
		ImGui::End();
		return;
	}

	const auto& debugTargets = _ctx->renderer->get_debug_targets();
	if (debugTargets.empty()) {
		ImGui::TextDisabled("No active RenderGraph targets available.");
		ImGui::End();
		return;
	}

	std::vector<std::string> targetNames;
	targetNames.reserve(debugTargets.size());
	for (const auto& [name, _] : debugTargets) {
		targetNames.push_back(name);
	}
	std::sort(targetNames.begin(), targetNames.end());

	if (!debugTargets.contains(_editorCtx.selectedDebugTarget) && !targetNames.empty()) {
		_editorCtx.selectedDebugTarget = targetNames[0];
		_editorCtx.selectedTargetLayer = 0;
	}

	if (ImGui::BeginCombo("Render Target", _editorCtx.selectedDebugTarget.c_str())) {
		for (const auto& name : targetNames) {
			bool isSelected = (name == _editorCtx.selectedDebugTarget);
			if (ImGui::Selectable(name.c_str(), isSelected)) {
				_editorCtx.selectedDebugTarget = name;
				_editorCtx.selectedTargetLayer = 0;
			}
			if (isSelected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}

	auto it = debugTargets.find(_editorCtx.selectedDebugTarget);
	if (it != debugTargets.end()) {
		const TargetInfo& info = it->second;
		ImGui::TextDisabled("Resolution: %u x %u | Format: %s%s",
			info.extent.width,
			info.extent.height,
			string_VkFormat(info.format),
			info.isCube ? " (Cubemap)" : "");

		VkImageView viewToDisplay = info.view;
		if (!info.faceViews.empty()) {
			int maxLayer = static_cast<int>(info.faceViews.size()) - 1;
			_editorCtx.selectedTargetLayer = std::clamp(_editorCtx.selectedTargetLayer, 0, maxLayer);
			const char* label = info.isCube ? "Cubemap Face (0..5)" : "Array Layer / Cascade";
			ImGui::SliderInt(label, &_editorCtx.selectedTargetLayer, 0, maxLayer);
			viewToDisplay = info.faceViews[_editorCtx.selectedTargetLayer];
		}

		if (viewToDisplay != VK_NULL_HANDLE && DefaultResources::get().defaultSamplerLinear.is_valid()) {
			VkDescriptorSet desc = _guiRenderer.get_image_descriptor(
				viewToDisplay,
				DefaultResources::get().defaultSamplerLinear->get_sampler(),
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			);

			if (desc != VK_NULL_HANDLE) {
				ImVec2 avail = ImGui::GetContentRegionAvail();
				float aspect = (info.extent.height > 0)
					? static_cast<float>(info.extent.width) / static_cast<float>(info.extent.height)
					: 1.0f;
				float displayW = avail.x;
				float displayH = displayW / std::max(0.01f, aspect);
				if (displayH > avail.y && avail.y > 0.0f) {
					displayH = avail.y;
					displayW = displayH * aspect;
				}
				ImGui::Image((ImTextureID)desc, ImVec2(displayW, displayH));
			}
		}
	}

	ImGui::End();
}

void EditorLayer::draw_performance_panel()
{
	if (!ImGui::Begin("Performance & Telemetry")) {
		ImGui::End();
		return;
	}

	// frame time and fps
	float avgFrameMs = 0.0f;
	std::vector<float> historyContiguous(_frameTimeHistory.begin(), _frameTimeHistory.end());
	for (float ms : historyContiguous) {
		avgFrameMs += ms;
	}
	if (!historyContiguous.empty()) {
		avgFrameMs /= static_cast<float>(historyContiguous.size());
	}
	float fps = (avgFrameMs > 0.0001f) ? (1000.0f / avgFrameMs) : 0.0f;

	ImGui::Text("FPS: %.1f (%.2f ms avg)", fps, avgFrameMs);
	if (!historyContiguous.empty()) {
		ImGui::PlotLines(
			"##FrameTimePlot",
			historyContiguous.data(),
			static_cast<int>(historyContiguous.size()),
			0,
			"Frame Time (ms)",
			0.0f,
			33.3f,
			ImVec2(-1.0f, 65.0f)
		);
	}

	// GPU frustum culling stats
	if (_ctx->renderer && ImGui::CollapsingHeader("GPU Frustum Culling Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
		FrameData& currentFrame = _ctx->renderer->get_current_frame();
		if (currentFrame._cullStatsBuffer.is_valid() && currentFrame._cullStatsBuffer->get_mapped_data()) {
			const auto* stats = static_cast<const CullStatsData*>(currentFrame._cullStatsBuffer->get_mapped_data());
			ImGui::BulletText("Visible Indirect Draws: %u", stats->visibleDraws);
			ImGui::BulletText("Visible Triangles: %u", stats->visibleTriangles);
		}
	}

	// GlobalGeometryPool and StagingUploader metrics
	if (_ctx->resourceManager && ImGui::CollapsingHeader("VRAM & Geometry Pool Residency", ImGuiTreeNodeFlags_DefaultOpen)) {
		GeometryPoolMetrics metrics = _ctx->resourceManager->get_geometry_pool().get_metrics();
		float vertUsedMB = static_cast<float>(metrics.usedVertexBytes) / (1024.0f * 1024.0f);
		float vertCapMB = static_cast<float>(metrics.totalVertexCapacityBytes) / (1024.0f * 1024.0f);
		float largestFreeMB = static_cast<float>(metrics.largestFreeVertexBlockBytes) / (1024.0f * 1024.0f);
		float idxUsedMB = static_cast<float>(metrics.usedIndexBytes) / (1024.0f * 1024.0f);
		float idxCapMB = static_cast<float>(metrics.totalIndexCapacityBytes) / (1024.0f * 1024.0f);

		ImGui::Text("Active 64MB BDA Vertex Pages: %u (Largest Contiguous Free: %.1f MB)",
			metrics.activeVertexPages, largestFreeMB);
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

	// virtual texture metrics
	if (_ctx->resourceManager && ImGui::CollapsingHeader("Virtual Texturing Residency & Cache", ImGuiTreeNodeFlags_DefaultOpen)) {
		VirtualTextureMetrics vt = _ctx->resourceManager->get_vtsystem().get_metrics();
		float atlasMB = static_cast<float>(vt.physicalAtlasVramBytes) / (1024.0f * 1024.0f);
		float ptKB = static_cast<float>(vt.pageTableVramBytes) / 1024.0f;

		float slotRatio = vt.totalPhysicalSlots > 0
			? static_cast<float>(vt.usedPhysicalSlots) / static_cast<float>(vt.totalPhysicalSlots)
			: 0.0f;
		ImGui::ProgressBar(slotRatio, ImVec2(-1.0f, 0.0f),
			fmt::format("Physical Atlas Slots: {} / {} ({} pinned | {:.1f} MB)",
				vt.usedPhysicalSlots, vt.totalPhysicalSlots, vt.pinnedPhysicalSlots, atlasMB).c_str());

		uint64_t totalLookups = vt.totalCacheHits + vt.totalCacheMisses;
		float hitRate = totalLookups > 0
			? (static_cast<float>(vt.totalCacheHits) / static_cast<float>(totalLookups)) * 100.0f
			: 100.0f;

		ImGui::BulletText("Registered Virtual Textures: %u (Page Table: %.1f KB)", vt.registeredVirtualTextures, ptKB);
		ImGui::BulletText("Tile Cache Hit Rate: %.1f%% (%llu hits, %llu misses, %llu evictions)",
			hitRate,
			static_cast<unsigned long long>(vt.totalCacheHits),
			static_cast<unsigned long long>(vt.totalCacheMisses),
			static_cast<unsigned long long>(vt.totalEvictions));
		ImGui::BulletText("Frame Feedback: %u raw -> %u unique pages | %u in-flight | %llu streamed",
			vt.lastFrameRawFeedbackCount,
			vt.lastFrameUniquePagesRequested,
			vt.inFlightTileRequests,
			static_cast<unsigned long long>(vt.totalTilesStreamed));
	}

	// profiler metrics
	if (ImGui::CollapsingHeader("RenderGraph Pass Timings (GPU / CPU)", ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto& results = Profiler::get().get_metrics();
		if (ImGui::BeginTable("PassTimingsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("Pass Name");
			ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
			ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
			ImGui::TableSetupColumn("CPU Avg (ms)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
			ImGui::TableSetupColumn("GPU Avg (ms)", ImGuiTableColumnFlags_WidthFixed, 75.0f);

			ImGui::TableHeadersRow();

			for (const ProfileMetric& metric : results) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(metric.name.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%.3f", metric.cpuTimeMs);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%.3f", metric.gpuTimeMs);
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%.3f", metric.avgCpuTimeMs);
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%.3f", metric.avgGpuTimeMs);
			}
			ImGui::EndTable();
		}
	}

	ImGui::End();
}

void EditorLayer::draw_renderer_settings_panel()
{
	if (!ImGui::Begin("Render Settings & CVars")) {
		ImGui::End();
		return;
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

	if (ImGui::CollapsingHeader("Console Variables (CVars)", ImGuiTreeNodeFlags_DefaultOpen)) {
		CVarSystem::Get()->DrawCVarEditor();
	}

	ImGui::End();
}

void EditorLayer::delete_entity_recursive(entt::entity entity, bool detachFromParent)
{
	if (!_ctx || !_ctx->activeRegistry) return;
	entt::registry& registry = *_ctx->activeRegistry;
	if (entity == entt::null || !registry.valid(entity)) return;

	// detach from parents children list
	if (detachFromParent) {
		if (auto* hc = registry.try_get<HierarchyComponent>(entity)) {
			if (hc->parent != entt::null && registry.valid(hc->parent)) {
				if (auto* parentHc = registry.try_get<HierarchyComponent>(hc->parent)) {
					std::erase(parentHc->children, entity);
				}
			}
		}
	}

	// recursively destroy all children
	if (auto* hc = registry.try_get<HierarchyComponent>(entity)) {
		std::vector<entt::entity> childrenCopy = hc->children;
		for (entt::entity child : childrenCopy) {
			delete_entity_recursive(child, false);
		}
	}

	// clear editor selection if this entity was selected
	if (_editorCtx.selectedEntity == entity) {
		_editorCtx.clear_selection();
	}

	registry.destroy(entity);

	if (_ctx->renderer) {
		_ctx->renderer->mark_scene_dirty();
	}
}

entt::entity EditorLayer::pick_entity_at_mouse(const glm::vec2& mousePos, const glm::vec2& viewportPos, const glm::vec2& viewportSize)
{
	if (!_ctx->activeRegistry || !_ctx->activeSceneData) return entt::null;
	if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) return entt::null;

	// convert mouse screen position to NDC [-1, 1]
	float ndcX = ((mousePos.x - viewportPos.x) / viewportSize.x) * 2.0f - 1.0f;
	float ndcY = ((mousePos.y - viewportPos.y) / viewportSize.y) * 2.0f - 1.0f;
	if (ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f) {
		return entt::null;
	}

	// construct world space ray from camera position 
	const GPUSceneData& sceneData = *_ctx->activeSceneData;
	glm::vec3 rayOrigin = glm::vec3(sceneData.cameraPosition);

	glm::mat4 invViewProj = glm::inverse(sceneData.viewproj);
	glm::vec4 targetWorld = invViewProj * glm::vec4(ndcX, ndcY, 0.5f, 1.0f);
	targetWorld /= targetWorld.w;

	glm::vec3 rayDir = glm::normalize(glm::vec3(targetWorld) - rayOrigin);
	if (glm::dot(rayDir, glm::vec3(glm::inverse(sceneData.view)[2])) > 0.0f) {
		rayDir = -rayDir; // ensure ray points forward along camera view direction
	}

	entt::registry& registry = *_ctx->activeRegistry;
	entt::entity closestEntity = entt::null;
	float closestDistance = std::numeric_limits<float>::max();

	auto view = registry.view<const TransformComponent, const MeshComponent>();
	for (auto [entity, tc, mc] : view.each()) {
		if (!mc.visible || !mc.mesh.is_valid()) continue;

		// world space bounding sphere precheck
		glm::vec3 worldSphereCenter = glm::vec3(tc.worldTransform * glm::vec4(mc.bounds.origin, 1.0f));
		float maxScale = std::max({
				glm::length(glm::vec3(tc.worldTransform[0])),
				glm::length(glm::vec3(tc.worldTransform[1])),
				glm::length(glm::vec3(tc.worldTransform[2]))
			});
		float worldRadius = mc.bounds.sphereRadius * maxScale;

		float sphereHitT = 0.0f;
		if (worldRadius > 0.0f && !intersect_ray_sphere(rayOrigin, rayDir, worldSphereCenter, worldRadius, sphereHitT)) {
			continue;
		}
		if (sphereHitT > closestDistance) {
			continue;
		}

		// exact triangle test in mesh local space if CPU picking geometry is available
		if (mc.mesh->has_picking_data()) {
			glm::mat4 invWorld = glm::inverse(tc.worldTransform);
			glm::vec3 localOrigin = glm::vec3(invWorld * glm::vec4(rayOrigin, 1.0f));
			glm::vec3 localDir = glm::normalize(glm::vec3(invWorld * glm::vec4(rayDir, 0.0f)));

			const auto positions = mc.mesh->get_picking_positions();
			const auto indices = mc.mesh->get_picking_indices();

			size_t startIdx = mc.startIndex;
			size_t endIdx = std::min(indices.size(), static_cast<size_t>(mc.startIndex + mc.count));
			if (endIdx == 0 || endIdx > indices.size()) {
				startIdx = 0;
				endIdx = indices.size();
			}

			for (size_t i = startIdx; i + 2 < endIdx; i += 3) {
				uint32_t i0 = indices[i + 0];
				uint32_t i1 = indices[i + 1];
				uint32_t i2 = indices[i + 2];
				if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) continue;

				float localT = 0.0f;
				if (intersect_ray_triangle(localOrigin, localDir, positions[i0], positions[i1], positions[i2], localT)) {
					glm::vec3 localHitPos = localOrigin + localDir * localT;
					glm::vec3 worldHitPos = glm::vec3(tc.worldTransform * glm::vec4(localHitPos, 1.0f));
					float worldDist = glm::length(worldHitPos - rayOrigin);
					if (worldDist < closestDistance) {
						closestDistance = worldDist;
						closestEntity = entity;
					}
				}
			}
		}
		else if (sphereHitT < closestDistance) {
			closestDistance = sphereHitT;
			closestEntity = entity;
		}
	}

	return closestEntity;
}

void EditorLayer::open_save_scene_as_popup()
{
	SceneLayer* sceneLayer = _ctx && _ctx->layerStack ? _ctx->layerStack->get_layer<SceneLayer>() : nullptr;
	if (sceneLayer)
	{
		const auto& env = sceneLayer->get_scene().get_environment_settings();
		const auto& curPath = sceneLayer->get_scene().get_file_path();

		std::snprintf(_saveAsSceneNameBuf, sizeof(_saveAsSceneNameBuf), "%s", env.sceneName.c_str());
		std::string stem = curPath.empty() ? "new_scene" : curPath.stem().string();
		std::snprintf(_saveAsFileNameBuf, sizeof(_saveAsFileNameBuf), "%s_copy.scene", stem.c_str());
	}
	_openSaveAsPopup = true;
}

void EditorLayer::draw_save_scene_as_popup()
{
	if (_openSaveAsPopup)
	{
		ImGui::OpenPopup("Save Scene As");
		_openSaveAsPopup = false;
	}

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		SceneLayer* sceneLayer = _ctx->layerStack ? _ctx->layerStack->get_layer<SceneLayer>() : nullptr;

		ImGui::InputText("Scene Name", _saveAsSceneNameBuf, sizeof(_saveAsSceneNameBuf));
		ImGui::InputText("File Name (.scene)", _saveAsFileNameBuf, sizeof(_saveAsFileNameBuf));

		std::filesystem::path scenesDir = "../../assets/scenes";
		std::filesystem::path candidateName = (_saveAsFileNameBuf[0] != '\0') ? _saveAsFileNameBuf : "untitled.scene";
		if (candidateName.extension() != ".scene")
		{
			candidateName.replace_extension(".scene");
		}
		std::filesystem::path targetPath = scenesDir / candidateName.filename();

		ImGui::Spacing();
		ImGui::TextDisabled("Target Path: %s", targetPath.generic_string().c_str());

		bool fileAlreadyExists = std::filesystem::exists(targetPath);
		if (fileAlreadyExists)
		{
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "Warning: '%s' already exists and will be overwritten.",
			                   candidateName.filename().string().c_str());
		}

		// Quick-select list of existing scenes in assets/scenes/
		auto existingScenes = AssetRegistry::get().get_assets_by_type(AssetType::Scene, false);
		if (!existingScenes.empty())
		{
			ImGui::Spacing();
			ImGui::TextUnformatted("Existing Scenes in assets/scenes/:");
			ImGui::BeginChild("ExistingScenesList", ImVec2(0.0f, 110.0f), true);
			for (const auto& sMeta : existingScenes)
			{
				std::string fname = sMeta.virtualPath.filename().string();
				if (ImGui::Selectable(fname.c_str(), fname == candidateName.filename().string()))
				{
					std::snprintf(_saveAsSceneNameBuf, sizeof(_saveAsSceneNameBuf), "%s", sMeta.name.c_str());
					std::snprintf(_saveAsFileNameBuf, sizeof(_saveAsFileNameBuf), "%s", fname.c_str());
				}
			}
			ImGui::EndChild();
		}

		ImGui::Spacing();
		ImGui::Separator();

		if (ImGui::Button("Save", ImVec2(140.0f, 0.0f)))
		{
			if (sceneLayer)
			{
				sceneLayer->save_scene_as(targetPath, _saveAsSceneNameBuf);
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

void EditorLayer::on_save_as(const Events::SceneSaveAsEvent&)
{
	open_save_scene_as_popup();
}
