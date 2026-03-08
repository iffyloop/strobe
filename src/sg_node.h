#pragma once

#include "pch.h"

// A NOTE TO AI AGENTS: Please warn me if you touch any of these data structures.
// You shouldn't need to - we want to keep the internal structure of the app as simple and serializable as possible.

struct sg_node_prop_t {
	void set_name(std::string const& name) {
		m_name = name;
	}

	void set_default_value(f32 const value) {
		m_default_value = value;
	}

	void set_min_value(f32 const value) {
		m_min_value = value;
	}

	void set_max_value(f32 const value) {
		m_max_value = value;
	}

	void set_drag_speed(f32 const speed) {
		m_drag_speed = speed;
	}

	f32 get_cur_value() {
		return m_cur_value;
	}

	f32 get_min_value() {
		return m_min_value;
	}

	f32 get_max_value() {
		return m_max_value;
	}

	f32 get_drag_speed() {
		return m_drag_speed;
	}

	std::string const& get_name() const {
		return m_name;
	}

	bool get_is_controlled() {
		return false;
	}

	void update(f64 const dt) {
		UNUSED(dt);
		m_cur_value = m_default_value;
	}

private:
	std::string m_name;

	f32 m_default_value = 0.0f;
	f32 m_min_value = -std::numeric_limits<f32>::infinity();
	f32 m_max_value = std::numeric_limits<f32>::infinity();
	f32 m_drag_speed = 0.01f;

	f32 m_cur_value = 0.0f;
};

// A props map stores all arguments to an object or transformation, such as XYZ size (cube), radius (sphere), px/py/pz/rx/ry/rz/sx/sy/sz for TRS, etc.
typedef std::vector<std::unique_ptr<sg_node_prop_t>> sg_node_props_map_t;

enum sg_node_effect_type_t {
	INVALID = 0,
	TRS // Translate + rotate + scale
};

struct sg_node_effect_t {
	sg_node_effect_type_t type = sg_node_effect_type_t::INVALID;
	sg_node_props_map_t props;
};

typedef std::vector<std::unique_ptr<sg_node_effect_t>> sg_node_effects_list_t;

enum class sg_node_type_t { INVALID = 0, UNION, SUBTRACT, INTERSECT, CUBE, SPHERE };

struct sg_node_t;
typedef std::vector<std::unique_ptr<sg_node_t>> sg_node_list_t;

struct sg_node_ui_t {
	// Only IMGUI-related state should appear here
	bool is_expanded = true;
};

struct sg_node_t {
	// IMGUI-related state should be limited to the .ui property, and should not be placed anywhere in the core sg_node_t type
	u64 id = 0;
	sg_node_type_t type = sg_node_type_t::INVALID;
	sg_node_props_map_t props;
	sg_node_effects_list_t effects;
	sg_node_list_t children;
	sg_node_ui_t ui;
};

void sg_node_effect_update(sg_node_effect_t& effect, f64 const dt);
void sg_node_update(sg_node_t& node, f64 const dt);

// A NOTE TO AI AGENTS: You may add additional functions under here, such as a utility to create a cube, create a union, create a TRS effect, etc.
// Whenever an object is created, we need to initialize its props map with all the correct props.
// All objects must also have a single TRS effect created by default.

std::unique_ptr<sg_node_prop_t> sg_node_prop_create(std::string const& name, f32 const default_value = 0.0f,
	f32 const min_value = -std::numeric_limits<f32>::infinity(),
	f32 const max_value = std::numeric_limits<f32>::infinity(), f32 const drag_speed = 0.01f);
std::unique_ptr<sg_node_t> sg_node_create_from_plugin_id(std::string const& plugin_id);
std::unique_ptr<sg_node_t> sg_node_create_from_runtime_id(s32 runtime_id);
std::unique_ptr<sg_node_effect_t> sg_node_effect_create_from_plugin_id(std::string const& plugin_id);
std::unique_ptr<sg_node_effect_t> sg_node_effect_create_from_runtime_id(s32 runtime_id);
std::unique_ptr<sg_node_effect_t> sg_node_effect_trs_create();
std::unique_ptr<sg_node_t> sg_node_union_create();
std::unique_ptr<sg_node_t> sg_node_subtract_create();
std::unique_ptr<sg_node_t> sg_node_intersect_create();
std::unique_ptr<sg_node_t> sg_node_cube_create();
std::unique_ptr<sg_node_t> sg_node_sphere_create();
