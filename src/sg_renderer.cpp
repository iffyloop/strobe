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
	renderer.marching_cubes_vertex_capacity = 0;
	renderer.marching_cubes_vertex_count = 0;
}

void sg_renderer_ensure_marching_cubes_buffers(sg_renderer_t& renderer) {
	u32 const cells_per_axis = std::max(1, renderer.marching_cubes_grid_resolution);
	u32 const num_cells = cells_per_axis * cells_per_axis * cells_per_axis;
	u32 const required_vertices = num_cells * k_marching_cubes_max_tris_per_cell * 3;
	if (required_vertices <= renderer.marching_cubes_vertex_capacity && renderer.marching_cubes_vertex_ssbo != 0 &&
		renderer.marching_cubes_counter_ssbo != 0) {
		return;
	}

	sg_renderer_destroy_marching_cubes_buffers(renderer);

	glGenBuffers(1, &renderer.marching_cubes_vertex_ssbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer.marching_cubes_vertex_ssbo);
	glBufferData(GL_SHADER_STORAGE_BUFFER,
		static_cast<GLsizeiptr>(required_vertices) * k_marching_cubes_vertex_stride_bytes, nullptr,
		GL_DYNAMIC_DRAW);

	glGenBuffers(1, &renderer.marching_cubes_counter_ssbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer.marching_cubes_counter_ssbo);
	GLuint const initial_counter = 0;
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), &initial_counter, GL_DYNAMIC_DRAW);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	renderer.marching_cubes_vertex_capacity = required_vertices;
	renderer.marching_cubes_vertex_count = 0;
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

	std::string compute_source;
	std::string build_error;
	if (!sg_plugins_build_marching_cubes_compute_source(candidate_plugins, compute_source, build_error)) {
		renderer.plugin_reload_status = "Shader generation failed: " + build_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}

	GLuint new_program = 0;
	std::string compile_error;
	if (!gl_shader_try_build_compute_program_from_source(
		    new_program, "generated/marching_cubes_plugins.comp", compute_source, compile_error)) {
		renderer.plugin_reload_status = "Shader compile/link failed: " + compile_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}

	if (renderer.marching_cubes_program != 0) {
		glDeleteProgram(renderer.marching_cubes_program);
	}
	renderer.marching_cubes_program = new_program;

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

	gl_render_pass_init(renderer.primary_render_pass, "shaders/mc_mesh.vert", "shaders/mc_mesh.frag", 2048.0f,
		2048.0f,
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

	glGenVertexArrays(1, &renderer.marching_cubes_vao);

	glGenBuffers(1, &renderer.program.buffer);
	glGenTextures(1, &renderer.program.texture);
	glGenBuffers(1, &renderer.primitive_meta.buffer);
	glGenTextures(1, &renderer.primitive_meta.texture);
	glGenBuffers(1, &renderer.primitive_params.buffer);
	glGenTextures(1, &renderer.primitive_params.texture);
	glGenBuffers(1, &renderer.primitive_scale.buffer);
	glGenTextures(1, &renderer.primitive_scale.texture);
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

void sg_renderer_update(
	sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene, fly_camera_t const& camera) {
	sg_renderer_upload_buffer_texture(renderer.program, compiled_scene.program, GL_RGBA32I);
	sg_renderer_upload_buffer_texture(renderer.combine_params, compiled_scene.combine_params, GL_RGBA32F);
	sg_renderer_upload_buffer_texture(renderer.primitive_meta, compiled_scene.primitive_meta, GL_RGBA32I);
	sg_renderer_upload_buffer_texture(renderer.primitive_params, compiled_scene.primitive_params, GL_RGBA32F);
	sg_renderer_upload_buffer_texture(renderer.primitive_scale, compiled_scene.primitive_scale, GL_RGBA32F);
	sg_renderer_upload_buffer_texture(
		renderer.primitive_effect_ranges, compiled_scene.primitive_effect_ranges, GL_RGBA32I);
	sg_renderer_upload_buffer_texture(renderer.effect_meta, compiled_scene.effect_meta, GL_RGBA32I);
	sg_renderer_upload_buffer_texture(renderer.effect_params, compiled_scene.effect_params, GL_RGBA32F);

	renderer.marching_cubes_vertex_count = 0;
	if (compiled_scene.has_output && renderer.marching_cubes_program != 0) {
		sg_renderer_ensure_marching_cubes_buffers(renderer);

		glUseProgram(renderer.marching_cubes_program);
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_has_scene"),
			compiled_scene.has_output ? 1 : 0);
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_program_count"),
			static_cast<GLint>(compiled_scene.program.size()));
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_max_stack_depth"),
			static_cast<GLint>(compiled_scene.max_stack_depth));
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_op_push_primitive"),
			SG_COMPILER_OP_PUSH_PRIMITIVE);
		glUniform1i(
			glGetUniformLocation(renderer.marching_cubes_program, "u_op_combine"), SG_COMPILER_OP_COMBINE);
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_combine_union"),
			sg_runtime_id_or_zero("union"));
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_combine_intersect"),
			sg_runtime_id_or_zero("intersect"));
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_combine_subtract"),
			sg_runtime_id_or_zero("subtract"));
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_grid_resolution"),
			renderer.marching_cubes_grid_resolution);
		glUniform1i(glGetUniformLocation(renderer.marching_cubes_program, "u_smooth_normals"),
			renderer.marching_cubes_smooth_normals ? 1 : 0);
		glUniform1f(glGetUniformLocation(renderer.marching_cubes_program, "u_iso_level"),
			renderer.marching_cubes_iso_level);
		glUniform3f(glGetUniformLocation(renderer.marching_cubes_program, "u_bounds_min"),
			-renderer.marching_cubes_bounds_extent, -renderer.marching_cubes_bounds_extent,
			-renderer.marching_cubes_bounds_extent);
		glUniform3f(glGetUniformLocation(renderer.marching_cubes_program, "u_bounds_max"),
			renderer.marching_cubes_bounds_extent, renderer.marching_cubes_bounds_extent,
			renderer.marching_cubes_bounds_extent);
		glUniform1ui(glGetUniformLocation(renderer.marching_cubes_program, "u_vertex_capacity"),
			renderer.marching_cubes_vertex_capacity);
		glUniform1f(glGetUniformLocation(renderer.marching_cubes_program, "u_normal_epsilon"), 0.001f);

		sg_renderer_bind_buffer_texture(
			renderer.marching_cubes_program, "u_program_tex", renderer.program.texture, GL_TEXTURE0);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_combine_params_tex",
			renderer.combine_params.texture, GL_TEXTURE1);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_primitive_meta_tex",
			renderer.primitive_meta.texture, GL_TEXTURE2);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_primitive_params_tex",
			renderer.primitive_params.texture, GL_TEXTURE3);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_primitive_scale_tex",
			renderer.primitive_scale.texture, GL_TEXTURE4);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_primitive_effect_range_tex",
			renderer.primitive_effect_ranges.texture, GL_TEXTURE5);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_effect_meta_tex",
			renderer.effect_meta.texture, GL_TEXTURE6);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_effect_params_tex",
			renderer.effect_params.texture, GL_TEXTURE7);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_mc_edge_table_tex",
			renderer.marching_cubes_edge_table.texture, GL_TEXTURE8);
		sg_renderer_bind_buffer_texture(renderer.marching_cubes_program, "u_mc_tri_table_tex",
			renderer.marching_cubes_tri_table.texture, GL_TEXTURE9);

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, renderer.marching_cubes_vertex_ssbo);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, renderer.marching_cubes_counter_ssbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer.marching_cubes_counter_ssbo);
		GLuint const reset_counter = 0;
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &reset_counter);

		u32 const groups = static_cast<u32>((renderer.marching_cubes_grid_resolution +
							    static_cast<s32>(k_marching_cubes_threads_per_axis) - 1) /
			k_marching_cubes_threads_per_axis);
		glDispatchCompute(groups, groups, groups);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

		GLuint generated_vertices = 0;
		glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &generated_vertices);
		renderer.marching_cubes_vertex_count =
			std::min(generated_vertices, renderer.marching_cubes_vertex_capacity);

		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	}

	gl_render_pass_begin(renderer.primary_render_pass);
	gl_render_pass_uniform_mat4(renderer.primary_render_pass, "u_view_proj", camera.proj_mat * camera.view_mat);
	GLint const camera_pos_location =
		glGetUniformLocation(renderer.primary_render_pass.shader.program, "u_camera_pos");
	if (camera_pos_location >= 0) {
		glUniform3fv(camera_pos_location, 1, glm::value_ptr(camera.pos));
	}

	glBindVertexArray(renderer.marching_cubes_vao);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, renderer.marching_cubes_vertex_ssbo);
	if (renderer.marching_cubes_vertex_count > 0) {
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(renderer.marching_cubes_vertex_count));
	}
	glBindVertexArray(0);
	gl_render_pass_end(renderer.primary_render_pass);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

bool sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled) {
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

	bool const clicked = input_enabled && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	ImGui::End();

	ImGui::SetNextWindowPos(ImVec2(400, preview_height));
	ImGui::SetNextWindowSize(ImVec2(window_width, controls_height));
	ImGui::Begin("Preview Controls###PreviewControlsWindow", nullptr, window_flags);
	ImGui::Text("FPS: %.0f", ImGui::GetIO().Framerate);
	ImGui::Text("Generated vertices: %u", renderer.marching_cubes_vertex_count);

	ImGui::SetNextItemWidth(140.0f);
	ImGui::DragInt("Grid", &renderer.marching_cubes_grid_resolution, 1.0f, 8, 96);

	ImGui::SameLine();
	ImGui::Checkbox("Smooth Normals", &renderer.marching_cubes_smooth_normals);

	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::DragFloat("Iso", &renderer.marching_cubes_iso_level, 0.005f, -2.0f, 2.0f, "%.3f");

	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	ImGui::DragFloat("Bounds", &renderer.marching_cubes_bounds_extent, 0.05f, 0.5f, 50.0f, "%.2f");

	if (ImGui::Button("Reload Plugins")) {
		if (!sg_renderer_rebuild_plugin_shader(renderer, true)) {
			std::cerr << "[sg_renderer] plugin reload failed; keeping previous shader" << std::endl;
		}
	}

	ImGui::SameLine();
	if (!renderer.plugin_reload_status.empty()) {
		ImGui::TextWrapped("%s", renderer.plugin_reload_status.c_str());
	}

	ImGui::End();
	return clicked;
}
