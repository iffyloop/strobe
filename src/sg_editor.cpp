#include "pch.h"

#include "sg_editor.h"
#include "sg_node.h"
#include "sg_plugins.h"

static char const* sg_node_type_to_string(sg_node_type_t const type) {
	auto const* def = sg_plugins_find_node_def_by_runtime_id(static_cast<s32>(type));
	if (def == nullptr) {
		return "Invalid";
	}
	return def->display_name.c_str();
}

static bool sg_node_can_add_child(sg_node_type_t const type) {
	auto const* def = sg_plugins_find_node_def_by_runtime_id(static_cast<s32>(type));
	return def != nullptr && def->category == sg_plugin_category_t::COMBINE;
}

static void sg_node_add_child(sg_node_t& parent, sg_node_type_t const type) {
	std::unique_ptr<sg_node_t> child = sg_node_create_from_runtime_id(static_cast<s32>(type));
	if (child == nullptr) {
		return;
	}
	parent.children.emplace_back(std::move(child));
	parent.ui.is_expanded = true;
}

static bool sg_node_is_descendant_of(sg_node_t const* node, sg_node_t const* ancestor) {
	if (!node || !ancestor) return false;
	for (auto const& child : ancestor->children) {
		if (child.get() == node) return true;
		if (sg_node_is_descendant_of(node, child.get())) return true;
	}
	return false;
}

static void sg_editor_clear_selected_if_descendant(app_t& app, sg_node_t* deleted_node) {
	if (app.selected_node && deleted_node) {
		if (app.selected_node == deleted_node || sg_node_is_descendant_of(app.selected_node, deleted_node)) {
			app.selected_node = nullptr;
		}
	}
}

static char const* sg_effect_type_to_string(sg_node_effect_type_t const type) {
	auto const* def = sg_plugins_find_effect_by_runtime_id(static_cast<s32>(type));
	if (def == nullptr) {
		return "Unknown";
	}
	return def->display_name.c_str();
}

static bool sg_node_render_tree(app_t& app, sg_node_t& parent, sg_node_t& node, bool is_root) {
	bool const is_selected = (app.selected_node == &node);
	bool did_delete = false;

	ImGui::PushID((s32)node.id);
	bool const is_leaf = node.children.empty();

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (is_leaf) flags |= ImGuiTreeNodeFlags_Leaf;
	if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;

	if (node.ui.is_expanded) {
		ImGui::SetNextItemOpen(true);
	}

	bool const is_open = ImGui::TreeNodeEx(sg_node_type_to_string(node.type), flags);

	if (!is_leaf) {
		node.ui.is_expanded = is_open;
	}

	if (ImGui::IsItemClicked()) {
		app.selected_node = &node;
	}

	if (ImGui::BeginPopupContextItem("node_context_menu")) {
		if (sg_node_can_add_child(node.type)) {
			if (ImGui::BeginMenu("Add Child")) {
				auto const node_defs = sg_plugins_list_node_defs();
				bool first_combine = true;
				for (auto const* def : node_defs) {
					if (def == nullptr) {
						continue;
					}
					if (def->category == sg_plugin_category_t::PRIMITIVE) {
					} else if (def->category == sg_plugin_category_t::COMBINE) {
						if (first_combine) {
							ImGui::Separator();
						}
						first_combine = false;
					} else {
						continue;
					}

					if (ImGui::Selectable(def->display_name.c_str())) {
						sg_node_add_child(node, static_cast<sg_node_type_t>(def->runtime_id));
					}
				}
				ImGui::EndMenu();
			}
		}
		if (!is_root) {
			if (ImGui::Selectable("Delete")) {
				sg_editor_clear_selected_if_descendant(app, &node);
				auto& parent_children = parent.children;
				auto it = std::find_if(parent_children.begin(), parent_children.end(),
					[&node](std::unique_ptr<sg_node_t> const& child) { return child.get() == &node; });
				if (it != parent_children.end()) {
					parent_children.erase(it);
					did_delete = true;
				}
			}
		}
		ImGui::EndPopup();
	}

	if (is_open) {
		for (size_t child_idx = 0; child_idx < node.children.size(); ++child_idx) {
			sg_node_t* child = node.children[child_idx].get();
			if (child == nullptr) {
				continue;
			}
			if (sg_node_render_tree(app, node, *child, false)) {
				did_delete = true;
				break;
			}
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
	return did_delete;
}

void sg_editor_init() {
}

void sg_editor_update(app_t& app) {
	f32 const window_width = 400.0f;
	f32 const half_height = ImGui::GetIO().DisplaySize.y * 0.5f;

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(window_width, half_height));
	ImGui::Begin("Scene Graph", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

	sg_node_render_tree(app, *app.sg_root, *app.sg_root, true);

	ImGui::End();

	ImGui::SetNextWindowPos(ImVec2(0, half_height));
	ImGui::SetNextWindowSize(ImVec2(window_width, half_height));
	ImGui::Begin("Properties", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

	if (app.selected_node != nullptr) {
		sg_node_t& node = *app.selected_node;
		ImGui::Text("%s", sg_node_type_to_string(node.type));
		ImGui::Separator();

		for (size_t i = 0; i < node.props.size(); ++i) {
			auto& prop = node.props[i];
			ImGui::PushID((s32)i);
			f32 value = prop->get_cur_value();
			ImGui::DragFloat(prop->get_name().c_str(), &value, prop->get_drag_speed(), prop->get_min_value(), prop->get_max_value());
			prop->set_default_value(value);
			ImGui::PopID();
		}

		for (size_t effect_idx = 0; effect_idx < node.effects.size(); ++effect_idx) {
			auto& effect = node.effects[effect_idx];
			ImGui::PushID((s32)(1000 + effect_idx));
			bool is_expanded = ImGui::CollapsingHeader(("Effect: " + std::string(sg_effect_type_to_string(effect->type))).c_str());
			if (is_expanded) {
				ImGui::Indent();
				for (size_t prop_idx = 0; prop_idx < effect->props.size(); ++prop_idx) {
					auto& prop = effect->props[prop_idx];
					ImGui::PushID((s32)prop_idx);
					f32 value = prop->get_cur_value();
					ImGui::DragFloat(prop->get_name().c_str(), &value, prop->get_drag_speed(), prop->get_min_value(), prop->get_max_value());
					prop->set_default_value(value);
					ImGui::PopID();
				}
				ImGui::Unindent();
			}
			ImGui::PopID();
		}
	} else {
		ImGui::Text("No node selected");
	}

	ImGui::End();
}
