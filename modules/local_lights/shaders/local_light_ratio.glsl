// lestoroer#local-lights
#[compute]
#version 450

#VERSION_DEFINES

#include "local_light_ratio_surface_inc.glsl"

// Проход после предварительной глубины: на пиксель считается доля света ламп ячейки, которую пропускает жёсткая тень от
// центра лампы, один gather на лампу. Отношение, радиус размытия, нормаль и глубина записываются для размытия.
// Раскладка данных ламп и обход ячейки — контракт с local_lights_inc.gdshaderinc и local_lights_world.cpp.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform sampler2D cull_grid;
layout(set = 0, binding = 2) uniform sampler2D cull_data;
layout(set = 0, binding = 3) uniform sampler2D shadow_atlas;
// Отношение — затенённый свет ламп к незатенённому по каналам: лампа в тени гасит только свой цвет.
layout(rg32ui, set = 0, binding = 7) uniform restrict writeonly uimage2D ratio_surface;
layout(set = 0, binding = 5, std140) uniform Params {
	mat4 inverse_projection;
	mat4 projection;
	mat4 view_to_world;
	vec4 cull_origin_and_step;
	ivec4 cull_dims;
	// x — радиус размытия на метр расстояния от лампы до точки; z — рост радиуса, когда свет точки не от одной лампы.
	vec4 blur;
	ivec4 view_size;
} params;

const int INDEX_W_LOG2 = 11;
const int INDEX_W_MASK = (1 << INDEX_W_LOG2) - 1;
const int SLOT_SHIFT = 3;
const float MIN_FACING_WEIGHT = 0.01;

vec3 view_point_at(ivec2 pixel, ivec2 size) {
	float depth = texelFetch(depth_buffer, pixel, 0).r;
	vec4 view = params.inverse_projection * vec4((vec2(pixel) + 0.5) / vec2(size) * 2.0 - 1.0, depth, 1.0);
	return view.xyz / view.w;
}

// Геометрическая нормаль по глубине: по каждой оси берётся сосед с меньшим скачком, чтобы не перешагивать край.
vec3 surface_normal_at(ivec2 pixel, ivec2 size, vec3 point) {
	vec3 forward_x = view_point_at(min(pixel + ivec2(1, 0), size - 1), size) - point;
	vec3 backward_x = point - view_point_at(max(pixel - ivec2(1, 0), ivec2(0)), size);
	vec3 forward_y = view_point_at(min(pixel + ivec2(0, 1), size - 1), size) - point;
	vec3 backward_y = point - view_point_at(max(pixel - ivec2(0, 1), ivec2(0)), size);
	// У края кадра соседа с одной стороны нет, разность там нулевая и выбирать её нельзя.
	vec3 along_x = pixel.x + 1 < size.x && (pixel.x == 0 || dot(forward_x, forward_x) < dot(backward_x, backward_x)) ? forward_x : backward_x;
	vec3 along_y = pixel.y + 1 < size.y && (pixel.y == 0 || dot(forward_y, forward_y) < dot(backward_y, backward_y)) ? forward_y : backward_y;
	vec3 perpendicular = cross(along_y, along_x);
	float perpendicular_length = length(perpendicular);
	// Соседи на одной прямой, например у края кадра: своей плоскости нет, точка считается обращённой к камере.
	if (perpendicular_length <= 0.0) {
		return -normalize(point);
	}
	vec3 normal = perpendicular / perpendicular_length;
	return dot(normal, point) > 0.0 ? -normal : normal;
}

// Запас грани сверх её ребра, тексели: контракт с local_lights_world.cpp.
const float CUBE_GUARD_TEXELS = 4.0;
// Единица начала плитки в атласе: контракт с local_lights_world.cpp.
const float MAP_ORIGIN_UNIT = 32.0;
// Доля текселя в смещении вдоль луча: складки мельче текселя карта не различает. Контракт с local_lights_inc.gdshaderinc, как и две следующие.
const float DEPTH_OFFSET_TEXELS = 0.125;
// Разброс глубин в пределах стольких текселей считается одной поверхностью: наклон до 70°.
const float SAME_SURFACE_TEXELS = 4.0;
const float MAP_DEPTH_STEPS = 65535.0;
const float THIN_WALL_TEXEL_METERS = 0.02;

// Ближняя плоскость карты: контракт с map_near_plane в local_lights_world.cpp.
float map_near_plane(float range) {
	return max(min(range / 64.0, 0.03), range * range / (65536.0 * 0.0125));
}

// Открытая доля по одному gather. axis_depth — глубина точки вдоль оси карты, texel_size — размер текселя на единицу этой глубины.
// Смещение — метры из данных лампы, доля текселя и половина шага записанной глубины: шаг растёт как квадрат расстояния, без него на стенах появляются кольца.
// Записанный ноль значит кастер ближе ближней плоскости, его глубина неизвестна: точка за этой плоскостью закрыта, смещение тут не даёт запаса.
float compare_depth(vec2 atlas_texel, float axis_depth, float axis_cosine, float depth_offset, float texel_size, float range) {
	// Выборка точно между четырьмя центрами: округление координаты в gather не выбирает другую четвёрку, чем считают веса.
	vec2 below = floor(atlas_texel - 0.5);
	vec4 stored = textureGather(shadow_atlas, (below + 1.0) / vec2(textureSize(shadow_atlas, 0)), 0);
	float near_plane = map_near_plane(range);
	float depth_bias = depth_offset * axis_cosine + axis_depth * (DEPTH_OFFSET_TEXELS * texel_size + axis_depth / (near_plane * MAP_DEPTH_STEPS * 2.0));
	float compared_distance = axis_depth - depth_bias;
	vec4 depth = 1.0 / ((1.0 - stored) / near_plane + stored / range);
	depth = mix(depth, vec4(near_plane - depth_bias), vec4(lessThanEqual(stored, vec4(0.0))));
	vec2 fraction = atlas_texel - 0.5 - below;
	vec4 weights = vec4((1.0 - fraction.x) * fraction.y, fraction.x * fraction.y, fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * (1.0 - fraction.y));
	// Четыре глубины одной поверхности: сравнивается её глубина точно в точке, записанное значение на плоскости линейно. Соседний тексель наклонной стенки
	// глубже точки за ней, поэтому раздельное сравнение пропускало бы свет, а сама поверхность затеняла бы себя. На разрыве глубин — PCF 2×2.
	float nearest = min(min(depth.x, depth.y), min(depth.z, depth.w));
	float farthest = max(max(depth.x, depth.y), max(depth.z, depth.w));
	float same_surface_depth = SAME_SURFACE_TEXELS * texel_size * axis_depth;
	if (farthest - nearest < same_surface_depth) {
		float stored_at_point = dot(stored, weights);
		return 1.0 / ((1.0 - stored_at_point) / near_plane + stored_at_point / range) >= compared_distance ? 1.0 : 0.0;
	}
	float open_share = dot(vec4(greaterThanEqual(depth, vec4(compared_distance))), weights);
	// Тексель крупнее тонкой стенки: её край занимает один-два текселя, и в соседних луч проходит мимо неё и мимо самой точки. Если поверхности точки нет ни в одном
	// из четырёх, а преграда есть, точка за преградой. Вблизи лампы правило не нужно и вредно: у тонких предметов своя поверхность тоже не попадает в тексели.
	if (open_share < 1.0 && texel_size * axis_depth > THIN_WALL_TEXEL_METERS && !any(lessThan(abs(depth - vec4(axis_depth)), vec4(same_surface_depth)))) {
		return 0.0;
	}
	return open_share;
}

// t4.xy: смещение вдоль луча, метры, и сдвиг направления по нормали, тексели; t6.w и t4.z: начало места в атласе, side: сторона плитки. Контракт с local_lights_world.cpp.
float omni_visibility(vec4 t4, vec4 t6, float side, vec3 to_point, vec3 surface_normal) {
	vec3 p = to_point + t6.xyz;
	float reach = length(p);
	vec3 direction = p / reach;
	float texel_size = 2.0 / side;
	vec3 sample_direction = normalize(direction + surface_normal * t4.y * texel_size);
	vec3 magnitude = abs(sample_direction);
	// Оси граней — контракт с local_lights_world.cpp.
	float face;
	vec3 local;
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
		face = sample_direction.x > 0.0 ? 0.0 : 1.0;
		local = vec3(sample_direction.z, sample_direction.y, magnitude.x);
	} else if (magnitude.y >= magnitude.z) {
		face = sample_direction.y < 0.0 ? 2.0 : 5.0;
		local = vec3(sample_direction.x, sample_direction.z, magnitude.y);
	} else {
		face = sample_direction.z > 0.0 ? 3.0 : 4.0;
		local = vec3(sample_direction.x, sample_direction.y, magnitude.z);
	}
	vec2 uv = local.xy / local.z / (1.0 + CUBE_GUARD_TEXELS / side) * 0.5 + 0.5;
	return compare_depth(vec2(t6.w, t4.z) * MAP_ORIGIN_UNIT + vec2(face * side, 0.0) + uv * side, reach * local.z, local.z, t4.x, texel_size, 1.0 / t4.w);
}

// Смещения как у Spot в Godot: нормаль сдвигает саму точку пропорционально расстоянию; shadow_bias / 100 прибавлен к
// перспективной глубине с ближней плоскостью SPOT_SHADOW_NEAR, поэтому в метрах он растёт вместе с расстоянием.
float spot_visibility(ivec2 sc, vec4 t4, vec4 t6, vec3 to_point, vec3 surface_normal) {
	vec3 p = to_point + t6.xyz;
	float reach = length(p);
	vec4 t5 = texelFetch(cull_data, sc + ivec2(5, 0), 0);
	vec4 t7 = texelFetch(cull_data, sc + ivec2(7, 0), 0);
	float side = t7.w;
	float texel_size = 2.0 / (side * t5.w);
	vec3 sample_direction = normalize(p / reach + surface_normal * t4.y * texel_size);
	vec3 point = vec3(dot(sample_direction, cross(t7.xyz, t5.xyz)), dot(sample_direction, t7.xyz), dot(sample_direction, t5.xyz));
	if (point.z <= 0.0) {
		return 0.0;
	}
	vec2 ndc = point.xy * (t5.w / point.z);
	if (any(greaterThan(abs(ndc), vec2(1.0)))) {
		return 0.0;
	}
	vec2 uv = ndc * 0.5 + 0.5;
	return compare_depth(vec2(t6.w, t4.z) * MAP_ORIGIN_UNIT + uv * side, reach * point.z, point.z, t4.x, texel_size, 1.0 / t4.w);
}

// Байты 1..255: ноль остаётся за пикселем без света ламп.
uint packed_normal(vec3 normal) {
	vec2 folded = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));
	vec2 octahedron = vec2(folded.x + folded.y, folded.x - folded.y);
	uvec2 bytes = uvec2(round((octahedron * 0.5 + 0.5) * 254.0)) + uvec2(1u);
	return (bytes.x << 8u) | bytes.y;
}

float color_weight(vec3 color) {
	return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
	ivec2 size = params.view_size.xy;
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, size))) {
		return;
	}
	// Обратный Z: ноль — дальняя плоскость, там только небо.
	if (texelFetch(depth_buffer, pixel, 0).r <= 0.0) {
		imageStore(ratio_surface, pixel, uvec4(0u));
		return;
	}
	vec3 view_point = view_point_at(pixel, size);
	vec3 view_normal = surface_normal_at(pixel, size, view_point);
	vec3 world_point = (params.view_to_world * vec4(view_point, 1.0)).xyz;
	vec3 world_normal = mat3(params.view_to_world) * view_normal;
	float cell_size = params.cull_origin_and_step.w;
	vec3 relative = world_point - params.cull_origin_and_step.xyz;
	ivec3 cell = ivec3(floor(relative / cell_size));
	float unshadowed = 0.0;
	vec3 unshadowed_color = vec3(0.0);
	vec3 shadowed_color = vec3(0.0);
	vec3 behind_unshadowed_color = vec3(0.0);
	vec3 behind_shadowed_color = vec3(0.0);
	float distance_light_sum = 0.0;
	float largest_light = 0.0;
	if (all(lessThan(uvec3(cell), uvec3(params.cull_dims.xyz)))) {
		vec4 members = texelFetch(cull_grid, ivec2(cell.x, cell.y + cell.z * params.cull_dims.y), 0);
		int offset = int(members.r);
		int omni_count = int(members.g);
		int spot_count = int(members.b);
		vec3 point_in_cell = relative - (vec3(cell) + vec3(0.5)) * cell_size;
		for (int member = 0; member < omni_count + spot_count; member++) {
			int texel = (offset + member) << SLOT_SHIFT;
			ivec2 sc = ivec2(texel & INDEX_W_MASK, texel >> INDEX_W_LOG2);
			vec4 position_and_range = texelFetch(cull_data, sc, 0);
			vec3 to_light = position_and_range.xyz - point_in_cell;
			float distance_squared = dot(to_light, to_light);
			float range_ratio = distance_squared * position_and_range.w;
			if (range_ratio >= 1.0) {
				continue;
			}
			float inverse_distance = inversesqrt(max(distance_squared, 1e-8));
			// Нормаль шейдинга сглажена и бывает обращена к лампе там, где грань уже нет: такой точке тоже нужно отношение.
			float geometric_facing = dot(world_normal, to_light) * inverse_distance;
			float facing = max(geometric_facing, MIN_FACING_WEIGHT);
			vec4 color_decay = texelFetch(cull_data, sc + ivec2(2, 0), 0);
			float window = 1.0 - range_ratio * range_ratio;
			float attenuation = window * window * pow(max(distance_squared * inverse_distance, 0.0001), -color_decay.w);
			bool is_spot = member >= omni_count;
			if (is_spot) {
				vec4 axis_cone = texelFetch(cull_data, sc + ivec2(1, 0), 0);
				float along_axis = dot(-to_light, axis_cone.xyz);
				if (along_axis <= 0.0 || along_axis * along_axis < axis_cone.w * distance_squared) {
					continue;
				}
				vec4 fin = texelFetch(cull_data, sc + ivec2(3, 0), 0);
				float rim = max(1e-4, (1.0 - max(along_axis * inverse_distance, fin.x)) / (1.0 - fin.x));
				attenuation *= 1.0 - pow(rim, fin.z);
			}
			vec3 light_color = attenuation * facing * color_decay.rgb;
			float light_weight = color_weight(light_color);
			float visibility = 1.0;
			vec4 t6 = texelFetch(cull_data, sc + ivec2(6, 0), 0);
			if (t6.w >= 0.0) {
				vec4 t4 = texelFetch(cull_data, sc + ivec2(4, 0), 0);
				visibility = is_spot ? spot_visibility(sc, t4, t6, -to_light, world_normal) : omni_visibility(t4, t6, texelFetch(cull_data, sc + ivec2(7, 0), 0).w, -to_light, world_normal);
			}
			unshadowed += light_weight;
			if (geometric_facing > 0.0) {
				unshadowed_color += light_color;
				shadowed_color += light_color * visibility;
			} else {
				behind_unshadowed_color += light_color;
				behind_shadowed_color += light_color * visibility;
			}
			// Несколько ламп дают один радиус: вес — незатенённый свет лампы в точке. Он известен и в свету, и в тени
			// и меняется плавно, поэтому радиус не рвёт форму тени.
			distance_light_sum += light_weight * sqrt(distance_squared);
			largest_light = max(largest_light, light_weight);
		}
	}
	if (unshadowed <= 0.0) {
		imageStore(ratio_surface, pixel, uvec4(0u));
		return;
	}
	float radius = clamp(params.blur.x * distance_light_sum / unshadowed * (1.0 + params.blur.z * (1.0 - largest_light / unshadowed)), MIN_RADIUS_METERS, MAX_RADIUS_METERS);
	// Лампа за поверхностью за тонкой стенкой может показаться открытой: сравнение знает глубину только с точностью до текселя и смещения.
	// Пока в точке есть лампа перед поверхностью, отношение считается только по таким лампам. Лампы за поверхностью важны одни: сглаженная нормаль бывает обращена к ним.
	bvec3 only_behind = lessThanEqual(unshadowed_color, vec3(0.0));
	unshadowed_color = mix(unshadowed_color, behind_unshadowed_color, only_behind);
	shadowed_color = mix(shadowed_color, behind_shadowed_color, only_behind);
	// Канал без света ламп ничего не умножает: единица не тянет соседей в темноту при размытии.
	vec3 ratio = mix(vec3(1.0), shadowed_color / unshadowed_color, greaterThan(unshadowed_color, vec3(0.0)));
	uint packed_depth = uint(round(clamp(-view_point.z / SURFACE_DEPTH_RANGE_METERS, 0.0, 1.0) * 65535.0));
	imageStore(ratio_surface, pixel, uvec4(packUnorm4x8(vec4(sqrt(ratio), (radius - MIN_RADIUS_METERS) / (MAX_RADIUS_METERS - MIN_RADIUS_METERS))), (packed_normal(view_normal) << 16u) | packed_depth, 0u, 0u));
}
