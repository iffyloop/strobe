#include "pch.h"

#include "sg_compiler.h"

#include "sg_plugins.h"

namespace {

constexpr f32 k_min_scale = 1.0e-4f;

struct sg_compile_state_t {
	std::vector<glm::ivec4> effect_meta;
	std::vector<glm::vec4> effect_params;
	f32 world_scale = 1.0f;
	glm::mat3 world_to_local_linear = glm::mat3(1.0f);
	glm::vec3 world_to_local_offset = glm::vec3(0.0f);
};

struct sg_compile_ctx_t {
	sg_compiled_scene_t* out = nullptr;
	u32 cur_stack_depth = 0;
};

glm::vec4 sg_pack_props(sg_node_props_map_t const& props, std::vector<sg_plugin_param_t> const& params) {
	glm::vec4 packed = glm::vec4(0.0f);
	if (params.size() > 4) {
		std::cerr << "[sg_compile] warning: plugin has more than 4 params; truncating to vec4" << std::endl;
	}

	for (size_t i = 0; i < params.size() && i < 4; ++i) {
		f32 value = params[i].default_value;
		for (auto const& prop_ptr : props) {
			if (prop_ptr != nullptr && prop_ptr->get_name() == params[i].name) {
				value = prop_ptr->get_cur_value();
				break;
			}
		}
		packed[static_cast<s32>(i)] = value;
	}

	return packed;
}

s32 sg_get_scale_effect_runtime_id() {
	auto const* scale_effect = sg_plugins_find_effect_by_id("scale");
	if (scale_effect == nullptr) {
		return 0;
	}
	return scale_effect->runtime_id;
}

s32 sg_get_union_runtime_id() {
	auto const* def = sg_plugins_find_combine_by_id("union");
	if (def == nullptr) {
		return 0;
	}
	return def->runtime_id;
}

glm::mat3 sg_abs_mat3(glm::mat3 const& m) {
	glm::mat3 out(1.0f);
	for (s32 c = 0; c < 3; ++c) {
		for (s32 r = 0; r < 3; ++r) {
			out[c][r] = std::abs(m[c][r]);
		}
	}
	return out;
}

glm::vec4 sg_compute_primitive_world_bounds(sg_compile_state_t const& state, sg_node_t const& node) {
	auto const* primitive_def = sg_plugins_find_primitive_by_runtime_id(static_cast<s32>(node.type));
	if (primitive_def == nullptr) {
		return glm::vec4(0.0f);
	}

	glm::vec3 local_half_extents(1.0f);
	if (primitive_def->id == "cube") {
		local_half_extents =
			glm::vec3(std::max(sg_pack_props(node.props, primitive_def->params).x * 0.5f, k_min_scale),
				std::max(sg_pack_props(node.props, primitive_def->params).y * 0.5f, k_min_scale),
				std::max(sg_pack_props(node.props, primitive_def->params).z * 0.5f, k_min_scale));
	} else if (primitive_def->id == "sphere") {
		f32 const radius = std::max(sg_pack_props(node.props, primitive_def->params).x, k_min_scale);
		local_half_extents = glm::vec3(radius);
	}

	f32 const det = glm::determinant(state.world_to_local_linear);
	if (std::abs(det) < 1.0e-8f) {
		return glm::vec4(0.0f);
	}

	glm::mat3 const local_to_world = glm::inverse(state.world_to_local_linear);
	glm::vec3 const center = -(local_to_world * state.world_to_local_offset);
	glm::vec3 const world_half = sg_abs_mat3(local_to_world) * local_half_extents;
	f32 const radius = glm::length(world_half);
	if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
		!std::isfinite(radius)) {
		return glm::vec4(0.0f);
	}
	return glm::vec4(center, std::max(radius, k_min_scale));
}

s32 sg_get_primitive_texture_id(sg_node_props_map_t const& props) {
	for (auto const& prop_ptr : props) {
		if (prop_ptr == nullptr) {
			continue;
		}
		if (prop_ptr->get_name() == "texture_id") {
			f32 const raw_value = prop_ptr->get_cur_value();
			return static_cast<s32>(std::lround(std::clamp(raw_value, 0.0f, 255.0f)));
		}
	}
	return 0;
}

void sg_emit_push_primitive(sg_compile_ctx_t& ctx, glm::ivec4 const& prim_meta, glm::vec4 const& prim_params,
	glm::vec4 const& prim_bounds, std::vector<glm::ivec4> const& effect_meta,
	std::vector<glm::vec4> const& effect_params, f32 scale) {
	s32 const primitive_index = static_cast<s32>(ctx.out->primitive_meta.size());
	s32 const effect_base = static_cast<s32>(ctx.out->effect_meta.size());
	s32 const effect_count = static_cast<s32>(effect_meta.size());

	ctx.out->primitive_meta.emplace_back(prim_meta);
	ctx.out->primitive_params.emplace_back(prim_params);
	ctx.out->primitive_scale.emplace_back(scale, 0.0f, 0.0f, 0.0f);
	ctx.out->primitive_bounds.emplace_back(prim_bounds);
	ctx.out->primitive_effect_ranges.emplace_back(effect_base, effect_count, 0, 0);

	for (size_t i = 0; i < effect_meta.size(); ++i) {
		ctx.out->effect_meta.emplace_back(effect_meta[i]);
		ctx.out->effect_params.emplace_back(effect_params[i]);
	}

	ctx.out->program.emplace_back(SG_COMPILER_OP_PUSH_PRIMITIVE, primitive_index, 0, 0);
	ctx.cur_stack_depth += 1;
	ctx.out->max_stack_depth = std::max(ctx.out->max_stack_depth, ctx.cur_stack_depth);
	ctx.out->has_output = true;
}

void sg_emit_combine(sg_compile_ctx_t& ctx, s32 combine_type_id, glm::vec4 const& combine_params) {
	if (ctx.cur_stack_depth < 2) {
		return;
	}
	s32 const combine_param_index = static_cast<s32>(ctx.out->combine_params.size());
	ctx.out->combine_params.emplace_back(combine_params);
	ctx.out->program.emplace_back(SG_COMPILER_OP_COMBINE, combine_type_id, combine_param_index, 0);
	if (combine_type_id != sg_get_union_runtime_id()) {
		ctx.out->union_only = false;
	}
	ctx.cur_stack_depth -= 1;
}

sg_compile_state_t sg_apply_node_effects(sg_compile_state_t const& parent_state, sg_node_t const& node) {
	sg_compile_state_t out_state = parent_state;
	s32 const scale_effect_runtime_id = sg_get_scale_effect_runtime_id();

	for (auto it = node.effects.rbegin(); it != node.effects.rend(); ++it) {
		auto const& effect_ptr = *it;
		if (effect_ptr == nullptr) {
			continue;
		}

		s32 const runtime_id = static_cast<s32>(effect_ptr->type);
		auto const* effect_def = sg_plugins_find_effect_by_runtime_id(runtime_id);
		if (effect_def == nullptr) {
			std::cerr << "[sg_compile] warning: skipping unknown effect runtime id " << runtime_id
				  << std::endl;
			continue;
		}

		glm::vec4 const packed_params = sg_pack_props(effect_ptr->props, effect_def->params);
		out_state.effect_meta.emplace_back(runtime_id, 0, 0, 0);
		out_state.effect_params.emplace_back(packed_params);

		if (runtime_id == scale_effect_runtime_id) {
			f32 const scale = std::max(std::abs(packed_params.x), k_min_scale);
			out_state.world_scale = std::max(out_state.world_scale * scale, k_min_scale);
		}

		if (effect_def->id == "translate") {
			glm::vec3 const t = glm::vec3(packed_params.x, packed_params.y, packed_params.z);
			out_state.world_to_local_offset -= t;
		} else if (effect_def->id == "scale") {
			f32 const s = std::max(std::abs(packed_params.x), k_min_scale);
			glm::mat3 const m = glm::mat3(1.0f / s);
			out_state.world_to_local_linear = m * out_state.world_to_local_linear;
			out_state.world_to_local_offset = m * out_state.world_to_local_offset;
		} else if (effect_def->id == "rotate") {
			f32 const cx = std::cos(-packed_params.x);
			f32 const sx = std::sin(-packed_params.x);
			f32 const cy = std::cos(-packed_params.y);
			f32 const sy = std::sin(-packed_params.y);
			f32 const cz = std::cos(-packed_params.z);
			f32 const sz = std::sin(-packed_params.z);
			glm::mat3 const rz_inv = glm::mat3(cz, sz, 0.0f, -sz, cz, 0.0f, 0.0f, 0.0f, 1.0f);
			glm::mat3 const ry_inv = glm::mat3(cy, 0.0f, -sy, 0.0f, 1.0f, 0.0f, sy, 0.0f, cy);
			glm::mat3 const rx_inv = glm::mat3(1.0f, 0.0f, 0.0f, 0.0f, cx, sx, 0.0f, -sx, cx);
			glm::mat3 const r = rx_inv * ry_inv * rz_inv;
			out_state.world_to_local_linear = r * out_state.world_to_local_linear;
			out_state.world_to_local_offset = r * out_state.world_to_local_offset;
		}
	}
	return out_state;
}

bool sg_compile_node(sg_compile_ctx_t& ctx, sg_node_t const& node, sg_compile_state_t const& parent_state) {
	sg_compile_state_t const node_state = sg_apply_node_effects(parent_state, node);
	s32 const node_runtime_id = static_cast<s32>(node.type);

	if (auto const* primitive_def = sg_plugins_find_primitive_by_runtime_id(node_runtime_id)) {
		glm::vec4 const prim_params = sg_pack_props(node.props, primitive_def->params);
		glm::vec4 const prim_bounds = sg_compute_primitive_world_bounds(node_state, node);
		s32 const texture_id = sg_get_primitive_texture_id(node.props);
		glm::ivec4 const prim_meta = glm::ivec4(node_runtime_id, texture_id, 0, 0);
		sg_emit_push_primitive(ctx, prim_meta, prim_params, prim_bounds, node_state.effect_meta,
			node_state.effect_params, node_state.world_scale);
		return true;
	}

	auto const* combine_def = sg_plugins_find_combine_by_runtime_id(node_runtime_id);
	if (combine_def == nullptr) {
		std::cerr << "[sg_compile] warning: skipping unknown node runtime id " << node_runtime_id << std::endl;
		return false;
	}

	glm::vec4 const combine_params = sg_pack_props(node.props, combine_def->params);
	bool has_value = false;
	for (auto const& child_ptr : node.children) {
		if (child_ptr == nullptr) {
			continue;
		}
		bool const child_has_value = sg_compile_node(ctx, *child_ptr.get(), node_state);
		if (!child_has_value) {
			continue;
		}
		if (!has_value) {
			has_value = true;
			continue;
		}

		sg_emit_combine(ctx, combine_def->runtime_id, combine_params);
	}
	return has_value;
}

} // namespace

void sg_compile(sg_compiled_scene_t& out, sg_node_t const& root) {
	out.program.clear();
	out.combine_params.clear();
	out.primitive_meta.clear();
	out.primitive_params.clear();
	out.primitive_scale.clear();
	out.primitive_bounds.clear();
	out.primitive_effect_ranges.clear();
	out.effect_meta.clear();
	out.effect_params.clear();
	out.has_output = false;
	out.union_only = true;
	out.max_stack_depth = 0;

	sg_compile_ctx_t ctx;
	ctx.out = &out;
	ctx.cur_stack_depth = 0;

	sg_compile_state_t root_state;
	root_state.world_scale = 1.0f;
	bool const has_output = sg_compile_node(ctx, root, root_state);
	out.has_output = has_output && !out.program.empty();
}
