#include "UIWidgets.h"

#include <resources/AssetRegistry.h>
#include <fmt/core.h>
#include <algorithm>
#include <cctype>
#include <imgui.h>


namespace UIWidgets {

	bool draw_vec3_control(
		std::string_view label,
		glm::vec3& values,
		const glm::vec3& defaultValues,
		float speed,
		bool* pLocked)
	{
		bool changed = false;
		ImGui::PushID(label.data());

		ImGui::Text("%s", label.data());
		ImGui::SameLine();

		// Reset button
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.25f, 0.6f));
		if (ImGui::SmallButton("Reset")) {
			values = defaultValues;
			changed = true;
		}
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) {

			ImGui::SetTooltip("Reset values to default");
		}

		// Uniform scale locking
		if (pLocked) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Button, *pLocked ? ImVec4(0.2f, 0.5f, 0.8f, 0.9f) : ImVec4(0.2f, 0.2f, 0.25f, 0.6f));
			if (ImGui::SmallButton(*pLocked ? "Locked" : "Unlocked")) {

				*pLocked = !(*pLocked);
			}
			ImGui::PopStyleColor();
		}

		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.16f, 0.24f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.16f, 0.22f, 0.32f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.20f, 0.26f, 0.38f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

		float totalWidth = ImGui::GetContentRegionAvail().x;
		float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
		float itemWidth = (totalWidth - (itemSpacing * 2.0f)) / 3.0f;
		float underlineHeight = 2.5f;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		glm::vec3 prevValues = values;

		const char* ids[3] = { "##X", "##Y", "##Z" };
		float* components[3] = { &values.x, &values.y, &values.z };
		const float* prevComponents[3] = { &prevValues.x, &prevValues.y, &prevValues.z };
		const ImU32 colors[3] = {
				IM_COL32(225, 45, 45, 255),   // Red (X)[cite: 1]
				IM_COL32(45, 200, 55, 255),   // Green (Y)[cite: 1]
				IM_COL32(35, 65, 240, 255)    // Blue (Z)[cite: 1]
		};

		for (int i = 0; i < 3; ++i) {
			if (i > 0) ImGui::SameLine();
			ImGui::SetNextItemWidth(itemWidth);

			if (ImGui::DragFloat(ids[i], components[i], speed, 0.0f, 0.0f, "%.3f")) {

				changed = true;
				if (pLocked && *pLocked) {

					float delta = *components[i] - *prevComponents[i];
					for (int j = 0; j < 3; ++j) {
						if (j != i) {

							*components[j] += delta;
						}
					}
				}
			}

			ImVec2 rectMin = ImGui::GetItemRectMin();
			ImVec2 rectMax = ImGui::GetItemRectMax();
			drawList->AddRectFilled(
				ImVec2(rectMin.x, rectMax.y - underlineHeight),
				rectMax,
				colors[i],
				ImGui::GetStyle().FrameRounding,
				ImDrawFlags_RoundCornersBottom
			);
		}

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		ImGui::PopID();

		return changed;
	}

	void draw_residency_badge(ResourceState state, uint64_t gpuSizeBytes)
	{
		ImVec4 color;
		std::string badgeText;

		switch (state) {
		case ResourceState::Unloaded:
			color = ImVec4(0.55f, 0.55f, 0.60f, 1.0f); // gray: disk only
			badgeText = "[Disk]";
			break;
		case ResourceState::Queued:
		case ResourceState::Loading:
			color = ImVec4(0.95f, 0.78f, 0.20f, 1.0f); // yellow: streaming
			badgeText = "[Streaming]";
			break;
		case ResourceState::Loaded:
			color = ImVec4(0.25f, 0.85f, 0.45f, 1.0f); // green: loaded on GPU
			if (gpuSizeBytes >= 1024 * 1024) {
				badgeText = fmt::format("[GPU {:.1f} MB]", static_cast<double>(gpuSizeBytes) / (1024.0 * 1024.0));
			}
			else if (gpuSizeBytes > 0) {
				badgeText = fmt::format("[GPU {:.0f} KB]", static_cast<double>(gpuSizeBytes) / 1024.0);
			}
			else {
				badgeText = "[GPU]";
			}
			break;
		case ResourceState::Failed:
		default:
			color = ImVec4(0.90f, 0.30f, 0.30f, 1.0f); // red: failed
			badgeText = "[Failed]";
			break;
		}

		ImGui::TextColored(color, "%s", badgeText.c_str());
	}

	bool draw_asset_slot(std::string_view label, UUID& assetUUID, AssetType expectedType, bool includeSubAssets)
	{
		bool changed = false;
		ImGui::PushID(label.data());

		// resolve current metadata from AssetRegistry 
		const AssetMetadata* meta = assetUUID.is_valid()
			? AssetRegistry::get().get_metadata(assetUUID)
			: nullptr;

		std::string displayName = "None (Default)";
		if (meta) {
			displayName = meta->name;
		}
		else if (assetUUID.is_valid()) {
			displayName = assetUUID.to_string().substr(0, 8) + "...";
		}

		ImGui::Text("%s", label.data());
		if (meta) {
			ImGui::SameLine();
			draw_residency_badge(meta->state, meta->gpuSizeBytes);
		}

		float availWidth = ImGui::GetContentRegionAvail().x;
		float buttonWidth = 62.0f;
		float clearWidth = assetUUID.is_valid() ? 26.0f : 0.0f;
		float slotWidth = std::max(80.0f, availWidth - buttonWidth - clearWidth - 8.0f);

		// clickable / droppable slot button
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.17f, 0.22f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.25f, 0.34f, 1.0f));
		std::string slotButtonText = fmt::format("{} ({})##SlotBtn", displayName, AssetUtils::asset_type_to_string(expectedType));
		if (ImGui::Button(slotButtonText.c_str(), ImVec2(slotWidth, 0.0f))) {
			ImGui::OpenPopup("AssetPickerPopup");
		}
		ImGui::PopStyleColor(2);

		// tooltip with full UUID & virtual path
		if (ImGui::IsItemHovered() && meta) {
			ImGui::BeginTooltip();
			ImGui::Text("Name: %s", meta->name.c_str());
			ImGui::Text("UUID: %s", meta->uuid.to_string().c_str());
			ImGui::Text("Path: %s", meta->virtualPath.generic_string().c_str());
			ImGui::EndTooltip();
		}

		// accept drag and drop UUID from ContentBrowserPanel
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_ASSET_UUID)) {
				if (payload->DataSize == sizeof(UUID)) {
					UUID droppedUUID = *static_cast<const UUID*>(payload->Data);
					const AssetMetadata* droppedMeta = AssetRegistry::get().get_metadata(droppedUUID);
					if (droppedMeta && (droppedMeta->type == expectedType ||
						(expectedType == AssetType::Mesh && droppedMeta->type == AssetType::Model))) {
						assetUUID = droppedUUID;
						changed = true;
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		// searchable "Select..." popup button
		ImGui::SameLine();
		if (ImGui::Button("Select...", ImVec2(buttonWidth, 0.0f))) {
			ImGui::OpenPopup("AssetPickerPopup");
		}

		// "Clear" button (resets to UUID::null())
		if (assetUUID.is_valid()) {
			ImGui::SameLine();
			if (ImGui::Button("X", ImVec2(clearWidth, 0.0f))) {
				assetUUID = UUID::null();
				changed = true;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Clear asset slot");
			}
		}

		// searchable asset picker modal/popup
		if (ImGui::BeginPopup("AssetPickerPopup")) {
			static char searchFilter[128] = "";
			ImGui::InputTextWithHint("##AssetSearch", "Search assets...", searchFilter, sizeof(searchFilter));
			ImGui::Separator();

			if (ImGui::Selectable("None (Clear Slot)", !assetUUID.is_valid())) {
				assetUUID = UUID::null();
				changed = true;
				ImGui::CloseCurrentPopup();
			}

			auto candidates = AssetRegistry::get().get_assets_by_type(expectedType, includeSubAssets);
			std::string filterLower = searchFilter;
			std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
				});

			ImGui::BeginChild("AssetCandidateList", ImVec2(340.0f, 240.0f), true);
			for (const auto& candidate : candidates) {
				if (!filterLower.empty()) {
					std::string nameLower = candidate.name;
					std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c) {
						return static_cast<char>(std::tolower(c));
						});
					if (nameLower.find(filterLower) == std::string::npos) {
						continue;
					}
				}

				ImGui::PushID(static_cast<int>(candidate.uuid.to_u64()));
				bool isSelected = (candidate.uuid == assetUUID);
				if (ImGui::Selectable(candidate.name.c_str(), isSelected)) {
					assetUUID = candidate.uuid;
					changed = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine(230.0f);
				draw_residency_badge(candidate.state, candidate.gpuSizeBytes);
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		}

		ImGui::PopID();
		return changed;
	}
}
