// lestoroer#gi
// Alpha экранного GI хранит глубину поверхности pre-pass; другие поверхности читают мировой кеш.

// Хеширование и выборка должны совпадать с world_hash_layout/world_hash_access игры; это проверяет world_cache_fragments.gd.
layout(set = 1, binding = 37, std430) readonly buffer SurfaceCacheHashLookup { uvec4 data[]; } surface_cache_hash_lookup;
layout(set = 1, binding = 38, std430) readonly buffer SurfaceCacheHashRooms { uvec4 header; vec4 data[]; } surface_cache_hash_rooms;

layout(set = 1, binding = 41) uniform texture2D surface_cache_gather;
layout(set = 1, binding = 43) uniform texture2D surface_cache_reflection;

const float SURFACE_CACHE_HASH_CELL = 0.08;
const float SURFACE_CACHE_HASH_NEAR = 12.0;
const float SURFACE_CACHE_HASH_ROOM_PROBE = 0.05;
const uint SURFACE_CACHE_HASH_PROBES = 8u;
// Относительная погрешность глубины в half-float alpha — около 5e-4.
const float SURFACE_CACHE_DEPTH_TOLERANCE = 2e-3;

uint surface_cache_hash(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	return x ^ (x >> 16u);
}

uint surface_cache_hash_room(vec3 position) {
	for (uint room = 0u; room < surface_cache_hash_rooms.header.x; room++) {
		uint base = room * 5u;
		vec3 local = vec3(dot(surface_cache_hash_rooms.data[base].xyz, position) + surface_cache_hash_rooms.data[base].w,
				dot(surface_cache_hash_rooms.data[base + 1u].xyz, position) + surface_cache_hash_rooms.data[base + 1u].w,
				dot(surface_cache_hash_rooms.data[base + 2u].xyz, position) + surface_cache_hash_rooms.data[base + 2u].w);
		if (all(greaterThanEqual(local, surface_cache_hash_rooms.data[base + 3u].xyz)) && all(lessThanEqual(local, surface_cache_hash_rooms.data[base + 4u].xyz))) {
			return room + 1u;
		}
	}
	return 0u;
}

uvec2 surface_cache_hash_key(ivec3 cell, uint tag, uint level) {
	uvec3 wrapped = uvec3(cell + ivec3(32768)) & 65535u;
	return uvec2(wrapped.x | (wrapped.y << 16u), wrapped.z | tag | (level << 19u));
}

bool surface_cache_hash_light(uvec2 key, out vec3 light) {
	uint mask = uint(surface_cache_hash_lookup.data.length()) - 1u;
	uint fingerprint = surface_cache_hash(key.x ^ surface_cache_hash(key.y ^ 0x68bc21ebu)) | 1u;
	uint base = surface_cache_hash(key.y ^ surface_cache_hash(key.x));
	light = vec3(0.0);
	for (uint i = 0u; i < SURFACE_CACHE_HASH_PROBES; i++) {
		uvec4 record = surface_cache_hash_lookup.data[(base + i) & mask];
		if (record.x == fingerprint) {
			light = uintBitsToFloat(record.yzw);
			return true;
		}
		if (record.x == 0u) {
			return false;
		}
	}
	return false;
}

bool surface_cache_hash_smooth(vec3 position, uint axis, uint tag, uint level, out vec3 light) {
	vec3 scaled = position / (SURFACE_CACHE_HASH_CELL * float(1u << level));
	ivec3 cell = ivec3(floor(scaled));
	uint u = (axis + 1u) % 3u, v = (axis + 2u) % 3u;
	vec2 point = vec2(scaled[u], scaled[v]) - 0.5;
	ivec2 corner = ivec2(floor(point));
	vec2 fraction = point - vec2(corner);
	vec3 sum = vec3(0.0);
	float total = 0.0;
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			cell[u] = corner.x + x;
			cell[v] = corner.y + y;
			vec3 value;
			if (surface_cache_hash_light(surface_cache_hash_key(cell, tag, level), value)) {
				float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
				sum += value * weight;
				total += weight;
			}
		}
	}
	light = total > 1e-4 ? sum / total : vec3(0.0);
	return total > 1e-4;
}

vec3 surface_cache_world_irradiance(vec3 position, vec3 normal, vec3 camera) {
	vec3 magnitude = abs(normal);
	uint axis = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0u : (magnitude.y >= magnitude.z ? 1u : 2u);
	uint face = axis * 2u + (normal[axis] < 0.0 ? 1u : 0u);
	uint tag = (face << 16u) | (surface_cache_hash_room(position + normal * SURFACE_CACHE_HASH_ROOM_PROBE) << 20u);
	uint level = distance(position, camera) < SURFACE_CACHE_HASH_NEAR ? 0u : 1u;
	vec3 light;
	if (surface_cache_hash_smooth(position, axis, tag, level, light) || surface_cache_hash_smooth(position, axis, tag, 1u - level, light)) {
		return max(light, vec3(0.0));
	}
	return vec3(0.0);
}

bool surface_cache_texel_owned(float texel_depth, float fragment_depth) {
	return texel_depth > 0.0 && abs(texel_depth - fragment_depth) <= texel_depth * SURFACE_CACHE_DEPTH_TOLERANCE;
}

vec3 surface_cache_for_fragment(vec3 position, vec3 normal, vec3 camera, vec3 fragment, out bool gather_used) {
	ivec2 pixel = ivec2(fragment.xy);
	ivec2 size = textureSize(sampler2D(surface_cache_gather, SAMPLER_NEAREST_CLAMP), 0);
	gather_used = false;
	if (all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, size))) {
		vec4 gather = texelFetch(sampler2D(surface_cache_gather, SAMPLER_NEAREST_CLAMP), pixel, 0);
		if (surface_cache_texel_owned(gather.a, fragment.z)) {
			gather_used = true;
			return gather.rgb;
		}
	}
	return surface_cache_world_irradiance(position, normal, camera);
}

bool surface_cache_reflection_for_fragment(vec3 fragment, out vec3 radiance) {
	ivec2 pixel = ivec2(fragment.xy);
	ivec2 size = textureSize(sampler2D(surface_cache_reflection, SAMPLER_NEAREST_CLAMP), 0);
	radiance = vec3(0.0);
	if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size))) {
		return false;
	}
	vec4 value = texelFetch(sampler2D(surface_cache_reflection, SAMPLER_NEAREST_CLAMP), pixel, 0);
	radiance = value.rgb;
	return surface_cache_texel_owned(value.a, fragment.z);
}

