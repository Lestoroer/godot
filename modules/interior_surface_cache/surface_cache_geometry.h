// Fork(Lestoroer): Interior Star Surface Cache runtime.
#pragma once

#include "core/object/object.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server_types.h"

// Preparation of a mesh for the Surface Cache: the material chart, its capture mesh and the ray
// triangle records (CacheTriangle of the game's Surface Cache cache_types). Every surface has one
// side (front only) or two; the caller takes them from its materials.
class SurfaceCacheGeometry : public Object {
	GDCLASS(SurfaceCacheGeometry, Object);

	static Dictionary _prepare(const Mesh *p_resource, RID p_mesh, float p_texel_size, const Vector<int32_t> &p_surface_sides, const Vector<RenderingServerTypes::SurfaceData> *p_source);

protected:
	static void _bind_methods();

public:
	static Dictionary prepare(const Ref<Mesh> &p_mesh, float p_texel_size, const Vector<int32_t> &p_surface_sides);
	static Dictionary prepare_from_rid(RID p_mesh, float p_texel_size, const Vector<int32_t> &p_surface_sides);
	// The world's frame-local readback batch.
	static Dictionary prepare_from_surfaces(RID p_mesh, const Vector<RenderingServerTypes::SurfaceData> &p_source, float p_texel_size, const Vector<int32_t> &p_surface_sides);
};
