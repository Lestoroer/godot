// lestoroer#local-lights
#[compute]
#version 450

#VERSION_DEFINES

// Размытие отношения жёсткой тени по видимой поверхности. Радиус в метрах приходит из прохода отношения. Диск лежит в
// касательной плоскости и проецируется на экран, число выборок постоянно, поэтому цена не зависит от радиуса; поворот диска
// меняется по кадрам и пикселям, шум накапливает TAA. Выборки с другой плоскости или нормалью не считаются.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(r16f, set = 0, binding = 1) uniform restrict readonly image2D blur_radius;
// Корень из отношения: 10 бит хватает в тёмных градиентах, материал возводит прочитанное в квадрат.
layout(rgb10_a2, set = 0, binding = 2) uniform restrict writeonly image2D blurred_ratio;
layout(rgba16, set = 0, binding = 4) uniform restrict readonly image2D ratio_and_normal;
layout(set = 0, binding = 3, std140) uniform Params {
	mat4 inverse_projection;
	mat4 projection;
	mat4 view_to_world;
	vec4 cull_origin_and_step;
	ivec4 cull_dims;
	// y — номер кадра шума; w больше нуля — показать маску без размытия.
	vec4 blur;
	ivec4 view_size;
} params;

const int DISK_TAPS = 12;
const float GOLDEN_ANGLE = 2.39996323;
const float TAU = 6.28318531;
const float PLANE_STOP_METERS = 0.03;
const float NORMAL_STOP_COSINE = 0.85;

vec3 view_point_at(ivec2 pixel, ivec2 size) {
	float depth = texelFetch(depth_buffer, pixel, 0).r;
	vec4 view = params.inverse_projection * vec4((vec2(pixel) + 0.5) / vec2(size) * 2.0 - 1.0, depth, 1.0);
	return view.xyz / view.w;
}

vec2 screen_position_of(vec3 view_point, ivec2 size) {
	vec4 clip = params.projection * vec4(view_point, 1.0);
	return (clip.xy / clip.w * 0.5 + 0.5) * vec2(size);
}

vec3 normal_of(float packed_normal) {
	uint bits = uint(round(packed_normal * 65535.0));
	vec2 octahedron = (vec2(bits >> 8u, bits & 255u) - vec2(1.0)) / 254.0 * 2.0 - vec2(1.0);
	vec2 unfolded = vec2(octahedron.x + octahedron.y, octahedron.x - octahedron.y) * 0.5;
	return normalize(vec3(unfolded, 1.0 - abs(unfolded.x) - abs(unfolded.y)));
}

void main() {
	ivec2 size = params.view_size.xy;
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, size))) {
		return;
	}
	vec4 own = imageLoad(ratio_and_normal, pixel);
	// У пикселя нет света ламп: тёмный итог не добавит свет там, где материал считает иначе.
	if (own.a <= 0.0) {
		imageStore(blurred_ratio, pixel, vec4(0.0));
		return;
	}
	if (params.blur.w > 0.0) {
		imageStore(blurred_ratio, pixel, vec4(sqrt(own.rgb), 0.0));
		return;
	}
	vec3 point = view_point_at(pixel, size);
	vec3 normal = normal_of(own.a);
	vec3 helper = abs(normal.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(helper, normal));
	vec3 bitangent = cross(normal, tangent);
	float radius = imageLoad(blur_radius, pixel).r;
	vec2 center = vec2(pixel) + 0.5;
	vec2 screen_tangent = screen_position_of(point + tangent * radius, size) - center;
	vec2 screen_bitangent = screen_position_of(point + bitangent * radius, size) - center;
	vec2 shifted = vec2(pixel) + 5.588238 * params.blur.y;
	float rotation = TAU * fract(52.9829189 * fract(dot(shifted, vec2(0.06711056, 0.00583715))));
	vec3 sum = own.rgb;
	float count = 1.0;
	for (int tap = 0; tap < DISK_TAPS; tap++) {
		float reach = sqrt((float(tap) + 0.5) / float(DISK_TAPS));
		float angle = rotation + float(tap) * GOLDEN_ANGLE;
		vec2 offset = reach * vec2(cos(angle), sin(angle));
		ivec2 tap_pixel = ivec2(floor(center + screen_tangent * offset.x + screen_bitangent * offset.y));
		if (any(lessThan(tap_pixel, ivec2(0))) || any(greaterThanEqual(tap_pixel, size)) || tap_pixel == pixel) {
			continue;
		}
		vec4 tap_value = imageLoad(ratio_and_normal, tap_pixel);
		if (tap_value.a <= 0.0 || dot(normal_of(tap_value.a), normal) < NORMAL_STOP_COSINE) {
			continue;
		}
		if (abs(dot(view_point_at(tap_pixel, size) - point, normal)) > PLANE_STOP_METERS) {
			continue;
		}
		sum += tap_value.rgb;
		count += 1.0;
	}
	imageStore(blurred_ratio, pixel, vec4(sqrt(sum / count), 0.0));
}
