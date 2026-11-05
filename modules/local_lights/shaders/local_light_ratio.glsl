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
layout(set = 0, binding = 4) uniform sampler2D lamp_list;
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

// Список ламп ячеек: индекс лампы в текселе. Таблица ламп: восемь текселей на лампу, в строке 256 ламп.
const int LIST_WIDTH_LOG2 = 11;
const int LIST_WIDTH_MASK = (1 << LIST_WIDTH_LOG2) - 1;
const int SLOT_SHIFT = 3;
const int LAMPS_PER_ROW_LOG2 = 8;
const int LAMPS_PER_ROW_MASK = (1 << LAMPS_PER_ROW_LOG2) - 1;
const float MIN_FACING_WEIGHT = 0.000001;

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

// Грань снята шире своего ребра на столько текселей в сумме, по половине с каждой стороны: контракт с local_lights_world.cpp.
const float CUBE_GUARD_TEXELS = 4.0;
// Основа запаса вдоль луча, вместе с долей текселя меньше самой тонкой стенки 5 мм. Доля текселя добавляется только до предела, иначе вдали запас станет толще стенки.
// Половина шага записанной глубины идёт сверху и на дальнем конце карты доходит до 6 мм: там стенка 5 мм может оказаться тоньше запаса.
const float WALL_MARGIN_METERS = 0.003;
const float BEND_MARGIN_TEXELS = 0.125;
const float BEND_MARGIN_LIMIT_METERS = 0.0015;
// Сдвиг направления выборки по нормали на тексель убирает самозатенение под любым наклоном и не пускает свет сквозь стенку: у обратной стороны он ведёт глубже в тень.
const float NORMAL_OFFSET_TEXELS = 1.0;
// Разброс глубин в пределах стольких текселей считается одной поверхностью: наклон до 70°.
const float SAME_SURFACE_TEXELS = 4.0;
// Насколько тексель может отойти от плоскости точки и считаться её поверхностью.
const float OWN_SURFACE_TEXELS = 0.25;
const float MAP_DEPTH_STEPS = 65535.0;

// Открытая доля лампы по четырём ближайшим текселям карты. sample_tangent — направление выборки в осях карты (x/z, y/z), texel_tangent — его шаг на тексель,
// axis_depth — глубина точки вдоль оси карты; plane_distance — расстояние плоскости точки от лампы вдоль нормали; local_normal — нормаль в осях карты.
float open_share(vec2 atlas_texel, vec2 sample_tangent, float texel_tangent, float axis_depth, float axis_cosine, float plane_distance, vec3 local_normal, float near_plane, float range) {
	// Выборка точно между четырьмя центрами: округление координаты в gather не выбирает другую четвёрку, чем считают веса.
	vec2 below = floor(atlas_texel - 0.5);
	vec4 stored = textureGather(shadow_atlas, (below + 1.0) / vec2(textureSize(shadow_atlas, 0)), 0);
	// Половина шага записанной глубины: шаг растёт как квадрат расстояния, без него на стенах появляются кольца.
	float margin = (WALL_MARGIN_METERS + min(BEND_MARGIN_TEXELS * texel_tangent * axis_depth, BEND_MARGIN_LIMIT_METERS)) * axis_cosine + axis_depth * axis_depth / (near_plane * MAP_DEPTH_STEPS * 2.0);
	float compared_depth = axis_depth - margin;
	vec4 depth = 1.0 / ((1.0 - stored) / near_plane + stored / range);
	// Записанный ноль значит кастер ближе ближней плоскости, его глубина неизвестна: точка за этой плоскостью закрыта.
	depth = mix(depth, vec4(near_plane - margin), vec4(lessThanEqual(stored, vec4(0.0))));
	vec2 fraction = atlas_texel - 0.5 - below;
	vec4 weights = vec4((1.0 - fraction.x) * fraction.y, fraction.x * fraction.y, fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * (1.0 - fraction.y));
	float spread = max(max(depth.x, depth.y), max(depth.z, depth.w)) - min(min(depth.x, depth.y), min(depth.z, depth.w));
	if (spread < SAME_SURFACE_TEXELS * texel_tangent * axis_depth) {
		// Одна поверхность: записанное значение на плоскости линейно, поэтому сравнивается её глубина точно в точке. Иначе соседний тексель наклонной
		// стенки оказался бы глубже точки за ней и пропустил свет.
		float stored_at_point = dot(stored, weights);
		return 1.0 / ((1.0 - stored_at_point) / near_plane + stored_at_point / range) >= compared_depth ? 1.0 : 0.0;
	}
	// Разрыв глубин. Если открытый тексель лежит на плоскости самой точки, это край настоящей тени, его сгладит доля открытых текселей.
	// Если нет, точка за преградой или у края чужого силуэта, а открытые тексели смотрят мимо неё: закрывает любой более близкий тексель.
	vec4 open_texels = vec4(greaterThanEqual(depth, vec4(compared_depth)));
	vec4 ray_x = sample_tangent.x + (vec4(0.0, 1.0, 1.0, 0.0) - fraction.x) * texel_tangent;
	vec4 ray_y = sample_tangent.y + (vec4(1.0, 1.0, 0.0, 0.0) - fraction.y) * texel_tangent;
	vec4 plane_depth = plane_distance / (local_normal.x * ray_x + local_normal.y * ray_y + local_normal.z);
	vec4 own_texels = vec4(lessThanEqual(abs(depth - plane_depth), vec4(margin + OWN_SURFACE_TEXELS * texel_tangent * axis_depth)));
	if (dot(open_texels, own_texels) <= 0.0) {
		return all(greaterThan(open_texels, vec4(0.5))) ? 1.0 : 0.0;
	}
	return dot(open_texels, weights);
}

// t4.x — ближняя плоскость карты, t4.w — её радиус; t6.w и t4.z — начало места в атласе; side — сторона плитки. Контракт с local_lights_world.cpp.
float omni_visibility(vec4 t4, vec4 t6, float side, vec3 to_point, vec3 surface_normal) {
	vec3 p = to_point + t6.xyz;
	float reach = length(p);
	float face_span = 1.0 + CUBE_GUARD_TEXELS / side;
	float texel_tangent = 2.0 * face_span / side;
	vec3 sample_direction = normalize(p / reach + surface_normal * NORMAL_OFFSET_TEXELS * texel_tangent);
	vec3 magnitude = abs(sample_direction);
	// Оси граней — контракт с local_lights_world.cpp.
	float face;
	vec3 local;
	vec3 local_normal;
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
		face = sample_direction.x > 0.0 ? 0.0 : 1.0;
		local = sample_direction.zyx;
		local_normal = surface_normal.zyx;
	} else if (magnitude.y >= magnitude.z) {
		face = sample_direction.y < 0.0 ? 2.0 : 5.0;
		local = sample_direction.xzy;
		local_normal = surface_normal.xzy;
	} else {
		face = sample_direction.z > 0.0 ? 3.0 : 4.0;
		local = sample_direction;
		local_normal = surface_normal;
	}
	local_normal.z *= sign(local.z);
	local.z = abs(local.z);
	vec2 tangent = local.xy / local.z;
	vec2 uv = tangent / face_span * 0.5 + 0.5;
	return open_share(vec2(t6.w, t4.z) + vec2(face * side, 0.0) + uv * side, tangent, texel_tangent, reach * local.z, local.z, dot(surface_normal, p), local_normal, t4.x, t4.w);
}

float spot_visibility(ivec2 sc, vec4 t4, vec4 t6, vec3 to_point, vec3 surface_normal) {
	vec3 p = to_point + t6.xyz;
	float reach = length(p);
	vec4 t5 = texelFetch(cull_data, sc + ivec2(5, 0), 0);
	vec4 t7 = texelFetch(cull_data, sc + ivec2(7, 0), 0);
	float side = t7.w;
	float texel_tangent = 2.0 / (side * t5.w);
	vec3 sample_direction = normalize(p / reach + surface_normal * NORMAL_OFFSET_TEXELS * texel_tangent);
	vec3 map_right = cross(t7.xyz, t5.xyz);
	vec3 local = vec3(dot(sample_direction, map_right), dot(sample_direction, t7.xyz), dot(sample_direction, t5.xyz));
	if (local.z <= 0.0) {
		return 0.0;
	}
	vec2 tangent = local.xy / local.z;
	vec2 ndc = tangent * t5.w;
	if (any(greaterThan(abs(ndc), vec2(1.0)))) {
		return 0.0;
	}
	vec3 local_normal = vec3(dot(surface_normal, map_right), dot(surface_normal, t7.xyz), dot(surface_normal, t5.xyz));
	// У края снимка четвёрка текселей не должна заходить в соседнюю плитку: конус шире 67° или лампа, повёрнутая при старой карте.
	vec2 tile_texel = clamp((ndc * 0.5 + 0.5) * side, vec2(1.0), vec2(side - 1.0));
	return open_share(vec2(t6.w, t4.z) + tile_texel, tangent, texel_tangent, reach * local.z, local.z, dot(surface_normal, p), local_normal, t4.x, t4.w);
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
	float distance_light_sum = 0.0;
	float largest_light = 0.0;
	if (all(lessThan(uvec3(cell), uvec3(params.cull_dims.xyz)))) {
		vec4 members = texelFetch(cull_grid, ivec2(cell.x, cell.y + cell.z * params.cull_dims.y), 0);
		int offset = int(members.r);
		int omni_count = int(members.g);
		int spot_count = int(members.b);
		for (int member = 0; member < omni_count + spot_count; member++) {
			int list_index = offset + member;
			int lamp = int(texelFetch(lamp_list, ivec2(list_index & LIST_WIDTH_MASK, list_index >> LIST_WIDTH_LOG2), 0).r);
			ivec2 sc = ivec2((lamp & LAMPS_PER_ROW_MASK) << SLOT_SHIFT, lamp >> LAMPS_PER_ROW_LOG2);
			vec4 position_and_range = texelFetch(cull_data, sc, 0);
			vec3 to_light = position_and_range.xyz - world_point;
			float distance_squared = dot(to_light, to_light);
			float range_ratio = distance_squared * position_and_range.w;
			if (range_ratio >= 1.0) {
				continue;
			}
			float inverse_distance = inversesqrt(max(distance_squared, 1e-8));
			// Лампа за плоскостью точки на отношение не влияет, пока есть лампа перед ней: иначе тень других ламп разбавлялась бы светом, которого нет.
			// Вес не нулевой: нормаль шейдинга сглажена и бывает обращена к лампе там, где грань уже нет, и такой точке тоже нужно отношение.
			float facing = max(dot(world_normal, to_light) * inverse_distance, MIN_FACING_WEIGHT);
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
			unshadowed_color += light_color;
			shadowed_color += light_color * visibility;
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
	// Канал без света ламп ничего не умножает: единица не тянет соседей в темноту при размытии.
	vec3 ratio = mix(vec3(1.0), shadowed_color / unshadowed_color, greaterThan(unshadowed_color, vec3(0.0)));
	uint packed_depth = uint(round(clamp(-view_point.z / SURFACE_DEPTH_RANGE_METERS, 0.0, 1.0) * 65535.0));
	imageStore(ratio_surface, pixel, uvec4(packUnorm4x8(vec4(sqrt(ratio), (radius - MIN_RADIUS_METERS) / (MAX_RADIUS_METERS - MIN_RADIUS_METERS))), (packed_normal(view_normal) << 16u) | packed_depth, 0u, 0u));
}
