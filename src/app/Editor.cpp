#include "app/Editor.h"

#include "app/editor/HierarchyPanel.h"
#include "app/editor/InspectorPanel.h"
#include "app/editor/EnvironmentPanel.h"
#include "app/editor/PerformancePanel.h"
#include "app/editor/CVarPanel.h"
#include "app/editor/GizmoPanel.h"
#include <core/Components.h>
#include <resources/vk_loader.h>
#include <imgui_impl_sdl2.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <limits>
#include <cmath>

Editor::Editor()
{
	add_panel<HierarchyPanel>();
	add_panel<InspectorPanel>();
	add_panel<EnvironmentPanel>();
	add_panel<PerformancePanel>();
	add_panel<CVarPanel>();
	add_panel<GizmoPanel>();
}

void Editor::draw_gui(entt::registry& registry, AssetManager& assetManager, const EngineStats& stats, const GPUSceneData& sceneData)
{
	draw_main_menu_bar();

	EditorContext ctx{
		.registry = registry,
		.assetManager = assetManager,
		.stats = stats,
		.selectedEntity = _selectedEntity,
		.sceneData = sceneData
	};

	for (auto& panel : _panels) {
		panel->on_draw(ctx);
	}

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


static bool intersect_ray_aabb(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& boxMin, const glm::vec3& boxMax, float& tOut)
{
	float tMin = 0.0f;
	float tMax = std::numeric_limits<float>::max();

	for (int i = 0; i < 3; ++i) {
		if (std::abs(rayDir[i]) < 1e-6f) {
			// ray is parallel to the slab
			if (rayOrigin[i] < boxMin[i] || rayOrigin[i] > boxMax[i])
				return false;
		}
		else {
			float invD = 1.0f / rayDir[i];
			float t0 = (boxMin[i] - rayOrigin[i]) * invD;
			float t1 = (boxMax[i] - rayOrigin[i]) * invD;
			if (t0 > t1) 
				std::swap(t0, t1);

			tMin = std::max(tMin, t0);
			tMax = std::min(tMax, t1);
			if (tMin > tMax)
				return false;
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
	if (t < 0.0f) t = -b + std::sqrt(discr); // if inside sphere, use exit point
	if (t < 0.0f) return false;

	tOut = t;
	return true;
}

// recursively traverse scene graph to test submesh bounding box in local space
static void raycast_node(const std::shared_ptr<Node>& node, const glm::mat4& parentMatrix, const glm::vec3& rayOrigin, const glm::vec3& rayDir, float& closestDist, entt::entity& hitEntity, entt::entity currentEntity)
{
	glm::mat4 nodeMatrix = parentMatrix * node->localTransform;
	auto meshNode = std::dynamic_pointer_cast<MeshNode>(node);

	if (meshNode && meshNode->mesh) {
		glm::mat4 invMat = glm::inverse(nodeMatrix);
		glm::vec3 localRayOrigin = glm::vec3(invMat * glm::vec4(rayOrigin, 1.0f));
		glm::vec3 localRayDir = glm::vec3(invMat * glm::vec4(rayDir, 0.0f));

		for (const auto& surface : meshNode->mesh->surfaces) {
			glm::vec3 bMin = surface.bounds.origin - surface.bounds.extents;
			glm::vec3 bMax = surface.bounds.origin + surface.bounds.extents;

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

void Editor::process_viewport_picking(EditorContext& ctx)
{
	ImGuiIO& io = ImGui::GetIO();

	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) // ignore non left click events
		return;

	if (io.WantCaptureMouse) // ignore if on imgui windows or menu bar 
		return;

	if (ImGuizmo::IsOver() || ImGuizmo::IsUsing()) // ignore if using gizmo
		return;

	if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) // ignore if freelooking
		return;

	ImVec2 mousePos = io.MousePos;
	if (mousePos.x < 0.0f || mousePos.y < 0.0f ||
		mousePos.x > io.DisplaySize.x || mousePos.y > io.DisplaySize.y) return;

	// convert screen space to normalized device coordinates [-1, 1]
	float ndcX = (2.0f * mousePos.x) / io.DisplaySize.x - 1.0f;
	float ndcY = (2.0f * mousePos.y) / io.DisplaySize.y - 1.0f;

	// unproject near and far plane coords
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

	// test all renderable mesh components
	auto meshView = ctx.registry.view<const MeshComponent>();
	for (auto [entity, mc] : meshView.each()) {
		if (!mc.visible || !mc.mesh) continue;

		glm::mat4 worldMatrix(1.0f);
		if (const auto* tc = ctx.registry.try_get<TransformComponent>(entity)) {
			worldMatrix = tc->get_transform();
		}

		for (const auto& topNode : mc.mesh->topNodes) {
			raycast_node(topNode, worldMatrix, rayOrigin, rayDir, closestDist, hitEntity, entity);
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
