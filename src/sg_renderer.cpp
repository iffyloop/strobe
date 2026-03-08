#include "pch.h"

#include "sg_renderer.h"

#include "sg_plugins.h"

namespace {

constexpr bool k_preload_diffusion_at_launch = false;
constexpr bool k_diffusion_worker_verbose_logs = false;

ImVec4 const k_diffusion_status_color = ImVec4(0.95f, 0.75f, 0.2f, 1.0f);

sg_diffusion_resolution_t default_resolution_for_model(sg_diffusion_model_t const model) {
	return (model == sg_diffusion_model_t::SD15) ? sg_diffusion_resolution_t::R512x512
						   : sg_diffusion_resolution_t::R1024x1024;
}

diffusion_model_t diffusion_renderer_model(sg_diffusion_model_t const model) {
	return model == sg_diffusion_model_t::SD15 ? diffusion_model_t::SD15 : diffusion_model_t::SDXL;
}

void diffusion_resolution_dimensions(sg_diffusion_resolution_t const resolution, s32& out_width, s32& out_height) {
	switch (resolution) {
	case sg_diffusion_resolution_t::R512x512:
		out_width = 512;
		out_height = 512;
		break;
	case sg_diffusion_resolution_t::R1024x1024:
		out_width = 1024;
		out_height = 1024;
		break;
	case sg_diffusion_resolution_t::R1344x768:
		out_width = 1344;
		out_height = 768;
		break;
	default:
		out_width = 1024;
		out_height = 1024;
		break;
	}
}

char const* diffusion_model_label(sg_diffusion_model_t const model) {
	return model == sg_diffusion_model_t::SD15 ? "SD 1.5" : "SDXL";
}

char const* diffusion_resolution_label(sg_diffusion_resolution_t const resolution) {
	switch (resolution) {
	case sg_diffusion_resolution_t::R512x512:
		return "512x512";
	case sg_diffusion_resolution_t::R1024x1024:
		return "1024x1024";
	case sg_diffusion_resolution_t::R1344x768:
		return "1344x768";
	default:
		return "1024x1024";
	}
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

void sg_renderer_bind_buffer_texture(
	gl_render_pass_t const& render_pass, char const* uniform_name, GLuint texture, GLenum texture_unit) {
	gl_render_pass_uniform_int(render_pass, uniform_name, texture_unit - GL_TEXTURE0);
	glActiveTexture(texture_unit);
	glBindTexture(GL_TEXTURE_BUFFER, texture);
}

void sg_renderer_draw_quad_with_program(gl_shader_t const& shader, gl_vertex_buffers_t const& vertex_buffers) {
	assert_release(vertex_buffers.vao);
	glBindVertexArray(vertex_buffers.vao);

	for (auto const& [name, buffer] : vertex_buffers.buffers) {
		GLint location = glGetAttribLocation(shader.program, name.c_str());
		if (location < 0) {
			continue;
		}
		glBindBuffer(GL_ARRAY_BUFFER, buffer.vbo);
		glEnableVertexAttribArray(location);
		glVertexAttribPointer(location, buffer.size, buffer.type, buffer.normalized, 0, 0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	GLuint const num_vertices = vertex_buffers.buffers.begin()->second.num_vertices;
	glDrawArrays(vertex_buffers.mode, 0, num_vertices);

	glBindVertexArray(0);
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

void sg_renderer_reap_diffusion_worker(sg_renderer_t& renderer) {
	if (!renderer.diffusion_worker.joinable()) {
		return;
	}

	bool is_running = false;
	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		is_running = renderer.diffusion_worker_running;
	}

	if (!is_running) {
		renderer.diffusion_worker.join();
		renderer.diffusion_worker_stopping = false;
	}
}

void sg_renderer_request_stop_diffusion_worker(sg_renderer_t& renderer) {
	if (!renderer.diffusion_worker.joinable()) {
		{
			std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
			renderer.diffusion_transition = sg_diffusion_transition_t::None;
		}
		renderer.diffusion_worker_stopping = false;
		return;
	}

	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_stop_requested = true;
		renderer.diffusion_ready = false;
		renderer.diffusion_has_pending_control = false;
		renderer.diffusion_pending_control_depth.clear();
		renderer.diffusion_transition = sg_diffusion_transition_t::Unloading;
	}
	renderer.diffusion_cv.notify_one();
	renderer.diffusion_worker_stopping = true;
}

void sg_renderer_upload_diffusion_texture_if_available(sg_renderer_t& renderer) {
	std::vector<u8> output_pixels;
	s32 output_width = 0;
	s32 output_height = 0;
	s32 output_channels = 0;

	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		if (!renderer.diffusion_has_output) {
			return;
		}

		output_pixels = std::move(renderer.diffusion_output_pixels);
		output_width = renderer.diffusion_output_width;
		output_height = renderer.diffusion_output_height;
		output_channels = renderer.diffusion_output_channels;
		renderer.diffusion_has_output = false;
	}

	if (output_pixels.empty() || output_width <= 0 || output_height <= 0) {
		return;
	}

	std::vector<u8> flipped_pixels;
	flipped_pixels.resize(output_pixels.size());
	size_t const row_bytes = static_cast<size_t>(output_width) * static_cast<size_t>(std::max(1, output_channels));
	for (s32 y = 0; y < output_height; ++y) {
		size_t const src_offset = static_cast<size_t>(y) * row_bytes;
		size_t const dst_offset = static_cast<size_t>(output_height - 1 - y) * row_bytes;
		std::memcpy(flipped_pixels.data() + dst_offset, output_pixels.data() + src_offset, row_bytes);
	}

	if (renderer.diffusion_output_texture == 0) {
		glGenTextures(1, &renderer.diffusion_output_texture);
	}

	GLenum texture_format = GL_RGBA;
	GLenum texture_internal_format = GL_RGBA8;
	if (output_channels == 3) {
		texture_format = GL_RGB;
		texture_internal_format = GL_RGB8;
	}

	glBindTexture(GL_TEXTURE_2D, renderer.diffusion_output_texture);
	if (renderer.diffusion_output_texture_width != output_width ||
		renderer.diffusion_output_texture_height != output_height ||
		renderer.diffusion_output_texture_channels != output_channels) {
		glTexImage2D(GL_TEXTURE_2D, 0, texture_internal_format, output_width, output_height, 0, texture_format,
			GL_UNSIGNED_BYTE, flipped_pixels.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		renderer.diffusion_output_texture_width = output_width;
		renderer.diffusion_output_texture_height = output_height;
		renderer.diffusion_output_texture_channels = output_channels;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, output_width, output_height, texture_format, GL_UNSIGNED_BYTE,
			flipped_pixels.data());
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	renderer.diffusion_output_texture_ready = true;
}

void sg_renderer_submit_depth_to_diffusion_worker(sg_renderer_t& renderer) {
	if (!renderer.diffusion_enabled || !renderer.diffusion_worker.joinable()) {
		return;
	}

	auto const now = std::chrono::steady_clock::now();
	if (renderer.diffusion_last_submit_time != std::chrono::steady_clock::time_point::min() &&
		now - renderer.diffusion_last_submit_time < renderer.diffusion_submit_cooldown) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		if (!renderer.diffusion_worker_running || !renderer.diffusion_ready ||
			renderer.diffusion_stop_requested || renderer.diffusion_failed || renderer.diffusion_busy ||
			renderer.diffusion_has_pending_control) {
			return;
		}
	}

	auto const& depth_output = renderer.primary_render_pass.internal_output_descriptors["o_color"];
	s32 const width = static_cast<s32>(renderer.primary_render_pass.width);
	s32 const height = static_cast<s32>(renderer.primary_render_pass.height);
	std::vector<f32> depth_rgba;
	depth_rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
	glBindTexture(GL_TEXTURE_2D, depth_output.texture);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, depth_rgba.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	f32 min_depth = std::numeric_limits<f32>::max();
	f32 max_depth = std::numeric_limits<f32>::lowest();
	for (size_t i = 0; i < static_cast<size_t>(width) * static_cast<size_t>(height); ++i) {
		f32 const alpha = depth_rgba[i * 4 + 3];
		if (alpha <= 0.5f) {
			continue;
		}
		f32 const depth01 = std::clamp(depth_rgba[i * 4 + 0], 0.0f, 1.0f);
		min_depth = std::min(min_depth, depth01);
		max_depth = std::max(max_depth, depth01);
	}
	bool const has_valid_depth = min_depth < max_depth;
	f32 const depth_span = has_valid_depth ? (max_depth - min_depth) : 1.0f;

	std::vector<f32> depth_values;
	depth_values.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
	for (s32 y = 0; y < height; ++y) {
		s32 const src_y = height - 1 - y;
		for (s32 x = 0; x < width; ++x) {
			size_t const src_index =
				(static_cast<size_t>(src_y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
			size_t const dst_index =
				static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
			f32 const depth01 = std::clamp(depth_rgba[src_index + 0], 0.0f, 1.0f);
			f32 const alpha = depth_rgba[src_index + 3];
			bool const is_hit = alpha > 0.5f;
			if (!is_hit) {
				depth_values[dst_index] = 0.0f;
				continue;
			}
			if (!has_valid_depth) {
				depth_values[dst_index] = 1.0f;
				continue;
			}
			f32 const normalized = std::clamp((depth01 - min_depth) / depth_span, 0.0f, 1.0f);
			depth_values[dst_index] = 1.0f - normalized;
		}
	}

	bool submitted = false;
	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		if (renderer.diffusion_worker_running && renderer.diffusion_ready &&
			!renderer.diffusion_stop_requested && !renderer.diffusion_busy &&
			!renderer.diffusion_has_pending_control) {
			renderer.diffusion_pending_control_depth = std::move(depth_values);
			renderer.diffusion_pending_control_width = width;
			renderer.diffusion_pending_control_height = height;
			renderer.diffusion_has_pending_control = true;
			submitted = true;
		}
	}

	if (submitted) {
		renderer.diffusion_last_submit_time = now;
		renderer.diffusion_cv.notify_one();
	}
}

void sg_renderer_diffusion_worker_main(
	sg_renderer_t& renderer, diffusion_model_t const model, s32 target_width, s32 target_height) {
	diffusion_renderer_t diffusion_renderer;
	bool const setup_ok = diffusion_renderer_setup(diffusion_renderer, model, target_width, target_height);

	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_ready = setup_ok;
		renderer.diffusion_busy = false;
		renderer.diffusion_failed = !setup_ok;
		renderer.diffusion_transition = sg_diffusion_transition_t::None;
		if (!setup_ok) {
			renderer.diffusion_stop_requested = true;
		}
	}
	renderer.diffusion_cv.notify_all();

	if (!setup_ok) {
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_worker_running = false;
		renderer.diffusion_ready = false;
		renderer.diffusion_busy = false;
		renderer.diffusion_transition = sg_diffusion_transition_t::None;
		renderer.diffusion_has_pending_control = false;
		renderer.diffusion_pending_control_depth.clear();
		renderer.diffusion_cv.notify_all();
		return;
	}

	while (true) {
		std::vector<f32> depth_values;
		s32 depth_width = 0;
		s32 depth_height = 0;
		std::string prompt;
		std::string negative_prompt;
		f32 control_strength = 0.5f;
		s64 seed = 1;

		{
			std::unique_lock<std::mutex> lock(renderer.diffusion_mutex);
			renderer.diffusion_cv.wait(lock, [&renderer]() {
				return renderer.diffusion_stop_requested || renderer.diffusion_has_pending_control;
			});

			if (renderer.diffusion_stop_requested) {
				break;
			}

			depth_values = std::move(renderer.diffusion_pending_control_depth);
			depth_width = renderer.diffusion_pending_control_width;
			depth_height = renderer.diffusion_pending_control_height;
			prompt = renderer.diffusion_prompt;
			negative_prompt = renderer.diffusion_negative_prompt;
			control_strength = renderer.diffusion_control_strength;
			seed = renderer.diffusion_seed;
			renderer.diffusion_has_pending_control = false;
			renderer.diffusion_busy = true;
		}

		std::vector<u8> output_pixels;
		s32 output_width = 0;
		s32 output_height = 0;
		s32 output_channels = 0;
		diffusion_renderer.control_strength = control_strength;
		auto const render_start = std::chrono::steady_clock::now();
		bool const ok = diffusion_renderer_render(diffusion_renderer, depth_values, depth_width, depth_height,
			prompt, negative_prompt, seed, output_pixels, output_width, output_height, output_channels);
		auto const render_end = std::chrono::steady_clock::now();
		auto const render_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(render_end - render_start).count();
		if constexpr (k_diffusion_worker_verbose_logs) {
			std::cerr << "[diffusion] render finished ok=" << (ok ? 1 : 0) << " in " << render_ms << " ms"
				  << std::endl;
		}

		{
			std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
			renderer.diffusion_busy = false;
			if (!renderer.diffusion_stop_requested && ok) {
				renderer.diffusion_output_pixels = std::move(output_pixels);
				renderer.diffusion_output_width = output_width;
				renderer.diffusion_output_height = output_height;
				renderer.diffusion_output_channels = output_channels;
				renderer.diffusion_has_output = true;
				renderer.diffusion_failed = false;
			} else if (!ok) {
				renderer.diffusion_failed = true;
				renderer.diffusion_stop_requested = true;
			}
		}
	}

	diffusion_renderer_destroy(diffusion_renderer);

	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_worker_running = false;
		renderer.diffusion_ready = false;
		renderer.diffusion_busy = false;
		renderer.diffusion_transition = sg_diffusion_transition_t::None;
		renderer.diffusion_has_pending_control = false;
		renderer.diffusion_pending_control_depth.clear();
	}
	renderer.diffusion_cv.notify_all();
}

void sg_renderer_start_diffusion_worker(sg_renderer_t& renderer) {
	sg_renderer_reap_diffusion_worker(renderer);
	if (renderer.diffusion_worker.joinable()) {
		return;
	}

	s32 target_width = 1024;
	s32 target_height = 1024;
	diffusion_resolution_dimensions(renderer.diffusion_resolution, target_width, target_height);
	diffusion_model_t const model = diffusion_renderer_model(renderer.diffusion_model);

	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_stop_requested = false;
		renderer.diffusion_worker_running = true;
		renderer.diffusion_ready = false;
		renderer.diffusion_busy = false;
		renderer.diffusion_failed = false;
		renderer.diffusion_has_pending_control = false;
		renderer.diffusion_pending_control_depth.clear();
		renderer.diffusion_transition = sg_diffusion_transition_t::Loading;
	}

	renderer.diffusion_output_texture_ready = false;
	renderer.diffusion_worker_stopping = false;
	renderer.diffusion_worker = std::thread(
		sg_renderer_diffusion_worker_main, std::ref(renderer), model, target_width, target_height);
}

void sg_renderer_preload_diffusion_worker(sg_renderer_t& renderer) {
	if (!k_preload_diffusion_at_launch) {
		return;
	}

	sg_renderer_start_diffusion_worker(renderer);
}

GLuint sg_renderer_run_depth_reduce(sg_renderer_t& renderer, GLuint raw_depth_texture) {
	assert_release(renderer.depth_reduce_texture_a != 0);
	assert_release(renderer.depth_reduce_texture_b != 0);
	assert_release(renderer.depth_reduce_fbo != 0);

	GLuint const width = renderer.primary_render_pass.width;
	GLuint const height = renderer.primary_render_pass.height;

	glBindFramebuffer(GL_FRAMEBUFFER, renderer.depth_reduce_fbo);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderer.depth_reduce_texture_a, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return 0;
	}
	glViewport(0, 0, width, height);

	GLint reduce_src_tex_location = glGetUniformLocation(renderer.depth_reduce_shader.program, "u_src_tex");
	GLint reduce_src_size_location = glGetUniformLocation(renderer.depth_reduce_shader.program, "u_src_size");
	assert_release(reduce_src_tex_location >= 0);
	assert_release(reduce_src_size_location >= 0);
	glUseProgram(renderer.depth_reduce_shader.program);
	glUniform1i(reduce_src_tex_location, 0);
	glActiveTexture(GL_TEXTURE0);

	GLuint src_tex = raw_depth_texture;
	GLuint dst_tex = renderer.depth_reduce_texture_a;
	GLuint src_width = width;
	GLuint src_height = height;

	while (src_width > 1 || src_height > 1) {
		GLuint const dst_width = std::max(1u, src_width >> 1);
		GLuint const dst_height = std::max(1u, src_height >> 1);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			break;
		}
		glViewport(0, 0, dst_width, dst_height);
		glUniform2i(reduce_src_size_location, static_cast<GLint>(src_width), static_cast<GLint>(src_height));
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_tex);
		sg_renderer_draw_quad_with_program(renderer.depth_reduce_shader, renderer.quad_vbo);

		src_tex = dst_tex;
		dst_tex = (dst_tex == renderer.depth_reduce_texture_a) ? renderer.depth_reduce_texture_b
								       : renderer.depth_reduce_texture_a;
		src_width = dst_width;
		src_height = dst_height;
	}

	glUseProgram(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return src_tex;
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
			{"o_color", {.internal_format = GL_RGBA32F, .format = GL_RGBA, .type = GL_FLOAT}},
		});
	bool const plugin_shader_ok = sg_renderer_rebuild_plugin_shader(renderer, false);
	assert_release(plugin_shader_ok);
	gl_render_pass_init(renderer.depth_normalize_render_pass, "shaders/quad.vert", "shaders/depth_normalize.frag",
		1024.0f, 1024.0f,
		{
			{"o_color", {.internal_format = GL_RGBA8, .format = GL_RGBA, .type = GL_UNSIGNED_BYTE}},
		});

	gl_shader_init(renderer.depth_reduce_shader, "shaders/quad.vert", "shaders/depth_reduce.frag");

	GLuint const width = renderer.primary_render_pass.width;
	GLuint const height = renderer.primary_render_pass.height;

	glGenTextures(1, &renderer.depth_reduce_texture_a);
	glBindTexture(GL_TEXTURE_2D, renderer.depth_reduce_texture_a);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &renderer.depth_reduce_texture_b);
	glBindTexture(GL_TEXTURE_2D, renderer.depth_reduce_texture_b);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glGenFramebuffers(1, &renderer.depth_reduce_fbo);

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

	renderer.diffusion_seed =
		static_cast<s64>(std::chrono::high_resolution_clock::now().time_since_epoch().count());

	sg_renderer_preload_diffusion_worker(renderer);
}

void sg_renderer_destroy(sg_renderer_t& renderer) {
	sg_renderer_request_stop_diffusion_worker(renderer);
	if (renderer.diffusion_worker.joinable()) {
		renderer.diffusion_worker.join();
	}
	renderer.diffusion_worker_stopping = false;

	if (renderer.diffusion_output_texture != 0) {
		glDeleteTextures(1, &renderer.diffusion_output_texture);
		renderer.diffusion_output_texture = 0;
	}
}

void sg_renderer_update(
	sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene, fly_camera_t const& camera) {
	sg_renderer_reap_diffusion_worker(renderer);
	if (renderer.diffusion_start_requested && renderer.diffusion_enabled && !renderer.diffusion_worker.joinable() &&
		!renderer.diffusion_worker_stopping) {
		renderer.diffusion_start_requested = false;
		sg_renderer_start_diffusion_worker(renderer);
	}
	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		if (renderer.diffusion_failed && !renderer.diffusion_worker.joinable()) {
			renderer.diffusion_enabled = false;
			renderer.diffusion_start_requested = false;
			renderer.diffusion_output_texture_ready = false;
			renderer.diffusion_transition = sg_diffusion_transition_t::None;
		}
	}

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

	glm::mat4 const view_proj = camera.proj_mat * camera.view_mat;
	glm::mat4 const inv_view_proj = glm::inverse(camera.proj_mat * camera.view_mat);
	gl_render_pass_uniform_mat4(renderer.primary_render_pass, "u_view_proj", view_proj);
	gl_render_pass_uniform_mat4(renderer.primary_render_pass, "u_inv_view_proj", inv_view_proj);

	GLint const has_scene = compiled_scene.has_output ? 1 : 0;
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_has_scene", has_scene);
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_op_push_primitive", SG_COMPILER_OP_PUSH_PRIMITIVE);
	gl_render_pass_uniform_int(renderer.primary_render_pass, "u_op_combine", SG_COMPILER_OP_COMBINE);
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

	gl_render_pass_draw(renderer.primary_render_pass, renderer.quad_vbo);
	gl_render_pass_end(renderer.primary_render_pass);

	GLuint const raw_depth_texture = renderer.primary_render_pass.internal_output_descriptors["o_color"].texture;
	GLuint const reduced_texture = sg_renderer_run_depth_reduce(renderer, raw_depth_texture);

	gl_render_pass_begin(renderer.depth_normalize_render_pass);
	gl_render_pass_uniform_texture(
		renderer.depth_normalize_render_pass, "u_raw_depth_tex", raw_depth_texture, GL_TEXTURE5);
	gl_render_pass_uniform_texture(
		renderer.depth_normalize_render_pass, "u_reduce_tex", reduced_texture, GL_TEXTURE6);
	gl_render_pass_uniform_int(
		renderer.depth_normalize_render_pass, "u_invert", renderer.preview_depth_invert ? 1 : 0);
	gl_render_pass_uniform_float(
		renderer.depth_normalize_render_pass, "u_depth_span_epsilon", renderer.preview_depth_span_epsilon);
	gl_render_pass_draw(renderer.depth_normalize_render_pass, renderer.quad_vbo);
	gl_render_pass_end(renderer.depth_normalize_render_pass);

	sg_renderer_upload_diffusion_texture_if_available(renderer);
	sg_renderer_submit_depth_to_diffusion_worker(renderer);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

bool sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled) {
	f32 const window_width = ImGui::GetIO().DisplaySize.x - 400.0f;
	f32 const window_height = ImGui::GetIO().DisplaySize.y;
	f32 const controls_height = 240.0f;
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
	GLuint display_texture = renderer.depth_normalize_render_pass.internal_output_descriptors["o_color"].texture;
	f32 image_aspect = static_cast<f32>(renderer.primary_render_pass.width) /
		static_cast<f32>(renderer.primary_render_pass.height);
	if (renderer.diffusion_enabled && renderer.diffusion_show_output && renderer.diffusion_output_texture_ready &&
		renderer.diffusion_output_texture != 0 && renderer.diffusion_output_texture_width > 0 &&
		renderer.diffusion_output_texture_height > 0) {
		display_texture = renderer.diffusion_output_texture;
		image_aspect = static_cast<f32>(renderer.diffusion_output_texture_width) /
			static_cast<f32>(renderer.diffusion_output_texture_height);
	}
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

	if (!renderer.diffusion_prompt_buffers_initialized) {
		std::snprintf(renderer.diffusion_prompt_buffer.data(), renderer.diffusion_prompt_buffer.size(), "%s",
			renderer.diffusion_prompt.c_str());
		std::snprintf(renderer.diffusion_negative_prompt_buffer.data(), renderer.diffusion_negative_prompt_buffer.size(),
			"%s", renderer.diffusion_negative_prompt.c_str());
		renderer.diffusion_prompt_buffers_initialized = true;
	}

	bool diffusion_failed = false;
	sg_diffusion_transition_t diffusion_transition = sg_diffusion_transition_t::None;
	{
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		diffusion_failed = renderer.diffusion_failed;
		diffusion_transition = renderer.diffusion_transition;
	}

	bool const diffusion_transitioning = diffusion_transition != sg_diffusion_transition_t::None;
	if (diffusion_transition == sg_diffusion_transition_t::Loading) {
		ImGui::TextColored(k_diffusion_status_color, "Loading diffusion model...");
	} else if (diffusion_transition == sg_diffusion_transition_t::Unloading) {
		ImGui::TextColored(k_diffusion_status_color, "Unloading diffusion model...");
	}

	ImGui::BeginDisabled(diffusion_transitioning);
	bool enable_diffusion = renderer.diffusion_enabled;
	if (ImGui::Checkbox("Enable Diffusion", &enable_diffusion)) {
		if (enable_diffusion) {
			renderer.diffusion_enabled = true;
			renderer.diffusion_show_output = true;
			renderer.diffusion_start_requested = true;
			if (!renderer.diffusion_worker.joinable() && !renderer.diffusion_worker_stopping) {
				renderer.diffusion_start_requested = false;
				sg_renderer_start_diffusion_worker(renderer);
			}
		} else {
			renderer.diffusion_enabled = false;
			renderer.diffusion_start_requested = false;
			renderer.diffusion_output_texture_ready = false;
			sg_renderer_request_stop_diffusion_worker(renderer);
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!renderer.diffusion_enabled);
	ImGui::Checkbox("Show Diffusion", &renderer.diffusion_show_output);
	ImGui::EndDisabled();

	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::BeginCombo("Model", diffusion_model_label(renderer.diffusion_model))) {
		for (s32 i = 0; i < 2; ++i) {
			sg_diffusion_model_t const model = i == 0 ? sg_diffusion_model_t::SDXL : sg_diffusion_model_t::SD15;
			bool const selected = renderer.diffusion_model == model;
			if (ImGui::Selectable(diffusion_model_label(model), selected)) {
				renderer.diffusion_model = model;
				renderer.diffusion_resolution = default_resolution_for_model(model);
				if (renderer.diffusion_enabled) {
					renderer.diffusion_start_requested = true;
					if (renderer.diffusion_worker.joinable()) {
						sg_renderer_request_stop_diffusion_worker(renderer);
					} else if (!renderer.diffusion_worker_stopping) {
						renderer.diffusion_start_requested = false;
						sg_renderer_start_diffusion_worker(renderer);
					}
				}
			}
			if (selected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::BeginCombo("Resolution", diffusion_resolution_label(renderer.diffusion_resolution))) {
		for (s32 i = 0; i < 3; ++i) {
			sg_diffusion_resolution_t resolution = sg_diffusion_resolution_t::R1024x1024;
			if (i == 0) {
				resolution = sg_diffusion_resolution_t::R512x512;
			} else if (i == 2) {
				resolution = sg_diffusion_resolution_t::R1344x768;
			}
			bool const selected = renderer.diffusion_resolution == resolution;
			if (ImGui::Selectable(diffusion_resolution_label(resolution), selected)) {
				renderer.diffusion_resolution = resolution;
				if (renderer.diffusion_enabled) {
					renderer.diffusion_start_requested = true;
					if (renderer.diffusion_worker.joinable()) {
						sg_renderer_request_stop_diffusion_worker(renderer);
					} else if (!renderer.diffusion_worker_stopping) {
						renderer.diffusion_start_requested = false;
						sg_renderer_start_diffusion_worker(renderer);
					}
				}
			}
			if (selected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}

	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputText("Prompt", renderer.diffusion_prompt_buffer.data(), renderer.diffusion_prompt_buffer.size())) {
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_prompt = renderer.diffusion_prompt_buffer.data();
	}

	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputText("Negative Prompt", renderer.diffusion_negative_prompt_buffer.data(),
		    renderer.diffusion_negative_prompt_buffer.size())) {
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_negative_prompt = renderer.diffusion_negative_prompt_buffer.data();
	}

	s64 seed_ui = renderer.diffusion_seed;
	ImGui::SetNextItemWidth(220.0f);
	if (ImGui::InputScalar("Random Seed", ImGuiDataType_S64, &seed_ui)) {
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_seed = seed_ui;
	}
	ImGui::SameLine();
	if (ImGui::Button("Randomize")) {
		s64 const randomized_seed =
			static_cast<s64>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
		{
			std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
			renderer.diffusion_seed = randomized_seed;
		}
		seed_ui = randomized_seed;
	}

	ImGui::SetNextItemWidth(160.0f);
	f32 control_strength_ui = renderer.diffusion_control_strength;
	if (ImGui::DragFloat("Control", &control_strength_ui, 0.01f, 0.0f, 2.0f, "%.2f")) {
		std::lock_guard<std::mutex> lock(renderer.diffusion_mutex);
		renderer.diffusion_control_strength = std::clamp(control_strength_ui, 0.0f, 2.0f);
	}
	ImGui::EndDisabled();

	if (diffusion_failed) {
		ImGui::TextUnformatted("Diffusion failed; disabled.");
	}
	// ImGui::SameLine();
	// ImGui::Checkbox("Invert", &renderer.preview_depth_invert);
	// ImGui::SetNextItemWidth(160.0f);
	// ImGui::DragFloat("Span Eps", &renderer.preview_depth_span_epsilon, 0.0001f, 0.0f, 0.1f, "%.5f");

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
