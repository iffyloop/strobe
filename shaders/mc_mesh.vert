#version 430 core

layout(std430, binding = 0) readonly buffer VertexBuffer {
    vec4 data[];
};

uniform mat4 u_view_proj;

out vec3 v_world_pos;
out vec3 v_world_normal;

void main() {
    int base = gl_VertexID * 2;
    vec4 pos = data[base + 0];
    vec4 normal = data[base + 1];
    v_world_pos = pos.xyz;
    v_world_normal = normalize(normal.xyz);
    gl_Position = u_view_proj * vec4(v_world_pos, 1.0);
}
