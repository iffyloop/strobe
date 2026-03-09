#include "pch.h"

#include "scene_invalidation.h"

#include "sg_plugins.h"

namespace {

constexpr f32 k_min_scale = 1.0e-4f;

f32 scene_find_prop_value(sg_node_props_map_t const& props, char const* name, f32 fallback) {
	for (auto const& prop_ptr : props) {
		if (prop_ptr != nullptr && prop_ptr->get_name() == name) {
			return prop_ptr->get_cur_value();
		}
	}
	return fallback;
}

struct scene_affine_t {
	glm::mat3 a = glm::mat3(1.0f);
	glm::vec3 b = glm::vec3(0.0f);
};

bool scene_is_finite_vec3(glm::vec3 const& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

glm::mat3 scene_abs_mat3(glm::mat3 const& m) {
	glm::mat3 out(1.0f);
	for (s32 c = 0; c < 3; ++c) {
		for (s32 r = 0; r < 3; ++r) {
			out[c][r] = std::abs(m[c][r]);
		}
	}
	return out;
}

void scene_apply_effect_affine(scene_affine_t& state, sg_node_effect_t const& effect) {
	auto const* effect_def = sg_plugins_find_effect_by_runtime_id(static_cast<s32>(effect.type));
	if (effect_def == nullptr) {
		return;
	}

	glm::mat3 effect_a(1.0f);
	glm::vec3 effect_b(0.0f);

	if (effect_def->id == "translate") {
		effect_b = -glm::vec3(scene_find_prop_value(effect.props, "pos_x", 0.0f),
			scene_find_prop_value(effect.props, "pos_y", 0.0f),
			scene_find_prop_value(effect.props, "pos_z", 0.0f));
	} else if (effect_def->id == "scale") {
		f32 const s = std::max(std::abs(scene_find_prop_value(effect.props, "scale", 1.0f)), k_min_scale);
		effect_a = glm::mat3(1.0f / s);
	} else if (effect_def->id == "rotate") {
		f32 const rx = scene_find_prop_value(effect.props, "rot_x", 0.0f);
		f32 const ry = scene_find_prop_value(effect.props, "rot_y", 0.0f);
		f32 const rz = scene_find_prop_value(effect.props, "rot_z", 0.0f);

		f32 const cx = std::cos(-rx);
		f32 const sx = std::sin(-rx);
		f32 const cy = std::cos(-ry);
		f32 const sy = std::sin(-ry);
		f32 const cz = std::cos(-rz);
		f32 const sz = std::sin(-rz);

		glm::mat3 const rz_inv = glm::mat3(cz, sz, 0.0f, -sz, cz, 0.0f, 0.0f, 0.0f, 1.0f);
		glm::mat3 const ry_inv = glm::mat3(cy, 0.0f, -sy, 0.0f, 1.0f, 0.0f, sy, 0.0f, cy);
		glm::mat3 const rx_inv = glm::mat3(1.0f, 0.0f, 0.0f, 0.0f, cx, sx, 0.0f, -sx, cx);
		effect_a = rx_inv * ry_inv * rz_inv;
	}

	state.a = effect_a * state.a;
	state.b = effect_a * state.b + effect_b;
}

void scene_collect_primitive_bounds_recursive(
	sg_node_t const& node, scene_affine_t const& parent_affine, std::unordered_map<u64, scene_aabb_t>& out_bounds) {
	scene_affine_t node_affine = parent_affine;
	for (auto it = node.effects.rbegin(); it != node.effects.rend(); ++it) {
		auto const& effect_ptr = *it;
		if (effect_ptr == nullptr) {
			continue;
		}
		scene_apply_effect_affine(node_affine, *effect_ptr.get());
	}

	auto const* primitive_def = sg_plugins_find_primitive_by_runtime_id(static_cast<s32>(node.type));
	if (primitive_def != nullptr) {
		glm::vec3 local_half_extents(1.0f);
		if (primitive_def->id == "cube") {
			local_half_extents.x =
				std::max(scene_find_prop_value(node.props, "size_x", 1.0f) * 0.5f, k_min_scale);
			local_half_extents.y =
				std::max(scene_find_prop_value(node.props, "size_y", 1.0f) * 0.5f, k_min_scale);
			local_half_extents.z =
				std::max(scene_find_prop_value(node.props, "size_z", 1.0f) * 0.5f, k_min_scale);
		} else if (primitive_def->id == "sphere") {
			f32 const radius = std::max(scene_find_prop_value(node.props, "radius", 1.0f), k_min_scale);
			local_half_extents = glm::vec3(radius);
		}

		f32 const det = glm::determinant(node_affine.a);
		if (std::abs(det) > 1.0e-8f) {
			glm::mat3 const world_matrix = glm::inverse(node_affine.a);
			glm::vec3 const center = -(world_matrix * node_affine.b);
			glm::vec3 const world_half = scene_abs_mat3(world_matrix) * local_half_extents;
			if (scene_is_finite_vec3(center) && scene_is_finite_vec3(world_half)) {
				scene_aabb_t bounds;
				bounds.min = center - world_half;
				bounds.max = center + world_half;
				out_bounds[node.id] = bounds;
			}
		}
	}

	for (auto const& child_ptr : node.children) {
		if (child_ptr == nullptr) {
			continue;
		}
		scene_collect_primitive_bounds_recursive(*child_ptr.get(), node_affine, out_bounds);
	}
}

bool scene_aabb_equal(scene_aabb_t const& a, scene_aabb_t const& b) {
	constexpr f32 epsilon = 1.0e-4f;
	return glm::all(glm::lessThanEqual(glm::abs(a.min - b.min), glm::vec3(epsilon))) &&
		glm::all(glm::lessThanEqual(glm::abs(a.max - b.max), glm::vec3(epsilon)));
}

void scene_hash_mix(u64& state, u64 value) {
	state ^= value + 0x9e3779b97f4a7c15ull + (state << 6) + (state >> 2);
}

u64 scene_hash_node(sg_node_t const& node) {
	u64 h = 1469598103934665603ull;
	scene_hash_mix(h, static_cast<u64>(node.type));
	scene_hash_mix(h, static_cast<u64>(node.props.size()));
	for (auto const& prop : node.props) {
		if (prop == nullptr) {
			continue;
		}
		scene_hash_mix(h, std::hash<std::string>{}(prop->get_name()));
		scene_hash_mix(h, std::hash<f32>{}(prop->get_cur_value()));
	}
	scene_hash_mix(h, static_cast<u64>(node.effects.size()));
	for (auto const& effect : node.effects) {
		if (effect == nullptr) {
			continue;
		}
		scene_hash_mix(h, static_cast<u64>(effect->type));
		for (auto const& prop : effect->props) {
			if (prop == nullptr) {
				continue;
			}
			scene_hash_mix(h, std::hash<std::string>{}(prop->get_name()));
			scene_hash_mix(h, std::hash<f32>{}(prop->get_cur_value()));
		}
	}
	scene_hash_mix(h, static_cast<u64>(node.children.size()));
	for (auto const& child : node.children) {
		if (child == nullptr) {
			continue;
		}
		scene_hash_mix(h, scene_hash_node(*child.get()));
	}
	return h;
}

} // namespace

u64 scene_hash_tree(sg_node_t const& root) {
	return scene_hash_node(root);
}

std::unordered_map<u64, scene_aabb_t> scene_collect_primitive_bounds(sg_node_t const& root) {
	std::unordered_map<u64, scene_aabb_t> bounds;
	scene_affine_t identity;
	scene_collect_primitive_bounds_recursive(root, identity, bounds);
	return bounds;
}

bool scene_bounds_equal(
	std::unordered_map<u64, scene_aabb_t> const& a, std::unordered_map<u64, scene_aabb_t> const& b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto const& [node_id, a_box] : a) {
		auto const it = b.find(node_id);
		if (it == b.end()) {
			return false;
		}
		if (!scene_aabb_equal(a_box, it->second)) {
			return false;
		}
	}
	return true;
}

void scene_mark_bounds_set_dirty(sg_renderer_t& renderer, std::unordered_map<u64, scene_aabb_t> const& bounds) {
	for (auto const& [_, box] : bounds) {
		sg_renderer_mark_bounds_dirty(renderer, box.min, box.max);
	}
}

void scene_mark_primitive_overlap_dirty(sg_renderer_t& renderer,
	std::unordered_map<u64, scene_aabb_t> const& old_bounds,
	std::unordered_map<u64, scene_aabb_t> const& new_bounds) {
	for (auto const& [node_id, new_box] : new_bounds) {
		auto const old_it = old_bounds.find(node_id);
		if (old_it == old_bounds.end()) {
			sg_renderer_mark_bounds_dirty(renderer, new_box.min, new_box.max);
			continue;
		}
		if (!scene_aabb_equal(old_it->second, new_box)) {
			glm::vec3 const dirty_min = glm::min(old_it->second.min, new_box.min);
			glm::vec3 const dirty_max = glm::max(old_it->second.max, new_box.max);
			sg_renderer_mark_bounds_dirty(renderer, dirty_min, dirty_max);
		}
	}

	for (auto const& [node_id, old_box] : old_bounds) {
		if (!new_bounds.contains(node_id)) {
			sg_renderer_mark_bounds_dirty(renderer, old_box.min, old_box.max);
		}
	}
}
