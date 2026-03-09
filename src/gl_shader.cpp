#include "pch.h"

#include "gl_shader.h"
#include "io_utils.h"

namespace {

GLuint gl_shader_compile(GLenum const type, std::string const& filename, std::string const& src) {
	GLuint const shader = glCreateShader(type);
	GLchar const* const src_ptr = src.data();
	glShaderSource(shader, 1, &src_ptr, nullptr);
	glCompileShader(shader);
	GLint success = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		GLchar buf[4096];
		glGetShaderInfoLog(shader, sizeof(buf), nullptr, buf);
		std::cerr << "shader_compile: \"" + filename + "\" compilation failed:\n" << buf << "\n";
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

bool gl_shader_try_link_program(GLuint vert_shader, GLuint frag_shader, std::string const& debug_name,
	GLuint& out_program, std::string& out_error) {
	GLuint program = glCreateProgram();
	glAttachShader(program, vert_shader);
	glAttachShader(program, frag_shader);
	glLinkProgram(program);

	GLint success = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success) {
		GLchar buf[4096];
		glGetProgramInfoLog(program, sizeof(buf), nullptr, buf);
		out_error = "Shader linking failed for \"" + debug_name + "\":\n" + std::string(buf);
		glDeleteProgram(program);
		return false;
	}

	out_program = program;
	return true;
}

} // namespace

void gl_shader_init(gl_shader_t& shader, std::string const& vert_filename, std::string const& frag_filename) {
	std::string full_vert_filename = get_resources_prefix() + vert_filename;
	std::string full_frag_filename = get_resources_prefix() + frag_filename;

	std::string vert_src, frag_src;
	load_file(vert_src, full_vert_filename);
	load_file(frag_src, full_frag_filename);
	GLuint const vert_shader = gl_shader_compile(GL_VERTEX_SHADER, full_vert_filename, vert_src);
	GLuint const frag_shader = gl_shader_compile(GL_FRAGMENT_SHADER, full_frag_filename, frag_src);
	assert_release(vert_shader && frag_shader);

	GLuint program = 0;
	std::string error;
	bool const ok = gl_shader_try_link_program(
		vert_shader, frag_shader, full_vert_filename + " + " + full_frag_filename, program, error);
	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);
	if (!ok) {
		std::cerr << error << std::endl;
		assert_release(false);
	}

	if (shader.program != 0) {
		glDeleteProgram(shader.program);
	}
	shader.program = program;
}

bool gl_shader_try_build_program_from_file_and_source(GLuint& out_program, std::string const& vert_filename,
	std::string const& frag_debug_name, std::string const& frag_source, std::string& out_error) {
	out_program = 0;

	std::string const full_vert_filename = get_resources_prefix() + vert_filename;
	std::string vert_src;
	try {
		load_file(vert_src, full_vert_filename);
	} catch (...) {
		out_error = "failed to read vertex shader file: " + full_vert_filename;
		return false;
	}

	GLuint const vert_shader = gl_shader_compile(GL_VERTEX_SHADER, full_vert_filename, vert_src);
	if (vert_shader == 0) {
		out_error = "vertex compile failed: " + full_vert_filename;
		return false;
	}

	GLuint const frag_shader = gl_shader_compile(GL_FRAGMENT_SHADER, frag_debug_name, frag_source);
	if (frag_shader == 0) {
		glDeleteShader(vert_shader);
		out_error = "fragment compile failed: " + frag_debug_name;
		return false;
	}

	bool const ok = gl_shader_try_link_program(vert_shader, frag_shader, frag_debug_name, out_program, out_error);
	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);
	return ok;
}

bool gl_shader_try_build_compute_program_from_source(
	GLuint& out_program, std::string const& debug_name, std::string const& compute_source, std::string& out_error) {
	out_program = 0;
	GLuint const compute_shader = glCreateShader(GL_COMPUTE_SHADER);
	GLchar const* const src_ptr = compute_source.data();
	glShaderSource(compute_shader, 1, &src_ptr, nullptr);
	glCompileShader(compute_shader);

	GLint compile_ok = 0;
	glGetShaderiv(compute_shader, GL_COMPILE_STATUS, &compile_ok);
	if (!compile_ok) {
		GLchar buf[4096];
		glGetShaderInfoLog(compute_shader, sizeof(buf), nullptr, buf);
		out_error = "Compute compilation failed for \"" + debug_name + "\":\n" + std::string(buf);
		glDeleteShader(compute_shader);
		return false;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, compute_shader);
	glLinkProgram(program);

	GLint success = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success) {
		GLchar buf[4096];
		glGetProgramInfoLog(program, sizeof(buf), nullptr, buf);
		out_error = "Compute linking failed for \"" + debug_name + "\":\n" + std::string(buf);
		glDeleteProgram(program);
		glDeleteShader(compute_shader);
		return false;
	}

	glDeleteShader(compute_shader);
	out_program = program;
	return true;
}
