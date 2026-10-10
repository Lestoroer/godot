// Fork(Lestoroer): instance/surface/primitive address, independent of the camera.
struct SurfaceCacheTriangle { vec4 a; vec4 b; vec4 c; vec4 uv_ab; vec4 uv_c; vec4 light_uv_ab; vec4 light_uv_c; uvec4 material_tile; uvec4 lighting_tile; };
struct SurfaceCacheMaterial { vec4 position; vec4 normal; vec4 reflectance; vec4 emission; uvec4 identity; vec4 specular; vec4 transmission; };
struct SurfaceCacheMapping { uvec4 material; uvec4 lighting; };
layout(set=1,binding=37,std430) readonly buffer SurfaceCacheSurfaces { SurfaceCacheMapping data[]; } surface_cache_surfaces;
layout(set=1,binding=38,std430) readonly buffer SurfaceCacheTriangles { SurfaceCacheTriangle data[]; } surface_cache_triangles;
layout(set=1,binding=39,std430) readonly buffer SurfaceCacheMaterials { SurfaceCacheMaterial data[]; } surface_cache_materials;
layout(set=1,binding=40,std430) readonly buffer SurfaceCacheIrradiance { vec4 data[]; } surface_cache_light;

layout(set=1,binding=41) uniform texture2D surface_cache_gather;
layout(set=1,binding=44) uniform texture2D surface_cache_glass;
layout(set=1,binding=45) uniform utexture2D surface_cache_glass_primary;
layout(set=1,binding=46) uniform texture2D surface_cache_transmittance;
layout(set=1,binding=43) uniform texture2D surface_cache_reflection;
layout(set=1,binding=42) uniform utexture2D surface_cache_primary;

vec3 surface_cache_irradiance(uint surface, uint primitive, vec3 position, vec3 normal) {
	uvec4 mapping=surface_cache_surfaces.data[surface].lighting;
	SurfaceCacheTriangle triangle=surface_cache_triangles.data[mapping.x+primitive];
	vec3 ab=triangle.b.xyz-triangle.a.xyz, ac=triangle.c.xyz-triangle.a.xyz, ap=position-triangle.a.xyz;
	vec3 geometric=cross(ab,ac), absolute_normal=abs(geometric);
	uint axis=absolute_normal.x>absolute_normal.y?0u:1u;
	if(absolute_normal.z>absolute_normal[axis]) axis=2u;
	vec2 barycentric=vec2(cross(ap,ac)[axis],cross(ab,ap)[axis])/geometric[axis];
	vec2 uv=triangle.light_uv_ab.xy*(1.0-barycentric.x-barycentric.y)+triangle.light_uv_ab.zw*barycentric.x+triangle.light_uv_c.xy*barycentric.y;
	uint side=dot(normal,cross(ab,ac)*triangle.a.w)<0.0?1u:0u;
	uint chart=floatBitsToUint(triangle.light_uv_c.z);
	ivec2 size=ivec2(mapping.zw);
	vec2 pixel=uv*vec2(size)-0.5;
	ivec2 base=ivec2(floor(pixel));
	vec2 fraction=fract(pixel);
	vec3 irradiance=vec3(0.0);
	float weight_sum=0.0;
	uvec4 tile=triangle.lighting_tile;
	ivec2 tile_size=ivec2(tile.z&65535u,tile.z>>16u);
	uint offset=mapping.y+tile.w+side*uint(tile_size.x*tile_size.y);
	for (int y=0;y<2;y++) for (int x=0;x<2;x++) {
		ivec2 point=base+ivec2(x,y);
		ivec2 local=point-ivec2(tile.xy);
		if (any(lessThan(local,ivec2(0))) || any(greaterThanEqual(local,tile_size))) continue;
		uint address=offset+uint(local.y*tile_size.x+local.x);
		if (surface_cache_materials.data[address].identity.x!=chart || surface_cache_materials.data[address].identity.y!=surface || surface_cache_materials.data[address].identity.w==0u) continue;
		float weight=(x==0?1.0-fraction.x:fraction.x)*(y==0?1.0-fraction.y:fraction.y);
		irradiance+=surface_cache_light.data[address].rgb*weight;
		weight_sum+=weight;
	}
	if (weight_sum>1e-6) return max(irradiance/weight_sum,vec3(0.0));
	// A chart edge may fall between raster samples. Search only this chart/side.
	float nearest=1e30;
	bool found=false;
	for (int y=-2;y<=2;y++) for (int x=-2;x<=2;x++) {
		ivec2 point=base+ivec2(x,y);
		ivec2 local=point-ivec2(tile.xy);
		if (any(lessThan(local,ivec2(0))) || any(greaterThanEqual(local,tile_size))) continue;
		uint address=offset+uint(local.y*tile_size.x+local.x);
		if (surface_cache_materials.data[address].identity.x!=chart || surface_cache_materials.data[address].identity.y!=surface || surface_cache_materials.data[address].identity.w==0u) continue;
		float distance=dot(vec2(point)-pixel,vec2(point)-pixel);
		if (distance<nearest) { nearest=distance; irradiance=surface_cache_light.data[address].rgb; found=true; }
	}
	// Visible coverage failure, never disguised as computed darkness.
	return found?max(irradiance,vec3(0.0)):vec3(1.0,0.0,1.0);
}


vec3 surface_cache_for_fragment(uint surface, uint primitive, vec3 position, vec3 normal, ivec2 pixel, float view_depth, out bool gather_used) {
	ivec2 size = textureSize(sampler2D(surface_cache_gather, SAMPLER_NEAREST_CLAMP), 0);
	gather_used = false;
	if (all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, size))) {
		vec4 gather = texelFetch(sampler2D(surface_cache_gather, SAMPLER_NEAREST_CLAMP), pixel, 0);
		// Transparent fragments cannot borrow irradiance of opaque geometry behind them.
		if (gather.a > 0.0 && all(equal(texelFetch(usampler2D(surface_cache_primary, SAMPLER_NEAREST_CLAMP), pixel, 0).xy, uvec2(surface + 1u, primitive + 1u)))) {
			gather_used = true;
			return gather.rgb;
		}
	}
	return surface_cache_irradiance(surface, primitive, position, normal);
}

// Fully BRDF-weighted radiance; never apply the environment DFG a second time.
bool surface_cache_reflection_for_fragment(uint surface, uint primitive, ivec2 pixel, out vec3 radiance) {
	ivec2 size=textureSize(sampler2D(surface_cache_reflection,SAMPLER_NEAREST_CLAMP),0);
	if(any(lessThan(pixel,ivec2(0))) || any(greaterThanEqual(pixel,size)))return false;
	if(any(notEqual(texelFetch(usampler2D(surface_cache_primary,SAMPLER_NEAREST_CLAMP),pixel,0).xy,uvec2(surface,primitive+1u))))return false;
	vec4 value=texelFetch(sampler2D(surface_cache_reflection,SAMPLER_NEAREST_CLAMP),pixel,0);
	radiance=value.rgb;
	return value.a>0.0;
}
