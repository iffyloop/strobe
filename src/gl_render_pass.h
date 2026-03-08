#pragma once

#include "pch.h"

#include "gl_shader.h"
#include "gl_vertex_buffers.h"

struct gl_render_pass_output_descriptor_t {
	glm::vec4 clear_color = glm::vec4(0.0f);
	GLfloat clear_depth = 1.0f;
	GLint internal_format = GL_RGBA8;
	GLenum format = GL_RGBA;
	GLenum type = GL_UNSIGNED_BYTE;
	GLint min_filter = GL_LINEAR;
	GLint mag_filter = GL_LINEAR;
	GLint wrap_s = GL_CLAMP_TO_EDGE;
	GLint wrap_t = GL_CLAMP_TO_EDGE;
};

struct gl_render_pass_internal_output_descriptor_t {
	gl_render_pass_internal_output_descriptor_t() = default;
	DELETE_COPY(gl_render_pass_internal_output_descriptor_t);
	DEFAULT_MOVE(gl_render_pass_internal_output_descriptor_t);

	gl_render_pass_output_descriptor_t base_descriptor;
	GLint location = -1;
	GLuint texture = 0;
};

struct gl_render_pass_t {
	gl_render_pass_t() = default;
	DELETE_COPY(gl_render_pass_t);
	DEFAULT_MOVE(gl_render_pass_t);

	gl_shader_t shader;

	GLuint width = 0, height = 0;
	GLuint fbo = 0;
	std::unordered_map<std::string, gl_render_pass_internal_output_descriptor_t> internal_output_descriptors;

	GLenum depth_func = GL_LESS;
	GLfloat clear_depth = 1.0f;
	GLenum cull_face = 0;
};

void gl_render_pass_init(gl_render_pass_t& render_pass, std::string const& vert_filename,
	std::string const& frag_filename, GLuint const width, GLuint const height,
	std::unordered_map<std::string, gl_render_pass_output_descriptor_t> const& output_descriptors);
void gl_render_pass_begin(gl_render_pass_t const& render_pass);
void gl_render_pass_draw(gl_render_pass_t const& render_pass, gl_vertex_buffers_t const& vertex_buffers);
void gl_render_pass_end(gl_render_pass_t const& render_pass);
void gl_render_pass_clear_with_override(gl_render_pass_t const& render_pass, std::string const& output_descriptor_name,
	glm::vec4 const& color, GLfloat const depth);

void gl_render_pass_uniform_mat4(gl_render_pass_t const& render_pass, std::string const& name, glm::mat4 const& value);
void gl_render_pass_uniform_texture(
	gl_render_pass_t const& render_pass, std::string const& name, GLuint texture, GLenum texture_unit);
void gl_render_pass_uniform_int(gl_render_pass_t const& render_pass, std::string const& name, GLint value);
void gl_render_pass_uniform_float(gl_render_pass_t const& render_pass, std::string const& name, GLfloat value);
void gl_render_pass_uniform_vec2(gl_render_pass_t const& render_pass, std::string const& name, glm::vec2 const& value);
void gl_render_pass_uniform_ivec2(
	gl_render_pass_t const& render_pass, std::string const& name, glm::ivec2 const& value);
void gl_render_pass_uniform_bool(gl_render_pass_t const& render_pass, std::string const& name, bool value);

template <typename T>
void gl_render_pass_download(
	gl_render_pass_t const& render_pass, std::string const& output_descriptor_name, std::vector<T>& out) {
	auto& descriptor = render_pass.internal_output_descriptors.at(output_descriptor_name);

	GLsizei num_channels = 0;
	switch (descriptor.base_descriptor.format) {
	case GL_RGBA:
		num_channels = 4;
		break;
	case GL_RGB:
		num_channels = 3;
		break;
	case GL_RED:
		num_channels = 1;
		break;
	case GL_DEPTH_COMPONENT:
		num_channels = 1;
		break;
	default:
		assert_release(false);
	}

	GLsizei bytes_per_channel = 0;
	switch (descriptor.base_descriptor.type) {
	case GL_UNSIGNED_BYTE:
		bytes_per_channel = 1;
		break;
	case GL_FLOAT:
		bytes_per_channel = 4;
		break;
	default:
		assert_release(false);
	}

	size_t out_num_bytes = render_pass.width * render_pass.height * num_channels * bytes_per_channel;
	out.resize(out_num_bytes);

	glBindTexture(GL_TEXTURE_2D, descriptor.texture);
	glGetTexImage(GL_TEXTURE_2D, 0, descriptor.base_descriptor.format, descriptor.base_descriptor.type, out.data());
	glBindTexture(GL_TEXTURE_2D, 0);
}
