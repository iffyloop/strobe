#include "pch.h"

#include "sg_renderer.h"

#include "sg_plugins.h"

namespace {

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

void sg_renderer_bind_buffer_texture(
	gl_render_pass_t const& render_pass, char const* uniform_name, GLuint texture, GLenum texture_unit) {
	gl_render_pass_uniform_int(render_pass, uniform_name, texture_unit - GL_TEXTURE0);
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
		magenta[0], magenta[1], magenta[2], black[0], black[1], black[2],
		black[0],   black[1],   black[2],   magenta[0], magenta[1], magenta[2],
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

	std::string frag_source;
	std::string build_error;
	if (!sg_plugins_build_raymarch_fragment_source(candidate_plugins, frag_source, build_error)) {
		renderer.plugin_reload_status = "Shader generation failed: " + build_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}

	GLuint new_program = 0;
	std::string compile_error;
	if (!gl_shader_try_build_program_from_file_and_source(
		    new_program, "shaders/quad.vert", "generated/raymarch_plugins.frag", frag_source, compile_error)) {
		renderer.plugin_reload_status = "Shader compile/link failed: " + compile_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}

	GLuint const old_program = renderer.primary_render_pass.shader.program;
	renderer.primary_render_pass.shader.program = new_program;
	if (old_program != 0) {
		glDeleteProgram(old_program);
	}

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

s32 sg_primitive_runtime_id_or_zero(std::string const& plugin_id) {
	auto const* def = sg_plugins_find_primitive_by_id(plugin_id);
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

	gl_render_pass_init(renderer.primary_render_pass, "shaders/quad.vert", "shaders/raymarch.frag", 1024.0f,
		1024.0f,
		{
			{"o_color", {.internal_format = GL_RGBA8, .format = GL_RGBA, .type = GL_UNSIGNED_BYTE}},
		});
	bool const plugin_shader_ok = sg_renderer_rebuild_plugin_shader(renderer, false);
	assert_release(plugin_shader_ok);

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

	if (renderer.checker_texture != 0) {
		glDeleteTextures(1, &renderer.checker_texture);
		renderer.checker_texture = 0;
	}
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

	gl_render_pass_begin(renderer.primary_render_pass);

	glm::mat4 const inv_view_proj = glm::inverse(camera.proj_mat * camera.view_mat);
	gl_render_pass_uniform_mat4(renderer.primary_render_pass, "u_inv_view_proj", inv_view_proj);

	GLint const has_scene = compiled_scene.has_output ? 1 : 0;
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_has_scene", has_scene);
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_op_push_primitive", SG_COMPILER_OP_PUSH_PRIMITIVE);
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_op_combine", SG_COMPILER_OP_COMBINE);
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_combine_union", sg_runtime_id_or_zero("union"));
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_combine_intersect", sg_runtime_id_or_zero("intersect"));
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_combine_subtract", sg_runtime_id_or_zero("subtract"));
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_primitive_cube", sg_primitive_runtime_id_or_zero("cube"));
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_primitive_sphere", sg_primitive_runtime_id_or_zero("sphere"));
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_program_count", static_cast<GLint>(compiled_scene.program.size()));
	gl_render_pass_uniform_int(
		renderer.primary_render_pass, "u_max_stack_depth", static_cast<GLint>(compiled_scene.max_stack_depth));
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_max_steps", renderer.preview_max_steps);
	gl_render_pass_uniform_float(
		renderer.primary_render_pass, "u_surface_epsilon", renderer.preview_surface_epsilon);
	gl_render_pass_uniform_float(renderer.primary_render_pass, "u_max_trace_dist", renderer.preview_max_trace_dist);

	GLint const camera_pos_location =
		glGetUniformLocation(renderer.primary_render_pass.shader.program, "u_camera_pos");
	assert_release(camera_pos_location >= 0);
	glUniform3fv(camera_pos_location, 1, glm::value_ptr(camera.pos));

	sg_renderer_bind_buffer_texture(
		renderer.primary_render_pass, "u_program_tex", renderer.program.texture, GL_TEXTURE0);
	sg_renderer_bind_buffer_texture(
		renderer.primary_render_pass, "u_combine_params_tex", renderer.combine_params.texture, GL_TEXTURE1);
	sg_renderer_bind_buffer_texture(
		renderer.primary_render_pass, "u_primitive_meta_tex", renderer.primitive_meta.texture, GL_TEXTURE2);
	sg_renderer_bind_buffer_texture(
		renderer.primary_render_pass, "u_primitive_params_tex", renderer.primitive_params.texture, GL_TEXTURE3);
	sg_renderer_bind_buffer_texture(
		renderer.primary_render_pass, "u_primitive_scale_tex", renderer.primitive_scale.texture, GL_TEXTURE4);
	sg_renderer_bind_buffer_texture(renderer.primary_render_pass, "u_primitive_effect_range_tex",
		renderer.primitive_effect_ranges.texture, GL_TEXTURE5);
	sg_renderer_bind_buffer_texture(
		renderer.primary_render_pass, "u_effect_meta_tex", renderer.effect_meta.texture, GL_TEXTURE6);
	sg_renderer_bind_buffer_texture(
		renderer.primary_render_pass, "u_effect_params_tex", renderer.effect_params.texture, GL_TEXTURE7);
	gl_render_pass_uniform_texture(renderer.primary_render_pass, "u_texture0", renderer.checker_texture, GL_TEXTURE8);

	gl_render_pass_draw(renderer.primary_render_pass, renderer.quad_vbo);
	gl_render_pass_end(renderer.primary_render_pass);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

bool sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled) {
	f32 const window_width = ImGui::GetIO().DisplaySize.x - 400.0f;
	f32 const window_height = ImGui::GetIO().DisplaySize.y;
	f32 const controls_height = 120.0f;
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

	ImGui::SetNextItemWidth(110.0f);
	ImGui::DragInt("Steps", &renderer.preview_max_steps, 1.0f, 8, 256);

	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::DragFloat("Epsilon", &renderer.preview_surface_epsilon, 0.0001f, 0.00005f, 0.05f, "%.5f");

	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	ImGui::DragFloat("Max Dist", &renderer.preview_max_trace_dist, 0.25f, 1.0f, 5000.0f, "%.2f");

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
