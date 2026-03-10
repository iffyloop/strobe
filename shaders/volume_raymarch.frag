#version 430 core

layout(location = 0) out vec4 o_color;

in vec2 v_uv;

uniform int u_has_scene;
uniform int u_grid_resolution;
uniform float u_iso_level;
uniform int u_clipmap_levels;
uniform int u_debug_lod;
uniform vec3 u_bounds_min[4];
uniform vec3 u_bounds_max[4];
uniform vec3 u_camera_pos;
uniform mat4 u_view_proj;
uniform mat4 u_inv_view_proj;
uniform sampler3D u_density_tex[4];

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
    vec3 grid_pos = ((p_world - u_bounds_min[level]) / (u_bounds_max[level] - u_bounds_min[level])) * grid_res;
    grid_pos = clamp(grid_pos, vec3(0.0), vec3(grid_res));
    float tex_dim = grid_res + 1.0;
    vec3 uvw = (grid_pos + vec3(0.5)) / tex_dim;
    return texture(u_density_tex[level], uvw).r;
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

float distance_to_bounds_edge(vec3 p_world, int level) {
    vec3 d0 = p_world - u_bounds_min[level];
    vec3 d1 = u_bounds_max[level] - p_world;
    vec3 d = min(d0, d1);
    return min(min(d.x, d.y), d.z);
}

float density_world(vec3 p_world) {
    int level = choose_lod(p_world);
    float d = density_world_level(p_world, level);

    int max_level = max(u_clipmap_levels - 1, 0);
    if (level < max_level) {
        float blend_width = (u_bounds_max[level].x - u_bounds_min[level].x) / float(max(u_grid_resolution, 1));
        blend_width = max(blend_width * 3.0, 0.001);
        float edge_dist = distance_to_bounds_edge(p_world, level);
        if (edge_dist < blend_width) {
            float t = smoothstep(0.0, blend_width, edge_dist);
            float coarse = density_world_level(p_world, level + 1);
            d = mix(coarse, d, t);
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

vec3 lod_debug_color(int level) {
    if (level == 0) return vec3(0.95, 0.25, 0.2);
    if (level == 1) return vec3(0.95, 0.75, 0.2);
    if (level == 2) return vec3(0.2, 0.85, 0.35);
    return vec3(0.2, 0.55, 0.95);
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

    float min_step = max(min(min(cell_step.x, cell_step.y), cell_step.z) * 0.15, 0.0005);
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
            float a = prev_t;
            float b = t;
            float da = prev_d;
            for (int j = 0; j < 5; ++j) {
                float m = 0.5 * (a + b);
                float dm = density_world(ro + rd * m) - u_iso_level;
                if ((da <= 0.0 && dm <= 0.0) || (da >= 0.0 && dm >= 0.0)) {
                    a = m;
                    da = dm;
                } else {
                    b = m;
                }
            }
            t = 0.5 * (a + b);
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
    if (u_debug_lod != 0) {
        vec3 c = lod_debug_color(hit_lod);
        o_color = vec4(c, 1.0);
        vec4 clip = u_view_proj * vec4(hit_pos, 1.0);
        float ndc_depth = clip.z / max(clip.w, 1e-8);
        gl_FragDepth = ndc_depth * 0.5 + 0.5;
        return;
    }

    vec3 n = estimate_normal(hit_pos, cell_step);
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
