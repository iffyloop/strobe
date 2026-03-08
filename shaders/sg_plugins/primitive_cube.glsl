float sg_plugin_primitive_cube(vec3 p, vec4 params) {
    vec3 half_extents = max(params.xyz * 0.5, vec3(0.0001));
    vec3 q = abs(p) - half_extents;
    return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}
