#pragma once

#include "pch.h"

struct fly_camera_t {
	glm::mat4 view_mat = glm::mat4(1.0f);
	glm::mat4 proj_mat = glm::mat4(1.0f);

	glm::vec3 pos = glm::vec3(0.0f);
	f32 angle_x = 0.0f, angle_y = 0.0f;

	bool key_w = false, key_a = false, key_s = false, key_d = false, key_shift = false;
};

void fly_camera_mouse_update(fly_camera_t& camera, f32 const mouse_x, f32 const mouse_y);
void fly_camera_key(fly_camera_t& camera, s32 const key, bool const down);
void fly_camera_keys_reset(fly_camera_t& camera);
void fly_camera_update(
	fly_camera_t& camera, f64 const dt, f32 const aspect, f32 const fov_y, f32 const z_near, f32 const z_far);
