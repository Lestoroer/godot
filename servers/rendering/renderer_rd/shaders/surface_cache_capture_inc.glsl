// Fork(Lestoroer): physical atlas samples. Padding never becomes a surface.
vec2 surface_cache_offset = vec2(0.0);
bool surface_cache_covered = true;
vec3 surface_cache_bary = vec3(0.0);
float surface_cache_sample(float v) { return v + dFdx(v) * surface_cache_offset.x + dFdy(v) * surface_cache_offset.y; }
vec2 surface_cache_sample(vec2 v) { return v + dFdx(v) * surface_cache_offset.x + dFdy(v) * surface_cache_offset.y; }
vec3 surface_cache_sample(vec3 v) { return v + dFdx(v) * surface_cache_offset.x + dFdy(v) * surface_cache_offset.y; }
vec4 surface_cache_sample(vec4 v) { return v + dFdx(v) * surface_cache_offset.x + dFdy(v) * surface_cache_offset.y; }
mat2 surface_cache_sample(mat2 v) { return mat2(surface_cache_sample(v[0]), surface_cache_sample(v[1])); }
mat3 surface_cache_sample(mat3 v) { return mat3(surface_cache_sample(v[0]), surface_cache_sample(v[1]), surface_cache_sample(v[2])); }
mat4 surface_cache_sample(mat4 v) { return mat4(surface_cache_sample(v[0]), surface_cache_sample(v[1]), surface_cache_sample(v[2]), surface_cache_sample(v[3])); }

#ifdef MODE_RENDER_MATERIAL
// All calculations relative to the texel center avoid cancellation in large atlases.
bool surface_cache_sample_point(vec2 a, vec2 b, vec2 c, out vec2 point) {
    vec2 polygon[8];
    polygon[0] = a; polygon[1] = b; polygon[2] = c;
    int count = 3;
    for (int edge = 0; edge < 4; edge++) {
        vec2 output_points[8];
        int output_count = 0;
        int axis = edge / 2;
        float sign_axis = (edge & 1) == 0 ? 1.0 : -1.0;
        for (int i = 0; i < count; i++) {
            vec2 current = polygon[i], previous = polygon[(i + count - 1) % count];
            float dc = 0.5 - current[axis] * sign_axis;
            float dp = 0.5 - previous[axis] * sign_axis;
            if ((dc >= 0.0) != (dp >= 0.0)) {
                vec2 intersection = previous + (current - previous) * (dp / (dp - dc));
                intersection[axis] = 0.5 * sign_axis;
                output_points[output_count++] = intersection;
            }
            if (dc >= 0.0) output_points[output_count++] = current;
        }
        count = output_count;
        for (int i = 0; i < count; i++) polygon[i] = output_points[i];
        if (count < 3) { point = vec2(0); return false; }
    }
    float area = 0.0;
    vec2 center = vec2(0.0), anchor = polygon[0];
    // Triangle fan avoids subtracting nearly equal shoelace terms for a
    // tiny intersection at a texel corner. All fan areas share the same sign.
    for (int i = 1; i + 1 < count; i++) {
        vec2 p = polygon[i] - anchor, q = polygon[i + 1] - anchor;
        float weight = p.x * q.y - p.y * q.x;
        area += weight;
        center += (p + q) * weight;
    }
    if (abs(area) <= 1e-20) { point = vec2(0); return false; }
    point = anchor + center / (3.0 * area);
    return true;
}
#endif
