vec3 sg_plugin_effect_scale(vec3 p, vec4 params) {
    float s = max(abs(params.x), 0.0001);
    return p / s;
}
