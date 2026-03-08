vec3 sg_plugin_effect_rotate(vec3 p, vec4 params) {
    float cx = cos(-params.x);
    float sx = sin(-params.x);
    float cy = cos(-params.y);
    float sy = sin(-params.y);
    float cz = cos(-params.z);
    float sz = sin(-params.z);

    vec3 v = p;

    v = vec3(
        cz * v.x - sz * v.y,
        sz * v.x + cz * v.y,
        v.z
    );

    v = vec3(
        cy * v.x + sy * v.z,
        v.y,
        -sy * v.x + cy * v.z
    );

    v = vec3(
        v.x,
        cx * v.y - sx * v.z,
        sx * v.y + cx * v.z
    );

    return v;
}
