#include "pch.h"

#include "app.h"
#include "sg_editor.h"
#include "sg_plugins.h"

#include <nfd.h>

namespace {

sg_node_prop_t* app_find_prop(sg_node_props_map_t& props, char const* name) {
	for (auto& prop : props) {
		if (prop != nullptr && prop->get_name() == name) {
			return prop.get();
		}
	}
	return nullptr;
}

sg_node_prop_t* app_find_effect_prop(sg_node_t& node, char const* name) {
	for (auto& effect : node.effects) {
		if (effect == nullptr) {
			continue;
		}
		sg_node_prop_t* prop = app_find_prop(effect->props, name);
		if (prop != nullptr) {
			return prop;
		}
	}
	return nullptr;
}

void app_add_random_primitives(sg_node_t& root, s32 count) {
	std::mt19937 rng(std::random_device{}());
	std::uniform_real_distribution<f32> pos_dist(-5.0f, 5.0f);
	std::uniform_real_distribution<f32> cube_size_dist(0.3f, 1.2f);
	std::uniform_real_distribution<f32> sphere_radius_dist(0.2f, 0.8f);
	std::uniform_int_distribution<s32> type_dist(0, 1);

	for (s32 i = 0; i < count; ++i) {
		std::unique_ptr<sg_node_t> node = type_dist(rng) == 0 ? sg_node_cube_create() : sg_node_sphere_create();
		if (node == nullptr) {
			continue;
		}

		if (node->type == sg_node_type_t::CUBE) {
			if (sg_node_prop_t* size_x = app_find_prop(node->props, "size_x")) {
				size_x->set_default_value(cube_size_dist(rng));
			}
			if (sg_node_prop_t* size_y = app_find_prop(node->props, "size_y")) {
				size_y->set_default_value(cube_size_dist(rng));
			}
			if (sg_node_prop_t* size_z = app_find_prop(node->props, "size_z")) {
				size_z->set_default_value(cube_size_dist(rng));
			}
		} else if (node->type == sg_node_type_t::SPHERE) {
			if (sg_node_prop_t* radius = app_find_prop(node->props, "radius")) {
				radius->set_default_value(sphere_radius_dist(rng));
			}
		}

		if (sg_node_prop_t* pos_x = app_find_effect_prop(*node, "pos_x")) {
			pos_x->set_default_value(pos_dist(rng));
		}
		if (sg_node_prop_t* pos_y = app_find_effect_prop(*node, "pos_y")) {
			pos_y->set_default_value(pos_dist(rng));
		}
		if (sg_node_prop_t* pos_z = app_find_effect_prop(*node, "pos_z")) {
			pos_z->set_default_value(pos_dist(rng));
		}

		root.children.emplace_back(std::move(node));
	}
}

void app_set_fly_mode(app_t& app, bool enabled) {
	if (app.is_fly_mode == enabled) {
		return;
	}
	assert_release(app.window != nullptr);

	ImGuiIO& io = ImGui::GetIO();
	if (enabled) {
		app.nav_keyboard_was_enabled = (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0;
		io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouse;

		glfwSetInputMode(app.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		glfwGetCursorPos(app.window, &app.fly_last_cursor_x, &app.fly_last_cursor_y);
		fly_camera_keys_reset(app.camera);
	} else {
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		if (app.nav_keyboard_was_enabled) {
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		}

		glfwSetInputMode(app.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		fly_camera_keys_reset(app.camera);
	}

	app.escape_was_down = false;
	app.is_fly_mode = enabled;
}

void app_update_fly_mode_input(app_t& app) {
	assert_release(app.window != nullptr);

	bool escape_down = glfwGetKey(app.window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
	if (app.is_fly_mode && escape_down && !app.escape_was_down) {
		app_set_fly_mode(app, false);
		escape_down = false;
	}
	app.escape_was_down = escape_down;

	if (!app.is_fly_mode) {
		return;
	}

	fly_camera_key(app.camera, GLFW_KEY_W, glfwGetKey(app.window, GLFW_KEY_W) == GLFW_PRESS);
	fly_camera_key(app.camera, GLFW_KEY_A, glfwGetKey(app.window, GLFW_KEY_A) == GLFW_PRESS);
	fly_camera_key(app.camera, GLFW_KEY_S, glfwGetKey(app.window, GLFW_KEY_S) == GLFW_PRESS);
	fly_camera_key(app.camera, GLFW_KEY_D, glfwGetKey(app.window, GLFW_KEY_D) == GLFW_PRESS);
	fly_camera_key(app.camera, GLFW_KEY_LEFT_SHIFT,
		glfwGetKey(app.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
			glfwGetKey(app.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

	f64 cursor_x = 0.0;
	f64 cursor_y = 0.0;
	glfwGetCursorPos(app.window, &cursor_x, &cursor_y);
	f32 const delta_x = static_cast<f32>(cursor_x - app.fly_last_cursor_x);
	f32 const delta_y = static_cast<f32>(cursor_y - app.fly_last_cursor_y);
	app.fly_last_cursor_x = cursor_x;
	app.fly_last_cursor_y = cursor_y;
	fly_camera_mouse_update(app.camera, delta_x, delta_y);
}

} // namespace

void app_init(app_t& app) {
	app.nfd_initialized = NFD_Init() == NFD_OKAY;
	if (!app.nfd_initialized) {
		std::cerr << "[app] native file dialog init failed: " << NFD_GetError() << std::endl;
	}

	sg_plugins_init_or_die();
	app.sg_root = sg_node_union_create();
	assert_release(app.sg_root != nullptr);
	app_add_random_primitives(*app.sg_root, 100);

	app.camera.pos.z = 4.0f;

	sg_editor_init();

	sg_renderer_init(app.sg_renderer);
	sg_renderer_mark_all_chunks_dirty(app.sg_renderer);
	app.plugin_generation = sg_plugins_get().generation;
}

void app_update(app_t& app, f64 const dt) {
	sg_editor_update(app);

	app_update_fly_mode_input(app);

	fly_camera_update(app.camera, dt,
		(f32)app.sg_renderer.primary_render_pass.width / (f32)app.sg_renderer.primary_render_pass.height,
		glm::radians(70.0f), 0.1f, app.sg_renderer.marching_cubes_bounds_extent * 4.0f);

	sg_node_update(*app.sg_root.get(), dt);

	u64 const plugin_generation = sg_plugins_get().generation;
	u64 const scene_hash = scene_hash_tree(*app.sg_root.get());
	bool const scene_changed = scene_hash != app.scene_hash || plugin_generation != app.plugin_generation;
	if (scene_changed) {
		auto updated_bounds = scene_collect_primitive_bounds(*app.sg_root.get());
		if (app.scene_hash == 0 || plugin_generation != app.plugin_generation) {
			sg_renderer_mark_all_chunks_dirty(app.sg_renderer);
		} else if (!scene_bounds_equal(app.primitive_bounds_by_node_id, updated_bounds)) {
			scene_mark_primitive_overlap_dirty(
				app.sg_renderer, app.primitive_bounds_by_node_id, updated_bounds);
		} else {
			scene_mark_bounds_set_dirty(app.sg_renderer, updated_bounds);
		}
		app.primitive_bounds_by_node_id = std::move(updated_bounds);
		app.scene_hash = scene_hash;
		app.plugin_generation = plugin_generation;
		sg_compile(app.sg_compiled_scene, *app.sg_root);
		app.scene_gpu_buffers_dirty = true;
	} else {
		app.scene_gpu_buffers_dirty = false;
	}

	sg_renderer_update(app.sg_renderer, app.sg_compiled_scene, app.camera, app.scene_gpu_buffers_dirty);

	bool const wants_fly_mode = sg_renderer_update_imgui(app.sg_renderer, !app.is_fly_mode);
	if (!app.is_fly_mode && wants_fly_mode) {
		app_set_fly_mode(app, true);
	}
}

void app_destroy(app_t& app) {
	sg_renderer_destroy(app.sg_renderer);
	if (app.nfd_initialized) {
		NFD_Quit();
		app.nfd_initialized = false;
	}
}
