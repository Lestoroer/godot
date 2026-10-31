// Fork(Lestoroer): Interior Star Surface Cache runtime.
#include "surface_cache_geometry.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

void SurfaceCacheGeometry::_bind_methods() {
	ClassDB::bind_static_method("SurfaceCacheGeometry", D_METHOD("prepare", "mesh", "surface_sides"), &SurfaceCacheGeometry::prepare);
	ClassDB::bind_static_method("SurfaceCacheGeometry", D_METHOD("prepare_from_rid", "mesh", "surface_sides"), &SurfaceCacheGeometry::prepare_from_rid);
}

// Material capture of a surface: one SURFACE_CAPTURE_SIZE square of the instance atlas. A triangle is
// projected along its dominant normal axis and scaled to the square by the bounds of its surface; the
// capture averages the square into the surface material (material_resolve of the game). One empty texel
// separates squares: the capture widens a triangle by one pixel for conservative raster.
static constexpr int SURFACE_CAPTURE_SIZE = 16;
static constexpr int SURFACE_CAPTURE_BORDER = 1;
static constexpr int SURFACE_CAPTURE_CELL = SURFACE_CAPTURE_SIZE + 2 * SURFACE_CAPTURE_BORDER;

// Surface Cache capture triangles own their three corners. Each vertex stream is copied element by
// element to the corners in its stored encoding, compression included, so the capture renders
// exactly the source attributes.
static void _remap_stream(const uint8_t *p_source, uint32_t p_element_size, const Vector<int> &p_remap, uint8_t *r_target) {
	const int *remap = p_remap.ptr();
	for (int corner = 0; corner < p_remap.size(); corner++) {
		memcpy(r_target + size_t(corner) * p_element_size, p_source + size_t(remap[corner]) * p_element_size, p_element_size);
	}
}

// Positions followed by normals and tangents, as Godot stores vertex_data and every blend shape.
static Vector<uint8_t> _remap_vertices(const uint8_t *p_source, uint32_t p_vertex_size, uint32_t p_normal_size, uint32_t p_vertex_count, const Vector<int> &p_remap) {
	Vector<uint8_t> result;
	result.resize(p_remap.size() * (p_vertex_size + p_normal_size));
	_remap_stream(p_source, p_vertex_size, p_remap, result.ptrw());
	if (p_normal_size > 0) {
		_remap_stream(p_source + size_t(p_vertex_size) * p_vertex_count, p_normal_size, p_remap, result.ptrw() + size_t(p_vertex_size) * p_remap.size());
	}
	return result;
}

Dictionary SurfaceCacheGeometry::prepare(const Ref<Mesh> &p_mesh, const Vector<int32_t> &p_surface_sides) {
	ERR_FAIL_COND_V(p_mesh.is_null(), Dictionary());
	return _prepare(p_mesh.ptr(), RID(), p_surface_sides, nullptr);
}

Dictionary SurfaceCacheGeometry::prepare_from_rid(RID p_mesh, const Vector<int32_t> &p_surface_sides) {
	ERR_FAIL_COND_V(!p_mesh.is_valid(), Dictionary());
	return _prepare(nullptr, p_mesh, p_surface_sides, nullptr);
}

Dictionary SurfaceCacheGeometry::prepare_from_surfaces(RID p_mesh, const Vector<RenderingServerTypes::SurfaceData> &p_source, const Vector<int32_t> &p_surface_sides) {
	return _prepare(nullptr, p_mesh, p_surface_sides, &p_source);
}

Dictionary SurfaceCacheGeometry::_prepare(const Mesh *p_resource, RID p_mesh, const Vector<int32_t> &p_surface_sides, const Vector<RenderingServerTypes::SurfaceData> *p_source) {
	uint64_t stage_start = OS::get_singleton()->get_ticks_usec();
	Dictionary operation_ms;
	static constexpr int TRIANGLE_BYTES = 48;
	RenderingServer *server = RS::get_singleton();
	const int surface_count = p_resource ? p_resource->get_surface_count() : server->mesh_get_surface_count(p_mesh);
	ERR_FAIL_COND_V_MSG(surface_count == 0, Dictionary(), "Surface Cache: mesh has no surfaces.");
	ERR_FAIL_COND_V_MSG(p_surface_sides.size() != surface_count, Dictionary(), "Surface Cache: sides are required for every surface.");
	ERR_FAIL_COND_V(p_source && p_source->size() != surface_count, Dictionary());
	const RID mesh_rid = p_resource ? p_resource->get_rid() : p_mesh;
	const int shape_count = server->mesh_get_blend_shape_count(mesh_rid);
	// Layout and ray records decode only positions, normals and indices; the capture copies every
	// stream as stored.
	const uint64_t attribute_formats = RSE::ARRAY_FORMAT_COLOR | RSE::ARRAY_FORMAT_TEX_UV | RSE::ARRAY_FORMAT_TEX_UV2 | RSE::ARRAY_FORMAT_CUSTOM0 | RSE::ARRAY_FORMAT_CUSTOM1 | RSE::ARRAY_FORMAT_CUSTOM2 | RSE::ARRAY_FORMAT_CUSTOM3 | RSE::ARRAY_FORMAT_BONES | RSE::ARRAY_FORMAT_WEIGHTS;
	LocalVector<RenderingServerTypes::SurfaceData> sources;
	LocalVector<Vector<Vector3>> positions;
	LocalVector<Vector<Vector3>> normals;
	LocalVector<Vector<int>> indices;
	sources.resize(surface_count);
	positions.resize(surface_count);
	normals.resize(surface_count);
	indices.resize(surface_count);
	for (int surface = 0; surface < surface_count; ++surface) {
		sources[surface] = p_source ? (*p_source)[surface] : server->mesh_get_surface(mesh_rid, surface);
		const RenderingServerTypes::SurfaceData &data = sources[surface];
		ERR_FAIL_COND_V(data.primitive != RSE::PRIMITIVE_TRIANGLES, Dictionary());
		RenderingServerTypes::SurfaceData geometry;
		geometry.format = data.format & ~attribute_formats;
		geometry.vertex_data = data.vertex_data;
		geometry.vertex_count = data.vertex_count;
		geometry.index_data = data.index_data;
		geometry.index_count = data.index_count;
		geometry.aabb = data.aabb;
		const Array arrays = server->mesh_create_arrays_from_surface_data(geometry);
		positions[surface] = arrays[Mesh::ARRAY_VERTEX];
		normals[surface] = arrays[Mesh::ARRAY_NORMAL];
		indices[surface] = arrays[Mesh::ARRAY_INDEX];
		ERR_FAIL_COND_V(positions[surface].is_empty(), Dictionary());
		if (indices[surface].is_empty()) {
			indices[surface].resize(positions[surface].size());
			int *index = indices[surface].ptrw();
			for (int i = 0; i < positions[surface].size(); i++) {
				index[i] = i;
			}
		}
		ERR_FAIL_COND_V(indices[surface].size() % 3 != 0, Dictionary());
	}
	operation_ms["mesh_read"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

	// Surface squares fill the rows of a near-square instance atlas.
	stage_start = OS::get_singleton()->get_ticks_usec();
	const int columns = int(Math::ceil(Math::sqrt(double(surface_count))));
	const Size2i atlas_size(columns * SURFACE_CAPTURE_CELL, ((surface_count + columns - 1) / columns) * SURFACE_CAPTURE_CELL);
	operation_ms["mesh_layout"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

	// Ray records and capture corners in one pass over the triangles.
	stage_start = OS::get_singleton()->get_ticks_usec();
	Array triangle_data;
	Array vertex_data;
	Array index_data;
	Array result_surfaces;
	// Capture corners of every triangle, surfaces in order: the capture reads them by triangle.
	int corner_total = 0;
	for (int surface = 0; surface < surface_count; surface++) {
		corner_total += indices[surface].size();
	}
	Vector<Vector2> capture_corners;
	capture_corners.resize_uninitialized(corner_total);
	Vector2 *pixel_write = capture_corners.ptrw();
	for (int surface = 0; surface < surface_count; surface++) {
		const int sides = p_surface_sides[surface];
		ERR_FAIL_COND_V_MSG(sides != 1 && sides != 2, Dictionary(), "Surface Cache: a surface has one or two sides.");
		const Vector3 *position = positions[surface].ptr();
		const Vector3 *normal = normals[surface].is_empty() ? nullptr : normals[surface].ptr();
		const int *index = indices[surface].ptr();
		const int corner_count = indices[surface].size();
		const Vector2i offset((surface % columns) * SURFACE_CAPTURE_CELL + SURFACE_CAPTURE_BORDER, (surface / columns) * SURFACE_CAPTURE_CELL + SURFACE_CAPTURE_BORDER);
		AABB bounds(position[0], Vector3());
		for (int i = 1; i < positions[surface].size(); i++) {
			bounds.expand_to(position[i]);
		}
		Vector<uint8_t> packed;
		packed.resize(corner_count / 3 * TRIANGLE_BYTES);
		uint8_t *write = packed.ptrw();
		for (int corner = 0; corner < corner_count; corner += 3) {
			const int triangle = corner / 3;
			Vector3 vertex[3];
			for (int c = 0; c < 3; c++) {
				vertex[c] = position[index[corner + c]];
			}
			const Vector3 face = (vertex[1] - vertex[0]).cross(vertex[2] - vertex[0]);
			const int axis = face.abs().max_axis_index();
			const int u = (axis + 1) % 3, v = (axis + 2) % 3;
			// CacheTriangle of the game: local corners and the orientation; the geometry pass makes the
			// world record (cache_types).
			float record[TRIANGLE_BYTES / 4] = {};
			for (int c = 0; c < 3; c++) {
				memcpy(&record[c * 4], &vertex[c], 12);
				const Vector2 square((vertex[c][u] - bounds.position[u]) / MAX(bounds.size[u], real_t(1e-6)), (vertex[c][v] - bounds.position[v]) / MAX(bounds.size[v], real_t(1e-6)));
				pixel_write[corner + c] = Vector2(offset) + square * SURFACE_CAPTURE_SIZE;
			}
			// Godot's authored winding is clockwise; normals establish the semantic front.
			float orientation = normal ? SIGN(normal[index[corner]].dot(face)) : -1.0f;
			if (orientation == 0.0f) {
				orientation = -1.0f;
			}
			record[11] = orientation;
			memcpy(write + triangle * TRIANGLE_BYTES, record, TRIANGLE_BYTES);
		}
		pixel_write += corner_count;
		triangle_data.push_back(packed);
		// Ray geometry: the source vertices and triangles, in the order of the records.
		Vector<uint8_t> ray_vertices;
		ray_vertices.resize(positions[surface].size() * 12);
		memcpy(ray_vertices.ptrw(), positions[surface].ptr(), ray_vertices.size());
		vertex_data.push_back(ray_vertices);
		Vector<uint8_t> ray_indices;
		ray_indices.resize(corner_count * 4);
		memcpy(ray_indices.ptrw(), index, ray_indices.size());
		index_data.push_back(ray_indices);
		Dictionary result_surface;
		result_surface["offset"] = offset;
		result_surfaces.push_back(result_surface);
	}
	operation_ms["mesh_pack"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

	// Capture mesh: the triangle corners take the source vertex streams through the index buffer.
	stage_start = OS::get_singleton()->get_ticks_usec();
	Ref<ArrayMesh> capture_mesh;
	capture_mesh.instantiate();
	const ArrayMesh *array_mesh = Object::cast_to<ArrayMesh>(p_resource);
	for (int shape = 0; shape < shape_count; ++shape) {
		capture_mesh->add_blend_shape(array_mesh ? array_mesh->get_blend_shape_name(shape) : StringName(vformat("shape_%d", shape)));
	}
	capture_mesh->set_blend_shape_mode(Mesh::BlendShapeMode(server->mesh_get_blend_shape_mode(mesh_rid)));
	for (int surface = 0; surface < surface_count; surface++) {
		const RenderingServerTypes::SurfaceData &source = sources[surface];
		const Vector<int> &remap = indices[surface];
		const int corner_count = remap.size();
		uint32_t offsets[RSE::ARRAY_MAX];
		uint32_t vertex_size, normal_size, attribute_size, skin_size;
		server->mesh_surface_make_offsets_from_format(source.format, source.vertex_count, source.index_count, offsets, vertex_size, normal_size, attribute_size, skin_size);
		Vector<uint8_t> attributes;
		attributes.resize(corner_count * attribute_size);
		if (attribute_size > 0) {
			_remap_stream(source.attribute_data.ptr(), attribute_size, remap, attributes.ptrw());
		}
		Vector<uint8_t> skin;
		skin.resize(corner_count * skin_size);
		if (skin_size > 0) {
			_remap_stream(source.skin_data.ptr(), skin_size, remap, skin.ptrw());
		}
		Vector<uint8_t> shapes;
		if (shape_count > 0) {
			uint32_t shape_vertex_size, shape_normal_size, shape_attribute_size, shape_skin_size;
			server->mesh_surface_make_offsets_from_format(source.format & RSE::ARRAY_FORMAT_BLEND_SHAPE_MASK, source.vertex_count, 0, offsets, shape_vertex_size, shape_normal_size, shape_attribute_size, shape_skin_size);
			const size_t shape_bytes = size_t(shape_vertex_size + shape_normal_size) * source.vertex_count;
			ERR_FAIL_COND_V(size_t(source.blend_shape_data.size()) != shape_bytes * shape_count, Dictionary());
			for (int shape = 0; shape < shape_count; ++shape) {
				shapes.append_array(_remap_vertices(source.blend_shape_data.ptr() + shape_bytes * shape, shape_vertex_size, shape_normal_size, source.vertex_count, remap));
			}
		}
		// Every triangle owns its corners; an identity index buffer is redundant.
		capture_mesh->add_surface(source.format & ~uint64_t(RSE::ARRAY_FORMAT_INDEX), Mesh::PRIMITIVE_TRIANGLES,
				_remap_vertices(source.vertex_data.ptr(), vertex_size, normal_size, source.vertex_count, remap),
				attributes, skin, corner_count, Vector<uint8_t>(), 0, source.aabb, shapes, source.bone_aabbs,
				Vector<RenderingServerTypes::SurfaceData::LOD>(), source.uv_scale);
		if (p_resource) {
			capture_mesh->surface_set_material(surface, p_resource->surface_get_material(surface));
		}
	}
	server->mesh_set_surface_cache_corners(capture_mesh->get_rid(), capture_corners);
	operation_ms["capture_mesh"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

	Dictionary layout;
	layout["size"] = Vector2i(atlas_size);
	layout["capture_size"] = SURFACE_CAPTURE_SIZE;
	layout["surfaces"] = result_surfaces;
	Dictionary result;
	result["operation_ms"] = operation_ms;
	result["layout"] = layout;
	result["mesh"] = capture_mesh;
	result["triangle_data"] = triangle_data;
	result["vertex_data"] = vertex_data;
	result["index_data"] = index_data;
	return result;
}
