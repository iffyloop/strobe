#pragma once

#include "pch.h"

#include "scene_invalidation.h"
#include "sg_compiler.h"
#include "sg_node.h"
#include "sg_renderer.h"

#include "fly_camera.h"

struct app_t {
	GLFWwindow* window = nullptr;

	std::unique_ptr<sg_node_t> sg_root;
	sg_node_t* selected_node = nullptr;

	sg_renderer_t sg_renderer;
	sg_compiled_scene_t sg_compiled_scene;

	fly_camera_t camera;

	bool is_fly_mode = false;
	bool escape_was_down = false;
	bool nav_keyboard_was_enabled = false;
	f64 fly_last_cursor_x = 0.0;
	f64 fly_last_cursor_y = 0.0;
	bool nfd_initialized = false;
	u64 scene_hash = 0;
	u64 plugin_generation = 0;
	bool scene_gpu_buffers_dirty = true;
	std::unordered_map<u64, scene_aabb_t> primitive_bounds_by_node_id;
};

void app_init(app_t& app);
void app_update(app_t& app, f64 const dt);
void app_destroy(app_t& app);
