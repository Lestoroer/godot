// lestoroer#local-lights
#[vertex]

#version 450

#VERSION_DEFINES

// Перспективная глубина первой поверхности вдоль оси грани. Без выреза фрагментный шейдер ничего не пишет и работает ранний тест.

layout(location = 0) in vec4 vertex_attrib;

#ifdef MAP_CUTOUT

layout(location = 3) in vec4 color_attrib;
layout(location = 4) in vec2 uv_attrib;

layout(location = 0) out vec2 uv_interp;
layout(location = 1) out float alpha_interp;
layout(location = 2) flat out float threshold_interp;

// Те же 128 байт, что без выреза: преобразование короче, освободившееся занято вырезом.
layout(push_constant, std430) uniform Params {
	mat3x4 to_light;
	// xy — тангенс половины угла снимка, z — порог выреза, w — альфа материала.
	vec4 projection_and_cut;
	// xy — глубина растра a и b, zw — распаковка сжатой развёртки, ноль — развёртка не сжата.
	vec4 extra;
	// xyz — распаковка сжатых позиций меша, w больше нуля — альфа вершины входит в вырез.
	vec4 decode_position;
	vec4 decode_size;
	// Повтор и сдвиг развёртки из материала.
	vec4 uv_transform;
} params;

void main() {
	vec3 position = vec4(params.decode_position.xyz + vertex_attrib.xyz * params.decode_size.xyz, 1.0) * params.to_light;
	gl_Position = vec4(position.x / params.projection_and_cut.x, position.y / params.projection_and_cut.y, params.extra.x * position.z + params.extra.y, position.z);
	vec2 uv = params.extra.zw != vec2(0.0) ? (uv_attrib - 0.5) * params.extra.zw : uv_attrib;
	uv_interp = uv * params.uv_transform.xy + params.uv_transform.zw;
	alpha_interp = params.projection_and_cut.w * (params.decode_position.w > 0.0 ? color_attrib.a : 1.0);
	threshold_interp = params.projection_and_cut.z;
}

#else

layout(push_constant, std430) uniform Params {
	mat4 to_light;
	// Тангенс половины угла снимка по x и y.
	vec4 projection;
	// Глубина растра a и b.
	vec4 extra;
	// Распаковка сжатых позиций меша.
	vec4 decode_position;
	vec4 decode_size;
} params;

void main() {
	vec4 position = params.to_light * vec4(params.decode_position.xyz + vertex_attrib.xyz * params.decode_size.xyz, 1.0);
	gl_Position = vec4(position.x / params.projection.x, position.y / params.projection.y, params.extra.x * position.z + params.extra.y, position.z);
}

#endif

#[fragment]

#version 450

#VERSION_DEFINES

#ifdef MAP_CUTOUT

layout(location = 0) in vec2 uv_interp;
layout(location = 1) in float alpha_interp;
layout(location = 2) flat in float threshold_interp;

layout(set = 0, binding = 0) uniform sampler2D alpha_texture;

void main() {
	if (alpha_interp * texture(alpha_texture, uv_interp).a < threshold_interp) {
		discard;
	}
}

#else

void main() {
}

#endif
