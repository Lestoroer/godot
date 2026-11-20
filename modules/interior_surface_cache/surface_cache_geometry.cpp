// lestoroer#gi
#include "surface_cache_geometry.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

void SurfaceCacheGeometry::_bind_methods() {
	ClassDB::bind_static_method("SurfaceCacheGeometry", D_METHOD("prepare", "mesh", "surface_sides"), &SurfaceCacheGeometry::prepare);
	ClassDB::bind_static_method("SurfaceCacheGeometry", D_METHOD("prepare_from_rid", "mesh", "surface_sides"), &SurfaceCacheGeometry::prepare_from_rid);
}

static constexpr int SURFACE_CAPTURE_SIZE = 16;
static constexpr int SURFACE_CAPTURE_SAMPLES = SURFACE_CAPTURE_SIZE * SURFACE_CAPTURE_SIZE;

// Потоки копируются в исходной кодировке, включая сжатие, чтобы захват сохранил точные атрибуты.
static void _remap_stream(const uint8_t *p_source, uint32_t p_element_size, const Vector<int> &p_remap, uint8_t *r_target) {
	const int *remap = p_remap.ptr();
	for (int corner = 0; corner < p_remap.size(); corner++) {
		memcpy(r_target + size_t(corner) * p_element_size, p_source + size_t(remap[corner]) * p_element_size, p_element_size);
	}
}

// В vertex_data и blend shapes позиции предшествуют нормалям и тангентам.
static Vector<uint8_t> _remap_vertices(const uint8_t *p_source, uint32_t p_vertex_size, uint32_t p_normal_size, uint32_t p_vertex_count, const Vector<int> &p_remap) {
	Vector<uint8_t> result;
	result.resize(p_remap.size() * (p_vertex_size + p_normal_size));
	_remap_stream(p_source, p_vertex_size, p_remap, result.ptrw());
	if (p_normal_size > 0) {
		_remap_stream(p_source + size_t(p_vertex_size) * p_vertex_count, p_normal_size, p_remap, result.ptrw() + size_t(p_vertex_size) * p_remap.size());
	}
	return result;
}

// Позиции вершин поверхности тройками float — вход BLAS. Несжатые лежат в потоке вершин как есть, сжатые
// (ARRAY_FLAG_COMPRESS_ATTRIBUTES) — долями рамки поверхности в 16 битах; разбор — тот же, что в RenderingServer.
static Vector<uint8_t> _ray_vertices(const RenderingServerTypes::SurfaceData &p_data, uint32_t p_vertex_size) {
	Vector<uint8_t> result;
	result.resize_uninitialized(size_t(p_data.vertex_count) * sizeof(Vector3));
	const uint8_t *source = p_data.vertex_data.ptr();
	Vector3 *write = reinterpret_cast<Vector3 *>(result.ptrw());
	if (!(p_data.format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES)) {
		for (uint32_t i = 0; i < p_data.vertex_count; i++) {
			memcpy(&write[i], source + size_t(i) * p_vertex_size, sizeof(Vector3));
		}
		return result;
	}
	for (uint32_t i = 0; i < p_data.vertex_count; i++) {
		const uint16_t *v = reinterpret_cast<const uint16_t *>(source + size_t(i) * p_vertex_size);
		const Vector3 share(float(v[0]) / 65535.0, float(v[1]) / 65535.0, float(v[2]) / 65535.0);
		write[i] = (share * p_data.aabb.size) + p_data.aabb.position;
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
	RenderingServer *server = RS::get_singleton();
	const int surface_count = p_resource ? p_resource->get_surface_count() : server->mesh_get_surface_count(p_mesh);
	ERR_FAIL_COND_V_MSG(surface_count == 0, Dictionary(), "Surface Cache: mesh has no surfaces.");
	ERR_FAIL_COND_V_MSG(p_surface_sides.size() != surface_count, Dictionary(), "Surface Cache: sides are required for every surface.");
	ERR_FAIL_COND_V(p_source && p_source->size() != surface_count, Dictionary());
	const RID mesh_rid = p_resource ? p_resource->get_rid() : p_mesh;
	const int shape_count = server->mesh_get_blend_shape_count(mesh_rid);
	LocalVector<RenderingServerTypes::SurfaceData> sources;
	LocalVector<Vector<uint8_t>> positions;
	LocalVector<Vector<uint8_t>> indices;
	LocalVector<int> index_sizes;
	sources.resize(surface_count);
	positions.resize(surface_count);
	indices.resize(surface_count);
	index_sizes.resize(surface_count);
	for (int surface = 0; surface < surface_count; ++surface) {
		sources[surface] = p_source ? (*p_source)[surface] : server->mesh_get_surface(mesh_rid, surface);
		const RenderingServerTypes::SurfaceData &data = sources[surface];
		ERR_FAIL_COND_V(data.primitive != RSE::PRIMITIVE_TRIANGLES || data.vertex_count == 0 || (data.format & RSE::ARRAY_FLAG_USE_2D_VERTICES), Dictionary());
		uint32_t offsets[RSE::ARRAY_MAX];
		uint32_t vertex_size, normal_size, attribute_size, skin_size;
		server->mesh_surface_make_offsets_from_format(data.format, data.vertex_count, data.index_count, offsets, vertex_size, normal_size, attribute_size, skin_size);
		ERR_FAIL_COND_V(size_t(data.vertex_data.size()) < size_t(data.vertex_count) * vertex_size, Dictionary());
		positions[surface] = _ray_vertices(data, vertex_size);
		if (data.index_count > 0) {
			indices[surface] = data.index_data;
			index_sizes[surface] = data.index_data.size() / data.index_count;
			ERR_FAIL_COND_V(index_sizes[surface] != 2 && index_sizes[surface] != 4, Dictionary());
		} else {
			indices[surface].resize_uninitialized(size_t(data.vertex_count) * sizeof(uint32_t));
			uint32_t *index = reinterpret_cast<uint32_t *>(indices[surface].ptrw());
			for (uint32_t i = 0; i < data.vertex_count; i++) {
				index[i] = i;
			}
			index_sizes[surface] = sizeof(uint32_t);
		}
		ERR_FAIL_COND_V((indices[surface].size() / index_sizes[surface]) % 3 != 0, Dictionary());
	}
	operation_ms["mesh_read"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

	stage_start = OS::get_singleton()->get_ticks_usec();
	const int columns = int(Math::ceil(Math::sqrt(double(surface_count))));
	const Size2i atlas_size(columns * SURFACE_CAPTURE_SIZE, ((surface_count + columns - 1) / columns) * SURFACE_CAPTURE_SIZE);
	operation_ms["mesh_layout"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

	stage_start = OS::get_singleton()->get_ticks_usec();
	Array vertex_data;
	Array index_data;
	Array result_surfaces;
	// 256 взвешенных по площади выборок на поверхность: по одному неперекрывающемуся треугольнику на пиксель
	// с заданными барицентрическими координатами ровно в центре пикселя.
	LocalVector<Vector<int>> capture_remaps;
	capture_remaps.resize(surface_count);
	const int capture_corner_total = surface_count * SURFACE_CAPTURE_SAMPLES * 3;
	Vector<Vector2> capture_corners;
	capture_corners.resize_uninitialized(capture_corner_total);
	Vector2 *pixel_write = capture_corners.ptrw();
	LocalVector<double> area_cdf;
	for (int surface = 0; surface < surface_count; surface++) {
		const int sides = p_surface_sides[surface];
		ERR_FAIL_COND_V_MSG(sides != 1 && sides != 2, Dictionary(), "Surface Cache: a surface has one or two sides.");
		const Vector3 *position = reinterpret_cast<const Vector3 *>(positions[surface].ptr());
		const uint16_t *narrow = index_sizes[surface] == 2 ? reinterpret_cast<const uint16_t *>(indices[surface].ptr()) : nullptr;
		const uint32_t *wide = reinterpret_cast<const uint32_t *>(indices[surface].ptr());
		const int triangle_count = (indices[surface].size() / index_sizes[surface]) / 3;
		ERR_FAIL_COND_V(triangle_count <= 0, Dictionary());
		area_cdf.resize(triangle_count);
		double total_area = 0.0;
		for (int t = 0; t < triangle_count; t++) {
			const int c0 = t * 3;
			const int i0 = narrow ? int(narrow[c0]) : int(wide[c0]);
			const int i1 = narrow ? int(narrow[c0 + 1]) : int(wide[c0 + 1]);
			const int i2 = narrow ? int(narrow[c0 + 2]) : int(wide[c0 + 2]);
			total_area += 0.5 * double((position[i1] - position[i0]).cross(position[i2] - position[i0]).length());
			area_cdf[t] = total_area;
		}
		const Vector2i offset((surface % columns) * SURFACE_CAPTURE_SIZE, (surface / columns) * SURFACE_CAPTURE_SIZE);
		capture_remaps[surface].resize(SURFACE_CAPTURE_SAMPLES * 3);
		int *capture_remap = capture_remaps[surface].ptrw();
		constexpr float HALF_SPAN = 0.45f;
		for (int sample = 0; sample < SURFACE_CAPTURE_SAMPLES; sample++) {
			int tri = 0;
			if (total_area > 0.0) {
				const double target = ((double(sample) + 0.5) / double(SURFACE_CAPTURE_SAMPLES)) * total_area;
				int lo = 0, hi = triangle_count - 1;
				while (lo < hi) {
					const int mid = (lo + hi) >> 1;
					if (area_cdf[mid] < target) {
						lo = mid + 1;
					} else {
						hi = mid;
					}
				}
				tri = lo;
			} else {
				tri = int(int64_t(sample) * triangle_count / SURFACE_CAPTURE_SAMPLES);
			}
			const int corner = tri * 3;
			for (int c = 0; c < 3; c++) {
				capture_remap[sample * 3 + c] = narrow ? int(narrow[corner + c]) : int(wide[corner + c]);
			}
			double u = Math::fposmod((double(sample) + 0.5) * 0.7548776662466927, 1.0);
			double v = Math::fposmod((double(sample) + 0.5) * 0.5698402909980532, 1.0);
			if (u + v > 1.0) {
				u = 1.0 - u;
				v = 1.0 - v;
			}
			const Vector2 shift(float(0.02 + 0.94 * u), float(0.02 + 0.94 * v));
			const Vector2 center(float(offset.x + (sample % SURFACE_CAPTURE_SIZE)) + 0.5f, float(offset.y + (sample / SURFACE_CAPTURE_SIZE)) + 0.5f);
			pixel_write[sample * 3 + 0] = center + (Vector2(0.0f, 0.0f) - shift) * HALF_SPAN;
			pixel_write[sample * 3 + 1] = center + (Vector2(1.0f, 0.0f) - shift) * HALF_SPAN;
			pixel_write[sample * 3 + 2] = center + (Vector2(0.0f, 1.0f) - shift) * HALF_SPAN;
		}
		pixel_write += SURFACE_CAPTURE_SAMPLES * 3;
		vertex_data.push_back(positions[surface]);
		index_data.push_back(indices[surface]);
		Dictionary result_surface;
		result_surface["offset"] = offset;
		result_surface["index_size"] = index_sizes[surface];
		result_surfaces.push_back(result_surface);
	}
	operation_ms["mesh_pack"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

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
		const Vector<int> &remap = capture_remaps[surface];
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
	result["vertex_data"] = vertex_data;
	result["index_data"] = index_data;
	return result;
}
