#include "pch.h"

#include "sg_renderer.h"

#include "marching_cubes_tables.h"
#include "sg_plugins.h"

#include <future>
#include <thread>

namespace {

constexpr s32 k_max_texture_slots = 16;
constexpr u32 k_marching_cubes_max_tris_per_cell = 5;
constexpr f32 k_min_scale = 1.0e-4f;

struct sg_renderer_chunk_layout_t {
	u32 cells_per_axis = 0;
	u32 chunk_resolution = 0;
	u32 chunks_per_axis = 0;
	u32 num_chunks = 0;
	u32 chunk_vertex_capacity = 0;
	u32 total_vertex_capacity = 0;
};

struct sg_renderer_mc_vertex_t {
	glm::vec3 pos = glm::vec3(0.0f);
	glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);
};

struct sg_renderer_runtime_ids_t {
	s32 combine_union = 0;
	s32 combine_intersect = 0;
	s32 combine_subtract = 0;
	s32 primitive_cube = 0;
	s32 primitive_sphere = 0;
	s32 effect_translate = 0;
	s32 effect_scale = 0;
	s32 effect_rotate = 0;
};

struct sg_renderer_chunk_build_result_t {
	u32 chunk_index = 0;
	std::vector<sg_renderer_mc_vertex_t> vertices;
};

struct sg_renderer_chunk_build_input_t {
	u32 chunk_index = 0;
	sg_renderer_chunk_layout_t layout;
	sg_renderer_runtime_ids_t runtime_ids;
	sg_compiled_scene_t const* compiled_scene = nullptr;
	u32 grid_resolution = 0;
	f32 iso_level = 0.0f;
	glm::vec3 bounds_min = glm::vec3(0.0f);
	glm::vec3 bounds_size = glm::vec3(1.0f);
	bool smooth_normals = true;
};

constexpr glm::ivec3 k_corner_offsets[8] = {
	glm::ivec3(0, 0, 0),
	glm::ivec3(1, 0, 0),
	glm::ivec3(1, 1, 0),
	glm::ivec3(0, 1, 0),
	glm::ivec3(0, 0, 1),
	glm::ivec3(1, 0, 1),
	glm::ivec3(1, 1, 1),
	glm::ivec3(0, 1, 1),
};

constexpr glm::ivec2 k_edge_corners[12] = {
	glm::ivec2(0, 1),
	glm::ivec2(1, 2),
	glm::ivec2(2, 3),
	glm::ivec2(3, 0),
	glm::ivec2(4, 5),
	glm::ivec2(5, 6),
	glm::ivec2(6, 7),
	glm::ivec2(7, 4),
	glm::ivec2(0, 4),
	glm::ivec2(1, 5),
	glm::ivec2(2, 6),
	glm::ivec2(3, 7),
};

sg_renderer_chunk_layout_t sg_renderer_compute_chunk_layout(sg_renderer_t const& renderer) {
	sg_renderer_chunk_layout_t layout;
	layout.cells_per_axis = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	layout.chunk_resolution = std::max(1u, renderer.marching_cubes_chunk_resolution);
	layout.chunks_per_axis = (layout.cells_per_axis + layout.chunk_resolution - 1u) / layout.chunk_resolution;
	layout.num_chunks = layout.chunks_per_axis * layout.chunks_per_axis * layout.chunks_per_axis;
	layout.chunk_vertex_capacity = layout.chunk_resolution * layout.chunk_resolution * layout.chunk_resolution *
		k_marching_cubes_max_tris_per_cell * 3u;
	layout.total_vertex_capacity = layout.num_chunks * layout.chunk_vertex_capacity;
	return layout;
}

u32 sg_renderer_chunk_index(u32 x, u32 y, u32 z, u32 chunks_per_axis) {
	return x + y * chunks_per_axis + z * chunks_per_axis * chunks_per_axis;
}

glm::vec3 sg_renderer_compute_snapped_center(sg_renderer_t const& renderer, glm::vec3 const& camera_pos) {
	f32 const extent = std::max(renderer.marching_cubes_bounds_extent, 0.001f);
	u32 const grid_cells = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	u32 const chunk_cells = std::max(1u, renderer.marching_cubes_chunk_resolution);
	f32 const cell_size = (extent * 2.0f) / static_cast<f32>(grid_cells);
	f32 const chunk_world_size = std::max(cell_size * static_cast<f32>(chunk_cells), 0.001f);
	return glm::round(camera_pos / chunk_world_size) * chunk_world_size;
}

f32 sg_renderer_chunk_world_size(sg_renderer_t const& renderer) {
	f32 const extent = std::max(renderer.marching_cubes_bounds_extent, 0.001f);
	u32 const grid_cells = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	u32 const chunk_cells = std::max(1u, renderer.marching_cubes_chunk_resolution);
	f32 const cell_size = (extent * 2.0f) / static_cast<f32>(grid_cells);
	return std::max(cell_size * static_cast<f32>(chunk_cells), 0.001f);
}

void sg_renderer_create_checker_texture(sg_renderer_t& renderer) {
	if (renderer.checker_texture != 0) {
		return;
	}

	constexpr u8 magenta[3] = {255, 0, 255};
	constexpr u8 black[3] = {0, 0, 0};
	u8 const pixels[2 * 2 * 3] = {
		magenta[0],
		magenta[1],
		magenta[2],
		black[0],
		black[1],
		black[2],
		black[0],
		black[1],
		black[2],
		magenta[0],
		magenta[1],
		magenta[2],
	};

	glGenTextures(1, &renderer.checker_texture);
	glBindTexture(GL_TEXTURE_2D, renderer.checker_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
}

std::string sg_renderer_normalize_texture_path(std::string const& path) {
	if (path.empty()) {
		return path;
	}

	std::filesystem::path fs_path(path);
	std::error_code ec;
	std::filesystem::path const absolute = std::filesystem::absolute(fs_path, ec);
	if (!ec) {
		fs_path = absolute;
	}
	fs_path = fs_path.lexically_normal();
	return fs_path.string();
}

sg_renderer_runtime_ids_t sg_renderer_get_runtime_ids() {
	sg_renderer_runtime_ids_t ids;
	auto const* combine_union = sg_plugins_find_combine_by_id("union");
	auto const* combine_intersect = sg_plugins_find_combine_by_id("intersect");
	auto const* combine_subtract = sg_plugins_find_combine_by_id("subtract");
	auto const* primitive_cube = sg_plugins_find_primitive_by_id("cube");
	auto const* primitive_sphere = sg_plugins_find_primitive_by_id("sphere");
	auto const* effect_translate = sg_plugins_find_effect_by_id("translate");
	auto const* effect_scale = sg_plugins_find_effect_by_id("scale");
	auto const* effect_rotate = sg_plugins_find_effect_by_id("rotate");
	ids.combine_union = combine_union == nullptr ? 0 : combine_union->runtime_id;
	ids.combine_intersect = combine_intersect == nullptr ? 0 : combine_intersect->runtime_id;
	ids.combine_subtract = combine_subtract == nullptr ? 0 : combine_subtract->runtime_id;
	ids.primitive_cube = primitive_cube == nullptr ? 0 : primitive_cube->runtime_id;
	ids.primitive_sphere = primitive_sphere == nullptr ? 0 : primitive_sphere->runtime_id;
	ids.effect_translate = effect_translate == nullptr ? 0 : effect_translate->runtime_id;
	ids.effect_scale = effect_scale == nullptr ? 0 : effect_scale->runtime_id;
	ids.effect_rotate = effect_rotate == nullptr ? 0 : effect_rotate->runtime_id;
	return ids;
}

void sg_renderer_destroy_marching_cubes_buffers(sg_renderer_t& renderer) {
	if (renderer.marching_cubes_vbo != 0) {
		glDeleteBuffers(1, &renderer.marching_cubes_vbo);
		renderer.marching_cubes_vbo = 0;
	}
	renderer.marching_cubes_vertex_capacity = 0;
	renderer.marching_cubes_vertex_count = 0;
	renderer.marching_cubes_chunks_per_axis = 0;
	renderer.marching_cubes_num_chunks = 0;
	renderer.marching_cubes_chunk_vertex_capacity = 0;
	renderer.marching_cubes_chunk_scan_cursor = 0;
	renderer.marching_cubes_chunk_dirty.clear();
	renderer.marching_cubes_chunk_vertex_counts.clear();
	renderer.marching_cubes_chunk_draw_first.clear();
	renderer.marching_cubes_chunk_draw_count.clear();
}

void sg_renderer_ensure_marching_cubes_buffers(sg_renderer_t& renderer) {
	sg_renderer_chunk_layout_t const layout = sg_renderer_compute_chunk_layout(renderer);
	if (renderer.marching_cubes_vbo != 0 && renderer.marching_cubes_vertex_capacity >= layout.total_vertex_capacity &&
		renderer.marching_cubes_num_chunks == layout.num_chunks &&
		renderer.marching_cubes_chunk_vertex_capacity == layout.chunk_vertex_capacity &&
		renderer.marching_cubes_chunks_per_axis == layout.chunks_per_axis) {
		return;
	}

	sg_renderer_destroy_marching_cubes_buffers(renderer);

	glGenBuffers(1, &renderer.marching_cubes_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, renderer.marching_cubes_vbo);
	glBufferData(GL_ARRAY_BUFFER,
		static_cast<GLsizeiptr>(layout.total_vertex_capacity) * static_cast<GLsizeiptr>(sizeof(sg_renderer_mc_vertex_t)),
		nullptr, GL_DYNAMIC_DRAW);

	glBindVertexArray(renderer.marching_cubes_vao);
	glBindBuffer(GL_ARRAY_BUFFER, renderer.marching_cubes_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(
		0, 3, GL_FLOAT, GL_FALSE, sizeof(sg_renderer_mc_vertex_t), reinterpret_cast<void*>(offsetof(sg_renderer_mc_vertex_t, pos)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(sg_renderer_mc_vertex_t),
		reinterpret_cast<void*>(offsetof(sg_renderer_mc_vertex_t, normal)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	renderer.marching_cubes_vertex_capacity = layout.total_vertex_capacity;
	renderer.marching_cubes_vertex_count = 0;
	renderer.marching_cubes_chunks_per_axis = layout.chunks_per_axis;
	renderer.marching_cubes_num_chunks = layout.num_chunks;
	renderer.marching_cubes_chunk_vertex_capacity = layout.chunk_vertex_capacity;
	renderer.marching_cubes_chunk_dirty.assign(layout.num_chunks, 1u);
	renderer.marching_cubes_chunk_vertex_counts.assign(layout.num_chunks, 0u);
	renderer.marching_cubes_chunk_draw_first.resize(layout.num_chunks);
	renderer.marching_cubes_chunk_draw_count.assign(layout.num_chunks, 0);
	for (u32 i = 0; i < layout.num_chunks; ++i) {
		renderer.marching_cubes_chunk_draw_first[i] = static_cast<GLint>(i * layout.chunk_vertex_capacity);
	}
}

bool sg_renderer_reload_plugins(sg_renderer_t& renderer) {
	sg_plugins_t candidate_plugins = sg_plugins_get();
	std::string load_error;
	if (!sg_plugins_load_candidate(candidate_plugins, load_error)) {
		renderer.plugin_reload_status = "Plugin load failed: " + load_error;
		std::cerr << "[sg_renderer] " << renderer.plugin_reload_status << std::endl;
		return false;
	}

	sg_plugins_commit(std::move(candidate_plugins));
	renderer.plugin_reload_status = "Plugins reloaded successfully!";
	return true;
}

f32 sg_renderer_eval_primitive(
	sg_compiled_scene_t const& scene, sg_renderer_runtime_ids_t const& runtime_ids, s32 primitive_index, glm::vec3 p_world) {
	if (primitive_index < 0 || primitive_index >= static_cast<s32>(scene.primitive_meta.size())) {
		return 1.0e6f;
	}

	glm::ivec4 const primitive_meta = scene.primitive_meta[primitive_index];
	glm::vec4 const primitive_params = scene.primitive_params[primitive_index];
	f32 const uniform_scale = std::max(scene.primitive_scale[primitive_index].x, k_min_scale);

	glm::vec3 p_local = p_world;
	glm::ivec4 const effect_range = scene.primitive_effect_ranges[primitive_index];
	for (s32 i = 0; i < effect_range.y; ++i) {
		s32 const effect_index = effect_range.x + i;
		if (effect_index < 0 || effect_index >= static_cast<s32>(scene.effect_meta.size())) {
			continue;
		}
		s32 const effect_op = scene.effect_meta[effect_index].x;
		glm::vec4 const effect_params = scene.effect_params[effect_index];
		if (effect_op == runtime_ids.effect_translate) {
			p_local -= glm::vec3(effect_params);
		} else if (effect_op == runtime_ids.effect_scale) {
			f32 const s = std::max(std::abs(effect_params.x), k_min_scale);
			p_local /= s;
		} else if (effect_op == runtime_ids.effect_rotate) {
			f32 const cx = std::cos(-effect_params.x);
			f32 const sx = std::sin(-effect_params.x);
			f32 const cy = std::cos(-effect_params.y);
			f32 const sy = std::sin(-effect_params.y);
			f32 const cz = std::cos(-effect_params.z);
			f32 const sz = std::sin(-effect_params.z);

			p_local = glm::vec3(cz * p_local.x - sz * p_local.y, sz * p_local.x + cz * p_local.y, p_local.z);
			p_local = glm::vec3(cy * p_local.x + sy * p_local.z, p_local.y, -sy * p_local.x + cy * p_local.z);
			p_local = glm::vec3(p_local.x, cx * p_local.y - sx * p_local.z, sx * p_local.y + cx * p_local.z);
		}
	}

	f32 dist = 1.0e6f;
	if (primitive_meta.x == runtime_ids.primitive_sphere) {
		dist = glm::length(p_local) - primitive_params.x;
	} else if (primitive_meta.x == runtime_ids.primitive_cube) {
		glm::vec3 const half_extents = glm::max(glm::vec3(primitive_params) * 0.5f, glm::vec3(k_min_scale));
		glm::vec3 const q = glm::abs(p_local) - half_extents;
		dist = glm::length(glm::max(q, glm::vec3(0.0f))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
	}

	return dist * uniform_scale;
}

f32 sg_renderer_eval_scene(sg_compiled_scene_t const& scene, sg_renderer_runtime_ids_t const& runtime_ids, glm::vec3 p_world) {
	constexpr s32 k_max_stack = 64;
	f32 stack[k_max_stack];
	s32 sp = 0;

	for (glm::ivec4 const& inst : scene.program) {
		if (inst.x == SG_COMPILER_OP_PUSH_PRIMITIVE) {
			if (sp >= k_max_stack) {
				return 1.0e6f;
			}
			stack[sp++] = sg_renderer_eval_primitive(scene, runtime_ids, inst.y, p_world);
			continue;
		}

		if (inst.x != SG_COMPILER_OP_COMBINE || sp < 2) {
			return 1.0e6f;
		}

		f32 const b = stack[--sp];
		f32 const a = stack[--sp];
		f32 out = a;
		if (inst.y == runtime_ids.combine_union) {
			out = std::min(a, b);
		} else if (inst.y == runtime_ids.combine_intersect) {
			out = std::max(a, b);
		} else if (inst.y == runtime_ids.combine_subtract) {
			out = std::max(a, -b);
		}
		stack[sp++] = out;
	}

	if (sp == 0) {
		return 1.0e6f;
	}
	return stack[sp - 1];
}

inline u32 sg_renderer_density_index(u32 x, u32 y, u32 z, glm::ivec3 const& dims) {
	return x + static_cast<u32>(dims.x) * (y + static_cast<u32>(dims.y) * z);
}

sg_renderer_chunk_build_result_t sg_renderer_build_chunk_mesh(sg_renderer_chunk_build_input_t const& input) {
	sg_renderer_chunk_build_result_t out;
	out.chunk_index = input.chunk_index;
	if (input.compiled_scene == nullptr || !input.compiled_scene->has_output) {
		return out;
	}

	u32 const chunks_per_axis = input.layout.chunks_per_axis;
	u32 const chunk_cells = input.layout.chunk_resolution;
	u32 const grid_cells = input.grid_resolution;

	u32 const chunk_z = input.chunk_index / (chunks_per_axis * chunks_per_axis);
	u32 const rem = input.chunk_index % (chunks_per_axis * chunks_per_axis);
	u32 const chunk_y = rem / chunks_per_axis;
	u32 const chunk_x = rem % chunks_per_axis;

	glm::ivec3 const chunk_origin = glm::ivec3(static_cast<s32>(chunk_x * chunk_cells),
		static_cast<s32>(chunk_y * chunk_cells), static_cast<s32>(chunk_z * chunk_cells));
	glm::ivec3 const chunk_limit = glm::ivec3(static_cast<s32>(std::min(grid_cells, (chunk_x + 1u) * chunk_cells)),
		static_cast<s32>(std::min(grid_cells, (chunk_y + 1u) * chunk_cells)),
		static_cast<s32>(std::min(grid_cells, (chunk_z + 1u) * chunk_cells)));
	glm::ivec3 const chunk_valid = chunk_limit - chunk_origin;
	if (chunk_valid.x <= 0 || chunk_valid.y <= 0 || chunk_valid.z <= 0) {
		return out;
	}

	glm::ivec3 const point_min = glm::max(chunk_origin - glm::ivec3(1), glm::ivec3(0));
	glm::ivec3 const point_max =
		glm::min(chunk_limit + glm::ivec3(1), glm::ivec3(static_cast<s32>(grid_cells)));
	glm::ivec3 const point_dims = point_max - point_min + glm::ivec3(1);
	u32 const density_count = static_cast<u32>(point_dims.x * point_dims.y * point_dims.z);
	std::vector<f32> densities(density_count, 1.0e6f);

	auto point_to_world = [&](glm::ivec3 const& p) {
		glm::vec3 const t = glm::vec3(p) / static_cast<f32>(grid_cells);
		return input.bounds_min + t * input.bounds_size;
	};

	for (s32 z = 0; z < point_dims.z; ++z) {
		for (s32 y = 0; y < point_dims.y; ++y) {
			for (s32 x = 0; x < point_dims.x; ++x) {
				glm::ivec3 const gp = point_min + glm::ivec3(x, y, z);
				u32 const idx = sg_renderer_density_index(
					static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z), point_dims);
				densities[idx] = sg_renderer_eval_scene(*input.compiled_scene, input.runtime_ids, point_to_world(gp));
			}
		}
	}

	auto sample_density = [&](glm::ivec3 const& gp) {
		glm::ivec3 const clamped = glm::clamp(gp, point_min, point_max);
		glm::ivec3 const local = clamped - point_min;
		u32 const idx = sg_renderer_density_index(
			static_cast<u32>(local.x), static_cast<u32>(local.y), static_cast<u32>(local.z), point_dims);
		return densities[idx];
	};

	auto density_gradient = [&](glm::ivec3 const& gp) {
		f32 const dx = sample_density(gp + glm::ivec3(1, 0, 0)) - sample_density(gp - glm::ivec3(1, 0, 0));
		f32 const dy = sample_density(gp + glm::ivec3(0, 1, 0)) - sample_density(gp - glm::ivec3(0, 1, 0));
		f32 const dz = sample_density(gp + glm::ivec3(0, 0, 1)) - sample_density(gp - glm::ivec3(0, 0, 1));
		glm::vec3 const g(dx, dy, dz);
		f32 const len2 = glm::dot(g, g);
		if (len2 <= 1.0e-20f) {
			return glm::vec3(0.0f, 1.0f, 0.0f);
		}
		return glm::normalize(g);
	};

	out.vertices.reserve(input.layout.chunk_vertex_capacity);

	for (s32 z = 0; z < chunk_valid.z; ++z) {
		for (s32 y = 0; y < chunk_valid.y; ++y) {
			for (s32 x = 0; x < chunk_valid.x; ++x) {
				glm::ivec3 const cell = chunk_origin + glm::ivec3(x, y, z);

				glm::ivec3 gp[8];
				glm::vec3 pos[8];
				f32 dens[8];
				glm::vec3 grad[8];

				for (s32 i = 0; i < 8; ++i) {
					gp[i] = cell + k_corner_offsets[i];
					pos[i] = point_to_world(gp[i]);
					dens[i] = sample_density(gp[i]);
					grad[i] = input.smooth_normals ? density_gradient(gp[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
				}

				s32 case_index = 0;
				for (s32 i = 0; i < 8; ++i) {
					if (dens[i] < input.iso_level) {
						case_index |= (1 << i);
					}
				}

				s32 const edge_mask = k_marching_cubes_edge_table[case_index];
				if (edge_mask == 0) {
					continue;
				}

				glm::vec3 edge_pos[12];
				glm::vec3 edge_normal[12];
				for (s32 e = 0; e < 12; ++e) {
					if ((edge_mask & (1 << e)) == 0) {
						continue;
					}
					glm::ivec2 const corners = k_edge_corners[e];
					s32 const a = corners.x;
					s32 const b = corners.y;
					f32 const denom = dens[b] - dens[a];
					f32 t = 0.5f;
					if (std::abs(denom) > 1.0e-6f) {
						t = std::clamp((input.iso_level - dens[a]) / denom, 0.0f, 1.0f);
					}
					edge_pos[e] = glm::mix(pos[a], pos[b], t);
					if (input.smooth_normals) {
						edge_normal[e] = glm::normalize(glm::mix(grad[a], grad[b], t));
					}
				}

				s32 const tri_base = case_index * 16;
				for (s32 t = 0; t < 16; t += 3) {
					s32 const ia = k_marching_cubes_tri_table[tri_base + t + 0];
					if (ia < 0) {
						break;
					}
					s32 const ib = k_marching_cubes_tri_table[tri_base + t + 1];
					s32 const ic = k_marching_cubes_tri_table[tri_base + t + 2];

					glm::vec3 const a = edge_pos[ia];
					glm::vec3 const b = edge_pos[ic];
					glm::vec3 const c = edge_pos[ib];
					glm::vec3 tri_n = glm::cross(c - a, b - a);
					f32 const tri_n_len2 = glm::dot(tri_n, tri_n);
					if (tri_n_len2 <= 1.0e-20f) {
						continue;
					}
					tri_n = glm::normalize(tri_n);

					sg_renderer_mc_vertex_t va;
					sg_renderer_mc_vertex_t vb;
					sg_renderer_mc_vertex_t vc;
					va.pos = a;
					vb.pos = b;
					vc.pos = c;

					if (input.smooth_normals) {
						va.normal = edge_normal[ia];
						vb.normal = edge_normal[ic];
						vc.normal = edge_normal[ib];
						if (!std::isfinite(va.normal.x) || !std::isfinite(vb.normal.x) || !std::isfinite(vc.normal.x)) {
							va.normal = tri_n;
							vb.normal = tri_n;
							vc.normal = tri_n;
						}
					} else {
						va.normal = tri_n;
						vb.normal = tri_n;
						vc.normal = tri_n;
					}

					out.vertices.emplace_back(va);
					out.vertices.emplace_back(vb);
					out.vertices.emplace_back(vc);
				}
			}
		}
	}

	return out;
}

void sg_renderer_clear_mesh_counts(sg_renderer_t& renderer) {
	renderer.marching_cubes_vertex_count = 0;
	std::fill(renderer.marching_cubes_chunk_vertex_counts.begin(), renderer.marching_cubes_chunk_vertex_counts.end(), 0u);
	std::fill(renderer.marching_cubes_chunk_draw_count.begin(), renderer.marching_cubes_chunk_draw_count.end(), 0);
}

void sg_renderer_remesh_dirty_chunks(sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene) {
	if (renderer.marching_cubes_chunk_dirty.empty() || renderer.marching_cubes_num_chunks == 0u) {
		return;
	}

	u32 const budget = static_cast<u32>(std::max(1, renderer.marching_cubes_remesh_budget_chunks));
	u32 const start_idx = renderer.marching_cubes_chunk_scan_cursor % renderer.marching_cubes_num_chunks;
	u32 scanned = 0;
	std::vector<u32> dirty_chunks;
	dirty_chunks.reserve(budget);
	while (scanned < renderer.marching_cubes_num_chunks && dirty_chunks.size() < budget) {
		u32 const chunk_idx = (start_idx + scanned) % renderer.marching_cubes_num_chunks;
		scanned += 1;
		if (renderer.marching_cubes_chunk_dirty[chunk_idx] == 0) {
			continue;
		}
		dirty_chunks.emplace_back(chunk_idx);
	}
	renderer.marching_cubes_chunk_scan_cursor = (start_idx + scanned) % renderer.marching_cubes_num_chunks;
	renderer.marching_cubes_last_remeshed_chunk_count = static_cast<u32>(dirty_chunks.size());
	if (dirty_chunks.empty()) {
		return;
	}

	sg_renderer_chunk_layout_t const layout = sg_renderer_compute_chunk_layout(renderer);
	sg_renderer_runtime_ids_t const runtime_ids = sg_renderer_get_runtime_ids();
	glm::vec3 const bounds_min = renderer.marching_cubes_center - glm::vec3(renderer.marching_cubes_bounds_extent);
	glm::vec3 const bounds_max = renderer.marching_cubes_center + glm::vec3(renderer.marching_cubes_bounds_extent);
	glm::vec3 const bounds_size = bounds_max - bounds_min;
	u32 const grid_resolution = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));

	u32 const hw_threads = std::max(1u, std::thread::hardware_concurrency());
	u32 const worker_count = std::min<u32>(hw_threads, static_cast<u32>(dirty_chunks.size()));
	size_t const batch_size = (dirty_chunks.size() + worker_count - 1u) / worker_count;

	std::vector<std::future<std::vector<sg_renderer_chunk_build_result_t>>> futures;
	futures.reserve(worker_count);
	for (u32 worker = 0; worker < worker_count; ++worker) {
		size_t const begin = static_cast<size_t>(worker) * batch_size;
		size_t const end = std::min(dirty_chunks.size(), begin + batch_size);
		if (begin >= end) {
			continue;
		}

		futures.emplace_back(std::async(std::launch::async,
			[begin, end, &dirty_chunks, layout, runtime_ids, &compiled_scene, grid_resolution, iso = renderer.marching_cubes_iso_level,
				bounds_min, bounds_size, smooth = renderer.marching_cubes_smooth_normals]() {
				std::vector<sg_renderer_chunk_build_result_t> results;
				results.reserve(end - begin);
				for (size_t i = begin; i < end; ++i) {
					sg_renderer_chunk_build_input_t input;
					input.chunk_index = dirty_chunks[i];
					input.layout = layout;
					input.runtime_ids = runtime_ids;
					input.compiled_scene = &compiled_scene;
					input.grid_resolution = grid_resolution;
					input.iso_level = iso;
					input.bounds_min = bounds_min;
					input.bounds_size = bounds_size;
					input.smooth_normals = smooth;
					results.emplace_back(sg_renderer_build_chunk_mesh(input));
				}
				return results;
			}));
	}

	glBindBuffer(GL_ARRAY_BUFFER, renderer.marching_cubes_vbo);
	s64 total_delta = 0;
	for (auto& future : futures) {
		std::vector<sg_renderer_chunk_build_result_t> const results = future.get();
		for (auto const& result : results) {
			u32 const chunk_idx = result.chunk_index;
			u32 const previous_count = renderer.marching_cubes_chunk_vertex_counts[chunk_idx];
			u32 const count = std::min<u32>(static_cast<u32>(result.vertices.size()), renderer.marching_cubes_chunk_vertex_capacity);
			renderer.marching_cubes_chunk_vertex_counts[chunk_idx] = count;
			renderer.marching_cubes_chunk_draw_count[chunk_idx] = static_cast<GLsizei>(count);
			total_delta += static_cast<s64>(count) - static_cast<s64>(previous_count);

			if (count > 0) {
				size_t const base_vertex = static_cast<size_t>(chunk_idx) * renderer.marching_cubes_chunk_vertex_capacity;
				GLintptr const dst_offset = static_cast<GLintptr>(base_vertex * sizeof(sg_renderer_mc_vertex_t));
				GLsizeiptr const upload_size = static_cast<GLsizeiptr>(count * sizeof(sg_renderer_mc_vertex_t));
				glBufferSubData(GL_ARRAY_BUFFER, dst_offset, upload_size, result.vertices.data());
			}

			renderer.marching_cubes_chunk_dirty[chunk_idx] = 0;
		}
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	s64 const updated_count = static_cast<s64>(renderer.marching_cubes_vertex_count) + total_delta;
	renderer.marching_cubes_vertex_count = static_cast<u32>(std::max<s64>(0, updated_count));
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

	gl_render_pass_init(renderer.primary_render_pass, "shaders/mc_mesh.vert", "shaders/mc_mesh.frag", 2048.0f,
		2048.0f,
		{
			{"o_color", {.internal_format = GL_RGBA8, .format = GL_RGBA, .type = GL_UNSIGNED_BYTE}},
			{"##DEPTH",
				{.internal_format = GL_DEPTH_COMPONENT24,
					.format = GL_DEPTH_COMPONENT,
					.type = GL_FLOAT}},
		});
	renderer.primary_render_pass.depth_func = GL_LESS;
	renderer.primary_render_pass.cull_face = GL_BACK;

	glGenVertexArrays(1, &renderer.marching_cubes_vao);
	sg_renderer_ensure_marching_cubes_buffers(renderer);
	sg_renderer_create_checker_texture(renderer);
	renderer.plugin_reload_status = "CPU marching cubes active";
}

void sg_renderer_destroy(sg_renderer_t& renderer) {
	sg_renderer_destroy_marching_cubes_buffers(renderer);

	if (renderer.marching_cubes_vao != 0) {
		glDeleteVertexArrays(1, &renderer.marching_cubes_vao);
		renderer.marching_cubes_vao = 0;
	}

	if (renderer.checker_texture != 0) {
		glDeleteTextures(1, &renderer.checker_texture);
		renderer.checker_texture = 0;
	}

	for (auto& texture_entry : renderer.primitive_textures) {
		if (texture_entry.texture != 0) {
			glDeleteTextures(1, &texture_entry.texture);
			texture_entry.texture = 0;
		}
	}
	renderer.primitive_textures.clear();
	renderer.primitive_texture_ids_by_path.clear();
}

s32 sg_renderer_get_or_load_primitive_texture(sg_renderer_t& renderer, std::string const& path) {
	std::string const normalized_path = sg_renderer_normalize_texture_path(path);
	if (normalized_path.empty()) {
		return 0;
	}

	auto it = renderer.primitive_texture_ids_by_path.find(normalized_path);
	if (it != renderer.primitive_texture_ids_by_path.end()) {
		return it->second;
	}

	if (renderer.primitive_textures.size() + 1 >= static_cast<size_t>(k_max_texture_slots)) {
		std::cerr << "[sg_renderer] maximum primitive texture slots reached (" << (k_max_texture_slots - 1)
				  << ")" << std::endl;
		return 0;
	}

	stbi_set_flip_vertically_on_load(1);
	s32 width = 0;
	s32 height = 0;
	s32 channels = 0;
	u8* pixels = stbi_load(normalized_path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
	if (pixels == nullptr) {
		std::cerr << "[sg_renderer] failed to load texture: " << normalized_path << std::endl;
		return 0;
	}

	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	stbi_image_free(pixels);

	sg_renderer_image_texture_t entry;
	entry.texture = texture;
	entry.path = normalized_path;
	renderer.primitive_textures.emplace_back(std::move(entry));

	s32 const texture_id = static_cast<s32>(renderer.primitive_textures.size());
	renderer.primitive_texture_ids_by_path[normalized_path] = texture_id;
	return texture_id;
}

std::string const* sg_renderer_get_primitive_texture_path(sg_renderer_t const& renderer, s32 texture_id) {
	if (texture_id <= 0) {
		return nullptr;
	}

	size_t const entry_index = static_cast<size_t>(texture_id - 1);
	if (entry_index >= renderer.primitive_textures.size()) {
		return nullptr;
	}

	return &renderer.primitive_textures[entry_index].path;
}

void sg_renderer_update(sg_renderer_t& renderer, sg_compiled_scene_t const& compiled_scene, fly_camera_t const& camera,
	bool scene_gpu_buffers_dirty) {
	if (scene_gpu_buffers_dirty) {
		sg_renderer_mark_all_chunks_dirty(renderer);
	}

	sg_renderer_ensure_marching_cubes_buffers(renderer);

	if (!compiled_scene.has_output) {
		sg_renderer_clear_mesh_counts(renderer);
		std::fill(renderer.marching_cubes_chunk_dirty.begin(), renderer.marching_cubes_chunk_dirty.end(), 1u);
	} else {
		if (renderer.marching_cubes_center_on_camera) {
			f32 const chunk_world_size = sg_renderer_chunk_world_size(renderer);
			f32 const deadzone_world =
				chunk_world_size * static_cast<f32>(std::max(1, renderer.marching_cubes_center_deadzone_chunks));
			glm::vec3 const camera_delta = glm::abs(camera.pos - renderer.marching_cubes_center);
			if (camera_delta.x > deadzone_world || camera_delta.y > deadzone_world || camera_delta.z > deadzone_world) {
				glm::vec3 const snapped_center = sg_renderer_compute_snapped_center(renderer, camera.pos);
				if (glm::any(
						glm::greaterThan(glm::abs(snapped_center - renderer.marching_cubes_center), glm::vec3(0.0001f)))) {
					renderer.marching_cubes_center = snapped_center;
					sg_renderer_mark_all_chunks_dirty(renderer);
				}
			}
		}

		bool has_dirty_chunk = false;
		for (u8 dirty_flag : renderer.marching_cubes_chunk_dirty) {
			if (dirty_flag != 0) {
				has_dirty_chunk = true;
				break;
			}
		}

		if (has_dirty_chunk) {
			sg_renderer_remesh_dirty_chunks(renderer, compiled_scene);
		} else {
			renderer.marching_cubes_last_remeshed_chunk_count = 0;
		}
	}

	gl_render_pass_begin(renderer.primary_render_pass);
	gl_render_pass_uniform_mat4(renderer.primary_render_pass, "u_view_proj", camera.proj_mat * camera.view_mat);
	GLint const camera_pos_location = glGetUniformLocation(renderer.primary_render_pass.shader.program, "u_camera_pos");
	if (camera_pos_location >= 0) {
		glUniform3fv(camera_pos_location, 1, glm::value_ptr(camera.pos));
	}

	glBindVertexArray(renderer.marching_cubes_vao);
	if (!renderer.marching_cubes_chunk_draw_count.empty()) {
		glMultiDrawArrays(GL_TRIANGLES, renderer.marching_cubes_chunk_draw_first.data(),
			renderer.marching_cubes_chunk_draw_count.data(), static_cast<GLsizei>(renderer.marching_cubes_num_chunks));
	}
	glBindVertexArray(0);
	gl_render_pass_end(renderer.primary_render_pass);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void sg_renderer_mark_all_chunks_dirty(sg_renderer_t& renderer) {
	if (renderer.marching_cubes_chunk_dirty.empty()) {
		return;
	}
	std::fill(renderer.marching_cubes_chunk_dirty.begin(), renderer.marching_cubes_chunk_dirty.end(), 1u);
}

void sg_renderer_mark_bounds_dirty(sg_renderer_t& renderer, glm::vec3 const& bounds_min, glm::vec3 const& bounds_max) {
	if (renderer.marching_cubes_chunk_dirty.empty() || renderer.marching_cubes_chunks_per_axis == 0u) {
		return;
	}
	if (bounds_min.x > bounds_max.x || bounds_min.y > bounds_max.y || bounds_min.z > bounds_max.z) {
		return;
	}

	f32 const extent = renderer.marching_cubes_bounds_extent;
	if (extent <= 0.0f) {
		sg_renderer_mark_all_chunks_dirty(renderer);
		return;
	}

	glm::vec3 const grid_min = renderer.marching_cubes_center - glm::vec3(extent);
	glm::vec3 const grid_max = renderer.marching_cubes_center + glm::vec3(extent);
	glm::vec3 const clamped_min = glm::max(bounds_min, grid_min);
	glm::vec3 const clamped_max = glm::min(bounds_max, grid_max);
	if (clamped_min.x > clamped_max.x || clamped_min.y > clamped_max.y || clamped_min.z > clamped_max.z) {
		return;
	}

	u32 const grid_cells = static_cast<u32>(std::max(1, renderer.marching_cubes_grid_resolution));
	f32 const cells_per_world_unit = static_cast<f32>(grid_cells) / (extent * 2.0f);
	auto world_to_cell = [&](f32 value, f32 axis_min) {
		return static_cast<s32>(std::floor((value - axis_min) * cells_per_world_unit));
	};

	s32 const min_x = std::clamp(world_to_cell(clamped_min.x, grid_min.x) - 1, 0, static_cast<s32>(grid_cells) - 1);
	s32 const min_y = std::clamp(world_to_cell(clamped_min.y, grid_min.y) - 1, 0, static_cast<s32>(grid_cells) - 1);
	s32 const min_z = std::clamp(world_to_cell(clamped_min.z, grid_min.z) - 1, 0, static_cast<s32>(grid_cells) - 1);
	s32 const max_x = std::clamp(world_to_cell(clamped_max.x, grid_min.x) + 1, 0, static_cast<s32>(grid_cells) - 1);
	s32 const max_y = std::clamp(world_to_cell(clamped_max.y, grid_min.y) + 1, 0, static_cast<s32>(grid_cells) - 1);
	s32 const max_z = std::clamp(world_to_cell(clamped_max.z, grid_min.z) + 1, 0, static_cast<s32>(grid_cells) - 1);

	u32 const chunk_cells = std::max(1u, renderer.marching_cubes_chunk_resolution);
	u32 const min_chunk_x = static_cast<u32>(min_x) / chunk_cells;
	u32 const min_chunk_y = static_cast<u32>(min_y) / chunk_cells;
	u32 const min_chunk_z = static_cast<u32>(min_z) / chunk_cells;
	u32 const max_chunk_x = static_cast<u32>(max_x) / chunk_cells;
	u32 const max_chunk_y = static_cast<u32>(max_y) / chunk_cells;
	u32 const max_chunk_z = static_cast<u32>(max_z) / chunk_cells;

	u32 const chunks_per_axis = renderer.marching_cubes_chunks_per_axis;
	for (u32 z = min_chunk_z; z <= max_chunk_z && z < chunks_per_axis; ++z) {
		for (u32 y = min_chunk_y; y <= max_chunk_y && y < chunks_per_axis; ++y) {
			for (u32 x = min_chunk_x; x <= max_chunk_x && x < chunks_per_axis; ++x) {
				u32 const idx = sg_renderer_chunk_index(x, y, z, chunks_per_axis);
				renderer.marching_cubes_chunk_dirty[idx] = 1u;
			}
		}
	}
}

bool sg_renderer_update_imgui(sg_renderer_t& renderer, bool input_enabled) {
	f32 const window_width = ImGui::GetIO().DisplaySize.x - 400.0f;
	f32 const window_height = ImGui::GetIO().DisplaySize.y;
	f32 const controls_height = 140.0f;
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
	GLuint const display_texture = renderer.primary_render_pass.internal_output_descriptors["o_color"].texture;
	f32 const image_aspect = static_cast<f32>(renderer.primary_render_pass.width) /
		static_cast<f32>(renderer.primary_render_pass.height);
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
	ImGui::Text("Generated vertices: CPU %u", renderer.marching_cubes_vertex_count);
	ImGui::Text("Remeshed chunks: %u", renderer.marching_cubes_last_remeshed_chunk_count);
	ImGui::Text("Volume center: %.2f, %.2f, %.2f", renderer.marching_cubes_center.x,
		renderer.marching_cubes_center.y, renderer.marching_cubes_center.z);
	bool remesh_settings_changed = false;

	ImGui::SetNextItemWidth(140.0f);
	remesh_settings_changed |= ImGui::DragInt("Grid", &renderer.marching_cubes_grid_resolution, 1.0f, 8, 96);

	ImGui::SameLine();
	remesh_settings_changed |= ImGui::Checkbox("Smooth Normals", &renderer.marching_cubes_smooth_normals);

	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	remesh_settings_changed |=
		ImGui::DragFloat("Iso", &renderer.marching_cubes_iso_level, 0.005f, -2.0f, 2.0f, "%.3f");

	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	remesh_settings_changed |=
		ImGui::DragFloat("Bounds", &renderer.marching_cubes_bounds_extent, 0.05f, 0.5f, 10.0f, "%.2f");

	ImGui::SameLine();
	bool center_follow_changed = ImGui::Checkbox("Center On Camera", &renderer.marching_cubes_center_on_camera);
	if (renderer.marching_cubes_center_on_camera) {
		ImGui::SetNextItemWidth(140.0f);
		center_follow_changed |=
			ImGui::DragInt("Deadzone Chunks", &renderer.marching_cubes_center_deadzone_chunks, 1.0f, 1, 16);
	}
	ImGui::SetNextItemWidth(140.0f);
	ImGui::DragInt("Remesh Budget", &renderer.marching_cubes_remesh_budget_chunks, 1.0f, 1, 512);
	if (center_follow_changed) {
		sg_renderer_mark_all_chunks_dirty(renderer);
	}

	if (remesh_settings_changed) {
		sg_renderer_mark_all_chunks_dirty(renderer);
	}

	if (ImGui::Button("Reload Plugins")) {
		if (!sg_renderer_reload_plugins(renderer)) {
			std::cerr << "[sg_renderer] plugin reload failed; keeping previous plugins" << std::endl;
		} else {
			sg_renderer_mark_all_chunks_dirty(renderer);
		}
	}

	ImGui::SameLine();
	if (!renderer.plugin_reload_status.empty()) {
		ImGui::TextWrapped("%s", renderer.plugin_reload_status.c_str());
	}

	ImGui::End();
	return clicked;
}
