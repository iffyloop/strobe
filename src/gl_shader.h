#pragma once

#include "pch.h"

struct gl_shader_t {
	gl_shader_t() = default;
	DELETE_COPY(gl_shader_t);
	DEFAULT_MOVE(gl_shader_t);

	GLuint program = 0;
};

void gl_shader_init(gl_shader_t& shader, std::string const& vert_filename, std::string const& frag_filename);
bool gl_shader_try_build_program_from_file_and_source(GLuint& out_program, std::string const& vert_filename,
	std::string const& frag_debug_name, std::string const& frag_source, std::string& out_error);
