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

void Editor::draw_gui(entt::registry& registry, ResourceManager& resourceManager, const EngineStats& stats, const GPUSceneData& sceneData, Renderer& renderer)
{
	draw_dockspace();
	draw_main_menu_bar();

	EditorContext ctx{
		.registry = registry,
		.resourceManager = resourceManager,
		.stats = stats,
		.selectedEntity = _selectedEntity,
		.sceneData = sceneData,
		.renderer = renderer
	};

	for (auto& panel : _panels) {
		panel->on_draw(ctx);
	}

	_isViewportHovered = ctx.isViewportHovered;
	process_viewport_picking(ctx);
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

static bool intersect_ray_aabb(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& boxMin, const glm::vec3& boxMax, float& tOut)
{
	float tMin = -std::numeric_limits<float>::max();
	float tMax = std::numeric_limits<float>::max();

	for (int i = 0; i < 3; ++i) {
		if (std::abs(rayDir[i]) < 1e-6f) {
			// ray is parallel to slab
			if (rayOrigin[i] < boxMin[i] || rayOrigin[i] > boxMax[i])
				return false;
		}
		else {
			float invD = 1.0f / rayDir[i];
			float t0 = (boxMin[i] - rayOrigin[i]) * invD;
			float t1 = (boxMax[i] - rayOrigin[i]) * invD;
			if (t0 > t1) std::swap(t0, t1);

			tMin = std::max(tMin, t0);
			tMax = std::min(tMax, t1);
			if (tMin > tMax) return false;
		}
	}

	// box is entirely behind ray
	if (tMax < 0.0f) return false;

	// if ray starts inside the box (tMin < 0), return exit distance tMax, otherwise entry distance tMin
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

	// unproject using scenes viewproj matrix (matching the viewport aspect ratio)
	glm::mat4 invVP = glm::inverse(ctx.sceneData.viewproj);
	glm::vec4 p0 = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
	glm::vec4 p1 = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);

	if (p0.w == 0.0f || p1.w == 0.0f) return;
	p0 /= p0.w;
	p1 /= p1.w;

	glm::vec3 rayOrigin = glm::vec3(ctx.sceneData.cameraPosition);
	glm::vec3 p0_3 = glm::vec3(p0);
	glm::vec3 p1_3 = glm::vec3(p1);
	glm::vec3 farPoint = (glm::distance(rayOrigin, p0_3) > glm::distance(rayOrigin, p1_3)) ? p0_3 : p1_3;
	glm::vec3 rayDir = glm::normalize(farPoint - rayOrigin);

	float closestDist = std::numeric_limits<float>::max();
	entt::entity hitEntity = entt::null;

	// test renderable mesh components
	auto meshView = ctx.registry.view<const MeshComponent>();
	for (auto [entity, mc] : meshView.each()) {
		if (!mc.visible || !mc.mesh.is_valid())
			continue;
		glm::mat4 worldMatrix(1.0f);
		if (const auto* tc = ctx.registry.try_get<TransformComponent>(entity)) {
			glm::mat4 worldMatrix = compute_world_transform(ctx.registry, entity);
		}
		glm::vec3 bMin = mc.bounds.origin - mc.bounds.extents;
		glm::vec3 bMax = mc.bounds.origin + mc.bounds.extents;
		float t;

		glm::mat4 invMat = glm::inverse(worldMatrix);
		glm::vec3 localRayOrigin = glm::vec3(invMat * glm::vec4(rayOrigin, 1.0f));
		glm::vec3 localRayDir = glm::vec3(invMat * glm::vec4(rayDir, 0.0f));

		if (intersect_ray_aabb(localRayOrigin, localRayDir, bMin, bMax, t)) {
			glm::vec3 localHit = localRayOrigin + localRayDir * t;
			glm::vec3 worldHit = glm::vec3(worldMatrix * glm::vec4(localHit, 1.0f));
			float worldDist = glm::distance(rayOrigin, worldHit);
			if (worldDist < closestDist) {
				closestDist = worldDist;
				hitEntity = entity;
			}
		}
	}

	// test point light components
	auto lightView = ctx.registry.view<const PointLightComponent>();
	for (auto [entity, pl] : lightView.each()) {
		if (!pl.visible) continue;

		glm::vec3 lightPos(0.0f);
		if (const auto* tc = ctx.registry.try_get<TransformComponent>(entity)) {
			lightPos = tc->position;
		}

		float t;
		if (intersect_ray_sphere(rayOrigin, rayDir, lightPos, 0.5f, t)) {
			if (t < closestDist) {
				closestDist = t;
				hitEntity = entity;
			}
		}
	}

	// update selected entity
	ctx.selectedEntity = hitEntity;
}
