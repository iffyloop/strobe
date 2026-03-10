#version 430 core

layout(location = 0) out vec4 o_color;

in vec2 v_uv;

uniform int u_has_scene;
uniform int u_grid_resolution;
uniform int u_chunk_resolution;
uniform float u_iso_level;
uniform int u_clipmap_levels;
uniform int u_debug_lod;
uniform int u_debug_surface_mode;
uniform vec3 u_bounds_min[4];
uniform vec3 u_bounds_max[4];
uniform int u_chunks_per_axis[4];
uniform int u_brick_pool_dim[4];
uniform vec3 u_camera_pos;
uniform mat4 u_view_proj;
uniform mat4 u_inv_view_proj;
uniform isampler3D u_brick_index_tex[4];
uniform sampler3D u_brick_atlas_tex[4];

vec2 ray_box_intersection(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax) {
    vec3 inv_rd = 1.0 / rd;
    vec3 t0 = (bmin - ro) * inv_rd;
    vec3 t1 = (bmax - ro) * inv_rd;
    vec3 tsmaller = min(t0, t1);
    vec3 tbigger = max(t0, t1);
    float t_near = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    float t_far = min(min(tbigger.x, tbigger.y), tbigger.z);
    return vec2(t_near, t_far);
}

float density_world_level(vec3 p_world, int level) {
    level = clamp(level, 0, max(u_clipmap_levels - 1, 0));
    float grid_res = float(u_grid_resolution);
    int chunk_res = max(u_chunk_resolution, 1);

    vec3 grid_pos_f = ((p_world - u_bounds_min[level]) / (u_bounds_max[level] - u_bounds_min[level])) * grid_res;
    grid_pos_f = clamp(grid_pos_f, vec3(0.0), vec3(grid_res));

    int chunks_axis = max(u_chunks_per_axis[level], 1);
    ivec3 gp = ivec3(floor(grid_pos_f));
    ivec3 chunk = clamp(gp / chunk_res, ivec3(0), ivec3(chunks_axis - 1));
    ivec4 brick_meta = texelFetch(u_brick_index_tex[level], chunk, 0);
    if (brick_meta.w <= 0) {
        return 1e4;
    }

    int brick_points = chunk_res + 1;
    vec3 local_f = clamp(grid_pos_f - vec3(chunk * chunk_res), vec3(0.0), vec3(float(chunk_res)));
    vec3 atlas_pos = vec3(brick_meta.xyz * brick_points) + local_f;
    float atlas_dim = float(max(u_brick_pool_dim[level], 1));
    vec3 uvw = (atlas_pos + vec3(0.5)) / atlas_dim;
    return texture(u_brick_atlas_tex[level], uvw).r;
}

int choose_lod(vec3 p_world) {
    int max_level = max(u_clipmap_levels - 1, 0);
    vec3 camera_delta = abs(p_world - u_camera_pos);
    float camera_space_dist = max(max(camera_delta.x, camera_delta.y), camera_delta.z);
    for (int level = 0; level < 4; ++level) {
        if (level > max_level) {
            break;
        }
        float half_extent = 0.5 * (u_bounds_max[level].x - u_bounds_min[level].x);
        if (camera_space_dist <= half_extent) {
            return level;
        }
    }
    return max_level;
}

float camera_space_linf(vec3 p_world) {
    vec3 d = abs(p_world - u_camera_pos);
    return max(max(d.x, d.y), d.z);
}

float density_world(vec3 p_world) {
    int level = choose_lod(p_world);
    float camera_dist = camera_space_linf(p_world);
    float d = density_world_level(p_world, level);

    int max_level = max(u_clipmap_levels - 1, 0);
    if (level < max_level) {
        float half_extent = 0.5 * (u_bounds_max[level].x - u_bounds_min[level].x);
        float blend_width = max((u_bounds_max[level].x - u_bounds_min[level].x) /
            float(max(u_grid_resolution, 1)) * 3.0, 0.001);
        float dist_to_boundary = abs(camera_dist - half_extent);
        if (dist_to_boundary < blend_width) {
            float t = smoothstep(half_extent - blend_width, half_extent + blend_width, camera_dist);
            float coarse = density_world_level(p_world, level + 1);
            d = mix(d, coarse, t);
        }
    }

    if (level > 0) {
        float half_extent_finer = 0.5 * (u_bounds_max[level - 1].x - u_bounds_min[level - 1].x);
        float blend_width = max((u_bounds_max[level - 1].x - u_bounds_min[level - 1].x) /
            float(max(u_grid_resolution, 1)) * 3.0, 0.001);
        float dist_to_boundary = abs(camera_dist - half_extent_finer);
        if (dist_to_boundary < blend_width) {
            float t = smoothstep(half_extent_finer - blend_width, half_extent_finer + blend_width, camera_dist);
            float finer = density_world_level(p_world, level - 1);
            d = mix(finer, d, t);
        }
    }

    return d;
}

vec3 estimate_normal(vec3 p_world, vec3 step) {
    float dx = density_world(p_world + vec3(step.x, 0.0, 0.0)) - density_world(p_world - vec3(step.x, 0.0, 0.0));
    float dy = density_world(p_world + vec3(0.0, step.y, 0.0)) - density_world(p_world - vec3(0.0, step.y, 0.0));
    float dz = density_world(p_world + vec3(0.0, 0.0, step.z)) - density_world(p_world - vec3(0.0, 0.0, step.z));
    vec3 g = vec3(dx, dy, dz);
    float len2 = dot(g, g);
    return len2 > 1e-20 ? normalize(g) : vec3(0.0, 1.0, 0.0);
}

vec3 lod_cell_step(int level) {
    level = clamp(level, 0, max(u_clipmap_levels - 1, 0));
    vec3 step = (u_bounds_max[level] - u_bounds_min[level]) / float(max(u_grid_resolution, 1));
    return max(step, vec3(1e-5));
}

vec3 refine_hit_gradient(vec3 p_world) {
    vec3 p = p_world;
    for (int i = 0; i < 3; ++i) {
        int level = choose_lod(p);
        vec3 step = lod_cell_step(level) * 0.5;
        float f = density_world(p) - u_iso_level;
        if (abs(f) < 1e-5) {
            break;
        }

        vec3 n = estimate_normal(p, step);
        float max_correction = max(max(step.x, step.y), step.z) * 1.5;
        float correction = clamp(f, -max_correction, max_correction);
        p -= n * correction;
    }
    return p;
}

vec3 lod_debug_color(int level) {
    if (level == 0) return vec3(0.95, 0.25, 0.2);
    if (level == 1) return vec3(0.95, 0.75, 0.2);
    if (level == 2) return vec3(0.2, 0.85, 0.35);
    return vec3(0.2, 0.55, 0.95);
}

float refine_root(vec3 ro, vec3 rd, float ta, float tb, float da, float db) {
    float a = ta;
    float b = tb;
    float fa = da;
    float fb = db;

    for (int i = 0; i < 10; ++i) {
        float denom = (fb - fa);
        float t_secant = abs(denom) > 1e-8 ? (a * fb - b * fa) / denom : 0.5 * (a + b);
        t_secant = clamp(t_secant, min(a, b), max(a, b));

        float fm = density_world(ro + rd * t_secant) - u_iso_level;
        if (abs(fm) < 1e-5 || abs(b - a) < 1e-5) {
            return t_secant;
        }

        if ((fa <= 0.0 && fm <= 0.0) || (fa >= 0.0 && fm >= 0.0)) {
            a = t_secant;
            fa = fm;
        } else {
            b = t_secant;
            fb = fm;
        }
    }

    return 0.5 * (a + b);
}

void main() {
    if (u_has_scene == 0) {
        o_color = vec4(0.0);
        return;
    }

    vec2 ndc = vec2(v_uv.x * 2.0 - 1.0, v_uv.y * 2.0 - 1.0);
    vec4 near_clip = vec4(ndc, -1.0, 1.0);
    vec4 far_clip = vec4(ndc, 1.0, 1.0);
    vec4 near_world_h = u_inv_view_proj * near_clip;
    vec4 far_world_h = u_inv_view_proj * far_clip;
    vec3 near_world = near_world_h.xyz / max(near_world_h.w, 1e-8);
    vec3 far_world = far_world_h.xyz / max(far_world_h.w, 1e-8);

    vec3 ro = u_camera_pos;
    vec3 rd = normalize(far_world - ro);

    int max_level = max(u_clipmap_levels - 1, 0);
    vec2 t_hit = ray_box_intersection(ro, rd, u_bounds_min[max_level], u_bounds_max[max_level]);
    if (t_hit.y < 0.0 || t_hit.x > t_hit.y) {
        discard;
    }

    float t = max(t_hit.x, 0.0);
    float t_end = t_hit.y;
    vec3 cell_step = (u_bounds_max[0] - u_bounds_min[0]) / float(max(u_grid_resolution, 1));

    float min_step = max(min(min(cell_step.x, cell_step.y), cell_step.z) * 0.04, 0.00025);
    float max_step = max(min(min(cell_step.x, cell_step.y), cell_step.z) * 3.0, 0.002);

    float prev_t = t;
    float prev_d = density_world(ro + rd * t) - u_iso_level;
    bool hit = false;
    vec3 hit_pos = ro + rd * t;

    const int k_max_steps = 160;
    for (int i = 0; i < k_max_steps && t < t_end; ++i) {
        float step_len = clamp(abs(prev_d) * 0.9, min_step, max_step);
        t += step_len;
        vec3 p = ro + rd * t;
        float d = density_world(p) - u_iso_level;
        if ((prev_d <= 0.0 && d >= 0.0) || (prev_d >= 0.0 && d <= 0.0)) {
            t = refine_root(ro, rd, prev_t, t, prev_d, d);
            hit_pos = ro + rd * t;
            hit = true;
            break;
        }
        prev_t = t;
        prev_d = d;
    }

    if (!hit) {
        discard;
    }

    int hit_lod = choose_lod(hit_pos);
    if (u_debug_surface_mode != 0 || u_debug_lod != 0) {
        vec3 c = vec3(0.0);
        int mode = u_debug_surface_mode;
        if (u_debug_lod != 0) {
            mode = 1;
        }

        if (mode == 1) {
            c = lod_debug_color(hit_lod);
        } else if (mode == 2) {
            float grid_res = float(u_grid_resolution);
            vec3 grid_pos = ((hit_pos - u_bounds_min[hit_lod]) /
                (u_bounds_max[hit_lod] - u_bounds_min[hit_lod])) * grid_res;
            c = clamp(grid_pos / max(grid_res, 1.0), vec3(0.0), vec3(1.0));
        } else if (mode == 3) {
            float residual = abs(density_world(hit_pos) - u_iso_level);
            vec3 step = lod_cell_step(hit_lod);
            float scale = max(max(step.x, step.y), step.z);
            float v = clamp(residual / max(scale, 1e-5), 0.0, 1.0);
            c = mix(vec3(0.1, 0.8, 0.2), vec3(1.0, 0.15, 0.1), v);
        } else if (mode == 4) {
            vec3 dbg_n = estimate_normal(hit_pos, lod_cell_step(hit_lod) * 0.5);
            c = dbg_n * 0.5 + 0.5;
        }

        o_color = vec4(c, 1.0);
        vec4 clip = u_view_proj * vec4(hit_pos, 1.0);
        float ndc_depth = clip.z / max(clip.w, 1e-8);
        gl_FragDepth = ndc_depth * 0.5 + 0.5;
        return;
    }

    hit_pos = refine_hit_gradient(hit_pos);
    int shade_lod = choose_lod(hit_pos);
    vec3 n = estimate_normal(hit_pos, lod_cell_step(shade_lod) * 0.5);
    vec3 l = normalize(vec3(0.5, 1.0, 0.35));
    vec3 v = normalize(u_camera_pos - hit_pos);
    vec3 h = normalize(l + v);
    float ndotl = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, h), 0.0), 32.0);
    vec3 base = vec3(0.78, 0.82, 0.88);
    vec3 color = base * (0.16 + ndotl * 0.84) + vec3(0.35) * spec;
    o_color = vec4(color, 1.0);

    vec4 clip = u_view_proj * vec4(hit_pos, 1.0);
    float ndc_depth = clip.z / max(clip.w, 1e-8);
    gl_FragDepth = ndc_depth * 0.5 + 0.5;
}
