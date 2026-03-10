#version 410 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

uniform mat4 u_view_proj;

out vec3 v_world_pos;
out vec3 v_world_normal;

void main() {
    v_world_pos = a_position;
    v_world_normal = normalize(a_normal);
    gl_Position = u_view_proj * vec4(v_world_pos, 1.0);
}
