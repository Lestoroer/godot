// lestoroer#local-lights
#[compute]

#version 450

#VERSION_DEFINES

// Моменты глубины карты лампы для мягкой тени: среднее и средний квадрат линейной глубины вдоль оси грани в долях радиуса
// лампы, по четвёрке текселей глубины на тексель моментов, затем разделимое гауссово размытие. За ребром грани Omni ядро
// читает соседнюю грань по направлению и пересчитывает её глубину на ось своей грани: швов на стыках нет, а сама карта
// глубины снята без лишнего запаса.

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
	// Тангенс половины снимка грани куба; ноль — Spot: плитка одна, и у её края ядро повторяет крайний тексель.
	float cube_extent;
} params;

// Группа считает плитку моментов TILE × TILE, каждый поток — четыре её текселя. Общая память не больше 32 КиБ, как на Apple M1.
const int TILE = 32;
// Контракт с MOMENT_MAX_RADIUS в local_lights_world.cpp.
const int MAX_RADIUS = 12;
const int MAX_SPAN = TILE + 2 * MAX_RADIUS;
const int THREADS = 256;
const int MAX_ROWS_PER_THREAD = (MAX_SPAN * TILE + THREADS - 1) / THREADS;

// Сначала моменты текселей окна ядра, после размытия по строкам — строки, TILE в каждой.
shared vec2 window_moments[MAX_SPAN * MAX_SPAN];
shared float weights[MAX_RADIUS + 1];

// Оси граней куба. Контракт с FACE_RIGHT, FACE_UP и FACE_FORWARD в local_lights_world.cpp и выбором грани в local_lights_inc.gdshaderinc.
const vec3 FACE_RIGHT[6] = vec3[](vec3(0, 0, 1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0));
const vec3 FACE_UP[6] = vec3[](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1));
const vec3 FACE_FORWARD[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, 1, 0));

// Запись линейна в обратной глубине; ноль — кастер ближе ближней плоскости, единица — пусто, то есть радиус лампы.
vec4 depth_shares(vec4 p_stored) {
	return params.inverse_range / mix(vec4(params.inverse_near), vec4(params.inverse_range), p_stored);
}

// Глубина вдоль оси грани p_face в направлении текселя p_texel этой грани, когда он лежит за её плиткой: из грани, куда смотрит направление.
float cube_depth_share(int p_face, ivec2 p_texel) {
	vec2 tangent = ((vec2(p_texel) + 0.5) / float(params.side) * 2.0 - 1.0) * params.cube_extent;
	vec3 direction = FACE_RIGHT[p_face] * tangent.x + FACE_UP[p_face] * tangent.y + FACE_FORWARD[p_face];
	vec3 magnitude = abs(direction);
	int face;
	vec3 local;
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
		face = direction.x > 0.0 ? 0 : 1;
		local = vec3(direction.z, direction.y, magnitude.x);
	} else if (magnitude.y >= magnitude.z) {
		face = direction.y < 0.0 ? 2 : 5;
		local = vec3(direction.x, direction.z, magnitude.y);
	} else {
		face = direction.z > 0.0 ? 3 : 4;
		local = vec3(direction.x, direction.y, magnitude.z);
	}
	vec2 texel = (local.xy / (local.z * params.cube_extent) * 0.5 + 0.5) * float(params.side);
	ivec2 fetched = clamp(ivec2(floor(texel)), ivec2(0), ivec2(params.side - 1));
	float stored = texelFetch(depth_atlas, params.map_origin + ivec2(face * params.side, 0) + fetched, 0).r;
	// Направление нормировано на свою ось, поэтому глубина по чужой оси делится на её проекцию.
	return min(depth_shares(vec4(stored)).x / local.z, 1.0);
}

void main() {
	int lane = int(gl_LocalInvocationIndex);
	int radius = params.radius;
	int span = TILE + 2 * radius;
	int moment_side = params.side >> 1;
	int face = int(gl_WorkGroupID.z);
	ivec2 face_origin = params.map_origin + ivec2(face * params.side, 0);
	ivec2 group_origin = ivec2(gl_WorkGroupID.xy) * TILE;
	vec2 inverse_atlas_size = 1.0 / vec2(textureSize(depth_atlas, 0));
	bool cube = params.cube_extent > 0.0;
	if (lane <= radius) {
		weights[lane] = exp(-0.5 * float(lane * lane) / (params.sigma * params.sigma));
	}
	for (int index = lane; index < span * span; index += THREADS) {
		ivec2 cell = group_origin - radius + ivec2(index % span, index / span);
		vec4 depth;
		if (all(greaterThanEqual(cell, ivec2(0))) && all(lessThan(cell, ivec2(moment_side))) || !cube) {
			// Четвёрка текселей глубины одной выборкой: координата на их общем углу.
			cell = clamp(cell, ivec2(0), ivec2(moment_side - 1));
			depth = depth_shares(textureGather(depth_atlas, vec2(face_origin + cell * 2 + 1) * inverse_atlas_size, 0));
		} else {
			ivec2 texel = cell * 2;
			depth = vec4(cube_depth_share(face, texel), cube_depth_share(face, texel + ivec2(1, 0)), cube_depth_share(face, texel + ivec2(0, 1)), cube_depth_share(face, texel + ivec2(1, 1)));
		}
		window_moments[index] = vec2(dot(depth, vec4(0.25)), dot(depth * depth, vec4(0.25)));
	}
	barrier();
	float total = weights[0];
	for (int k = 1; k <= radius; k++) {
		total += 2.0 * weights[k];
	}
	float inverse_total = 1.0 / total;
	// Строки окна по TILE столбцов, размытые по x; держатся в регистрах, пока окно ещё читают другие потоки.
	vec2 rows[MAX_ROWS_PER_THREAD];
	for (int step = 0; step < MAX_ROWS_PER_THREAD; step++) {
		int index = lane + step * THREADS;
		if (index < span * TILE) {
			int center = (index / TILE) * span + index % TILE + radius;
			vec2 sum = window_moments[center] * weights[0];
			for (int k = 1; k <= radius; k++) {
				sum += (window_moments[center - k] + window_moments[center + k]) * weights[k];
			}
			rows[step] = sum * inverse_total;
		}
	}
	barrier();
	for (int step = 0; step < MAX_ROWS_PER_THREAD; step++) {
		int index = lane + step * THREADS;
		if (index < span * TILE) {
			window_moments[index] = rows[step];
		}
	}
	barrier();
	ivec2 local = ivec2(gl_LocalInvocationID.xy);
	for (int quarter = 0; quarter < 4; quarter++) {
		ivec2 texel = local + ivec2(quarter & 1, quarter >> 1) * 16;
		// Сторона, не кратная плитке, бывает у наибольшей плитки из настройки и у стороны 96.
		if (any(greaterThanEqual(group_origin + texel, ivec2(moment_side)))) {
			continue;
		}
		int center = (texel.y + radius) * TILE + texel.x;
		vec2 sum = window_moments[center] * weights[0];
		for (int k = 1; k <= radius; k++) {
			sum += (window_moments[center - k * TILE] + window_moments[center + k * TILE]) * weights[k];
		}
		imageStore(moment_atlas, face_origin / 2 + group_origin + texel, vec4(sum * inverse_total, 0.0, 0.0));
	}
}
