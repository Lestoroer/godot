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
// Error-free transforms keep edge-only contacts out of the capture. The filter
// handles ordinary cells cheaply; ambiguous signs use a short exact expansion.
void surface_cache_two_diff(float a, float b, out float x, out float y) {
    precise float difference = a - b;
    precise float b_virtual = a - difference;
    precise float a_virtual = difference + b_virtual;
    precise float b_roundoff = b_virtual - b;
    precise float a_roundoff = a - a_virtual;
    precise float error = a_roundoff + b_roundoff;
    x = difference;
    y = error;
}

void surface_cache_two_sum(float a, float b, out float x, out float y) {
    precise float sum = a + b;
    precise float b_virtual = sum - a;
    precise float a_virtual = sum - b_virtual;
    precise float b_roundoff = b - b_virtual;
    precise float a_roundoff = a - a_virtual;
    precise float error = a_roundoff + b_roundoff;
    x = sum;
    y = error;
}

void surface_cache_grow_expansion(inout float expansion[16], inout int count, float value) {
    float grown[16];
    int grown_count = 0;
    precise float accumulator = value;
    for (int i = 0; i < count; i++) {
        precise float sum, error;
        surface_cache_two_sum(accumulator, expansion[i], sum, error);
        if (error != 0.0) grown[grown_count++] = error;
        accumulator = sum;
    }
    if (accumulator != 0.0 || grown_count == 0) grown[grown_count++] = accumulator;
    for (int i = 0; i < 16; i++) expansion[i] = i < grown_count ? grown[i] : 0.0;
    count = grown_count;
}

void surface_cache_add_product(inout float expansion[16], inout int count, float a, float b, float sign_value) {
    precise float product = a * b;
    precise float error = fma(a, b, -product);
    surface_cache_grow_expansion(expansion, count, sign_value * error);
    surface_cache_grow_expansion(expansion, count, sign_value * product);
}

int surface_cache_cross_differences_sign(float dx_hi, float dx_lo, float dy_hi, float dy_lo,
        float px_hi, float px_lo, float py_hi, float py_lo) {
    precise float left = dx_hi * py_hi;
    precise float right = dy_hi * px_hi;
    precise float determinant = left - right;
    // Include coordinate-difference tails in the filter bound. Returning an
    // uncertain sign is safe; it only costs the exact fallback below.
    precise float tails = abs(dx_lo * py_hi) + abs(dx_hi * py_lo) + abs(dx_lo * py_lo)
            + abs(dy_lo * px_hi) + abs(dy_hi * px_lo) + abs(dy_lo * px_lo);
    precise float error_bound = 5.0e-7 * (abs(left) + abs(right) + tails) + tails;
    if (abs(determinant) > error_bound) return determinant > 0.0 ? 1 : -1;

    float expansion[16];
    for (int i = 0; i < 16; i++) expansion[i] = 0.0;
    int count = 0;
    surface_cache_add_product(expansion, count, dx_hi, py_hi, 1.0);
    surface_cache_add_product(expansion, count, dx_hi, py_lo, 1.0);
    surface_cache_add_product(expansion, count, dx_lo, py_hi, 1.0);
    surface_cache_add_product(expansion, count, dx_lo, py_lo, 1.0);
    surface_cache_add_product(expansion, count, dy_hi, px_hi, -1.0);
    surface_cache_add_product(expansion, count, dy_hi, px_lo, -1.0);
    surface_cache_add_product(expansion, count, dy_lo, px_hi, -1.0);
    surface_cache_add_product(expansion, count, dy_lo, px_lo, -1.0);
    for (int i = count - 1; i >= 0; i--) {
        if (expansion[i] != 0.0) return expansion[i] > 0.0 ? 1 : -1;
    }
    return 0;
}

int surface_cache_orient_exact_sign(vec2 a, vec2 b, vec2 c) {
    float dx_hi, dx_lo, dy_hi, dy_lo, px_hi, px_lo, py_hi, py_lo;
    surface_cache_two_diff(b.x, a.x, dx_hi, dx_lo);
    surface_cache_two_diff(b.y, a.y, dy_hi, dy_lo);
    surface_cache_two_diff(c.x, a.x, px_hi, px_lo);
    surface_cache_two_diff(c.y, a.y, py_hi, py_lo);
    return surface_cache_cross_differences_sign(dx_hi, dx_lo, dy_hi, dy_lo, px_hi, px_lo, py_hi, py_lo);
}

int surface_cache_orient_difference_sign(vec2 edge_a, vec2 edge_b, vec2 point, vec2 origin) {
    float dx_hi, dx_lo, dy_hi, dy_lo, px_hi, px_lo, py_hi, py_lo;
    surface_cache_two_diff(edge_b.x, edge_a.x, dx_hi, dx_lo);
    surface_cache_two_diff(edge_b.y, edge_a.y, dy_hi, dy_lo);
    surface_cache_two_diff(point.x, origin.x, px_hi, px_lo);
    surface_cache_two_diff(point.y, origin.y, py_hi, py_lo);
    return surface_cache_cross_differences_sign(dx_hi, dx_lo, dy_hi, dy_lo, px_hi, px_lo, py_hi, py_lo);
}

// Strict SAT in original atlas coordinates. Integer cell corners are exact;
// requiring strict overlap on every polygon axis excludes point/edge contacts.
bool surface_cache_triangle_cell_covered(vec2 a, vec2 b, vec2 c, vec2 cell_min) {
    vec2 cell_max = cell_min + vec2(1.0);
    if (max(a.x, max(b.x, c.x)) <= cell_min.x || min(a.x, min(b.x, c.x)) >= cell_max.x
            || max(a.y, max(b.y, c.y)) <= cell_min.y || min(a.y, min(b.y, c.y)) >= cell_max.y) return false;

    int winding = surface_cache_orient_exact_sign(a, b, c);
    if (winding == 0) return false;
    for (int edge_index = 0; edge_index < 3; edge_index++) {
        vec2 edge_a = edge_index == 0 ? a : (edge_index == 1 ? b : c);
        vec2 edge_b = edge_index == 0 ? b : (edge_index == 1 ? c : a);
        vec2 opposite = edge_index == 0 ? c : (edge_index == 1 ? a : b);
        vec2 edge = edge_b - edge_a;
        vec2 support_max = cell_min, support_min = cell_min;
        if (-edge.y * float(winding) > 0.0) support_max.x = cell_max.x;
        else support_min.x = cell_max.x;
        if (edge.x * float(winding) > 0.0) support_max.y = cell_max.y;
        else support_min.y = cell_max.y;
        if (surface_cache_orient_exact_sign(edge_a, edge_b, support_max) * winding <= 0) return false;
        if (surface_cache_orient_difference_sign(edge_a, edge_b, support_min, opposite) * winding >= 0) return false;
    }
    return true;
}

// Clipping coordinates are relative to the selected cell center to avoid
// cancellation in large atlases.
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
    if (area == 0.0) { point = vec2(0); return false; }
    point = anchor + center / (3.0 * area);
    // A positive but unrepresentable cell has no physical row. Sampling uses
    // a neighbouring valid cell of this same primitive, never its remote centroid.
    if (any(lessThanEqual(point, vec2(-0.5))) || any(greaterThanEqual(point, vec2(0.5)))) return false;
    return true;
}
#endif
