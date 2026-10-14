#include "app/Editor.h"

#include "app/editor/HierarchyPanel.h"
#include "app/editor/InspectorPanel.h"
#include "app/editor/EnvironmentPanel.h"
#include "app/editor/PerformancePanel.h"
#include "app/editor/ViewportPanel.h"
#include "app/editor/CVarPanel.h"
#include "app/editor/GizmoPanel.h"
#include "app/editor/RenderTargetViewPanel.h"
#include "app/editor/ContentBrowserPanel.h"

#include <graphics/Renderer.h>
#include <graphics/pass/CustomPostVFXPass.h>

#include <core/Components.h>
#include <resources/Model.h>
#include <imgui_impl_sdl2.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <limits>
#include <cmath>

Editor::Editor()
{
	add_panel<ViewportPanel>();
	add_panel<HierarchyPanel>();
	add_panel<InspectorPanel>();
	add_panel<ContentBrowserPanel>();
	add_panel<EnvironmentPanel>();
	add_panel<PerformancePanel>();
	add_panel<CVarPanel>();
	add_panel<GizmoPanel>();
	add_panel<RenderTargetViewPanel>();
}

void Editor::draw_gui(entt::registry& registry, ResourceManager& resourceManager, const GPUSceneData& sceneData, Renderer& renderer)
{
	draw_dockspace();
	draw_main_menu_bar();

	EditorContext ctx{
		.registry = registry,
		.resourceManager = resourceManager,
		.selectedEntity = _selectedEntity,
		.sceneData = sceneData,
		.renderer = renderer
	};

	for (auto& panel : _panels) {
		panel->on_draw(ctx);
	}

	_isViewportHovered = ctx.isViewportHovered;
	process_viewport_picking(ctx);

	if (ImGui::Begin("Render Pipeline & VFX")) {
		auto& features = renderer.get_features();

		// runtime VFX pass 
		if (ImGui::Button("+ Add Stylized Post VFX Pass")) {
			auto* vfx = renderer.insert_feature_before<CustomPostVFXPass>(
				"BloomPass",
				"StylizedVFX_" + std::to_string(features.size()),
				"vfx_stylized_post.frag",
				true);
			vfx->set_float("aberrationStrength", 0.004f, 0.0f, 0.02f);
			vfx->set_float("edgeStrength", 1.5f, 0.0f, 5.0f);
			vfx->set_float("vignetteIntensity", 0.4f, 0.0f, 2.0f);
		}

		ImGui::Separator();

		// iterate through all registered RenderFeatures
		for (size_t i = 0; i < features.size(); ++i) {
			RenderFeature* feature = features[i].get();
			ImGui::PushID(static_cast<int>(i));

			bool enabled = feature->is_enabled();
			if (ImGui::Checkbox("##Enabled", &enabled)) {
				feature->set_enabled(enabled);
			}
			ImGui::SameLine();

			// optional up/down arrows to reorder post/VFX passes 
			if (i > 0 && ImGui::SmallButton("^")) {
				std::swap(features[i], features[i - 1]);
				ImGui::PopID();
				break;
			}
			ImGui::SameLine();
			if (i + 1 < features.size() && ImGui::SmallButton("v")) {
				std::swap(features[i], features[i + 1]);
				ImGui::PopID();
				break;
			}
			ImGui::SameLine();

			bool open = ImGui::TreeNodeEx(feature->get_name().c_str(), ImGuiTreeNodeFlags_DefaultOpen);
			if (open) {
				if (enabled) {
					feature->on_imgui();
				}
				else {
					ImGui::TextDisabled("Pass disabled (using blackboard fallback)");
				}
				ImGui::TreePop();
			}

			ImGui::PopID();
		}
	}
	ImGui::End();
}

void Editor::on_input_event(const Events::SDLInputEvent& event)
{
	ImGui_ImplSDL2_ProcessEvent(&event.raw);
}

void Editor::on_entity_destroyed(const Events::EntityDestroyedEvent& event)
{
	if (_selectedEntity == event.entity) {
		_selectedEntity = entt::null;
	}
}

bool Editor::want_capture_mouse() const
{
	return ImGui::GetIO().WantCaptureMouse;
}

bool Editor::want_capture_keyboard() const
{
	return ImGui::GetIO().WantCaptureKeyboard;
}

void Editor::draw_main_menu_bar()
{
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("Window")) {
			for (auto& panel : _panels) {
				ImGui::MenuItem(panel->get_name(), nullptr, &panel->is_open);
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}
}

void Editor::draw_dockspace()
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
		ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
	}
}

struct RaycastHit {
	entt::entity hitEntity{ entt::null };
	float distance{ std::numeric_limits<float>::max() };
	glm::vec3 worldPosition{ 0.0f };
	glm::vec3 worldNormal{ 0.0f };
	uint32_t triangleIndex{ 0 };
	glm::vec2 barycentricCoords{ 0.0f };
};

static bool intersect_ray_aabb(const glm::vec3& rayOrigin, const glm::vec3& rayDirUnnorm, const glm::vec3& boxMin, const glm::vec3& boxMax, float& tOut) {
	float tMin = -std::numeric_limits<float>::max();
	float tMax = std::numeric_limits<float>::max();

	for (int i = 0; i < 3; ++i) {
		if (std::abs(rayDirUnnorm[i]) < 1e-6f) {
			if (rayOrigin[i] < boxMin[i] || rayOrigin[i] > boxMax[i]) return false;
		}
		else {
			float invD = 1.0f / rayDirUnnorm[i];
			float t0 = (boxMin[i] - rayOrigin[i]) * invD;
			float t1 = (boxMax[i] - rayOrigin[i]) * invD;

			if (t0 > t1) std::swap(t0, t1);
			tMin = std::max(tMin, t0);
			tMax = std::min(tMax, t1);

			if (tMin > tMax) return false;
		}
	}

	if (tMax < 0.0f) return false;
	tOut = (tMin < 0.0f) ? tMax : tMin;
	return true;
}

static bool intersect_ray_sphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& sphereCenter, float sphereRadius, float& tOut)
{
	glm::vec3 m = rayOrigin - sphereCenter;
	float b = glm::dot(m, rayDir);
	float c = glm::dot(m, m) - sphereRadius * sphereRadius;

	// ray origin is outside the sphere and pointing away
	if (c > 0.0f && b > 0.0f) return false;
	float discr = b * b - c;
	if (discr < 0.0f) return false;

	float t = -b - std::sqrt(discr);
	if (t < 0.0f) t = -b + std::sqrt(discr);
	if (t < 0.0f) return false;

	tOut = t;
	return true;
}

static bool intersect_ray_triangle(const glm::vec3& orig, const glm::vec3& dirUnnorm, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, bool doubleSided, float& t, glm::vec2& baryCoords)
{
	constexpr float EPSILON = 1e-7f;
	glm::vec3 edge1 = v1 - v0;
	glm::vec3 edge2 = v2 - v0;
	glm::vec3 h = glm::cross(dirUnnorm, edge2);
	float a = glm::dot(edge1, h);

	// Backface & parallel culling
	if (!doubleSided) {
		if (a < EPSILON) return false;
	}
	else {
		if (a > -EPSILON && a < EPSILON) return false;
	}

	float f = 1.0f / a;
	glm::vec3 s = orig - v0;
	float u = f * glm::dot(s, h);
	if (u < 0.0f || u > 1.0f) return false;

	glm::vec3 q = glm::cross(s, edge1);
	float v = f * glm::dot(dirUnnorm, q);
	if (v < 0.0f || u + v > 1.0f) return false;

	float t_val = f * glm::dot(edge2, q);

	// t_val is natively world distance because dirUnnorm holds world scale
	if (t_val > EPSILON) {
		t = t_val;
		baryCoords = glm::vec2(u, v);
		return true;
	}
	return false;
}

// recursively traverse scene graph to test submesh bounding box in local space
static void raycast_node(const std::shared_ptr<Node>& node, const glm::mat4& parentMatrix, const glm::vec3& rayOrigin, const glm::vec3& rayDir, float& closestDist, entt::entity& hitEntity, entt::entity currentEntity)
{
	glm::mat4 nodeMatrix = parentMatrix * node->localTransform;
	auto modelNode = std::dynamic_pointer_cast<ModelNode>(node);

	if (modelNode) {
		glm::mat4 invMat = glm::inverse(nodeMatrix);
		glm::vec3 localRayOrigin = glm::vec3(invMat * glm::vec4(rayOrigin, 1.0f));
		glm::vec3 localRayDir = glm::vec3(invMat * glm::vec4(rayDir, 0.0f));

		for (const auto& sm : modelNode->submeshes) {
			glm::vec3 bMin = sm.bounds.origin - sm.bounds.extents;
			glm::vec3 bMax = sm.bounds.origin + sm.bounds.extents;
			float localT;
			if (intersect_ray_aabb(localRayOrigin, localRayDir, bMin, bMax, localT)) {
				glm::vec3 localHit = localRayOrigin + localRayDir * localT;
				glm::vec3 worldHit = glm::vec3(nodeMatrix * glm::vec4(localHit, 1.0f));
				float worldDist = glm::distance(rayOrigin, worldHit);
				if (worldDist < closestDist) {
					closestDist = worldDist;
					hitEntity = currentEntity;
				}
			}
		}
	}

	for (const auto& child : node->children) {
		raycast_node(child, nodeMatrix, rayOrigin, rayDir, closestDist, hitEntity, currentEntity);
	}
}

static glm::mat4 compute_world_transform(entt::registry& registry, entt::entity e)
{
	glm::mat4 world(1.0f);
	entt::entity current = e;
	while (current != entt::null) {
		if (const auto* tc = registry.try_get<TransformComponent>(current)) {
			world = tc->get_transform() * world;
		}
		if (const auto* hc = registry.try_get<HierarchyComponent>(current)) {
			current = hc->parent;
		}
		else {
			current = entt::null;
		}
	}
	return world;
}

void Editor::process_viewport_picking(EditorContext& ctx)
{
	ImGuiIO& io = ImGui::GetIO();

	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) // ignore non left click events
		return;

	if (!ctx.isViewportHovered)  // ignore if mouse is not over viewport
		return;

	if (ImGuizmo::IsOver() || ImGuizmo::IsUsing() || ctx.isGizmoControlsHovered) // ignore if mouse is over gizmo controls
		return;

	if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) // ignore if freelooking
		return;
	ImVec2 mousePos = io.MousePos;

	// convert mouse screen position to viewport local coords
	float relX = mousePos.x - ctx.viewportPos.x;
	float relY = mousePos.y - ctx.viewportPos.y;

	if (relX < 0.0f || relY < 0.0f || relX > ctx.viewportSize.x || relY > ctx.viewportSize.y) // ignore if mouse is outside viewport
		return;
	if (ctx.viewportSize.x <= 0.0f || ctx.viewportSize.y <= 0.0f) // ignore if viewport size is invalid
		return;

	// convert viewport local coords to NDC [-1, 1]
	float ndcX = (2.0f * relX) / ctx.viewportSize.x - 1.0f;
	float ndcY = (2.0f * relY) / ctx.viewportSize.y - 1.0f;

	// generate world space ray
	glm::mat4 invVP = glm::inverse(ctx.sceneData.viewproj);

	glm::vec4 target = invVP * glm::vec4(ndcX, ndcY, 0.5f, 1.0f);
	target /= target.w;

	glm::vec3 rayOrigin = glm::vec3(ctx.sceneData.cameraPosition);
	glm::vec3 rayDir = glm::normalize(glm::vec3(target) - rayOrigin);

	RaycastHit bestHit;

	// iterate renderable entities
	auto meshView = ctx.registry.view<const MeshComponent>();
	for (auto [entity, mc] : meshView.each()) {
		if (!mc.visible || !mc.mesh.is_valid() || !mc.mesh->has_picking_data()) continue;

		glm::mat4 worldMatrix = compute_world_transform(ctx.registry, entity);
		glm::mat4 invWorld = glm::inverse(worldMatrix);

		// transform ray to local mesh space
		glm::vec3 localRayOrigin = glm::vec3(invWorld * glm::vec4(rayOrigin, 1.0f));
		glm::vec3 localRayDirUnnorm = glm::vec3(invWorld * glm::vec4(rayDir, 0.0f));

		// AABB check
		glm::vec3 bMin = mc.bounds.origin - mc.bounds.extents;
		glm::vec3 bMax = mc.bounds.origin + mc.bounds.extents;
		float tBox;

		if (!intersect_ray_aabb(localRayOrigin, localRayDirUnnorm, bMin, bMax, tBox)) continue;
		if (tBox > bestHit.distance) continue; // box is further than a confirmed narrowphase hit

		// triangle Setup
		std::span<const glm::vec3> positions = mc.mesh->get_picking_positions();
		std::span<const uint32_t> indices = mc.mesh->get_picking_indices();

		bool doubleSided = false;
		if (const auto* mat = ctx.registry.try_get<MaterialComponent>(entity)) {
			doubleSided = mat->doubleSided;
		}

		// triangle Query
		uint32_t endIdx = mc.startIndex + mc.count;
		for (uint32_t i = mc.startIndex; i < endIdx; i += 3) {
			const glm::vec3& v0 = positions[indices[i]];
			const glm::vec3& v1 = positions[indices[i + 1]];
			const glm::vec3& v2 = positions[indices[i + 2]];

			float tTri;
			glm::vec2 bary;
			if (intersect_ray_triangle(localRayOrigin, localRayDirUnnorm, v0, v1, v2, doubleSided, tTri, bary)) {

				if (tTri < bestHit.distance) {
					bestHit.distance = tTri;
					bestHit.hitEntity = entity;
					bestHit.triangleIndex = i / 3;
					bestHit.barycentricCoords = bary;

					glm::vec3 localNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

					if (doubleSided && glm::dot(localRayDirUnnorm, localNormal) > 0.0f) {
						localNormal = -localNormal;
					}

					// convert normal to world space 
					glm::mat3 normalMatrix = glm::transpose(glm::mat3(invWorld));
					bestHit.worldNormal = glm::normalize(normalMatrix * localNormal);
					bestHit.worldPosition = rayOrigin + rayDir * tTri;
				}
			}
		}
	}

	if (bestHit.hitEntity != entt::null) {
		ctx.selectedEntity = bestHit.hitEntity;
	}
	else {
		// drop selection if clicked on negative space
		ctx.selectedEntity = entt::null;
	}
}
