#version 410 core

layout(location = 0) out vec4 o_color;

in vec2 v_tex_coord;

uniform mat4 u_inv_view_proj;
uniform mat4 u_view_proj;
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

uniform int u_op_push_primitive;
uniform int u_op_combine;

const int MAX_PROGRAM_STACK = 64;

/*__SG_PLUGIN_FUNCTIONS__*/

/*__SG_PLUGIN_PRIMITIVE_DISPATCH__*/

/*__SG_PLUGIN_COMBINE_DISPATCH__*/

/*__SG_PLUGIN_EFFECT_DISPATCH__*/

float eval_primitive(int primitive_index, vec3 p_world) {
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

    return sg_eval_primitive_plugin(primitive_meta.x, p_local, primitive_params) * uniform_scale;
}

float eval_scene(vec3 p_world) {
    float stack[MAX_PROGRAM_STACK];
    int sp = 0;

    for (int i = 0; i < u_program_count; ++i) {
        ivec4 inst = texelFetch(u_program_tex, i);

        if (inst.x == u_op_push_primitive) {
            if (sp >= MAX_PROGRAM_STACK) {
                return 1e6;
            }
            stack[sp++] = eval_primitive(inst.y, p_world);
            continue;
        }

        if (inst.x != u_op_combine) {
            return 1e6;
        }

        if (sp < 2) {
            return 1e6;
        }

        float b = stack[--sp];
        float a = stack[--sp];
        vec4 combine_params = texelFetch(u_combine_params_tex, inst.z);
        float out_d = sg_eval_combine_plugin(inst.y, a, b, combine_params);
        stack[sp++] = out_d;
    }

    if (sp == 0) {
        return 1e6;
    }
    return stack[sp - 1];
}

bool raymarch(vec3 ro, vec3 rd, float hit_epsilon, out vec3 hit_pos) {
    float t = 0.0;
    float prev_t = 0.0;
    float prev_d = 1e20;
    int max_steps = min(u_max_steps, 256);
    for (int i = 0; i < max_steps; ++i) {
        vec3 p = ro + rd * t;
        float d = eval_scene(p);
        if (d < hit_epsilon) {
            float t_hit = t;
            if (i > 0) {
                float denom = d - prev_d;
                if (abs(denom) > 1e-8) {
                    float t_lin = t - d * (t - prev_t) / denom;
                    t_hit = t_lin;
                }
            }
            hit_pos = ro + rd * t_hit;
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

void main(void) {
    if (u_has_scene == 0 || u_program_count == 0 || u_max_stack_depth > MAX_PROGRAM_STACK) {
        o_color = vec4(1.0, 1.0, 0.0, 0.0);
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

    float hit_epsilon = u_surface_epsilon;

    vec3 hit_pos = vec3(0.0);
    if (!raymarch(ro, rd, hit_epsilon, hit_pos)) {
        o_color = vec4(1.0, 1.0, 0.0, 0.0);
        return;
    }

    vec4 clip = u_view_proj * vec4(hit_pos, 1.0);
    if (abs(clip.w) < 1e-6) {
        o_color = vec4(1.0, 1.0, 0.0, 0.0);
        return;
    }
    float ndc_depth = clip.z / clip.w;
    float depth01 = ndc_depth * 0.5 + 0.5;
    o_color = vec4(depth01, depth01, 0.0, 1.0);
}
