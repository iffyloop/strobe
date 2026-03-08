#include "pch.h"

#include "app.h"
#include "sg_editor.h"
#include "sg_plugins.h"

#include <nfd.h>

namespace {

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

	app.camera.pos.z = 4.0f;

	sg_editor_init();

	sg_renderer_init(app.sg_renderer);
}

void app_update(app_t& app, f64 const dt) {
	sg_editor_update(app);

	app_update_fly_mode_input(app);

	fly_camera_update(app.camera, dt,
		(f32)app.sg_renderer.primary_render_pass.width / (f32)app.sg_renderer.primary_render_pass.height,
		glm::radians(70.0f), 0.1f, app.sg_renderer.preview_max_trace_dist);

	sg_node_update(*app.sg_root.get(), dt);
	sg_compile(app.sg_compiled_scene, *app.sg_root);

	sg_renderer_update(app.sg_renderer, app.sg_compiled_scene, app.camera);

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
