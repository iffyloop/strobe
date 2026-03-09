#pragma once

#include "pch.h"

#include "sg_node.h"
#include "sg_renderer.h"

struct scene_aabb_t {
	glm::vec3 min = glm::vec3(0.0f);
	glm::vec3 max = glm::vec3(0.0f);
};

u64 scene_hash_tree(sg_node_t const& root);
std::unordered_map<u64, scene_aabb_t> scene_collect_primitive_bounds(sg_node_t const& root);
bool scene_bounds_equal(std::unordered_map<u64, scene_aabb_t> const& a, std::unordered_map<u64, scene_aabb_t> const& b);
void scene_mark_bounds_set_dirty(sg_renderer_t& renderer, std::unordered_map<u64, scene_aabb_t> const& bounds);
void scene_mark_primitive_overlap_dirty(sg_renderer_t& renderer,
	std::unordered_map<u64, scene_aabb_t> const& old_bounds,
	std::unordered_map<u64, scene_aabb_t> const& new_bounds);
