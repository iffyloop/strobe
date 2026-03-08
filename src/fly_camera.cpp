#include "pch.h"

#include "fly_camera.h"

void fly_camera_mouse_update(fly_camera_t& camera, f32 const mouse_x, f32 const mouse_y) {
	static f32 constexpr const pi = std::numbers::pi_v<f32>;

	camera.angle_y += static_cast<f32>(mouse_x) * 0.005f;
	while (camera.angle_y < 0.0f) {
		camera.angle_y += pi * 2.0f;
	}
	while (camera.angle_y >= pi * 2.0f) {
		camera.angle_y -= pi * 2.0f;
	}

	camera.angle_x += static_cast<f32>(mouse_y) * 0.005f;
	if (camera.angle_x < -pi * 0.5f) {
		camera.angle_x = -pi * 0.5f;
	}
	if (camera.angle_x > pi * 0.5f) {
		camera.angle_x = pi * 0.5f;
	}
}

void fly_camera_key(fly_camera_t& camera, s32 const key, bool const down) {
	switch (key) {
	case GLFW_KEY_W:
		camera.key_w = down;
		break;
	case GLFW_KEY_S:
		camera.key_s = down;
		break;
	case GLFW_KEY_A:
		camera.key_a = down;
		break;
	case GLFW_KEY_D:
		camera.key_d = down;
		break;
	case GLFW_KEY_LEFT_SHIFT:
	case GLFW_KEY_RIGHT_SHIFT:
		camera.key_shift = down;
		break;
	default:
		break;
	}
}

void fly_camera_keys_reset(fly_camera_t& camera) {
	camera.key_w = false;
	camera.key_s = false;
	camera.key_a = false;
	camera.key_d = false;
	camera.key_shift = false;
}

void fly_camera_update(
	fly_camera_t& camera, f64 const dt, f32 const aspect, f32 const fov_y, f32 const z_near, f32 const z_far) {
	glm::vec3 dir = glm::vec3(0.0f, 0.0f, 0.0f);
	if (camera.key_w) {
		dir.z -= 1.0f;
	}
	if (camera.key_s) {
		dir.z += 1.0f;
	}
	if (camera.key_a) {
		dir.x -= 1.0f;
	}
	if (camera.key_d) {
		dir.x += 1.0f;
	}
	dir *= static_cast<f32>(dt) * (camera.key_shift ? 10.0f : 5.0f);

	camera.view_mat = glm::mat4(1.0f);
	camera.view_mat = glm::rotate(camera.view_mat, camera.angle_x, glm::vec3(1.0f, 0.0f, 0.0f));
	camera.view_mat = glm::rotate(camera.view_mat, camera.angle_y, glm::vec3(0.0f, 1.0f, 0.0f));

	glm::mat4 inv_view_mtx = glm::inverse(camera.view_mat);
	dir = inv_view_mtx * glm::vec4(dir, 1.0f);
	camera.pos += dir;

	camera.view_mat = glm::translate(camera.view_mat, -camera.pos);

	camera.proj_mat = glm::perspective(fov_y, aspect, z_near, z_far);
}
