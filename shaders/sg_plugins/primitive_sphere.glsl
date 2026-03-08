float sg_plugin_primitive_sphere(vec3 p, vec4 params) {
    return length(p) - params.x;
}
