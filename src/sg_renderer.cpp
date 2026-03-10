#include "pch.h"

#include "sg_renderer.h"

#include "marching_cubes_tables.h"
#include "sg_plugins.h"

namespace {

constexpr s32 k_max_texture_slots = 16;
constexpr GLenum k_texture_unit_base = GL_TEXTURE8;
constexpr u32 k_marching_cubes_max_tris_per_cell = 5;
constexpr u32 k_marching_cubes_threads_per_axis = 4;
constexpr u32 k_marching_cubes_vertex_stride_bytes = sizeof(glm::vec4) * 2;
constexpr u32 k_indirect_build_threads = 64;

struct draw_arrays_indirect_command_t {
	u32 count = 0;
	u32 instance_count = 1;
	u32 first = 0;
	u32 base_instance = 0;
};

char const* k_marching_cubes_indirect_build_comp = R"(#version 430 core
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

uniform uint u_min_chunk;
uniform uint u_max_chunk;
uniform uint u_chunk_vertex_capacity;

struct DrawArraysIndirectCommand {
    uint count;
    uint instanceCount;
    uint first;
    uint baseInstance;
};

layout(std430, binding = 1) readonly buffer CounterBuffer {
    uint chunk_vertex_counts[];
};

layout(std430, binding = 2) writeonly buffer IndirectBuffer {
    DrawArraysIndirectCommand commands[];
};

void main() {
    uint idx = u_min_chunk + gl_GlobalInvocationID.x;
    if (idx > u_max_chunk) {
        return;
    }

    commands[idx].count = chunk_vertex_counts[idx];
    commands[idx].instanceCount = 1u;
    commands[idx].first = idx * u_chunk_vertex_capacity;
    commands[idx].baseInstance = 0u;
}
)";

struct sg_renderer_chunk_layout_t {
	u32 cells_per_axis = 0;
	u32 chunk_resolution = 0;
	u32 chunks_per_axis = 0;
	u32 num_chunks = 0;
	u32 chunk_vertex_capacity = 0;
	u32 total_vertex_capacity = 0;
};

sg_renderer_chunk_layout_t sg_renderer_compute_chunk_layout(sg_renderer_t const& renderer) {
	sg_renderer_chunk_layout_t layout;
	layout.cells_per_axis = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	layout.chunk_resolution = std::max(1u, renderer.marching_cubes_chunk_resolution);
	layout.chunks_per_axis = (layout.cells_per_axis + layout.chunk_resolution - 1u) / layout.chunk_resolution;
	layout.num_chunks = layout.chunks_per_axis * layout.chunks_per_axis * layout.chunks_per_axis;
	layout.chunk_vertex_capacity = layout.chunk_resolution * layout.chunk_resolution * layout.chunk_resolution *
		k_marching_cubes_max_tris_per_cell * 3u;
	layout.total_vertex_capacity = layout.num_chunks * layout.chunk_vertex_capacity;
	return layout;
}

u32 sg_renderer_chunk_index(u32 x, u32 y, u32 z, u32 chunks_per_axis) {
	return x + y * chunks_per_axis + z * chunks_per_axis * chunks_per_axis;
}

glm::vec3 sg_renderer_compute_snapped_center(sg_renderer_t const& renderer, glm::vec3 const& camera_pos) {
	f32 const extent = std::max(renderer.marching_cubes_bounds_extent, 0.001f);
	u32 const grid_cells = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	u32 const chunk_cells = std::max(1u, renderer.marching_cubes_chunk_resolution);
	f32 const cell_size = (extent * 2.0f) / static_cast<f32>(grid_cells);
	f32 const chunk_world_size = std::max(cell_size * static_cast<f32>(chunk_cells), 0.001f);
	return glm::round(camera_pos / chunk_world_size) * chunk_world_size;
}

f32 sg_renderer_chunk_world_size(sg_renderer_t const& renderer) {
	f32 const extent = std::max(renderer.marching_cubes_bounds_extent, 0.001f);
	u32 const grid_cells = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	u32 const chunk_cells = std::max(1u, renderer.marching_cubes_chunk_resolution);
	f32 const cell_size = (extent * 2.0f) / static_cast<f32>(grid_cells);
	return std::max(cell_size * static_cast<f32>(chunk_cells), 0.001f);
}

template <typename T>
void sg_renderer_upload_buffer_texture(
	sg_renderer_buffer_texture_t const& buffer_texture, std::vector<T> const& data, GLenum internal_format) {
	assert_release(buffer_texture.buffer != 0);
	assert_release(buffer_texture.texture != 0);

	std::vector<T> upload_data = data;
	if (upload_data.empty()) {
		u32 const num_elements = static_cast<u32>(4 / sizeof(T));
		u32 const fallback_count = std::max(1u, num_elements);
		upload_data.resize(fallback_count);
	}

	glBindBuffer(GL_TEXTURE_BUFFER, buffer_texture.buffer);
	glBufferData(GL_TEXTURE_BUFFER, sizeof(T) * upload_data.size(), upload_data.data(), GL_DYNAMIC_DRAW);
	glBindTexture(GL_TEXTURE_BUFFER, buffer_texture.texture);
	glTexBuffer(GL_TEXTURE_BUFFER, internal_format, buffer_texture.buffer);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
	glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void sg_renderer_bind_buffer_texture(GLuint program, char const* uniform_name, GLuint texture, GLenum texture_unit) {
	GLint const location = glGetUniformLocation(program, uniform_name);
	if (location < 0) {
		return;
	}
	glUniform1i(location, texture_unit - GL_TEXTURE0);
	glActiveTexture(texture_unit);
	glBindTexture(GL_TEXTURE_BUFFER, texture);
}

void sg_renderer_destroy_buffer_texture(sg_renderer_buffer_texture_t& buffer_texture) {
	if (buffer_texture.texture != 0) {
		glDeleteTextures(1, &buffer_texture.texture);
		buffer_texture.texture = 0;
	}
	if (buffer_texture.buffer != 0) {
		glDeleteBuffers(1, &buffer_texture.buffer);
		buffer_texture.buffer = 0;
	}
}

void sg_renderer_create_checker_texture(sg_renderer_t& renderer) {
	if (renderer.checker_texture != 0) {
		return;
	}

	constexpr u8 magenta[3] = {255, 0, 255};
	constexpr u8 black[3] = {0, 0, 0};
	u8 const pixels[2 * 2 * 3] = {
		magenta[0],
		magenta[1],
		magenta[2],
		black[0],
		black[1],
		black[2],
		black[0],
		black[1],
		black[2],
		magenta[0],
		magenta[1],
		magenta[2],
	};

	glGenTextures(1, &renderer.checker_texture);
	glBindTexture(GL_TEXTURE_2D, renderer.checker_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
}

std::string sg_renderer_normalize_texture_path(std::string const& path) {
	if (path.empty()) {
		return path;
	}

	std::filesystem::path fs_path = std::filesystem::u8path(path);
	std::error_code ec;
	std::filesystem::path const absolute = std::filesystem::absolute(fs_path, ec);
	if (!ec) {
		fs_path = absolute;
	}
	fs_path = fs_path.lexically_normal();
	return fs_path.string();
}

void sg_renderer_destroy_marching_cubes_buffers(sg_renderer_t& renderer) {
	if (renderer.marching_cubes_vertex_ssbo != 0) {
		glDeleteBuffers(1, &renderer.marching_cubes_vertex_ssbo);
		renderer.marching_cubes_vertex_ssbo = 0;
	}
	if (renderer.marching_cubes_counter_ssbo != 0) {
		glDeleteBuffers(1, &renderer.marching_cubes_counter_ssbo);
		renderer.marching_cubes_counter_ssbo = 0;
	}
	if (renderer.marching_cubes_indirect_buffer != 0) {
		glDeleteBuffers(1, &renderer.marching_cubes_indirect_buffer);
		renderer.marching_cubes_indirect_buffer = 0;
	}
	for (GLuint& density_texture : renderer.marching_cubes_density_textures) {
		if (density_texture != 0) {
			glDeleteTextures(1, &density_texture);
			density_texture = 0;
		}
	}
	renderer.marching_cubes_density_textures.clear();
	renderer.marching_cubes_vertex_capacity = 0;
	renderer.marching_cubes_vertex_count = 0;
	renderer.marching_cubes_chunks_per_axis = 0;
	renderer.marching_cubes_num_chunks = 0;
	renderer.marching_cubes_chunk_vertex_capacity = 0;
	renderer.marching_cubes_chunk_scan_cursor = 0;
	renderer.marching_cubes_chunk_dirty.clear();
	renderer.marching_cubes_chunk_vertex_counts.clear();
	renderer.clipmap_chunk_dirty.clear();
	renderer.clipmap_chunk_scan_cursor.clear();
}

void sg_renderer_ensure_marching_cubes_buffers(sg_renderer_t& renderer) {
	renderer.clipmap_levels = std::clamp(renderer.clipmap_levels, 1u, 4u);
	sg_renderer_chunk_layout_t const layout = sg_renderer_compute_chunk_layout(renderer);
	u32 const clip_levels = std::max(1u, renderer.clipmap_levels);
	if (renderer.marching_cubes_vertex_ssbo != 0 && renderer.marching_cubes_counter_ssbo != 0 &&
		renderer.marching_cubes_vertex_capacity >= layout.total_vertex_capacity &&
		renderer.marching_cubes_num_chunks == layout.num_chunks &&
		renderer.marching_cubes_chunk_vertex_capacity == layout.chunk_vertex_capacity &&
		renderer.marching_cubes_chunks_per_axis == layout.chunks_per_axis &&
		renderer.marching_cubes_density_textures.size() == clip_levels &&
		renderer.clipmap_chunk_dirty.size() == clip_levels &&
		renderer.clipmap_chunk_scan_cursor.size() == clip_levels) {
		return;
	}

	sg_renderer_destroy_marching_cubes_buffers(renderer);

	glGenBuffers(1, &renderer.marching_cubes_vertex_ssbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer.marching_cubes_vertex_ssbo);
	glBufferData(GL_SHADER_STORAGE_BUFFER,
		static_cast<GLsizeiptr>(layout.total_vertex_capacity) * k_marching_cubes_vertex_stride_bytes, nullptr,
		GL_DYNAMIC_DRAW);

	glGenBuffers(1, &renderer.marching_cubes_counter_ssbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer.marching_cubes_counter_ssbo);
	std::vector<GLuint> initial_counters(layout.num_chunks, 0u);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * initial_counters.size(), initial_counters.data(),
		GL_DYNAMIC_DRAW);

	glGenBuffers(1, &renderer.marching_cubes_indirect_buffer);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, renderer.marching_cubes_indirect_buffer);
	std::vector<draw_arrays_indirect_command_t> initial_commands(layout.num_chunks);
	for (u32 i = 0; i < layout.num_chunks; ++i) {
		initial_commands[i].count = 0;
		initial_commands[i].instance_count = 1;
		initial_commands[i].first = i * layout.chunk_vertex_capacity;
		initial_commands[i].base_instance = 0;
	}
	glBufferData(GL_DRAW_INDIRECT_BUFFER,
		static_cast<GLsizeiptr>(sizeof(draw_arrays_indirect_command_t) * initial_commands.size()),
		initial_commands.data(), GL_DYNAMIC_DRAW);

	renderer.marching_cubes_density_textures.assign(clip_levels, 0);
	glGenTextures(static_cast<GLsizei>(clip_levels), renderer.marching_cubes_density_textures.data());
	GLsizei const density_dim = static_cast<GLsizei>(layout.cells_per_axis + 1u);
	for (u32 level = 0; level < clip_levels; ++level) {
		glBindTexture(GL_TEXTURE_3D, renderer.marching_cubes_density_textures[level]);
		glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32F, density_dim, density_dim, density_dim);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_3D, 0);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
	renderer.marching_cubes_vertex_capacity = layout.total_vertex_capacity;
	renderer.marching_cubes_vertex_count = 0;
	renderer.marching_cubes_chunks_per_axis = layout.chunks_per_axis;
	renderer.marching_cubes_num_chunks = layout.num_chunks;
	renderer.marching_cubes_chunk_vertex_capacity = layout.chunk_vertex_capacity;
	renderer.marching_cubes_chunk_dirty.assign(layout.num_chunks, 1u);
	renderer.marching_cubes_chunk_vertex_counts.assign(layout.num_chunks, 0u);
	renderer.clipmap_chunk_dirty.assign(clip_levels, std::vector<u8>(layout.num_chunks, 1u));
	renderer.clipmap_chunk_scan_cursor.assign(clip_levels, 0u);
}

bool sg_renderer_rebuild_plugin_shader(sg_renderer_t& renderer, bool reload_plugins) {
	sg_plugins_t candidate_plugins = sg_plugins_get();
	if (reload_plugins) {
		std::string load_error;
		if (!sg_plugins_load_candidate(candidate_plugins, load_error)) {
			renderer.plugin_reload_status = "Plugin load failed: " + load_error;
			std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
			return false;
		}
	}

	std::string mesh_compute_source;
	std::string density_compute_source;
	std::string build_error;
	if (!sg_plugins_build_marching_cubes_compute_source(candidate_plugins, mesh_compute_source, build_error)) {
		renderer.plugin_reload_status = "Shader generation failed: " + build_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}
	if (!sg_plugins_build_compute_source_from_template(
		    candidate_plugins, "shaders/sdf_density_template.comp", density_compute_source, build_error)) {
		renderer.plugin_reload_status = "Shader generation failed: " + build_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}

	GLuint new_mesh_program = 0;
	GLuint new_density_program = 0;
	std::string compile_error;
	if (!gl_shader_try_build_compute_program_from_source(
		    new_mesh_program, "generated/marching_cubes_plugins.comp", mesh_compute_source, compile_error)) {
		renderer.plugin_reload_status = "Shader compile/link failed: " + compile_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}
	if (!gl_shader_try_build_compute_program_from_source(
		    new_density_program, "generated/sdf_density.comp", density_compute_source, compile_error)) {
		glDeleteProgram(new_mesh_program);
		renderer.plugin_reload_status = "Shader compile/link failed: " + compile_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}

	if (renderer.marching_cubes_program != 0) {
		glDeleteProgram(renderer.marching_cubes_program);
	}
	if (renderer.marching_cubes_density_program != 0) {
		glDeleteProgram(renderer.marching_cubes_density_program);
	}
	renderer.marching_cubes_program = new_mesh_program;
	renderer.marching_cubes_density_program = new_density_program;

	if (reload_plugins) {
		sg_plugins_commit(std::move(candidate_plugins));
	}

	renderer.plugin_reload_status = "Plugins and shader loaded successfully!";
	return true;
}

s32 sg_runtime_id_or_zero(std::string const& plugin_id) {
	auto const* def = sg_plugins_find_combine_by_id(plugin_id);
	return def == nullptr ? 0 : def->runtime_id;
}

} // namespace

void sg_renderer_init(sg_renderer_t& renderer) {
	auto vbo_position_data = std::vector<f32>({
		-1.0f,
		-1.0f,
		1.0f,
		-1.0f,
		-1.0f,
		1.0f,
		1.0f,
		1.0f,
	});
	auto vbo_tex_coord_data = std::vector<f32>({
		0.0f,
		0.0f,
		1.0f,
		0.0f,
		0.0f,
		1.0f,
		1.0f,
		1.0f,
	});
	gl_vertex_buffers_upload(renderer.quad_vbo, "a_position", vbo_position_data, 2, GL_FLOAT, GL_FALSE);
	gl_vertex_buffers_upload(renderer.quad_vbo, "a_tex_coord", vbo_tex_coord_data, 2, GL_FLOAT, GL_FALSE);
	renderer.quad_vbo.mode = GL_TRIANGLE_STRIP;

	gl_render_pass_init(renderer.primary_render_pass, "shaders/volume_raymarch.vert",
		"shaders/volume_raymarch.frag", 2048.0f, 2048.0f,
		{
			{"o_color", {.internal_format = GL_RGBA8, .format = GL_RGBA, .type = GL_UNSIGNED_BYTE}},
			{"##DEPTH",
				{.internal_format = GL_DEPTH_COMPONENT24,
					.format = GL_DEPTH_COMPONENT,
					.type = GL_FLOAT}},
		});
	renderer.primary_render_pass.depth_func = GL_LESS;
	renderer.primary_render_pass.cull_face = GL_BACK;

	bool const plugin_shader_ok = sg_renderer_rebuild_plugin_shader(renderer, false);
	if (!plugin_shader_ok) {
		std::cerr << "[sg_renderer] compute mesher unavailable: " << renderer.plugin_reload_status << std::endl;
	}

	{
		std::string error;
		if (!gl_shader_try_build_compute_program_from_source(renderer.marching_cubes_indirect_program,
			    "generated/marching_cubes_indirect.comp", k_marching_cubes_indirect_build_comp, error)) {
			std::cerr << "[sg_renderer] indirect command compute unavailable: " << error << std::endl;
			assert_release(false);
		}
	}

	glGenVertexArrays(1, &renderer.marching_cubes_vao);

	glGenBuffers(1, &renderer.program.buffer);
	glGenTextures(1, &renderer.program.texture);
	glGenBuffers(1, &renderer.primitive_meta.buffer);
	glGenTextures(1, &renderer.primitive_meta.texture);
	glGenBuffers(1, &renderer.primitive_params.buffer);
	glGenTextures(1, &renderer.primitive_params.texture);
	glGenBuffers(1, &renderer.primitive_scale.buffer);
	glGenTextures(1, &renderer.primitive_scale.texture);
	glGenBuffers(1, &renderer.primitive_bounds.buffer);
	glGenTextures(1, &renderer.primitive_bounds.texture);
	glGenBuffers(1, &renderer.primitive_effect_ranges.buffer);
	glGenTextures(1, &renderer.primitive_effect_ranges.texture);
	glGenBuffers(1, &renderer.effect_meta.buffer);
	glGenTextures(1, &renderer.effect_meta.texture);
	glGenBuffers(1, &renderer.effect_params.buffer);
	glGenTextures(1, &renderer.effect_params.texture);
	glGenBuffers(1, &renderer.combine_params.buffer);
	glGenTextures(1, &renderer.combine_params.texture);
	glGenBuffers(1, &renderer.marching_cubes_edge_table.buffer);
	glGenTextures(1, &renderer.marching_cubes_edge_table.texture);
	glGenBuffers(1, &renderer.marching_cubes_tri_table.buffer);
	glGenTextures(1, &renderer.marching_cubes_tri_table.texture);

	std::vector<s32> const edge_table_data(k_marching_cubes_edge_table.begin(), k_marching_cubes_edge_table.end());
	std::vector<s32> const tri_table_data(k_marching_cubes_tri_table.begin(), k_marching_cubes_tri_table.end());
	sg_renderer_upload_buffer_texture(renderer.marching_cubes_edge_table, edge_table_data, GL_R32I);
	sg_renderer_upload_buffer_texture(renderer.marching_cubes_tri_table, tri_table_data, GL_R32I);

	sg_renderer_ensure_marching_cubes_buffers(renderer);
	sg_renderer_create_checker_texture(renderer);
}

void sg_renderer_destroy(sg_renderer_t& renderer) {
	sg_renderer_destroy_buffer_texture(renderer.program);
	sg_renderer_destroy_buffer_texture(renderer.primitive_meta);
	sg_renderer_destroy_buffer_texture(renderer.primitive_params);
	sg_renderer_destroy_buffer_texture(renderer.primitive_scale);
	sg_renderer_destroy_buffer_texture(renderer.primitive_bounds);
	sg_renderer_destroy_buffer_texture(renderer.primitive_effect_ranges);
	sg_renderer_destroy_buffer_texture(renderer.effect_meta);
	sg_renderer_destroy_buffer_texture(renderer.effect_params);
	sg_renderer_destroy_buffer_texture(renderer.combine_params);
	sg_renderer_destroy_buffer_texture(renderer.marching_cubes_edge_table);
	sg_renderer_destroy_buffer_texture(renderer.marching_cubes_tri_table);
	sg_renderer_destroy_marching_cubes_buffers(renderer);

	if (renderer.marching_cubes_program != 0) {
		glDeleteProgram(renderer.marching_cubes_program);
		renderer.marching_cubes_program = 0;
	}
	if (renderer.marching_cubes_indirect_program != 0) {
		glDeleteProgram(renderer.marching_cubes_indirect_program);
		renderer.marching_cubes_indirect_program = 0;
	}
	if (renderer.marching_cubes_density_program != 0) {
		glDeleteProgram(renderer.marching_cubes_density_program);
		renderer.marching_cubes_density_program = 0;
	}
	if (renderer.marching_cubes_vao != 0) {
		glDeleteVertexArrays(1, &renderer.marching_cubes_vao);
		renderer.marching_cubes_vao = 0;
	}

	if (renderer.checker_texture != 0) {
		glDeleteTextures(1, &renderer.checker_texture);
		renderer.checker_texture = 0;
	}

	for (auto& texture_entry : renderer.primitive_textures) {
		if (texture_entry.texture != 0) {
			glDeleteTextures(1, &texture_entry.texture);
			texture_entry.texture = 0;
		}
	}
	renderer.primitive_textures.clear();
	renderer.primitive_texture_ids_by_path.clear();
}

s32 sg_renderer_get_or_load_primitive_texture(sg_renderer_t& renderer, std::string const& path) {
	std::string const normalized_path = sg_renderer_normalize_texture_path(path);
	if (normalized_path.empty()) {
		return 0;
	}

	auto it = renderer.primitive_texture_ids_by_path.find(normalized_path);
	if (it != renderer.primitive_texture_ids_by_path.end()) {
		return it->second;
	}

	if (renderer.primitive_textures.size() + 1 >= static_cast<size_t>(k_max_texture_slots)) {
		std::cerr << "[sg_renderer] maximum primitive texture slots reached (" << (k_max_texture_slots - 1)
			  << ")" << std::endl;
		return 0;
	}

	stbi_set_flip_vertically_on_load(1);
	s32 width = 0;
	s32 height = 0;
	s32 channels = 0;
	u8* pixels = stbi_load(normalized_path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
	if (pixels == nullptr) {
		std::cerr << "[sg_renderer] failed to load texture: " << normalized_path << std::endl;
		return 0;
	}

	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	stbi_image_free(pixels);

	sg_renderer_image_texture_t entry;
	entry.texture = texture;
	entry.path = normalized_path;
	renderer.primitive_textures.emplace_back(std::move(entry));

	s32 const texture_id = static_cast<s32>(renderer.primitive_textures.size());
	renderer.primitive_texture_ids_by_path[normalized_path] = texture_id;
	return texture_id;
}

std::string const* sg_renderer_get_primitive_texture_path(sg_renderer_t const& renderer, s32 texture_id) {
	if (texture_id <= 0) {
		return nullptr;
	}

	size_t const entry_index = static_cast<size_t>(texture_id - 1);
	if (entry_index >= renderer.primitive_textures.size()) {
		return nullptr;
	}

	return &renderer.primitive_textures[entry_index].path;
}

void sg_renderer_update(sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene, fly_camera_t const& camera,
	bool scene_gpu_buffers_dirty) {
	if (scene_gpu_buffers_dirty) {
		sg_renderer_upload_buffer_texture(renderer.program, compiled_scene.program, GL_RGBA32I);
		sg_renderer_upload_buffer_texture(renderer.combine_params, compiled_scene.combine_params, GL_RGBA32F);
		sg_renderer_upload_buffer_texture(renderer.primitive_meta, compiled_scene.primitive_meta, GL_RGBA32I);
		sg_renderer_upload_buffer_texture(
			renderer.primitive_params, compiled_scene.primitive_params, GL_RGBA32F);
		sg_renderer_upload_buffer_texture(renderer.primitive_scale, compiled_scene.primitive_scale, GL_RGBA32F);
		sg_renderer_upload_buffer_texture(
			renderer.primitive_bounds, compiled_scene.primitive_bounds, GL_RGBA32F);
		sg_renderer_upload_buffer_texture(
			renderer.primitive_effect_ranges, compiled_scene.primitive_effect_ranges, GL_RGBA32I);
		sg_renderer_upload_buffer_texture(renderer.effect_meta, compiled_scene.effect_meta, GL_RGBA32I);
		sg_renderer_upload_buffer_texture(renderer.effect_params, compiled_scene.effect_params, GL_RGBA32F);
	}

	if (!compiled_scene.has_output) {
		renderer.marching_cubes_vertex_count = 0;
		std::fill(renderer.marching_cubes_chunk_vertex_counts.begin(),
			renderer.marching_cubes_chunk_vertex_counts.end(), 0u);
		if (renderer.marching_cubes_indirect_buffer != 0 && renderer.marching_cubes_num_chunks > 0) {
			std::vector<draw_arrays_indirect_command_t> cleared(renderer.marching_cubes_num_chunks);
			for (u32 i = 0; i < renderer.marching_cubes_num_chunks; ++i) {
				cleared[i].count = 0;
				cleared[i].instance_count = 1;
				cleared[i].first = i * renderer.marching_cubes_chunk_vertex_capacity;
				cleared[i].base_instance = 0;
			}
			glBindBuffer(GL_DRAW_INDIRECT_BUFFER, renderer.marching_cubes_indirect_buffer);
			glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
				static_cast<GLsizeiptr>(sizeof(draw_arrays_indirect_command_t) * cleared.size()),
				cleared.data());
			glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
		}
	}

	if (compiled_scene.has_output) {
		sg_renderer_ensure_marching_cubes_buffers(renderer);
		renderer.marching_cubes_last_remeshed_chunk_count = 0;

		if (renderer.marching_cubes_center_on_camera) {
			f32 const chunk_world_size = sg_renderer_chunk_world_size(renderer);
			f32 const deadzone_world = chunk_world_size *
				static_cast<f32>(std::max(1, renderer.marching_cubes_center_deadzone_chunks));
			glm::vec3 const camera_delta = glm::abs(camera.pos - renderer.marching_cubes_center);
			if (camera_delta.x > deadzone_world || camera_delta.y > deadzone_world ||
				camera_delta.z > deadzone_world) {
				glm::vec3 const snapped_center =
					sg_renderer_compute_snapped_center(renderer, camera.pos);
				if (glm::any(glm::greaterThan(glm::abs(snapped_center - renderer.marching_cubes_center),
					    glm::vec3(0.0001f)))) {
					renderer.marching_cubes_center = snapped_center;
					sg_renderer_mark_all_chunks_dirty(renderer);
				}
			}
		}

		bool has_dirty_chunk = false;
		for (auto const& level_dirty : renderer.clipmap_chunk_dirty) {
			for (u8 dirty_flag : level_dirty) {
				if (dirty_flag != 0) {
					has_dirty_chunk = true;
					break;
				}
			}
			if (has_dirty_chunk) {
				break;
			}
		}

		if (has_dirty_chunk) {
			if (renderer.marching_cubes_density_program == 0 ||
				renderer.marching_cubes_density_textures.empty()) {
				sg_renderer_mark_all_chunks_dirty(renderer);
				return;
			}

			glUseProgram(renderer.marching_cubes_density_program);
			glUniform1i(glGetUniformLocation(renderer.marching_cubes_density_program, "u_has_scene"),
				compiled_scene.has_output ? 1 : 0);
			glUniform1i(glGetUniformLocation(renderer.marching_cubes_density_program, "u_program_count"),
				static_cast<GLint>(compiled_scene.program.size()));
			glUniform1i(glGetUniformLocation(renderer.marching_cubes_density_program, "u_max_stack_depth"),
				static_cast<GLint>(compiled_scene.max_stack_depth));
			glUniform1i(
				glGetUniformLocation(renderer.marching_cubes_density_program, "u_op_push_primitive"),
				SG_COMPILER_OP_PUSH_PRIMITIVE);
			glUniform1i(glGetUniformLocation(renderer.marching_cubes_density_program, "u_op_combine"),
				SG_COMPILER_OP_COMBINE);
			glUniform1i(glGetUniformLocation(renderer.marching_cubes_density_program, "u_grid_resolution"),
				renderer.marching_cubes_grid_resolution);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program, "u_program_tex",
				renderer.program.texture, GL_TEXTURE0);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program, "u_combine_params_tex",
				renderer.combine_params.texture, GL_TEXTURE1);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program, "u_primitive_meta_tex",
				renderer.primitive_meta.texture, GL_TEXTURE2);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program,
				"u_primitive_params_tex", renderer.primitive_params.texture, GL_TEXTURE3);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program,
				"u_primitive_scale_tex", renderer.primitive_scale.texture, GL_TEXTURE4);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program,
				"u_primitive_effect_range_tex", renderer.primitive_effect_ranges.texture, GL_TEXTURE5);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program, "u_effect_meta_tex",
				renderer.effect_meta.texture, GL_TEXTURE6);
			sg_renderer_bind_buffer_texture(renderer.marching_cubes_density_program, "u_effect_params_tex",
				renderer.effect_params.texture, GL_TEXTURE7);

			u32 const point_groups = (renderer.marching_cubes_chunk_resolution + 1u +
							 k_marching_cubes_threads_per_axis - 1u) /
				k_marching_cubes_threads_per_axis;
			u32 const chunks_per_axis = renderer.marching_cubes_chunks_per_axis;
			u32 const chunk_cells = renderer.marching_cubes_chunk_resolution;
			u32 const grid_cells = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
			u32 const budget = static_cast<u32>(std::max(1, renderer.marching_cubes_remesh_budget_chunks));
			u32 const clip_levels = std::min<u32>(renderer.clipmap_levels,
				static_cast<u32>(renderer.marching_cubes_density_textures.size()));
			for (u32 level = 0; level < clip_levels; ++level) {
				auto& dirty = renderer.clipmap_chunk_dirty[level];
				u32& scan_cursor = renderer.clipmap_chunk_scan_cursor[level];
				if (dirty.empty()) {
					continue;
				}

				f32 const level_scale = std::pow(2.0f, static_cast<f32>(level));
				f32 const level_extent = renderer.marching_cubes_bounds_extent * level_scale;
				glUniform3f(
					glGetUniformLocation(renderer.marching_cubes_density_program, "u_bounds_min"),
					renderer.marching_cubes_center.x - level_extent,
					renderer.marching_cubes_center.y - level_extent,
					renderer.marching_cubes_center.z - level_extent);
				glUniform3f(
					glGetUniformLocation(renderer.marching_cubes_density_program, "u_bounds_max"),
					renderer.marching_cubes_center.x + level_extent,
					renderer.marching_cubes_center.y + level_extent,
					renderer.marching_cubes_center.z + level_extent);
				glBindImageTexture(0, renderer.marching_cubes_density_textures[level], 0, GL_TRUE, 0,
					GL_WRITE_ONLY, GL_R32F);

				u32 const start_idx = scan_cursor % renderer.marching_cubes_num_chunks;
				u32 scanned = 0;
				u32 processed = 0;
				while (scanned < renderer.marching_cubes_num_chunks && processed < budget) {
					u32 const chunk_idx =
						(start_idx + scanned) % renderer.marching_cubes_num_chunks;
					scanned += 1;
					if (dirty[chunk_idx] == 0) {
						continue;
					}
					processed += 1;
					renderer.marching_cubes_last_remeshed_chunk_count += 1;

					u32 const chunk_z = chunk_idx / (chunks_per_axis * chunks_per_axis);
					u32 const rem = chunk_idx % (chunks_per_axis * chunks_per_axis);
					u32 const chunk_y = rem / chunks_per_axis;
					u32 const chunk_x = rem % chunks_per_axis;

					glm::ivec3 const chunk_origin =
						glm::ivec3(static_cast<s32>(chunk_x * chunk_cells),
							static_cast<s32>(chunk_y * chunk_cells),
							static_cast<s32>(chunk_z * chunk_cells));
					glm::ivec3 const chunk_limit = glm::ivec3(
						static_cast<s32>(std::min(grid_cells, (chunk_x + 1u) * chunk_cells)),
						static_cast<s32>(std::min(grid_cells, (chunk_y + 1u) * chunk_cells)),
						static_cast<s32>(std::min(grid_cells, (chunk_z + 1u) * chunk_cells)));
					glm::ivec3 const point_min =
						glm::max(chunk_origin - glm::ivec3(1), glm::ivec3(0));
					glm::ivec3 const point_max = glm::min(
						chunk_limit + glm::ivec3(1), glm::ivec3(static_cast<s32>(grid_cells)));
					glm::ivec3 const point_valid = point_max - point_min + glm::ivec3(1);

					glUniform3i(glGetUniformLocation(renderer.marching_cubes_density_program,
							    "u_chunk_point_origin"),
						point_min.x, point_min.y, point_min.z);
					glUniform3i(glGetUniformLocation(renderer.marching_cubes_density_program,
							    "u_chunk_valid_points"),
						point_valid.x, point_valid.y, point_valid.z);
					glDispatchCompute(point_groups, point_groups, point_groups);
					dirty[chunk_idx] = 0;
				}
				scan_cursor = (start_idx + scanned) % renderer.marching_cubes_num_chunks;
				glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
			}
		}
	}

	gl_render_pass_begin(renderer.primary_render_pass);
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_has_scene", compiled_scene.has_output ? 1 : 0);
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_grid_resolution", renderer.marching_cubes_grid_resolution);
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_clipmap_levels", static_cast<s32>(renderer.clipmap_levels));
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_debug_lod", renderer.debug_show_lod_levels ? 1 : 0);
	gl_render_pass_uniform_float(renderer.primary_render_pass, "u_iso_level", renderer.marching_cubes_iso_level);
	gl_render_pass_uniform_mat4(renderer.primary_render_pass, "u_view_proj", camera.proj_mat * camera.view_mat);
	gl_render_pass_uniform_mat4(
		renderer.primary_render_pass, "u_inv_view_proj", glm::inverse(camera.proj_mat * camera.view_mat));
	for (u32 level = 0; level < std::min<u32>(renderer.clipmap_levels, 4u); ++level) {
		f32 const level_scale = std::pow(2.0f, static_cast<f32>(level));
		f32 const level_extent = renderer.marching_cubes_bounds_extent * level_scale;
		std::string const min_name = "u_bounds_min[" + std::to_string(level) + "]";
		std::string const max_name = "u_bounds_max[" + std::to_string(level) + "]";
		GLint const min_loc =
			glGetUniformLocation(renderer.primary_render_pass.shader.program, min_name.c_str());
		GLint const max_loc =
			glGetUniformLocation(renderer.primary_render_pass.shader.program, max_name.c_str());
		if (min_loc >= 0) {
			glUniform3f(min_loc, renderer.marching_cubes_center.x - level_extent,
				renderer.marching_cubes_center.y - level_extent,
				renderer.marching_cubes_center.z - level_extent);
		}
		if (max_loc >= 0) {
			glUniform3f(max_loc, renderer.marching_cubes_center.x + level_extent,
				renderer.marching_cubes_center.y + level_extent,
				renderer.marching_cubes_center.z + level_extent);
		}
	}
	GLint const camera_pos_location =
		glGetUniformLocation(renderer.primary_render_pass.shader.program, "u_camera_pos");
	if (camera_pos_location >= 0) {
		glUniform3fv(camera_pos_location, 1, glm::value_ptr(camera.pos));
	}
	for (u32 level = 0; level < std::min<u32>(renderer.clipmap_levels, 4u) &&
		level < renderer.marching_cubes_density_textures.size();
		++level) {
		std::string const tex_name = "u_density_tex[" + std::to_string(level) + "]";
		GLint const tex_loc =
			glGetUniformLocation(renderer.primary_render_pass.shader.program, tex_name.c_str());
		if (tex_loc >= 0) {
			glUniform1i(tex_loc, static_cast<GLint>(level));
			glActiveTexture(GL_TEXTURE0 + level);
			glBindTexture(GL_TEXTURE_3D, renderer.marching_cubes_density_textures[level]);
		}
	}
	gl_render_pass_draw(renderer.primary_render_pass, renderer.quad_vbo);
	gl_render_pass_end(renderer.primary_render_pass);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
	for (u32 level = 0; level < std::min<u32>(renderer.clipmap_levels, 4u); ++level) {
		glActiveTexture(GL_TEXTURE0 + level);
		glBindTexture(GL_TEXTURE_3D, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void sg_renderer_mark_all_chunks_dirty(sg_renderer_t& renderer) {
	if (renderer.clipmap_chunk_dirty.empty()) {
		return;
	}
	for (auto& level_dirty : renderer.clipmap_chunk_dirty) {
		std::fill(level_dirty.begin(), level_dirty.end(), 1u);
	}
}

void sg_renderer_mark_bounds_dirty(sg_renderer_t& renderer, glm::vec3 const& bounds_min, glm::vec3 const& bounds_max) {
	if (renderer.clipmap_chunk_dirty.empty() || renderer.marching_cubes_chunks_per_axis == 0u) {
		return;
	}
	if (bounds_min.x > bounds_max.x || bounds_min.y > bounds_max.y || bounds_min.z > bounds_max.z) {
		return;
	}

	if (renderer.marching_cubes_bounds_extent <= 0.0f) {
		sg_renderer_mark_all_chunks_dirty(renderer);
		return;
	}

	u32 const grid_cells = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	u32 const chunk_cells = std::max(1u, renderer.marching_cubes_chunk_resolution);
	u32 const chunks_per_axis = renderer.marching_cubes_chunks_per_axis;
	u32 const clip_levels =
		std::min<u32>(renderer.clipmap_levels, static_cast<u32>(renderer.clipmap_chunk_dirty.size()));
	for (u32 level = 0; level < clip_levels; ++level) {
		auto& level_dirty = renderer.clipmap_chunk_dirty[level];
		f32 const extent = renderer.marching_cubes_bounds_extent * std::pow(2.0f, static_cast<f32>(level));
		glm::vec3 const grid_min = renderer.marching_cubes_center - glm::vec3(extent);
		glm::vec3 const grid_max = renderer.marching_cubes_center + glm::vec3(extent);
		glm::vec3 const clamped_min = glm::max(bounds_min, grid_min);
		glm::vec3 const clamped_max = glm::min(bounds_max, grid_max);
		if (clamped_min.x > clamped_max.x || clamped_min.y > clamped_max.y || clamped_min.z > clamped_max.z) {
			continue;
		}

		f32 const cells_per_world_unit = static_cast<f32>(grid_cells) / (extent * 2.0f);
		auto world_to_cell = [&](f32 value, f32 axis_min) {
			return static_cast<s32>(std::floor((value - axis_min) * cells_per_world_unit));
		};

		s32 const min_x =
			std::clamp(world_to_cell(clamped_min.x, grid_min.x) - 1, 0, static_cast<s32>(grid_cells) - 1);
		s32 const min_y =
			std::clamp(world_to_cell(clamped_min.y, grid_min.y) - 1, 0, static_cast<s32>(grid_cells) - 1);
		s32 const min_z =
			std::clamp(world_to_cell(clamped_min.z, grid_min.z) - 1, 0, static_cast<s32>(grid_cells) - 1);
		s32 const max_x =
			std::clamp(world_to_cell(clamped_max.x, grid_min.x) + 1, 0, static_cast<s32>(grid_cells) - 1);
		s32 const max_y =
			std::clamp(world_to_cell(clamped_max.y, grid_min.y) + 1, 0, static_cast<s32>(grid_cells) - 1);
		s32 const max_z =
			std::clamp(world_to_cell(clamped_max.z, grid_min.z) + 1, 0, static_cast<s32>(grid_cells) - 1);

		u32 const min_chunk_x = static_cast<u32>(min_x) / chunk_cells;
		u32 const min_chunk_y = static_cast<u32>(min_y) / chunk_cells;
		u32 const min_chunk_z = static_cast<u32>(min_z) / chunk_cells;
		u32 const max_chunk_x = static_cast<u32>(max_x) / chunk_cells;
		u32 const max_chunk_y = static_cast<u32>(max_y) / chunk_cells;
		u32 const max_chunk_z = static_cast<u32>(max_z) / chunk_cells;

		for (u32 z = min_chunk_z; z <= max_chunk_z && z < chunks_per_axis; ++z) {
			for (u32 y = min_chunk_y; y <= max_chunk_y && y < chunks_per_axis; ++y) {
				for (u32 x = min_chunk_x; x <= max_chunk_x && x < chunks_per_axis; ++x) {
					u32 const idx = sg_renderer_chunk_index(x, y, z, chunks_per_axis);
					if (idx < level_dirty.size()) {
						level_dirty[idx] = 1u;
					}
				}
			}
		}
	}
}

sg_preview_interaction_t sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled) {
	sg_preview_interaction_t interaction;
	f32 const window_width = ImGui::GetIO().DisplaySize.x - 400.0f;
	f32 const window_height = ImGui::GetIO().DisplaySize.y;
	f32 const controls_height = 140.0f;
	f32 const preview_height = std::max(100.0f, window_height - controls_height);

	ImGuiWindowFlags window_flags =
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
	if (!input_enabled) {
		window_flags |= ImGuiWindowFlags_NoInputs;
	}

	ImGui::SetNextWindowPos(ImVec2(400, 0));
	ImGui::SetNextWindowSize(ImVec2(window_width, preview_height));
	ImGui::Begin("Preview###PreviewWindow", nullptr, window_flags);

	ImVec2 const content_region = ImGui::GetContentRegionAvail();
	GLuint const display_texture = renderer.primary_render_pass.internal_output_descriptors["o_color"].texture;
	f32 const image_aspect = static_cast<f32>(renderer.primary_render_pass.width) /
		static_cast<f32>(renderer.primary_render_pass.height);
	f32 const window_aspect = content_region.x / content_region.y;

	f32 display_width = content_region.x;
	f32 display_height = content_region.y;
	if (image_aspect > window_aspect) {
		display_height = content_region.x / image_aspect;
	} else {
		display_width = content_region.y * image_aspect;
	}

	f32 const offset_x = (content_region.x - display_width) * 0.5f;
	f32 const offset_y = (content_region.y - display_height) * 0.5f;

	ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(offset_x, offset_y));
	ImGui::Image(display_texture, ImVec2(display_width, display_height), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

	if (input_enabled && ImGui::IsItemHovered()) {
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			ImVec2 const image_min = ImGui::GetItemRectMin();
			ImVec2 const image_max = ImGui::GetItemRectMax();
			ImVec2 const mouse = ImGui::GetMousePos();
			f32 const width = std::max(1.0f, image_max.x - image_min.x);
			f32 const height = std::max(1.0f, image_max.y - image_min.y);
			interaction.pick_requested = true;
			interaction.pick_uv.x = std::clamp((mouse.x - image_min.x) / width, 0.0f, 1.0f);
			interaction.pick_uv.y = std::clamp((mouse.y - image_min.y) / height, 0.0f, 1.0f);
		}
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
			interaction.fly_mode_requested = true;
		}
	}
	ImGui::End();

	ImGui::SetNextWindowPos(ImVec2(400, preview_height));
	ImGui::SetNextWindowSize(ImVec2(window_width, controls_height));
	ImGui::Begin("Preview Controls###PreviewControlsWindow", nullptr, window_flags);
	ImGui::Text("FPS: %.0f", ImGui::GetIO().Framerate);
	ImGui::Text("Renderer: cached SDF volume raymarch");
	ImGui::Text("Remeshed chunks: %u", renderer.marching_cubes_last_remeshed_chunk_count);
	ImGui::Text("Volume center: %.2f, %.2f, %.2f", renderer.marching_cubes_center.x,
		renderer.marching_cubes_center.y, renderer.marching_cubes_center.z);
	bool remesh_settings_changed = false;

	ImGui::SetNextItemWidth(140.0f);
	remesh_settings_changed |= ImGui::DragInt("Grid", &renderer.marching_cubes_grid_resolution, 1.0f, 8, 96);
	ImGui::SameLine();
	remesh_settings_changed |= ImGui::DragScalar(
		"Clipmap Levels", ImGuiDataType_U32, &renderer.clipmap_levels, 1.0f, nullptr, nullptr, "%u");

	ImGui::SameLine();
	remesh_settings_changed |= ImGui::Checkbox("Smooth Normals", &renderer.marching_cubes_smooth_normals);
	ImGui::SameLine();
	ImGui::Checkbox("Debug LOD Colors", &renderer.debug_show_lod_levels);

	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	remesh_settings_changed |=
		ImGui::DragFloat("Iso", &renderer.marching_cubes_iso_level, 0.005f, -2.0f, 2.0f, "%.3f");

	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	remesh_settings_changed |=
		ImGui::DragFloat("Bounds", &renderer.marching_cubes_bounds_extent, 0.05f, 0.5f, 10.0f, "%.2f");

	ImGui::SameLine();
	bool center_follow_changed = ImGui::Checkbox("Center On Camera", &renderer.marching_cubes_center_on_camera);
	if (renderer.marching_cubes_center_on_camera) {
		ImGui::SetNextItemWidth(140.0f);
		center_follow_changed |=
			ImGui::DragInt("Deadzone Chunks", &renderer.marching_cubes_center_deadzone_chunks, 1.0f, 1, 16);
	}
	ImGui::SetNextItemWidth(140.0f);
	ImGui::DragInt("Remesh Budget", &renderer.marching_cubes_remesh_budget_chunks, 1.0f, 1, 512);
	if (center_follow_changed) {
		sg_renderer_mark_all_chunks_dirty(renderer);
	}

	if (remesh_settings_changed) {
		sg_renderer_mark_all_chunks_dirty(renderer);
	}

	if (ImGui::Button("Reload Plugins")) {
		if (!sg_renderer_rebuild_plugin_shader(renderer, true)) {
			std::cerr << "[sg_renderer] plugin reload failed; keeping previous shader" << std::endl;
		} else {
			sg_renderer_mark_all_chunks_dirty(renderer);
		}
	}

	ImGui::SameLine();
	if (!renderer.plugin_reload_status.empty()) {
		ImGui::TextWrapped("%s", renderer.plugin_reload_status.c_str());
	}

	ImGui::End();
	return interaction;
}
