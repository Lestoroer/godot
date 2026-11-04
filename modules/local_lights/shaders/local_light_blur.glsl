// lestoroer#local-lights
#[compute]
#version 450

#VERSION_DEFINES

#include "local_light_ratio_surface_inc.glsl"

// Размытие отношения жёсткой тени по видимой поверхности. Радиус в метрах приходит из прохода отношения. Диск лежит в
// касательной плоскости и проецируется на экран, число выборок постоянно, поэтому цена не зависит от радиуса; поворот диска
// меняется по кадрам и пикселям, шум накапливает TAA. Выборки с другой плоскости или нормалью не считаются.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rg32ui, set = 0, binding = 1) uniform restrict readonly uimage2D ratio_surface;
// Корень из отношения: 10 бит хватает в тёмных градиентах, материал возводит прочитанное в квадрат.
layout(rgb10_a2, set = 0, binding = 2) uniform restrict writeonly image2D blurred_ratio;
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
// Единичный диск на золотой спирали, от края к центру.
const vec2 DISK_OFFSETS[DISK_TAPS] = vec2[](vec2(0.292982, 0.934074), vec2(0.396472, -0.847237), vec2(-0.822442, 0.339492), vec2(0.790557, 0.288710), vec2(-0.364379, -0.701590), vec2(-0.191064, 0.710747), vec2(0.571225, -0.363367), vec2(-0.603011, -0.106664), vec2(0.328595, 0.428593), vec2(0.039904, -0.454688), vec2(-0.260699, 0.238822), vec2(0.204124, 0.000000));
const float TAU = 6.28318531;
const float PLANE_STOP_METERS = 0.03;
const float NORMAL_STOP_COSINE = 0.85;

vec3 view_point_at(ivec2 pixel, ivec2 size, uint surface) {
	float view_z = -float(surface & 65535u) / 65535.0 * SURFACE_DEPTH_RANGE_METERS;
	vec2 ndc = (vec2(pixel) + 0.5) / vec2(size) * 2.0 - 1.0;
	float clip_w = params.projection[2][3] * view_z + params.projection[3][3];
	return vec3((ndc * clip_w - params.projection[2].xy * view_z - params.projection[3].xy) * vec2(params.inverse_projection[0][0], params.inverse_projection[1][1]), view_z);
}

vec2 screen_position_of(vec3 view_point, ivec2 size) {
	vec4 clip = params.projection * vec4(view_point, 1.0);
	return (clip.xy / clip.w * 0.5 + 0.5) * vec2(size);
}

vec3 normal_of(uint packed_normal) {
	vec2 octahedron = (vec2(packed_normal >> 8u, packed_normal & 255u) - vec2(1.0)) / 254.0 * 2.0 - vec2(1.0);
	vec2 unfolded = vec2(octahedron.x + octahedron.y, octahedron.x - octahedron.y) * 0.5;
	return normalize(vec3(unfolded, 1.0 - abs(unfolded.x) - abs(unfolded.y)));
}

void main() {
	ivec2 size = params.view_size.xy;
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, size))) {
		return;
	}
	uvec2 own_texel = imageLoad(ratio_surface, pixel).xy;
	// У пикселя нет света ламп: тёмный итог не добавит свет там, где материал считает иначе.
	if (own_texel.y == 0u) {
		imageStore(blurred_ratio, pixel, vec4(0.0));
		return;
	}
	vec4 own = unpackUnorm4x8(own_texel.x);
	if (params.blur.w > 0.0) {
		imageStore(blurred_ratio, pixel, vec4(own.rgb, 0.0));
		return;
	}
	vec3 own_ratio = own.rgb * own.rgb;
	uint own_normal = own_texel.y >> 16u;
	vec3 point = view_point_at(pixel, size, own_texel.y);
	vec3 normal = normal_of(own_normal);
	vec3 helper = abs(normal.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(helper, normal));
	vec3 bitangent = cross(normal, tangent);
	float radius = mix(MIN_RADIUS_METERS, MAX_RADIUS_METERS, own.a);
	vec2 center = vec2(pixel) + 0.5;
	vec2 screen_tangent = screen_position_of(point + tangent * radius, size) - center;
	vec2 screen_bitangent = screen_position_of(point + bitangent * radius, size) - center;
	vec2 shifted = vec2(pixel) + 5.588238 * params.blur.y;
	float rotation = TAU * fract(52.9829189 * fract(dot(shifted, vec2(0.06711056, 0.00583715))));
	float rotation_cosine = cos(rotation);
	float rotation_sine = sin(rotation);
	vec2 rotated_tangent = screen_tangent * rotation_cosine + screen_bitangent * rotation_sine;
	vec2 rotated_bitangent = screen_bitangent * rotation_cosine - screen_tangent * rotation_sine;
	vec3 sum = own_ratio;
	float count = 1.0;
	for (int tap = 0; tap < DISK_TAPS; tap++) {
		ivec2 tap_pixel = ivec2(floor(center + rotated_tangent * DISK_OFFSETS[tap].x + rotated_bitangent * DISK_OFFSETS[tap].y));
		if (any(lessThan(tap_pixel, ivec2(0))) || any(greaterThanEqual(tap_pixel, size)) || tap_pixel == pixel) {
			continue;
		}
		uvec2 tap_texel = imageLoad(ratio_surface, tap_pixel).xy;
		uint tap_normal = tap_texel.y >> 16u;
		// Та же упакованная нормаль почти на всех выборках: распаковка только при отличии.
		if (tap_normal == 0u || (tap_normal != own_normal && dot(normal_of(tap_normal), normal) < NORMAL_STOP_COSINE)) {
			continue;
		}
		if (abs(dot(view_point_at(tap_pixel, size, tap_texel.y) - point, normal)) > PLANE_STOP_METERS) {
			continue;
		}
		vec3 tap_ratio = unpackUnorm4x8(tap_texel.x).rgb;
		tap_ratio *= tap_ratio;
		sum += tap_ratio;
		count += 1.0;
	}
	imageStore(blurred_ratio, pixel, vec4(sqrt(sum / count), 0.0));
}
