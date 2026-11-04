// lestoroer#local-lights
#[vertex]

#version 450

#VERSION_DEFINES

// Перспективная глубина первой поверхности вдоль оси грани: фрагментный шейдер ничего не пишет, работает ранний тест.

layout(location = 0) in vec4 vertex_attrib;

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

#[fragment]

#version 450

#VERSION_DEFINES

void main() {
}
