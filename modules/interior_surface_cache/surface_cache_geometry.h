// lestoroer#gi
#pragma once

#include "core/object/object.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server_types.h"

class SurfaceCacheGeometry : public Object {
	GDCLASS(SurfaceCacheGeometry, Object);

	static Dictionary _prepare(const Mesh *p_resource, RID p_mesh, const Vector<int32_t> &p_surface_sides, const Vector<RenderingServerTypes::SurfaceData> *p_source);

protected:
	static void _bind_methods();

public:
	static Dictionary prepare(const Ref<Mesh> &p_mesh, const Vector<int32_t> &p_surface_sides);
	static Dictionary prepare_from_rid(RID p_mesh, const Vector<int32_t> &p_surface_sides);
	static Dictionary prepare_from_surfaces(RID p_mesh, const Vector<RenderingServerTypes::SurfaceData> &p_source, const Vector<int32_t> &p_surface_sides);
};
