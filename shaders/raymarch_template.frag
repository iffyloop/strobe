#version 410 core

layout(location = 0) out vec4 o_color;

in vec2 v_tex_coord;

uniform mat4 u_inv_view_proj;
uniform vec3 u_camera_pos;

uniform int u_has_scene;
uniform int u_program_count;
uniform int u_max_stack_depth;
uniform int u_max_steps;
uniform float u_surface_epsilon;
uniform float u_max_trace_dist;

uniform isamplerBuffer u_program_tex;
uniform isamplerBuffer u_primitive_meta_tex;
uniform samplerBuffer u_primitive_params_tex;
uniform samplerBuffer u_primitive_scale_tex;
uniform isamplerBuffer u_primitive_effect_range_tex;
uniform isamplerBuffer u_effect_meta_tex;
uniform samplerBuffer u_effect_params_tex;
uniform samplerBuffer u_combine_params_tex;

uniform sampler2D u_texture0;

uniform int u_op_push_primitive;
uniform int u_op_combine;
uniform int u_combine_union;
uniform int u_combine_intersect;
uniform int u_combine_subtract;
uniform int u_primitive_cube;
uniform int u_primitive_sphere;

const int MAX_PROGRAM_STACK = 64;

/*__SG_PLUGIN_FUNCTIONS__*/

/*__SG_PLUGIN_PRIMITIVE_DISPATCH__*/

/*__SG_PLUGIN_COMBINE_DISPATCH__*/

/*__SG_PLUGIN_EFFECT_DISPATCH__*/

struct PrimitiveEval {
    float dist;
    vec3 p_local;
    vec4 params;
    int texture_id;
    int primitive_runtime_id;
};

struct SceneEval {
    float dist;
    PrimitiveEval hit;
};

PrimitiveEval eval_primitive(int primitive_index, vec3 p_world) {
    ivec4 primitive_meta = texelFetch(u_primitive_meta_tex, primitive_index);
    vec4 primitive_params = texelFetch(u_primitive_params_tex, primitive_index);
    float uniform_scale = max(texelFetch(u_primitive_scale_tex, primitive_index).x, 0.0001);

    vec3 p_local = p_world;
    ivec4 effect_range = texelFetch(u_primitive_effect_range_tex, primitive_index);
    for (int i = 0; i < effect_range.y; ++i) {
        int effect_index = effect_range.x + i;
        ivec4 effect_meta = texelFetch(u_effect_meta_tex, effect_index);
        vec4 effect_params = texelFetch(u_effect_params_tex, effect_index);
        p_local = sg_apply_effect_plugin(effect_meta.x, p_local, effect_params);
    }

    PrimitiveEval out_eval;
    out_eval.dist = sg_eval_primitive_plugin(primitive_meta.x, p_local, primitive_params) * uniform_scale;
    out_eval.p_local = p_local;
    out_eval.params = primitive_params;
    out_eval.texture_id = primitive_meta.y;
    out_eval.primitive_runtime_id = primitive_meta.x;
    return out_eval;
}

PrimitiveEval combine_eval(int combine_op, PrimitiveEval a, PrimitiveEval b, vec4 combine_params) {
    PrimitiveEval out_eval = a;
    out_eval.dist = sg_eval_combine_plugin(combine_op, a.dist, b.dist, combine_params);

    if (combine_op == u_combine_union) {
        PrimitiveEval selected = (b.dist < a.dist) ? b : a;
        selected.dist = out_eval.dist;
        return selected;
    }
    if (combine_op == u_combine_intersect) {
        PrimitiveEval selected = (b.dist > a.dist) ? b : a;
        selected.dist = out_eval.dist;
        return selected;
    }
    if (combine_op == u_combine_subtract) {
        float a_term = a.dist;
        float b_term = -b.dist;
        PrimitiveEval selected = (b_term > a_term) ? b : a;
        selected.dist = out_eval.dist;
        return selected;
    }

    return out_eval;
}

SceneEval eval_scene(vec3 p_world) {
    PrimitiveEval stack[MAX_PROGRAM_STACK];
    int sp = 0;

    for (int i = 0; i < u_program_count; ++i) {
        ivec4 inst = texelFetch(u_program_tex, i);

        if (inst.x == u_op_push_primitive) {
            if (sp >= MAX_PROGRAM_STACK) {
                SceneEval fail;
                fail.dist = 1e6;
                return fail;
            }
            stack[sp++] = eval_primitive(inst.y, p_world);
            continue;
        }

        if (inst.x != u_op_combine || sp < 2) {
            SceneEval fail;
            fail.dist = 1e6;
            return fail;
        }

        PrimitiveEval b = stack[--sp];
        PrimitiveEval a = stack[--sp];
        vec4 combine_params = texelFetch(u_combine_params_tex, inst.z);
        stack[sp++] = combine_eval(inst.y, a, b, combine_params);
    }

    SceneEval out_eval;
    if (sp == 0) {
        out_eval.dist = 1e6;
        return out_eval;
    }
    out_eval.hit = stack[sp - 1];
    out_eval.dist = out_eval.hit.dist;
    return out_eval;
}

float eval_scene_dist(vec3 p_world) {
    return eval_scene(p_world).dist;
}

bool raymarch(vec3 ro, vec3 rd, float hit_epsilon, out SceneEval out_hit, out vec3 out_hit_pos) {
    float t = 0.0;
    float prev_t = 0.0;
    float prev_d = 1e20;
    int max_steps = min(u_max_steps, 256);
    for (int i = 0; i < max_steps; ++i) {
        vec3 p = ro + rd * t;
        SceneEval scene_sample = eval_scene(p);
        float d = scene_sample.dist;
        if (d < hit_epsilon) {
            float t_hit = t;
            if (i > 0) {
                float denom = d - prev_d;
                if (abs(denom) > 1e-8) {
                    float t_lin = t - d * (t - prev_t) / denom;
                    t_hit = t_lin;
                }
            }
            out_hit_pos = ro + rd * t_hit;
            out_hit = eval_scene(out_hit_pos);
            return true;
        }
        if (t > u_max_trace_dist) {
            break;
        }
        prev_t = t;
        prev_d = d;
        t += d;
    }
    return false;
}

vec3 estimate_normal(vec3 p) {
    vec2 e = vec2(1.0, -1.0) * 0.5773;
    return normalize(
        e.xyy * eval_scene_dist(p + e.xyy * u_surface_epsilon) +
        e.yyx * eval_scene_dist(p + e.yyx * u_surface_epsilon) +
        e.yxy * eval_scene_dist(p + e.yxy * u_surface_epsilon) +
        e.xxx * eval_scene_dist(p + e.xxx * u_surface_epsilon));
}

vec2 cube_uv(vec3 p_local, vec3 half_extents) {
    vec3 pn = p_local / max(half_extents, vec3(0.0001));
    vec3 an = abs(pn);
    if (an.x > an.y && an.x > an.z) {
        if (pn.x > 0.0) {
            return vec2((-p_local.z / half_extents.z) * 0.5 + 0.5, (p_local.y / half_extents.y) * 0.5 + 0.5);
        }
        return vec2((p_local.z / half_extents.z) * 0.5 + 0.5, (p_local.y / half_extents.y) * 0.5 + 0.5);
    }
    if (an.y > an.z) {
        if (pn.y > 0.0) {
            return vec2((p_local.x / half_extents.x) * 0.5 + 0.5, (-p_local.z / half_extents.z) * 0.5 + 0.5);
        }
        return vec2((p_local.x / half_extents.x) * 0.5 + 0.5, (p_local.z / half_extents.z) * 0.5 + 0.5);
    }
    if (pn.z > 0.0) {
        return vec2((p_local.x / half_extents.x) * 0.5 + 0.5, (p_local.y / half_extents.y) * 0.5 + 0.5);
    }
    return vec2((-p_local.x / half_extents.x) * 0.5 + 0.5, (p_local.y / half_extents.y) * 0.5 + 0.5);
}

vec2 sphere_uv(vec3 p_local, float radius) {
    vec3 unit = normalize(p_local / max(radius, 0.0001));
    float u = atan(unit.z, unit.x) / (2.0 * 3.14159265) + 0.5;
    float v = asin(clamp(unit.y, -1.0, 1.0)) / 3.14159265 + 0.5;
    return vec2(u, v);
}

vec3 sample_primitive_albedo(SceneEval hit) {
    vec2 uv = vec2(0.0);
    if (hit.hit.primitive_runtime_id == u_primitive_cube) {
        vec3 half_extents = max(hit.hit.params.xyz * 0.5, vec3(0.0001));
        uv = cube_uv(hit.hit.p_local, half_extents);
    } else if (hit.hit.primitive_runtime_id == u_primitive_sphere) {
        uv = sphere_uv(hit.hit.p_local, hit.hit.params.x);
    } else {
        uv = fract(hit.hit.p_local.xy * 0.25 + vec2(0.5));
    }

    int texture_id = max(hit.hit.texture_id, 0);
    if (texture_id == 0) {
        return texture(u_texture0, uv).rgb;
    }
    return texture(u_texture0, uv).rgb;
}

void main(void) {
    if (u_has_scene == 0 || u_program_count == 0 || u_max_stack_depth > MAX_PROGRAM_STACK) {
        o_color = vec4(0.03, 0.04, 0.05, 1.0);
        return;
    }

    vec2 uv = v_tex_coord;
    vec2 ndc = uv * 2.0 - 1.0;

    vec4 world_near = u_inv_view_proj * vec4(ndc, -1.0, 1.0);
    vec4 world_far = u_inv_view_proj * vec4(ndc, 1.0, 1.0);
    world_near /= world_near.w;
    world_far /= world_far.w;

    vec3 ro = u_camera_pos;
    vec3 rd = normalize(world_far.xyz - world_near.xyz);

    SceneEval hit;
    vec3 hit_pos = vec3(0.0);
    if (!raymarch(ro, rd, u_surface_epsilon, hit, hit_pos)) {
        vec3 bg = mix(vec3(0.05, 0.06, 0.08), vec3(0.01, 0.015, 0.02), uv.y);
        o_color = vec4(bg, 1.0);
        return;
    }

    vec3 normal = estimate_normal(hit_pos);
    vec3 albedo = sample_primitive_albedo(hit);
    vec3 light_dir = normalize(vec3(0.4, 0.8, 0.5));
    float ndotl = max(dot(normal, light_dir), 0.0);
    vec3 color = albedo * (0.2 + 0.8 * ndotl);
    o_color = vec4(color, 1.0);
}
