#version 410 core

layout(location = 0) out vec4 o_color;

in vec2 v_tex_coord;

uniform sampler2D u_raw_depth_tex;
uniform sampler2D u_reduce_tex;
uniform int u_invert;
uniform float u_depth_span_epsilon;

void main() {
        vec4 raw = texture(u_raw_depth_tex, v_tex_coord);
        bool is_hit = raw.a > 0.5;
        if (!is_hit) {
                float bg = (u_invert == 1) ? 0.0 : 1.0;
                o_color = vec4(bg, bg, bg, 1.0);
                return;
        }

        vec4 reduced = texelFetch(u_reduce_tex, ivec2(0, 0), 0);
        vec2 minmax_depth = reduced.rg;
        bool has_valid = reduced.a > 0.5;
        float depth_norm = 0.0;
        if (has_valid && minmax_depth.r < minmax_depth.g && abs(minmax_depth.r) < 1e19 && abs(minmax_depth.g) < 1e19) {
                float depth_span = minmax_depth.g - minmax_depth.r;
                if (depth_span > max(u_depth_span_epsilon, 0.0)) {
                        depth_norm = clamp((raw.r - minmax_depth.r) / depth_span, 0.0, 1.0);
                } else {
                        depth_norm = 0.0;
                }
        } else {
                depth_norm = 0.0;
        }
        if (u_invert == 1) {
                depth_norm = 1.0 - depth_norm;
        }
        o_color = vec4(vec3(depth_norm), 1.0);
}
