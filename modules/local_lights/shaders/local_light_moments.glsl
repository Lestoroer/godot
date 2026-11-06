// lestoroer#local-lights
#[compute]

#version 450

#VERSION_DEFINES

// Моменты глубины карты лампы для мягкой тени: среднее и средний квадрат линейной глубины вдоль оси грани в долях радиуса
// лампы, по четвёрке текселей глубины на тексель моментов, затем разделимое гауссово размытие. Грань Omni снята с запасом
// на радиус ядра, поэтому у её ребра ядро читает настоящие соседние направления, а не край плитки.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_atlas;
layout(rg16, set = 0, binding = 1) uniform restrict writeonly image2D moment_atlas;

layout(push_constant, std430) uniform Params {
	// Начало первой грани карты в атласе глубины, тексели; грани Omni лежат подряд по x.
	ivec2 map_origin;
	// Сторона грани в текселях глубины.
	int side;
	// Радиус ядра в текселях моментов, не больше MAX_RADIUS.
	int radius;
	float inverse_near;
	float inverse_range;
	// Сигма ядра в текселях моментов.
	float sigma;
	float unused;
} params;

const int GROUP = 16;
// Контракт с MOMENT_MAX_RADIUS в local_lights_world.cpp.
const int MAX_RADIUS = 12;
const int MAX_SPAN = GROUP + 2 * MAX_RADIUS;

shared vec2 cell_moments[MAX_SPAN * MAX_SPAN];
shared vec2 row_moments[MAX_SPAN * GROUP];
shared float weights[MAX_RADIUS + 1];

// Запись линейна в обратной глубине; ноль — кастер ближе ближней плоскости, единица — пусто, то есть радиус лампы.
float depth_share(ivec2 p_texel) {
	float stored = texelFetch(depth_atlas, p_texel, 0).r;
	return params.inverse_range / mix(params.inverse_near, params.inverse_range, stored);
}

void main() {
	int lane = int(gl_LocalInvocationIndex);
	int radius = params.radius;
	int span = GROUP + 2 * radius;
	int moment_side = params.side >> 1;
	ivec2 face_origin = params.map_origin + ivec2(int(gl_WorkGroupID.z) * params.side, 0);
	ivec2 group_origin = ivec2(gl_WorkGroupID.xy) * GROUP;
	if (lane <= radius) {
		weights[lane] = exp(-0.5 * float(lane * lane) / (params.sigma * params.sigma));
	}
	// Ядро у края плитки повторяет крайний тексель: соседняя плитка — другая грань или другая лампа.
	for (int index = lane; index < span * span; index += GROUP * GROUP) {
		ivec2 cell = clamp(group_origin - radius + ivec2(index % span, index / span), ivec2(0), ivec2(moment_side - 1));
		ivec2 texel = face_origin + cell * 2;
		vec4 depth = vec4(depth_share(texel), depth_share(texel + ivec2(1, 0)), depth_share(texel + ivec2(0, 1)), depth_share(texel + ivec2(1, 1)));
		cell_moments[index] = vec2(dot(depth, vec4(0.25)), dot(depth * depth, vec4(0.25)));
	}
	barrier();
	float total = weights[0];
	for (int k = 1; k <= radius; k++) {
		total += 2.0 * weights[k];
	}
	float inverse_total = 1.0 / total;
	for (int index = lane; index < span * GROUP; index += GROUP * GROUP) {
		int center = (index / GROUP) * span + index % GROUP + radius;
		vec2 sum = cell_moments[center] * weights[0];
		for (int k = 1; k <= radius; k++) {
			sum += (cell_moments[center - k] + cell_moments[center + k]) * weights[k];
		}
		row_moments[index] = sum * inverse_total;
	}
	barrier();
	ivec2 local = ivec2(gl_LocalInvocationID.xy);
	int center = (local.y + radius) * GROUP + local.x;
	vec2 sum = row_moments[center] * weights[0];
	for (int k = 1; k <= radius; k++) {
		sum += (row_moments[center - k * GROUP] + row_moments[center + k * GROUP]) * weights[k];
	}
	// Сторона, не кратная группе, бывает у наибольшей плитки из настройки.
	if (all(lessThan(group_origin + local, ivec2(moment_side)))) {
		imageStore(moment_atlas, face_origin / 2 + group_origin + local, vec4(sum * inverse_total, 0.0, 0.0));
	}
}
