#pragma once

#include "pch.h"

enum class sg_plugin_category_t {
	INVALID = 0,
	COMBINE,
	PRIMITIVE,
	EFFECT,
};

struct sg_plugin_param_t {
	std::string name;
	f32 default_value = 0.0f;
	f32 min_value = -std::numeric_limits<f32>::infinity();
	f32 max_value = std::numeric_limits<f32>::infinity();
	f32 drag_speed = 0.01f;
};

struct sg_plugin_def_t {
	s32 runtime_id = 0;
	std::string id;
	std::string display_name;
	sg_plugin_category_t category = sg_plugin_category_t::INVALID;
	std::string function_name;
	std::string glsl_path;
	std::string glsl_source;
	std::vector<sg_plugin_param_t> params;
	bool default_on_nodes = false;
};

struct sg_plugins_t {
	std::vector<sg_plugin_def_t> combine_defs;
	std::vector<sg_plugin_def_t> primitive_defs;
	std::vector<sg_plugin_def_t> effect_defs;

	std::unordered_map<std::string, s32> combine_id_to_runtime_id;
	std::unordered_map<std::string, s32> primitive_id_to_runtime_id;
	std::unordered_map<std::string, s32> effect_id_to_runtime_id;

	u64 generation = 0;
};

void sg_plugins_init_or_die();

sg_plugins_t const& sg_plugins_get();
void sg_plugins_commit(sg_plugins_t&& plugins);
bool sg_plugins_load_candidate(sg_plugins_t& out_plugins, std::string& out_error);
bool sg_plugins_build_marching_cubes_compute_source(
	sg_plugins_t const& plugins, std::string& out_src, std::string& out_error);

sg_plugin_def_t const* sg_plugins_find_combine_by_runtime_id(s32 runtime_id);
sg_plugin_def_t const* sg_plugins_find_primitive_by_runtime_id(s32 runtime_id);
sg_plugin_def_t const* sg_plugins_find_effect_by_runtime_id(s32 runtime_id);

sg_plugin_def_t const* sg_plugins_find_combine_by_id(std::string const& id);
sg_plugin_def_t const* sg_plugins_find_primitive_by_id(std::string const& id);
sg_plugin_def_t const* sg_plugins_find_effect_by_id(std::string const& id);

sg_plugin_def_t const* sg_plugins_find_node_def_by_runtime_id(s32 runtime_id);
sg_plugin_def_t const* sg_plugins_find_node_def_by_id(std::string const& id);

std::vector<sg_plugin_def_t const*> sg_plugins_list_node_defs();
std::vector<sg_plugin_def_t const*> sg_plugins_list_effect_defs();
