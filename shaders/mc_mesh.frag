#version 430 core

layout(location = 0) out vec4 o_color;

in vec3 v_world_pos;
in vec3 v_world_normal;

uniform vec3 u_camera_pos;

void main() {
    vec3 n = normalize(v_world_normal);
    vec3 l = normalize(vec3(0.5, 1.0, 0.35));
    vec3 v = normalize(u_camera_pos - v_world_pos);
    vec3 h = normalize(l + v);
    float ndotl = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, h), 0.0), 32.0);
    vec3 base = vec3(0.78, 0.82, 0.88);
    vec3 color = base * (0.16 + ndotl * 0.84) + vec3(0.35) * spec;
    o_color = vec4(color, 1.0);
}
