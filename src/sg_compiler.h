#pragma once

#include "pch.h"

#include "sg_node.h"

enum sg_compiler_opcode_t {
	SG_COMPILER_OP_PUSH_PRIMITIVE = 1,
	SG_COMPILER_OP_COMBINE = 2,
};

struct sg_compiled_scene_t {
	std::vector<glm::ivec4> program;
	std::vector<glm::vec4> combine_params;
	std::vector<glm::ivec4> primitive_meta;
	std::vector<glm::vec4> primitive_params;
	std::vector<glm::vec4> primitive_scale;
	std::vector<glm::vec4> primitive_bounds;
	std::vector<glm::ivec4> primitive_effect_ranges;
	std::vector<glm::ivec4> effect_meta;
	std::vector<glm::vec4> effect_params;

	bool has_output = false;
	bool union_only = true;
	u32 max_stack_depth = 0;
};

void sg_compile(sg_compiled_scene_t& out, sg_node_t const& root);
