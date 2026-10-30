// Fork(Lestoroer): Interior Star Surface Cache runtime.
#include "surface_cache_geometry.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/rendering_server.h"

void SurfaceCacheGeometry::_bind_methods() {
	ClassDB::bind_static_method("SurfaceCacheGeometry", D_METHOD("prepare", "mesh", "texel_size", "surface_sides"), &SurfaceCacheGeometry::prepare);
	ClassDB::bind_static_method("SurfaceCacheGeometry", D_METHOD("prepare_from_rid", "mesh", "texel_size", "surface_sides"), &SurfaceCacheGeometry::prepare_from_rid);
}

// Empty texels around every chart. The capture widens a triangle by one pixel for conservative
// raster and keeps only fragments in the triangle's own cells, so one texel separates charts.
static constexpr int CHART_BORDER = 1;

// Shelf packing: charts sorted by height, tallest first, fill the rows of a near-square atlas in one
// pass. The charts of a dense mesh are mostly equal single-cell charts, which pack without gaps.
static void _pack_charts(const Vector<Size2i> &p_charts, Vector<Point2i> &r_offsets, Size2i &r_size) {
	int64_t area = 0;
	int widest = 0, tallest = 0;
	for (const Size2i &chart : p_charts) {
		area += int64_t(chart.x) * chart.y;
		widest = MAX(widest, chart.x);
		tallest = MAX(tallest, chart.y);
	}
	const int width = MAX(widest, int(Math::ceil(Math::sqrt(double(area)))));
	// Counting sort by height; equal heights keep their order.
	Vector<int> starts;
	starts.resize(tallest + 1);
	starts.fill(0);
	int *start = starts.ptrw();
	for (const Size2i &chart : p_charts) {
		start[tallest - chart.y]++;
	}
	for (int bucket = 0, total = 0; bucket <= tallest; bucket++) {
		const int count = start[bucket];
		start[bucket] = total;
		total += count;
	}
	Vector<int> order;
	order.resize(p_charts.size());
	int *order_write = order.ptrw();
	for (int chart = 0; chart < p_charts.size(); chart++) {
		order_write[start[tallest - p_charts[chart].y]++] = chart;
	}
	r_offsets.resize_uninitialized(p_charts.size());
	Point2i *offset = r_offsets.ptrw();
	int x = 0, y = 0, row = 0, used = 0;
	for (int chart : order) {
		const Size2i &size = p_charts[chart];
		if (x + size.x > width) {
			y += row;
			x = 0;
			row = 0;
		}
		offset[chart] = Point2i(x, y);
		x += size.x;
		row = MAX(row, size.y);
		used = MAX(used, x);
	}
	r_size = Size2i(used, y + row);
}

// One surface laid out in its own chart space. Every triangle has its own chart: sub-texel
// triangles never overwrite each other's material or emission.
struct SurfaceChart {
	LocalVector<Vector2> corners; // Capture pixels of every triangle corner in the surface chart.
	LocalVector<int32_t> tiles; // Per triangle: first cell x, y, width | height << 16, first sample.
	LocalVector<int32_t> charts; // Per triangle: chart id, 0 for a degenerate triangle.
	Size2i size;
	int sample_count = 0;
};

// Density along the longest edge is preserved; one sample across narrow charts ensures robust
// raster coverage without changing or eroding the physical geometry. A one-sided surface has no
// back-side cells. Samples are numbered from p_first_sample.
static void _layout_surface(const Vector<Vector3> &p_positions, const Vector<int> &p_indices, float p_texel_size, int p_sides, int p_first_sample, SurfaceChart &r_chart) {
	const int triangle_count = p_indices.size() / 3;
	const Vector3 *positions = p_positions.ptr();
	const int *indices = p_indices.ptr();
	r_chart.corners.resize_uninitialized(p_indices.size());
	r_chart.tiles.resize(triangle_count * 4);
	r_chart.charts.resize(triangle_count);
	Vector<Size2i> rectangles;
	rectangles.resize_uninitialized(triangle_count);
	Size2i *rectangle_write = rectangles.ptrw();
	int sample = p_first_sample;
	for (int triangle = 0; triangle < triangle_count; triangle++) {
		const int corner = triangle * 3;
		const Vector3 v[3] = { positions[indices[corner]], positions[indices[corner + 1]], positions[indices[corner + 2]] };
		int first = 0;
		for (int edge = 1; edge < 3; edge++) {
			if ((v[(edge + 1) % 3] - v[edge]).length_squared() > (v[(first + 1) % 3] - v[first]).length_squared()) {
				first = edge;
			}
		}
		const int second = (first + 1) % 3, third = (first + 2) % 3;
		const Vector3 edge = v[second] - v[first];
		const float length = edge.length();
		const float height = length > 0.0f ? edge.cross(v[third] - v[first]).length() / length : 0.0f;
		Vector2 *uv = &r_chart.corners[corner];
		int32_t *tile = &r_chart.tiles[triangle * 4];
		if (height > 0.0f) {
			const float width_pixels = MAX(1.0f, length / p_texel_size);
			const float height_pixels = MAX(1.0f, height / p_texel_size);
			uv[first] = Vector2(CHART_BORDER, CHART_BORDER);
			uv[second] = Vector2(CHART_BORDER + width_pixels, CHART_BORDER);
			uv[third] = Vector2(CHART_BORDER + (v[third] - v[first]).dot(edge) / (length * length) * width_pixels, CHART_BORDER + height_pixels);
			const Size2i cells(int(Math::ceil(width_pixels)), int(Math::ceil(height_pixels)));
			rectangle_write[triangle] = cells + Size2i(2 * CHART_BORDER, 2 * CHART_BORDER);
			tile[2] = cells.x | (cells.y << 16);
			sample += cells.x * cells.y * p_sides;
			r_chart.charts[triangle] = triangle + 1;
		} else {
			uv[0] = uv[1] = uv[2] = Vector2();
			rectangle_write[triangle] = Size2i(1, 1);
			tile[2] = 1 | (1 << 16);
			sample += p_sides;
			r_chart.charts[triangle] = 0;
		}
		tile[0] = CHART_BORDER;
		tile[1] = CHART_BORDER;
		tile[3] = sample - (tile[2] & 0xFFFF) * (tile[2] >> 16) * p_sides;
	}
	Vector<Point2i> offsets;
	_pack_charts(rectangles, offsets, r_chart.size);
	for (int triangle = 0; triangle < triangle_count; triangle++) {
		const Point2i offset = offsets[triangle];
		r_chart.tiles[triangle * 4] += offset.x;
		r_chart.tiles[triangle * 4 + 1] += offset.y;
		for (int corner = triangle * 3; corner < triangle * 3 + 3; corner++) {
			r_chart.corners[corner] += Vector2(offset);
		}
	}
	r_chart.sample_count = sample - p_first_sample;
}

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

// the capture's strict SAT (surface_cache_capture_inc.glsl) with the same
// error-free float arithmetic. Its signs are exact, so a cell it rejects is never captured; the
// capture only narrows the result further.
namespace SurfaceCacheCoverage {
static void two_diff(float a, float b, float &r_x, float &r_y) {
	const float difference = a - b;
	const float b_virtual = a - difference;
	const float a_virtual = difference + b_virtual;
	const float b_roundoff = b_virtual - b;
	const float a_roundoff = a - a_virtual;
	r_x = difference;
	r_y = a_roundoff + b_roundoff;
}

static void two_sum(float a, float b, float &r_x, float &r_y) {
	const float sum = a + b;
	const float b_virtual = sum - a;
	const float a_virtual = sum - b_virtual;
	const float b_roundoff = b - b_virtual;
	const float a_roundoff = a - a_virtual;
	r_x = sum;
	r_y = a_roundoff + b_roundoff;
}

static void grow_expansion(float *r_expansion, int &r_count, float p_value) {
	float grown[16];
	int grown_count = 0;
	float accumulator = p_value;
	for (int i = 0; i < r_count; i++) {
		float sum, error;
		two_sum(accumulator, r_expansion[i], sum, error);
		if (error != 0.0f) {
			grown[grown_count++] = error;
		}
		accumulator = sum;
	}
	if (accumulator != 0.0f || grown_count == 0) {
		grown[grown_count++] = accumulator;
	}
	for (int i = 0; i < grown_count; i++) {
		r_expansion[i] = grown[i];
	}
	r_count = grown_count;
}

static void add_product(float *r_expansion, int &r_count, float a, float b, float p_sign) {
	const float product = a * b;
	const float error = std::fma(a, b, -product);
	grow_expansion(r_expansion, r_count, p_sign * error);
	grow_expansion(r_expansion, r_count, p_sign * product);
}

static int cross_differences_sign(float dx_hi, float dx_lo, float dy_hi, float dy_lo, float px_hi, float px_lo, float py_hi, float py_lo) {
	// Axis-aligned edges are common in the chart projection. Their exact sign
	// needs no products or expansion, including a point on the edge itself.
	if (dx_hi == 0.0f && dx_lo == 0.0f) {
		return -SIGN(dy_hi != 0.0f ? dy_hi : dy_lo) * SIGN(px_hi != 0.0f ? px_hi : px_lo);
	}
	if (dy_hi == 0.0f && dy_lo == 0.0f) {
		return SIGN(dx_hi != 0.0f ? dx_hi : dx_lo) * SIGN(py_hi != 0.0f ? py_hi : py_lo);
	}

	const float left = dx_hi * py_hi;
	const float right = dy_hi * px_hi;
	const float determinant = left - right;
	const float tails = std::abs(dx_lo * py_hi) + std::abs(dx_hi * py_lo) + std::abs(dx_lo * py_lo) + std::abs(dy_lo * px_hi) + std::abs(dy_hi * px_lo) + std::abs(dy_lo * px_lo);
	const float error_bound = 5.0e-7f * (std::abs(left) + std::abs(right) + tails) + tails;
	if (std::abs(determinant) > error_bound) {
		return determinant > 0.0f ? 1 : -1;
	}
	float expansion[16] = {};
	int count = 0;
	add_product(expansion, count, dx_hi, py_hi, 1.0f);
	add_product(expansion, count, dx_hi, py_lo, 1.0f);
	add_product(expansion, count, dx_lo, py_hi, 1.0f);
	add_product(expansion, count, dx_lo, py_lo, 1.0f);
	add_product(expansion, count, dy_hi, px_hi, -1.0f);
	add_product(expansion, count, dy_hi, px_lo, -1.0f);
	add_product(expansion, count, dy_lo, px_hi, -1.0f);
	add_product(expansion, count, dy_lo, px_lo, -1.0f);
	for (int i = count - 1; i >= 0; i--) {
		if (expansion[i] != 0.0f) {
			return expansion[i] > 0.0f ? 1 : -1;
		}
	}
	return 0;
}

static int orient_difference_sign(const Vector2 &p_edge_a, const Vector2 &p_edge_b, const Vector2 &p_point, const Vector2 &p_origin) {
	float dx_hi, dx_lo, dy_hi, dy_lo, px_hi, px_lo, py_hi, py_lo;
	two_diff(p_edge_b.x, p_edge_a.x, dx_hi, dx_lo);
	two_diff(p_edge_b.y, p_edge_a.y, dy_hi, dy_lo);
	two_diff(p_point.x, p_origin.x, px_hi, px_lo);
	two_diff(p_point.y, p_origin.y, py_hi, py_lo);
	return cross_differences_sign(dx_hi, dx_lo, dy_hi, dy_lo, px_hi, px_lo, py_hi, py_lo);
}

static int orient_exact_sign(const Vector2 &a, const Vector2 &b, const Vector2 &c) {
	return orient_difference_sign(a, b, c, a);
}

// triangle invariants are shared by all its material cells.
struct Triangle {
	struct Edge {
		Vector2 origin, opposite, support_max, support_min;
		float dx_hi, dx_lo, dy_hi, dy_lo;
		int sign(const Vector2 &point, const Vector2 &relative_to) const {
			float px_hi, px_lo, py_hi, py_lo;
			two_diff(point.x, relative_to.x, px_hi, px_lo);
			two_diff(point.y, relative_to.y, py_hi, py_lo);
			return cross_differences_sign(dx_hi, dx_lo, dy_hi, dy_lo, px_hi, px_lo, py_hi, py_lo);
		}
	};
	Edge edges[3];
	Vector2 minimum, maximum;
	int winding;
	Triangle(const Vector2 &a, const Vector2 &b, const Vector2 &c) {
		minimum = Vector2(MIN(a.x, MIN(b.x, c.x)), MIN(a.y, MIN(b.y, c.y)));
		maximum = Vector2(MAX(a.x, MAX(b.x, c.x)), MAX(a.y, MAX(b.y, c.y)));
		winding = orient_exact_sign(a, b, c);
		const Vector2 corners[3] = { a, b, c };
		for (int i = 0; i < 3; ++i) {
			Edge &edge = edges[i];
			edge.origin = corners[i];
			const Vector2 end = corners[(i + 1) % 3];
			edge.opposite = corners[(i + 2) % 3];
			two_diff(end.x, edge.origin.x, edge.dx_hi, edge.dx_lo);
			two_diff(end.y, edge.origin.y, edge.dy_hi, edge.dy_lo);
			edge.support_max = Vector2(-edge.dy_hi * float(winding) > 0.0f ? 1.0f : 0.0f, edge.dx_hi * float(winding) > 0.0f ? 1.0f : 0.0f);
			edge.support_min = Vector2(1.0f, 1.0f) - edge.support_max;
		}
	}
	bool covers(const Vector2 &cell_min) const {
		const Vector2 cell_max = cell_min + Vector2(1.0f, 1.0f);
		if (winding == 0 || maximum.x <= cell_min.x || minimum.x >= cell_max.x || maximum.y <= cell_min.y || minimum.y >= cell_max.y) {
			return false;
		}
		for (const Edge &edge : edges) {
			if (edge.sign(cell_min + edge.support_max, edge.origin) * winding <= 0 || edge.sign(cell_min + edge.support_min, edge.opposite) * winding >= 0) {
				return false;
			}
		}
		return true;
	}
};
} // namespace SurfaceCacheCoverage

Dictionary SurfaceCacheGeometry::prepare(const Ref<Mesh> &p_mesh, float p_texel_size, const Vector<int32_t> &p_surface_sides) {
	ERR_FAIL_COND_V(p_mesh.is_null(), Dictionary());
	return _prepare(p_mesh.ptr(), RID(), p_texel_size, p_surface_sides, nullptr);
}

Dictionary SurfaceCacheGeometry::prepare_from_rid(RID p_mesh, float p_texel_size, const Vector<int32_t> &p_surface_sides) {
	ERR_FAIL_COND_V(!p_mesh.is_valid(), Dictionary());
	return _prepare(nullptr, p_mesh, p_texel_size, p_surface_sides, nullptr);
}

Dictionary SurfaceCacheGeometry::prepare_from_surfaces(RID p_mesh, const Vector<RenderingServerTypes::SurfaceData> &p_source, float p_texel_size, const Vector<int32_t> &p_surface_sides) {
	return _prepare(nullptr, p_mesh, p_texel_size, p_surface_sides, &p_source);
}

Dictionary SurfaceCacheGeometry::_prepare(const Mesh *p_resource, RID p_mesh, float p_texel_size, const Vector<int32_t> &p_surface_sides, const Vector<RenderingServerTypes::SurfaceData> *p_source) {
	ERR_FAIL_COND_V(!Math::is_finite(p_texel_size) || p_texel_size <= 0, Dictionary());
	uint64_t stage_start = OS::get_singleton()->get_ticks_usec();
	Dictionary operation_ms;
	static constexpr int TRIANGLE_BYTES = 96;
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

	// Surfaces stack vertically in one atlas, each with its border.
	stage_start = OS::get_singleton()->get_ticks_usec();
	LocalVector<SurfaceChart> charts;
	LocalVector<Vector2i> surface_offsets;
	charts.resize(surface_count);
	surface_offsets.resize(surface_count);
	Size2i atlas_size;
	int sample_count = 0;
	for (int surface = 0; surface < surface_count; surface++) {
		const int sides = p_surface_sides[surface];
		ERR_FAIL_COND_V_MSG(sides != 1 && sides != 2, Dictionary(), "Surface Cache: a surface has one or two sides.");
		SurfaceChart &chart = charts[surface];
		_layout_surface(positions[surface], indices[surface], p_texel_size, sides, sample_count, chart);
		const Vector2i offset(CHART_BORDER, atlas_size.y + CHART_BORDER);
		for (int triangle = 0; triangle < int(chart.charts.size()); triangle++) {
			chart.tiles[triangle * 4] += offset.x;
			chart.tiles[triangle * 4 + 1] += offset.y;
		}
		surface_offsets[surface] = offset;
		atlas_size.x = MAX(atlas_size.x, chart.size.x + 2 * CHART_BORDER);
		atlas_size.y += chart.size.y + 2 * CHART_BORDER;
		sample_count += chart.sample_count;
	}
	operation_ms["mesh_layout"] = (OS::get_singleton()->get_ticks_usec() - stage_start) / 1000.0;

	// Ray records, coverage and capture pixels in one pass over the triangles.
	stage_start = OS::get_singleton()->get_ticks_usec();
	// Material cells the capture can cover (bit per cell, every side) and the covered count before
	// every 32-cell word: storage is allocated only for covered cells.
	PackedInt32Array material_coverage;
	material_coverage.resize((sample_count + 31) / 32);
	material_coverage.fill(0);
	int32_t *coverage = material_coverage.ptrw();
	const Vector2 atlas = Vector2(atlas_size);
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
		const SurfaceChart &chart = charts[surface];
		const Vector3 *position = positions[surface].ptr();
		const Vector3 *normal = normals[surface].is_empty() ? nullptr : normals[surface].ptr();
		const int *index = indices[surface].ptr();
		const int corner_count = indices[surface].size();
		const Vector2 offset = Vector2(surface_offsets[surface]);
		Vector<uint8_t> packed;
		packed.resize(corner_count / 3 * TRIANGLE_BYTES);
		uint8_t *write = packed.ptrw();
		for (int corner = 0; corner < corner_count; corner += 3) {
			const int triangle = corner / 3;
			const int32_t *tile = &chart.tiles[triangle * 4];
			const int chart_id = chart.charts[triangle];
			Vector3 vertex[3];
			// CacheTriangle: corners with their source vertex, atlas UV, chart, orientation, tile.
			uint32_t record[TRIANGLE_BYTES / 4];
			for (int c = 0; c < 3; c++) {
				const int original = index[corner + c];
				vertex[c] = position[original];
				const Vector2 pixel = chart.corners[corner + c] + offset;
				const Vector2 uv = pixel / atlas;
				pixel_write[corner + c] = pixel;
				memcpy(&record[c * 4], &vertex[c], 12);
				record[c * 4 + 3] = uint32_t(original);
				memcpy(&record[12 + c * 2], &uv, 8);
			}
			// Godot's authored winding is clockwise; normals establish the semantic front.
			float orientation = normal ? SIGN(normal[index[corner]].dot((vertex[1] - vertex[0]).cross(vertex[2] - vertex[0]))) : -1.0f;
			if (orientation == 0.0f) {
				orientation = -1.0f;
			}
			record[18] = uint32_t(chart_id);
			memcpy(&record[19], &orientation, 4);
			memcpy(&record[20], tile, 16);
			memcpy(write + triangle * TRIANGLE_BYTES, record, TRIANGLE_BYTES);

			const int width = tile[2] & 0xFFFF, area = width * (tile[2] >> 16);
			// A nondegenerate 1x1 chart is stretched to a unit base and unit height. Its only cell is
			// always covered; avoid six SAT tests per tiny triangle.
			if (area == 1 && chart_id != 0) {
				for (int side = 0; side < sides; side++) {
					const int address = tile[3] + side;
					coverage[address >> 5] |= int32_t(1u << (address & 31));
				}
			} else {
				const SurfaceCacheCoverage::Triangle covered(pixel_write[corner], pixel_write[corner + 1], pixel_write[corner + 2]);
				for (int y = 0; y < (tile[2] >> 16); y++) {
					for (int x = 0; x < width; x++) {
						if (covered.covers(Vector2(tile[0] + x, tile[1] + y))) {
							for (int side = 0; side < sides; side++) {
								const int address = tile[3] + side * area + y * width + x;
								coverage[address >> 5] |= int32_t(1u << (address & 31));
							}
						}
					}
				}
			}
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
		result_surface["offset"] = surface_offsets[surface];
		result_surface["size"] = chart.size;
		result_surface["sample_count"] = chart.sample_count;
		result_surfaces.push_back(result_surface);
	}
	PackedInt32Array material_coverage_ranks;
	material_coverage_ranks.resize(material_coverage.size());
	int covered_cells = 0;
	int32_t *rank_write = material_coverage_ranks.ptrw();
	for (int word = 0; word < material_coverage.size(); word++) {
		rank_write[word] = covered_cells;
		uint32_t bits = uint32_t(coverage[word]);
		bits = bits - ((bits >> 1) & 0x55555555u);
		bits = (bits & 0x33333333u) + ((bits >> 2) & 0x33333333u);
		covered_cells += int((((bits + (bits >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
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
	layout["sample_count"] = sample_count;
	layout["surfaces"] = result_surfaces;
	Dictionary result;
	result["operation_ms"] = operation_ms;
	result["layout"] = layout;
	result["mesh"] = capture_mesh;
	result["material_coverage"] = material_coverage;
	result["material_coverage_ranks"] = material_coverage_ranks;
	result["triangle_data"] = triangle_data;
	result["vertex_data"] = vertex_data;
	result["index_data"] = index_data;
	return result;
}
