#pragma once

#include "pch.h"

#include "sg_compiler.h"

#include "fly_camera.h"
#include "gl_render_pass.h"
#include "gl_shader.h"
#include "gl_vertex_buffers.h"

struct sg_renderer_buffer_texture_t {
	GLuint buffer = 0;
	GLuint texture = 0;
};

struct sg_renderer_t {
	gl_vertex_buffers_t quad_vbo;
	gl_render_pass_t primary_render_pass;

	sg_renderer_buffer_texture_t program;
	sg_renderer_buffer_texture_t primitive_meta;
	sg_renderer_buffer_texture_t primitive_params;
	sg_renderer_buffer_texture_t primitive_scale;
	sg_renderer_buffer_texture_t primitive_effect_ranges;
	sg_renderer_buffer_texture_t effect_meta;
	sg_renderer_buffer_texture_t effect_params;
	sg_renderer_buffer_texture_t combine_params;

	std::string plugin_reload_status;

	s32 preview_max_steps = 64;
	f32 preview_surface_epsilon = 0.001f;
	f32 preview_max_trace_dist = 200.0f;
	GLuint checker_texture = 0;
};

void sg_renderer_init(sg_renderer_t& renderer);
void sg_renderer_destroy(sg_renderer_t& renderer);
void sg_renderer_update(sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene, fly_camera_t const& camera);
bool sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled);
