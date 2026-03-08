#pragma once

#include "pch.h"

#include "sg_compiler.h"

#include "fly_camera.h"
#include "diffusion_renderer.h"
#include "gl_render_pass.h"
#include "gl_shader.h"
#include "gl_vertex_buffers.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <array>

struct sg_renderer_buffer_texture_t {
	GLuint buffer = 0;
	GLuint texture = 0;
};

enum class sg_diffusion_transition_t {
	None = 0,
	Loading,
	Unloading,
};

enum class sg_diffusion_resolution_t {
	R512x512 = 0,
	R1024x1024,
	R1344x768,
};

enum class sg_diffusion_model_t {
	SDXL = 0,
	SD15,
};

struct sg_renderer_t {
	gl_vertex_buffers_t quad_vbo;
	gl_render_pass_t primary_render_pass;
	gl_render_pass_t depth_normalize_render_pass;
	gl_shader_t depth_reduce_shader;

	GLuint depth_reduce_texture_a = 0;
	GLuint depth_reduce_texture_b = 0;
	GLuint depth_reduce_fbo = 0;

	sg_renderer_buffer_texture_t program;
	sg_renderer_buffer_texture_t primitive_meta;
	sg_renderer_buffer_texture_t primitive_params;
	sg_renderer_buffer_texture_t primitive_scale;
	sg_renderer_buffer_texture_t primitive_effect_ranges;
	sg_renderer_buffer_texture_t effect_meta;
	sg_renderer_buffer_texture_t effect_params;
	sg_renderer_buffer_texture_t combine_params;

	std::string plugin_reload_status;

	bool preview_depth_invert = true;
	f32 preview_depth_span_epsilon = 0.001f;
	s32 preview_max_steps = 64;
	f32 preview_surface_epsilon = 0.001f;
	f32 preview_max_trace_dist = 200.0f;

	bool diffusion_enabled = false;
	bool diffusion_show_output = true;
	bool diffusion_worker_stopping = false;
	sg_diffusion_transition_t diffusion_transition = sg_diffusion_transition_t::None;
	bool diffusion_start_requested = false;
	std::string diffusion_prompt = "highly detailed cinematic render of smooth geometric sculpture";
	std::string diffusion_negative_prompt = "";
	f32 diffusion_control_strength = 0.5f;
	s64 diffusion_seed = 1;
	sg_diffusion_model_t diffusion_model = sg_diffusion_model_t::SDXL;
	sg_diffusion_resolution_t diffusion_resolution = sg_diffusion_resolution_t::R1024x1024;
	bool diffusion_prompt_buffers_initialized = false;
	std::array<char, 512> diffusion_prompt_buffer = {};
	std::array<char, 512> diffusion_negative_prompt_buffer = {};

	std::thread diffusion_worker;
	std::mutex diffusion_mutex;
	std::condition_variable diffusion_cv;
	bool diffusion_worker_running = false;
	bool diffusion_ready = false;
	bool diffusion_stop_requested = false;
	bool diffusion_busy = false;
	bool diffusion_failed = false;
	bool diffusion_has_pending_control = false;
	std::vector<f32> diffusion_pending_control_depth;
	s32 diffusion_pending_control_width = 0;
	s32 diffusion_pending_control_height = 0;
	std::chrono::steady_clock::time_point diffusion_last_submit_time = std::chrono::steady_clock::time_point::min();
	std::chrono::milliseconds diffusion_submit_cooldown = std::chrono::milliseconds(33);

	bool diffusion_has_output = false;
	std::vector<u8> diffusion_output_pixels;
	s32 diffusion_output_width = 0;
	s32 diffusion_output_height = 0;
	s32 diffusion_output_channels = 0;

	GLuint diffusion_output_texture = 0;
	s32 diffusion_output_texture_width = 0;
	s32 diffusion_output_texture_height = 0;
	s32 diffusion_output_texture_channels = 0;
	bool diffusion_output_texture_ready = false;
};

void sg_renderer_init(sg_renderer_t& renderer);
void sg_renderer_destroy(sg_renderer_t& renderer);
void sg_renderer_update(sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene, fly_camera_t const& camera);
bool sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled);
