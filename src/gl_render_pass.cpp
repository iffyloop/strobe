#include "pch.h"

#include "gl_render_pass.h"

void gl_render_pass_init(gl_render_pass_t& render_pass, std::string const& vert_filename,
	std::string const& frag_filename, GLuint const width, GLuint const height,
	std::unordered_map<std::string, gl_render_pass_output_descriptor_t> const& output_descriptors) {
	gl_shader_init(render_pass.shader, vert_filename, frag_filename);

	render_pass.width = width;
	render_pass.height = height;

	for (auto const& [name, base_descriptor] : output_descriptors) {
		render_pass.internal_output_descriptors.try_emplace(name);
		gl_render_pass_internal_output_descriptor_t& descriptor = render_pass.internal_output_descriptors[name];

		descriptor.base_descriptor = base_descriptor;

		if (name != "##DEPTH") {
			GLint location = glGetFragDataLocation(render_pass.shader.program, name.c_str());
			assert_release(location >= 0);
			descriptor.location = location;
		} else {
			descriptor.location = -1;
		}

		GLuint texture = 0;
		glGenTextures(1, &texture);
		assert_release(texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, descriptor.base_descriptor.internal_format, render_pass.width,
			render_pass.height, 0, descriptor.base_descriptor.format, descriptor.base_descriptor.type,
			nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, descriptor.base_descriptor.min_filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, descriptor.base_descriptor.mag_filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, descriptor.base_descriptor.wrap_s);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, descriptor.base_descriptor.wrap_t);
		glBindTexture(GL_TEXTURE_2D, 0);
		descriptor.texture = texture;
	}

	glGenFramebuffers(1, &render_pass.fbo);
	assert_release(render_pass.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, render_pass.fbo);
	std::vector<GLenum> draw_buffers;
	for (auto const& [name, descriptor] : render_pass.internal_output_descriptors) {
		if (descriptor.base_descriptor.format == GL_DEPTH_COMPONENT) {
			glFramebufferTexture2D(
				GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, descriptor.texture, 0);
		} else {
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + descriptor.location,
				GL_TEXTURE_2D, descriptor.texture, 0);
			draw_buffers.push_back(GL_COLOR_ATTACHMENT0 + descriptor.location);
		}
	}
	if (draw_buffers.size() > 0) {
		glDrawBuffers(static_cast<GLsizei>(draw_buffers.size()), draw_buffers.data());
	}
	assert_release(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_render_pass_begin(gl_render_pass_t const& render_pass) {
	glBindFramebuffer(GL_FRAMEBUFFER, render_pass.fbo);

	glViewport(0, 0, render_pass.width, render_pass.height);

	for (auto const& [name, descriptor] : render_pass.internal_output_descriptors) {
		glClearBufferfv(GL_COLOR, descriptor.location, glm::value_ptr(descriptor.base_descriptor.clear_color));
	}

	if (render_pass.depth_func) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(render_pass.depth_func);
		glClearDepth(render_pass.clear_depth);
		glClear(GL_DEPTH_BUFFER_BIT);
	} else {
		glDisable(GL_DEPTH_TEST);
	}

	if (render_pass.cull_face) {
		glEnable(GL_CULL_FACE);
		glCullFace(render_pass.cull_face);
	} else {
		glDisable(GL_CULL_FACE);
	}

	glUseProgram(render_pass.shader.program);
}

void gl_render_pass_draw(gl_render_pass_t const& render_pass, gl_vertex_buffers_t const& vertex_buffers) {
	if (vertex_buffers.buffers.size() == 0) {
		return;
	}

	assert_release(vertex_buffers.vao);
	glBindVertexArray(vertex_buffers.vao);

	for (auto const& [name, buffer] : vertex_buffers.buffers) {
		GLint location = glGetAttribLocation(render_pass.shader.program, name.c_str());
		if (location >= 0) {
			glBindBuffer(GL_ARRAY_BUFFER, buffer.vbo);
			glEnableVertexAttribArray(location);
			glVertexAttribPointer(location, buffer.size, buffer.type, buffer.normalized, 0, 0);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
		} else {
#ifdef DEBUG
			std::cerr << "[WARN] Attribute location for \"" << name << "\" not found in shader"
				  << std::endl;
#endif
		}
	}

	GLuint num_vertices = vertex_buffers.buffers.begin()->second.num_vertices;
#ifdef DEBUG
	for (auto const& [name, buffer] : vertex_buffers.buffers) {
		if (buffer.num_vertices != num_vertices) {
			assert_release(false);
		}
	}
#endif
	glDrawArrays(vertex_buffers.mode, 0, num_vertices);

	glBindVertexArray(0);
}

void gl_render_pass_end(gl_render_pass_t const& render_pass) {
	UNUSED(render_pass);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_render_pass_clear_with_override(gl_render_pass_t const& render_pass, std::string const& output_descriptor_name,
	glm::vec4 const& color, GLfloat const depth) {
	glBindFramebuffer(GL_FRAMEBUFFER, render_pass.fbo);
	glViewport(0, 0, render_pass.width, render_pass.height);
	auto& descriptor = render_pass.internal_output_descriptors.at(output_descriptor_name);
	glClearBufferfv(GL_COLOR, descriptor.location, glm::value_ptr(color));
	glClearDepth(depth);
	glClear(GL_DEPTH_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_render_pass_uniform_mat4(gl_render_pass_t const& render_pass, std::string const& name, glm::mat4 const& value) {
	GLint location = glGetUniformLocation(render_pass.shader.program, name.c_str());
	assert_release(location >= 0);
	glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(value));
}

void gl_render_pass_uniform_texture(
	gl_render_pass_t const& render_pass, std::string const& name, GLuint texture, GLenum texture_unit) {
	GLint location = glGetUniformLocation(render_pass.shader.program, name.c_str());
	assert_release(location >= 0);
	glUniform1i(location, texture_unit - GL_TEXTURE0);
	glActiveTexture(texture_unit);
	glBindTexture(GL_TEXTURE_2D, texture);
}

void gl_render_pass_uniform_int(gl_render_pass_t const& render_pass, std::string const& name, GLint value) {
	GLint location = glGetUniformLocation(render_pass.shader.program, name.c_str());
	assert_release(location >= 0);
	glUniform1i(location, value);
}

void gl_render_pass_uniform_float(gl_render_pass_t const& render_pass, std::string const& name, GLfloat value) {
	GLint location = glGetUniformLocation(render_pass.shader.program, name.c_str());
	assert_release(location >= 0);
	glUniform1f(location, value);
}

void gl_render_pass_uniform_vec2(gl_render_pass_t const& render_pass, std::string const& name, glm::vec2 const& value) {
	GLint location = glGetUniformLocation(render_pass.shader.program, name.c_str());
	assert_release(location >= 0);
	glUniform2fv(location, 1, glm::value_ptr(value));
}

void gl_render_pass_uniform_ivec2(
	gl_render_pass_t const& render_pass, std::string const& name, glm::ivec2 const& value) {
	GLint location = glGetUniformLocation(render_pass.shader.program, name.c_str());
	assert_release(location >= 0);
	glUniform2iv(location, 1, glm::value_ptr(value));
}

void gl_render_pass_uniform_bool(gl_render_pass_t const& render_pass, std::string const& name, bool value) {
	GLint location = glGetUniformLocation(render_pass.shader.program, name.c_str());
	assert_release(location >= 0);
	glUniform1i(location, value);
}
