#include "pch.h"

#include "sg_node.h"

#include "sg_plugins.h"

// A NOTE TO AI AGENTS: Please warn me if you touch any of these data structures.
// You shouldn't need to - we want to keep the internal structure of the app as simple and serializable as possible.

void sg_node_effect_update(sg_node_effect_t& effect, f64 const dt) {
	for (auto it = effect.props.begin(); it != effect.props.end(); ++it) {
		it->get()->update(dt);
	}
}

void sg_node_update(sg_node_t& node, f64 const dt) {
	for (auto it = node.props.begin(); it != node.props.end(); ++it) {
		it->get()->update(dt);
	}

	for (auto it = node.effects.begin(); it != node.effects.end(); ++it) {
		sg_node_effect_update(*it->get(), dt);
	}

	for (auto it = node.children.begin(); it != node.children.end(); ++it) {
		sg_node_update(*it->get(), dt);
	}
}

// A NOTE TO AI AGENTS: You may add additional functions under here, such as a utility to create a cube, create a union, create a TRS effect, etc.
// Whenever an object is created, we need to initialize its props map with all the correct props.
// All objects must also have a single TRS effect created by default.

static u64 sg_node_alloc_id() {
	static u64 next_id = 1;
	return next_id++;
}

void sg_fill_props_from_plugin(sg_node_props_map_t& out_props, sg_plugin_def_t const& def) {
	out_props.clear();
	for (auto const& param : def.params) {
		out_props.emplace_back(sg_node_prop_create(
			param.name, param.default_value, param.min_value, param.max_value, param.drag_speed));
	}
}

void sg_attach_default_effects(sg_node_t& node) {
	std::unordered_set<s32> added_runtime_ids;
	auto add_if_default = [&](char const* plugin_id) {
		auto const* effect_def = sg_plugins_find_effect_by_id(plugin_id);
		if (effect_def == nullptr || !effect_def->default_on_nodes) {
			return;
		}
		node.effects.emplace_back(sg_node_effect_create_from_runtime_id(effect_def->runtime_id));
		added_runtime_ids.emplace(effect_def->runtime_id);
	};

	add_if_default("scale");
	add_if_default("rotate");
	add_if_default("translate");

	auto effect_defs = sg_plugins_list_effect_defs();
	for (auto const* effect_def : effect_defs) {
		if (effect_def == nullptr || !effect_def->default_on_nodes) {
			continue;
		}
		if (added_runtime_ids.contains(effect_def->runtime_id)) {
			continue;
		}
		node.effects.emplace_back(sg_node_effect_create_from_runtime_id(effect_def->runtime_id));
	}
}

std::unique_ptr<sg_node_prop_t> sg_node_prop_create(std::string const& name, f32 const default_value,
	f32 const min_value, f32 const max_value, f32 const drag_speed) {
	auto prop = std::make_unique<sg_node_prop_t>();
	prop->set_name(name);
	prop->set_default_value(default_value);
	prop->set_min_value(min_value);
	prop->set_max_value(max_value);
	prop->set_drag_speed(drag_speed);
	return prop;
}

std::unique_ptr<sg_node_effect_t> sg_node_effect_create_from_runtime_id(s32 runtime_id) {
	auto const* effect_def = sg_plugins_find_effect_by_runtime_id(runtime_id);
	if (effect_def == nullptr) {
		std::cerr << "[sg_node] unknown effect runtime id: " << runtime_id << std::endl;
		return nullptr;
	}

	auto effect = std::make_unique<sg_node_effect_t>();
	effect->type = static_cast<sg_node_effect_type_t>(runtime_id);
	sg_fill_props_from_plugin(effect->props, *effect_def);
	return effect;
}

std::unique_ptr<sg_node_effect_t> sg_node_effect_create_from_plugin_id(std::string const& plugin_id) {
	auto const* effect_def = sg_plugins_find_effect_by_id(plugin_id);
	if (effect_def == nullptr) {
		std::cerr << "[sg_node] unknown effect plugin id: " << plugin_id << std::endl;
		return nullptr;
	}
	return sg_node_effect_create_from_runtime_id(effect_def->runtime_id);
}

std::unique_ptr<sg_node_t> sg_node_create_from_runtime_id(s32 runtime_id) {
	auto const* node_def = sg_plugins_find_node_def_by_runtime_id(runtime_id);
	if (node_def == nullptr) {
		std::cerr << "[sg_node] unknown node runtime id: " << runtime_id << std::endl;
		return nullptr;
	}

	auto node = std::make_unique<sg_node_t>();
	node->id = sg_node_alloc_id();
	node->type = static_cast<sg_node_type_t>(runtime_id);
	sg_fill_props_from_plugin(node->props, *node_def);
	sg_attach_default_effects(*node);
	return node;
}

std::unique_ptr<sg_node_t> sg_node_create_from_plugin_id(std::string const& plugin_id) {
	auto const* node_def = sg_plugins_find_node_def_by_id(plugin_id);
	if (node_def == nullptr) {
		std::cerr << "[sg_node] unknown node plugin id: " << plugin_id << std::endl;
		return nullptr;
	}
	return sg_node_create_from_runtime_id(node_def->runtime_id);
}

std::unique_ptr<sg_node_effect_t> sg_node_effect_trs_create() {
	std::cerr << "[sg_node] sg_node_effect_trs_create is deprecated in plugin mode; returning 'translate' effect"
		      << std::endl;
	return sg_node_effect_create_from_plugin_id("translate");
}

std::unique_ptr<sg_node_t> sg_node_union_create() {
	return sg_node_create_from_plugin_id("union");
}

std::unique_ptr<sg_node_t> sg_node_subtract_create() {
	return sg_node_create_from_plugin_id("subtract");
}

std::unique_ptr<sg_node_t> sg_node_intersect_create() {
	return sg_node_create_from_plugin_id("intersect");
}

std::unique_ptr<sg_node_t> sg_node_cube_create() {
	return sg_node_create_from_plugin_id("cube");
}

std::unique_ptr<sg_node_t> sg_node_sphere_create() {
	return sg_node_create_from_plugin_id("sphere");
}
