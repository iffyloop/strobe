#pragma once

#include "pch.h"

struct gl_vertex_buffer_t {
	gl_vertex_buffer_t() = default;
	DELETE_COPY(gl_vertex_buffer_t);
	DEFAULT_MOVE(gl_vertex_buffer_t);

	GLint size = 0;
	GLenum type = 0;
	GLboolean normalized = 0;

	GLuint vbo = 0;
	GLuint num_vertices = 0;
};

struct gl_vertex_buffers_t {
	gl_vertex_buffers_t() = default;
	DELETE_COPY(gl_vertex_buffers_t);
	DEFAULT_MOVE(gl_vertex_buffers_t);

	GLuint vao = 0;
	GLenum mode = GL_TRIANGLES;

	std::unordered_map<std::string, gl_vertex_buffer_t> buffers;
};

template <typename T>
void gl_vertex_buffers_upload(gl_vertex_buffers_t& vertex_buffers, std::string const& name, std::vector<T> const& data,
	GLint size, GLenum type, GLboolean normalized) {
	assert_release(data.size() % size == 0);

	if (!vertex_buffers.vao) {
		glGenVertexArrays(1, &vertex_buffers.vao);
		assert_release(vertex_buffers.vao);
	}
	glBindVertexArray(vertex_buffers.vao);

	if (vertex_buffers.buffers.find(name) == vertex_buffers.buffers.end()) {
		vertex_buffers.buffers.try_emplace(name);
	}
	gl_vertex_buffer_t& buffer = vertex_buffers.buffers[name];
	buffer.size = size;
	buffer.type = type;
	buffer.normalized = normalized;
	buffer.num_vertices = static_cast<GLuint>(data.size() / size);
	if (!buffer.vbo) {
		glGenBuffers(1, &buffer.vbo);
		assert_release(buffer.vbo);
	}

	glBindBuffer(GL_ARRAY_BUFFER, buffer.vbo);
	glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(T), data.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glBindVertexArray(0);
}
