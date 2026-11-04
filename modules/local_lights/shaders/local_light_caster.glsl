// lestoroer#local-lights
#[vertex]

#version 450

#VERSION_DEFINES

// Глубина первой поверхности от центра лампы: расстояние вдоль луча, деленное на радиус лампы.

layout(location = 0) in vec4 vertex_attrib;

layout(push_constant, std430) uniform Params {
	mat4 to_light;
	// Тангенс половины угла снимка по x и y.
	vec4 projection;
	// Глубина растра a и b, затем 1/радиус.
	vec4 extra;
	// Распаковка сжатых позиций меша.
	vec4 decode_position;
	vec4 decode_size;
} params;

layout(location = 0) out vec3 light_space_position;

void main() {
	vec4 position = params.to_light * vec4(params.decode_position.xyz + vertex_attrib.xyz * params.decode_size.xyz, 1.0);
	light_space_position = position.xyz;
	gl_Position = vec4(position.x / params.projection.x, position.y / params.projection.y, params.extra.x * position.z + params.extra.y, position.z);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec3 light_space_position;

layout(push_constant, std430) uniform Params {
	mat4 to_light;
	vec4 projection;
	vec4 extra;
	vec4 decode_position;
	vec4 decode_size;
} params;

void main() {
	gl_FragDepth = clamp(length(light_space_position) * params.extra.z, 0.0, 1.0);
}
