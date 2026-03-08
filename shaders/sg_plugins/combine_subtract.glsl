float sg_plugin_combine_subtract(float a, float b, vec4 params) {
    return max(a, -b);
}
