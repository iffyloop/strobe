#version 410 core

layout(location = 0) out vec4 o_minmax;

in vec2 v_tex_coord;

uniform sampler2D u_src_tex;

void main() {
        vec4 raw = texture(u_src_tex, v_tex_coord);
        if (raw.a < 0.5) {
                o_minmax = vec4(1e20, -1e20, 0.0, 0.0);
                return;
        }
        o_minmax = vec4(raw.r, raw.r, 0.0, 0.0);
}
