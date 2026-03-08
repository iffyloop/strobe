#version 410 core

layout(location = 0) out vec4 o_minmax;

uniform sampler2D u_src_tex;
uniform ivec2 u_src_size;

void main() {
        ivec2 dst = ivec2(gl_FragCoord.xy);
        ivec2 src_base = dst * 2;
        ivec2 src_size = u_src_size;

        ivec2 p0 = clamp(src_base + ivec2(0, 0), ivec2(0), src_size - ivec2(1));
        ivec2 p1 = clamp(src_base + ivec2(1, 0), ivec2(0), src_size - ivec2(1));
        ivec2 p2 = clamp(src_base + ivec2(0, 1), ivec2(0), src_size - ivec2(1));
        ivec2 p3 = clamp(src_base + ivec2(1, 1), ivec2(0), src_size - ivec2(1));

        vec4 s0 = texelFetch(u_src_tex, p0, 0);
        vec4 s1 = texelFetch(u_src_tex, p1, 0);
        vec4 s2 = texelFetch(u_src_tex, p2, 0);
        vec4 s3 = texelFetch(u_src_tex, p3, 0);

        float v0 = step(0.5, s0.a);
        float v1 = step(0.5, s1.a);
        float v2 = step(0.5, s2.a);
        float v3 = step(0.5, s3.a);

        float mn = 1e20;
        float mx = -1e20;
        if (v0 > 0.5) { mn = min(mn, s0.r); mx = max(mx, s0.g); }
        if (v1 > 0.5) { mn = min(mn, s1.r); mx = max(mx, s1.g); }
        if (v2 > 0.5) { mn = min(mn, s2.r); mx = max(mx, s2.g); }
        if (v3 > 0.5) { mn = min(mn, s3.r); mx = max(mx, s3.g); }

        float valid = max(max(v0, v1), max(v2, v3));
        o_minmax = vec4(mn, mx, 0.0, valid);
}
