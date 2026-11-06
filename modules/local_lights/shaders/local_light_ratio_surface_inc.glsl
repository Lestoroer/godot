// lestoroer#local-lights
// Тексель прохода отношения. Всё, что нужно выборке размытия, лежит в нём: вторая текстура в выборке стоила бы столько же, сколько вся остальная её работа.
// x — корень из отношения rgb и радиус размытия между наименьшим и наибольшим, по 8 бит: корень держит точность в тёмных значениях.
// y — в старших 16 битах нормаль в осях вида, полусфера в октаэдре, байты 1..255, ноль — у пикселя нет света ламп; в младших 16 битах линейная глубина.

const float MIN_RADIUS_METERS = 0.02;
const float MAX_RADIUS_METERS = 0.2;
// Шаг глубины 2 мм, на порядок меньше допуска плоскости в размытии. Дальше предела радиус меньше пикселя и глубина не важна.
const float SURFACE_DEPTH_RANGE_METERS = 128.0;

layout(set = 0, binding = 5, std140) uniform Params {
	mat4 inverse_projection;
	mat4 projection;
	mat4 view_to_world;
	vec4 cull_origin_and_step;
	ivec4 cull_dims;
	// x — радиус размытия на метр расстояния от лампы до точки; y — номер кадра шума; z — рост радиуса, когда свет точки не от одной лампы; w больше нуля — показать маску без размытия.
	vec4 blur;
	// xy — размер вида; z не ноль — итог прошлого кадра годен для накопления.
	ivec4 view_size;
	// Мир в кадр прошлого кадра того же вида: где точка была на экране.
	mat4 previous_world_to_clip;
} params;

// Шаг координат кадра на пиксель, считается один раз на пиксель прохода: 2 / размер вида.
vec2 ndc_per_pixel;

// Обе проекции без наклона по z, поэтому точка получается из глубины по оси вида без обратной матрицы.
vec3 view_point_at(ivec2 pixel, float view_z) {
	vec2 ndc = (vec2(pixel) + 0.5) * ndc_per_pixel - 1.0;
	float clip_w = params.projection[2][3] * view_z + params.projection[3][3];
	return vec3((ndc * clip_w - params.projection[2].xy * view_z - params.projection[3].xy) * vec2(params.inverse_projection[0][0], params.inverse_projection[1][1]), view_z);
}
