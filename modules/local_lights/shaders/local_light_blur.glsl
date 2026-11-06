// lestoroer#local-lights
#[compute]
#version 450

#VERSION_DEFINES

#include "local_light_ratio_surface_inc.glsl"

// Размытие отношения жёсткой тени по видимой поверхности. Радиус в метрах приходит из прохода отношения. Диск лежит в
// касательной плоскости и проецируется на экран, число выборок постоянно, поэтому цена не зависит от радиуса; поворот диска
// меняется по кадрам и пикселям. Выборки с другой плоскости или нормалью не считаются.
// Шум выборок накапливается здесь же: новый кадр — среднее окна 3x3 той же поверхности, к нему подмешивается итог прошлого
// кадра в той же точке мира. TAA в движении почти не накапливает. Прошлый итог ограничивается разбросом среднего окна,
// поэтому отставание при движении ламп и предметов не выходит за шум этого среднего.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rg32ui, set = 0, binding = 1) uniform restrict readonly uimage2D ratio_surface;
// Корень из отношения: 10 бит хватает в тёмных градиентах, материал возводит прочитанное в квадрат. Альфа 1 — у пикселя есть свет ламп.
layout(rgb10_a2, set = 0, binding = 2) uniform restrict writeonly image2D blurred_ratio;
// Итог прошлого кадра в том же виде.
layout(set = 0, binding = 3) uniform sampler2D history_ratio;
const int DISK_TAPS = 16;
// Единичный диск на золотой спирали, от края к центру.
const vec2 DISK_OFFSETS[DISK_TAPS] = vec2[](vec2(-0.126487, -0.976090), vec2(-0.547507, 0.778772), vec2(0.897134, -0.197232), vec2(-0.764746, -0.443186), vec2(0.253730, 0.808932), vec2(0.343354, -0.733729), vec2(-0.712256, 0.294009), vec2(0.684642, 0.250030), vec2(-0.315561, -0.607594), vec2(-0.165466, 0.615525), vec2(0.494695, -0.314685), vec2(-0.522223, -0.092374), vec2(0.284571, 0.371173), vec2(0.034558, -0.393771), vec2(-0.225772, 0.206826), vec2(0.176777, 0.000000));
const float TAU = 6.28318531;
const float PLANE_STOP_METERS = 0.03;
const float NORMAL_STOP_COSINE = 0.85;
// Доля нового кадра в накоплении и ширина окна прошлого итога в разбросах среднего 3x3.
const float CURRENT_SHARE = 1.0 / 16.0;
const float HISTORY_SPREADS = 1.5;
// Нижняя граница шума среднего при сравнении с прошлым итогом: в ровном свету разброс окна нулевой.
const float LEAST_DEVIATION = 0.002;
const vec3 LUMINANCE = vec3(0.2126, 0.7152, 0.0722);

// Оценки и поверхности пикселей группы 8x8 для окна: у края группы окно меньше.
shared vec3 group_estimate[64];
shared uint group_surface[64];
// Прошлый итог годен, только если четыре ближайших к нему пикселя прошлого кадра со светом.
const float WHOLE_HISTORY = 0.99;

float view_z_of(uint surface) {
	return -float(surface & 65535u) / 65535.0 * SURFACE_DEPTH_RANGE_METERS;
}

// Обе проекции без наклона: из матрицы нужны только x, y и w.
vec2 screen_position_of(vec3 view_point) {
	vec2 clip = vec2(params.projection[0].x, params.projection[1].y) * view_point.xy + params.projection[2].xy * view_point.z + params.projection[3].xy;
	float clip_w = params.projection[2][3] * view_point.z + params.projection[3][3];
	return (clip / clip_w + 1.0) / ndc_per_pixel;
}

vec3 normal_of(uint packed_normal) {
	vec2 octahedron = (vec2(packed_normal >> 8u, packed_normal & 255u) - vec2(1.0)) / 254.0 * 2.0 - vec2(1.0);
	vec2 unfolded = vec2(octahedron.x + octahedron.y, octahedron.x - octahedron.y) * 0.5;
	return normalize(vec3(unfolded, 1.0 - abs(unfolded.x) - abs(unfolded.y)));
}

// Итог прошлого кадра в точке экрана по Catmull-Rom из пяти билинейных выборок: билинейная выборка каждый кадр
// размывала бы накопленное при движении камеры.
vec3 history_at(vec2 position) {
	vec2 texture_size = vec2(textureSize(history_ratio, 0));
	vec2 start = floor(position - 0.5) + 0.5;
	vec2 f = position - start;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 middle = (start + w2 / w12) / texture_size;
	vec2 before = (start - 1.0) / texture_size;
	vec2 after = (start + 2.0) / texture_size;
	vec3 sum = textureLod(history_ratio, vec2(middle.x, before.y), 0.0).rgb * (w12.x * w0.y) + textureLod(history_ratio, vec2(before.x, middle.y), 0.0).rgb * (w0.x * w12.y) +
			textureLod(history_ratio, middle, 0.0).rgb * (w12.x * w12.y) + textureLod(history_ratio, vec2(after.x, middle.y), 0.0).rgb * (w3.x * w12.y) +
			textureLod(history_ratio, vec2(middle.x, after.y), 0.0).rgb * (w12.x * w3.y);
	return clamp(sum / (w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y), 0.0, 1.0);
}

// Среднее отношения по диску размытия вокруг точки; ниже — накопление с прошлым кадром.
vec3 disk_average(ivec2 pixel, uvec2 own_texel, vec3 own_ratio, float radius, vec3 point) {
	ivec2 size = params.view_size.xy;
	uint own_normal = own_texel.y >> 16u;
	vec3 normal = normal_of(own_normal);
	vec3 helper = abs(normal.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(helper, normal));
	vec3 bitangent = cross(normal, tangent);
	vec2 center = vec2(pixel) + 0.5;
	vec2 screen_tangent = screen_position_of(point + tangent * radius) - center;
	vec2 screen_bitangent = screen_position_of(point + bitangent * radius) - center;
	// Поворот по пикселям — шум с ровным распределением в каждом окне 3x3. По кадрам узор сдвигается на точку двумерной
	// последовательности R2: остаток накопления не складывается в регулярную сетку, как при сдвиге вдоль одной прямой.
	vec2 shifted = vec2(pixel) + 64.0 * fract(params.blur.y * vec2(0.7548777, 0.5698403));
	float rotation = TAU * fract(52.9829189 * fract(dot(shifted, vec2(0.06711056, 0.00583715))));
	float rotation_cosine = cos(rotation);
	float rotation_sine = sin(rotation);
	vec2 rotated_tangent = screen_tangent * rotation_cosine + screen_bitangent * rotation_sine;
	vec2 rotated_bitangent = screen_bitangent * rotation_cosine - screen_tangent * rotation_sine;
	vec3 sum = own_ratio;
	float count = 1.0;
	for (int tap = 0; tap < DISK_TAPS; tap++) {
		ivec2 tap_pixel = ivec2(floor(center + rotated_tangent * DISK_OFFSETS[tap].x + rotated_bitangent * DISK_OFFSETS[tap].y));
		if (any(greaterThanEqual(uvec2(tap_pixel), uvec2(size))) || tap_pixel == pixel) {
			continue;
		}
		uvec2 tap_texel = imageLoad(ratio_surface, tap_pixel).xy;
		uint tap_normal = tap_texel.y >> 16u;
		// Та же упакованная нормаль почти на всех выборках: распаковка только при отличии.
		if (tap_normal == 0u || (tap_normal != own_normal && dot(normal_of(tap_normal), normal) < NORMAL_STOP_COSINE)) {
			continue;
		}
		if (abs(dot(view_point_at(tap_pixel, view_z_of(tap_texel.y)) - point, normal)) > PLANE_STOP_METERS) {
			continue;
		}
		vec3 tap_ratio = unpackUnorm4x8(tap_texel.x).rgb;
		tap_ratio *= tap_ratio;
		sum += tap_ratio;
		count += 1.0;
	}
	return sum / count;
}

void main() {
	ivec2 size = params.view_size.xy;
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	bool inside = all(lessThan(pixel, size));
	uvec2 own_texel = inside ? imageLoad(ratio_surface, pixel).xy : uvec2(0u);
	// У пикселя нет света ламп: тёмный итог не добавит свет там, где материал считает иначе.
	bool lit = own_texel.y != 0u;
	ndc_per_pixel = 2.0 / vec2(size);
	vec3 point = vec3(0.0);
	vec3 estimate = vec3(0.0);
	if (lit) {
		vec4 own = unpackUnorm4x8(own_texel.x);
		vec3 own_ratio = own.rgb * own.rgb;
		point = view_point_at(pixel, view_z_of(own_texel.y));
		estimate = params.blur.w > 0.0 ? own_ratio : disk_average(pixel, own_texel, own_ratio, mix(MIN_RADIUS_METERS, MAX_RADIUS_METERS, own.a), point);
	}
	// Окно читают все пиксели группы, поэтому выход только после барьера.
	group_estimate[gl_LocalInvocationIndex] = estimate;
	group_surface[gl_LocalInvocationIndex] = own_texel.y;
	barrier();
	if (!inside) {
		return;
	}
	if (!lit) {
		imageStore(blurred_ratio, pixel, vec4(0.0));
		return;
	}
	vec3 result = estimate;
	if (params.blur.w <= 0.0) {
		vec3 normal = normal_of(own_texel.y >> 16u);
		ivec2 local = ivec2(gl_LocalInvocationID.xy);
		vec3 mean = vec3(0.0);
		vec3 square = vec3(0.0);
		float count = 0.0;
		for (int y = max(local.y - 1, 0); y <= min(local.y + 1, 7); y++) {
			for (int x = max(local.x - 1, 0); x <= min(local.x + 1, 7); x++) {
				uint surface = group_surface[y * 8 + x];
				// Окно не переходит на другую поверхность — по тем же стопам, что и выборки диска.
				if (surface == 0u || ((surface >> 16u) != (own_texel.y >> 16u) && dot(normal_of(surface >> 16u), normal) < NORMAL_STOP_COSINE)) {
					continue;
				}
				ivec2 neighbor = pixel + ivec2(x, y) - local;
				if (abs(dot(view_point_at(neighbor, view_z_of(surface)) - point, normal)) > PLANE_STOP_METERS) {
					continue;
				}
				vec3 value = group_estimate[y * 8 + x];
				mean += value;
				square += value * value;
				count += 1.0;
			}
		}
		mean /= count;
		result = mean;
		if (params.view_size.z != 0) {
			vec4 previous_clip = params.previous_world_to_clip * (params.view_to_world * vec4(point, 1.0));
			vec2 previous_position = (previous_clip.xy / previous_clip.w + 1.0) / ndc_per_pixel;
			if (previous_clip.w > 0.0 && all(greaterThanEqual(previous_position, vec2(0.5))) && all(lessThanEqual(previous_position, vec2(size) - 0.5))) {
				if (texture(history_ratio, previous_position / vec2(textureSize(history_ratio, 0))).a >= WHOLE_HISTORY) {
					vec3 history = history_at(previous_position);
					vec3 previous = history * history;
					vec3 deviation = sqrt(max(square / count - mean * mean, vec3(0.0)) / count);
					vec3 clamped = clamp(previous, mean - HISTORY_SPREADS * deviation, mean + HISTORY_SPREADS * deviation);
					// Прошлый итог дальше шума среднего — свет в точке изменился: новый кадр берётся с большей долей, и отставание короче.
					float apart = abs(dot(previous - mean, LUMINANCE)) / (dot(deviation, LUMINANCE) + LEAST_DEVIATION);
					result = mix(clamped, mean, mix(CURRENT_SHARE, 0.5, smoothstep(1.0, 3.0, apart)));
				}
			}
		}
	}
	imageStore(blurred_ratio, pixel, vec4(sqrt(result), 1.0));
}
