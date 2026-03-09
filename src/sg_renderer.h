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

struct sg_renderer_image_texture_t {
	GLuint texture = 0;
	std::string path;
};

struct sg_renderer_t {
	gl_vertex_buffers_t quad_vbo;
	gl_render_pass_t primary_render_pass;
	GLuint marching_cubes_program = 0;
	GLuint marching_cubes_vao = 0;
	GLuint marching_cubes_vertex_ssbo = 0;
	GLuint marching_cubes_counter_ssbo = 0;
	u32 marching_cubes_vertex_capacity = 0;
	u32 marching_cubes_vertex_count = 0;

	sg_renderer_buffer_texture_t program;
	sg_renderer_buffer_texture_t primitive_meta;
	sg_renderer_buffer_texture_t primitive_params;
	sg_renderer_buffer_texture_t primitive_scale;
	sg_renderer_buffer_texture_t primitive_effect_ranges;
	sg_renderer_buffer_texture_t effect_meta;
	sg_renderer_buffer_texture_t effect_params;
	sg_renderer_buffer_texture_t combine_params;
	sg_renderer_buffer_texture_t marching_cubes_edge_table;
	sg_renderer_buffer_texture_t marching_cubes_tri_table;

	std::string plugin_reload_status;

	s32 marching_cubes_grid_resolution = 40;
	f32 marching_cubes_iso_level = 0.0f;
	f32 marching_cubes_bounds_extent = 3.0f;
	bool marching_cubes_smooth_normals = true;
	GLuint checker_texture = 0;

	std::vector<sg_renderer_image_texture_t> primitive_textures;
	std::unordered_map<std::string, s32> primitive_texture_ids_by_path;
};

void sg_renderer_init(sg_renderer_t& renderer);
void sg_renderer_destroy(sg_renderer_t& renderer);
void sg_renderer_update(sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene, fly_camera_t const& camera);
bool sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled);

s32 sg_renderer_get_or_load_primitive_texture(sg_renderer_t& renderer, std::string const& path);
std::string const* sg_renderer_get_primitive_texture_path(sg_renderer_t const& renderer, s32 texture_id);
