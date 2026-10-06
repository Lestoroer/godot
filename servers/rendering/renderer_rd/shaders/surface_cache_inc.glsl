// Fork(Lestoroer): instance/surface/primitive address, independent of the camera.
struct SurfaceCacheTriangle { vec4 a; vec4 b; vec4 c; vec4 uv_ab; vec4 uv_c; };
struct SurfaceCacheMaterial { vec4 position; vec4 normal; vec4 reflectance; vec4 emission; uvec4 identity; };
layout(set=1,binding=37,std430) readonly buffer SurfaceCacheSurfaces { uvec4 data[]; } surface_cache_surfaces;
layout(set=1,binding=38,std430) readonly buffer SurfaceCacheTriangles { SurfaceCacheTriangle data[]; } surface_cache_triangles;
layout(set=1,binding=39,std430) readonly buffer SurfaceCacheMaterials { SurfaceCacheMaterial data[]; } surface_cache_materials;
layout(set=1,binding=40,std430) readonly buffer SurfaceCacheIrradiance { vec4 data[]; } surface_cache_light;

vec3 surface_cache_irradiance(uint surface, uint primitive, vec3 position, vec3 normal) {
    uvec4 mapping=surface_cache_surfaces.data[surface];
    SurfaceCacheTriangle triangle=surface_cache_triangles.data[mapping.x+primitive];
    vec3 ab=triangle.b.xyz-triangle.a.xyz, ac=triangle.c.xyz-triangle.a.xyz, ap=position-triangle.a.xyz;
    float aa=dot(ab,ab), cc=dot(ac,ac), cross_term=dot(ab,ac);
    float determinant=max(aa*cc-cross_term*cross_term,1e-30);
    vec2 barycentric=vec2(cc*dot(ap,ab)-cross_term*dot(ap,ac),aa*dot(ap,ac)-cross_term*dot(ap,ab))/determinant;
    vec2 uv=triangle.uv_ab.xy*(1.0-barycentric.x-barycentric.y)+triangle.uv_ab.zw*barycentric.x+triangle.uv_c.xy*barycentric.y;
    uint side=dot(normal,cross(ab,ac)*triangle.a.w)<0.0?1u:0u;
    uint chart=floatBitsToUint(triangle.uv_c.z);
    ivec2 size=ivec2(mapping.zw);
    vec2 pixel=uv*vec2(size)-0.5;
    ivec2 base=ivec2(floor(pixel));
    vec2 fraction=fract(pixel);
    vec3 irradiance=vec3(0.0);
    float weight_sum=0.0;
    uint offset=mapping.y+side*mapping.z*mapping.w;
    for (int y=0;y<2;y++) for (int x=0;x<2;x++) {
        ivec2 point=base+ivec2(x,y);
        if (any(lessThan(point,ivec2(0))) || any(greaterThanEqual(point,size))) continue;
        uint address=offset+uint(point.y)*mapping.z+uint(point.x);
        if (surface_cache_materials.data[address].identity.x!=chart || surface_cache_materials.data[address].identity.w==0u) continue;
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
        if (any(lessThan(point,ivec2(0))) || any(greaterThanEqual(point,size))) continue;
        uint address=offset+uint(point.y)*mapping.z+uint(point.x);
        if (surface_cache_materials.data[address].identity.x!=chart || surface_cache_materials.data[address].identity.w==0u) continue;
        float distance=dot(vec2(point)-pixel,vec2(point)-pixel);
        if (distance<nearest) { nearest=distance; irradiance=surface_cache_light.data[address].rgb; found=true; }
    }
    // Visible coverage failure, never disguised as computed darkness.
    return found?max(irradiance,vec3(0.0)):vec3(1.0,0.0,1.0);
}
