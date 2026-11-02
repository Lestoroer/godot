/**************************************************************************/
/*  cloth_mesh.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// lestoroer@physics

#include "cloth_mesh.h"

#include "core/object/class_db.h"
#include "core/templates/hash_map.h"

namespace {

// Positions of every blend shape of a surface, converted to absolute positions.
Vector<PackedVector3Array> read_shape_positions(const Ref<ArrayMesh> &p_mesh, int p_surface, const PackedVector3Array &p_base) {
	Vector<PackedVector3Array> shapes;
	const TypedArray<Array> shape_arrays = p_mesh->surface_get_blend_shape_arrays(p_surface);
	const bool relative = p_mesh->get_blend_shape_mode() == Mesh::BLEND_SHAPE_MODE_RELATIVE;
	for (int i = 0; i < shape_arrays.size(); i++) {
		const Array shape = shape_arrays[i];
		PackedVector3Array positions = shape[Mesh::ARRAY_VERTEX];
		ERR_FAIL_COND_V(positions.size() != p_base.size(), Vector<PackedVector3Array>());
		if (relative) {
			for (int v = 0; v < positions.size(); v++) {
				positions.set(v, p_base[v] + positions[v]);
			}
		}
		shapes.push_back(positions);
	}
	return shapes;
}

// Barycentric weights of a point in a triangle on the fabric; negative outside it.
Vector3 fabric_barycentric(const Vector2 &p_point, const Vector2 &p_a, const Vector2 &p_b, const Vector2 &p_c) {
	const real_t area = (p_b - p_a).cross(p_c - p_a);
	if (Math::abs(area) < 1e-12) {
		return Vector3(-1, -1, -1);
	}
	const real_t a = (p_b - p_point).cross(p_c - p_point) / area;
	const real_t b = (p_c - p_point).cross(p_a - p_point) / area;
	return Vector3(a, b, 1.0 - a - b);
}

} // namespace

void ClothMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_triangles", "triangles"), &ClothMesh::set_triangles);
	ClassDB::bind_method(D_METHOD("get_triangles"), &ClothMesh::get_triangles);
	ClassDB::bind_method(D_METHOD("set_pin_weights", "weights"), &ClothMesh::set_pin_weights);
	ClassDB::bind_method(D_METHOD("get_pin_weights"), &ClothMesh::get_pin_weights);
	ClassDB::bind_method(D_METHOD("has_pins"), &ClothMesh::has_pins);
	ClassDB::bind_method(D_METHOD("set_max_distances", "distances"), &ClothMesh::set_max_distances);
	ClassDB::bind_method(D_METHOD("get_max_distances"), &ClothMesh::get_max_distances);
	ClassDB::bind_method(D_METHOD("set_poses", "poses"), &ClothMesh::set_poses);
	ClassDB::bind_method(D_METHOD("get_poses"), &ClothMesh::get_poses);
	ClassDB::bind_method(D_METHOD("set_render_mesh", "mesh"), &ClothMesh::set_render_mesh);
	ClassDB::bind_method(D_METHOD("get_render_mesh"), &ClothMesh::get_render_mesh);
	ClassDB::bind_method(D_METHOD("get_particle_count"), &ClothMesh::get_particle_count);
	ClassDB::bind_method(D_METHOD("get_pose_count"), &ClothMesh::get_pose_count);
	ClassDB::bind_static_method("ClothMesh", D_METHOD("build", "render_mesh", "simulation_mesh", "max_distance"), &ClothMesh::build);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "triangles", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_triangles", "get_triangles");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "pin_weights", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_pin_weights", "get_pin_weights");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "max_distances", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_max_distances", "get_max_distances");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "poses", PROPERTY_HINT_ARRAY_TYPE, "PackedVector3Array", PROPERTY_USAGE_NO_EDITOR), "set_poses", "get_poses");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "render_mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh"), "set_render_mesh", "get_render_mesh");
}

bool ClothMesh::has_pins() const {
	for (float weight : pin_weights) {
		if (weight > 0.5f) {
			return true;
		}
	}
	return false;
}

void ClothMesh::set_triangles(const PackedInt32Array &p_triangles) {
	triangles = p_triangles;
	frames_ready = false;
}

void ClothMesh::set_poses(const TypedArray<PackedVector3Array> &p_poses) {
	poses = p_poses;
	frames_ready = false;
}

int ClothMesh::get_particle_count() const {
	return poses.is_empty() ? 0 : PackedVector3Array(poses[0]).size();
}

void ClothMesh::sample_pose(float p_position, LocalVector<Vector3> &r_positions) const {
	ERR_FAIL_COND(poses.is_empty());
	const float position = CLAMP(p_position, 0.0f, float(poses.size() - 1));
	const int from = MIN(int(Math::floor(position)), poses.size() - 1);
	const int to = MIN(from + 1, poses.size() - 1);
	const float weight = position - float(from);

	const PackedVector3Array from_positions = poses[from];
	const PackedVector3Array to_positions = poses[to];
	r_positions.resize(from_positions.size());
	for (int i = 0; i < from_positions.size(); i++) {
		r_positions[i] = from_positions[i].lerp(to_positions[i], weight);
	}
}

void ClothMesh::_prepare_frames() const {
	const int particle_count = get_particle_count();
	frame_triangle_offsets.resize(particle_count + 1);
	frame_neighbors.resize(particle_count);
	for (int i = 0; i <= particle_count; i++) {
		frame_triangle_offsets[i] = 0;
	}
	for (int i = 0; i < triangles.size(); i++) {
		frame_triangle_offsets[triangles[i] + 1]++;
	}
	for (int i = 0; i < particle_count; i++) {
		frame_triangle_offsets[i + 1] += frame_triangle_offsets[i];
	}
	frame_triangles.resize(triangles.size());
	LocalVector<int32_t> fill;
	fill.resize(particle_count);
	for (int i = 0; i < particle_count; i++) {
		fill[i] = frame_triangle_offsets[i];
		frame_neighbors[i] = -1;
	}
	for (int t = 0; t < triangles.size() / 3; t++) {
		for (int corner = 0; corner < 3; corner++) {
			const int particle = triangles[t * 3 + corner];
			frame_triangles[fill[particle]++] = t;
			if (frame_neighbors[particle] < 0) {
				frame_neighbors[particle] = triangles[t * 3 + (corner + 1) % 3];
			}
		}
	}
	frames_ready = true;
}

void ClothMesh::calculate_frames(const LocalVector<Vector3> &p_positions, LocalVector<Basis> &r_frames) const {
	if (!frames_ready) {
		_prepare_frames();
	}
	const int particle_count = get_particle_count();
	ERR_FAIL_COND((int)p_positions.size() != particle_count);
	r_frames.resize(particle_count);

	for (int i = 0; i < particle_count; i++) {
		Vector3 normal;
		for (int k = frame_triangle_offsets[i]; k < frame_triangle_offsets[i + 1]; k++) {
			const int t = frame_triangles[k] * 3;
			const Vector3 &a = p_positions[triangles[t]];
			normal += (p_positions[triangles[t + 1]] - a).cross(p_positions[triangles[t + 2]] - a);
		}
		if (normal.length_squared() < CMP_EPSILON2 || frame_neighbors[i] < 0) {
			r_frames[i] = Basis();
			continue;
		}
		normal.normalize();
		Vector3 tangent = p_positions[frame_neighbors[i]] - p_positions[i];
		tangent -= normal * tangent.dot(normal);
		if (tangent.length_squared() < CMP_EPSILON2) {
			tangent = normal.get_any_perpendicular();
		}
		tangent.normalize();
		r_frames[i] = Basis(tangent, normal, tangent.cross(normal));
	}
}

Ref<ClothMesh> ClothMesh::build(const Ref<ArrayMesh> &p_render_mesh, const Ref<ArrayMesh> &p_simulation_mesh, float p_max_distance) {
	ERR_FAIL_COND_V(p_render_mesh.is_null() || p_simulation_mesh.is_null(), Ref<ClothMesh>());
	ERR_FAIL_COND_V_MSG(p_simulation_mesh->get_surface_count() != 1, Ref<ClothMesh>(), "Cloth simulation mesh must have exactly one surface.");
	ERR_FAIL_COND_V_MSG(p_render_mesh->get_blend_shape_count() != p_simulation_mesh->get_blend_shape_count(), Ref<ClothMesh>(), "Cloth render and simulation meshes must have the same poses.");

	// Simulation particles: weld vertices split by the exporter so the fabric stays connected.
	const Array simulation_arrays = p_simulation_mesh->surface_get_arrays(0);
	const PackedVector3Array simulation_vertices = simulation_arrays[Mesh::ARRAY_VERTEX];
	PackedInt32Array simulation_indices = simulation_arrays[Mesh::ARRAY_INDEX];
	const PackedColorArray simulation_colors = simulation_arrays[Mesh::ARRAY_COLOR];
	const PackedVector2Array simulation_fabric = simulation_arrays[Mesh::ARRAY_TEX_UV2];
	ERR_FAIL_COND_V_MSG(simulation_colors.size() != simulation_vertices.size(), Ref<ClothMesh>(), "Cloth simulation mesh must carry pins and allowed distances in its vertex colors.");
	ERR_FAIL_COND_V_MSG(simulation_fabric.size() != simulation_vertices.size(), Ref<ClothMesh>(), "Cloth simulation mesh must carry fabric coordinates in UV2.");
	if (simulation_indices.is_empty()) {
		for (int i = 0; i < simulation_vertices.size(); i++) {
			simulation_indices.push_back(i);
		}
	}
	const Vector<PackedVector3Array> simulation_shapes = read_shape_positions(p_simulation_mesh, 0, simulation_vertices);

	HashMap<Vector3, int> welded_by_position;
	LocalVector<int> welded_index;
	LocalVector<int> source_of_particle;
	welded_index.resize(simulation_vertices.size());
	for (int i = 0; i < simulation_vertices.size(); i++) {
		const Vector3 key = simulation_vertices[i].snappedf(0.00001);
		HashMap<Vector3, int>::Iterator found = welded_by_position.find(key);
		if (found) {
			welded_index[i] = found->value;
		} else {
			welded_index[i] = source_of_particle.size();
			welded_by_position.insert(key, source_of_particle.size());
			source_of_particle.push_back(i);
		}
	}

	Ref<ClothMesh> cloth;
	cloth.instantiate();
	const int particle_count = source_of_particle.size();

	TypedArray<PackedVector3Array> poses;
	for (int pose = 0; pose <= simulation_shapes.size(); pose++) {
		const PackedVector3Array &source = pose == 0 ? simulation_vertices : simulation_shapes[pose - 1];
		PackedVector3Array positions;
		positions.resize(particle_count);
		for (int p = 0; p < particle_count; p++) {
			positions.set(p, source[source_of_particle[p]]);
		}
		poses.push_back(positions);
	}

	PackedFloat32Array pins;
	PackedFloat32Array distances;
	PackedVector2Array particle_fabric;
	pins.resize(particle_count);
	distances.resize(particle_count);
	particle_fabric.resize(particle_count);
	for (int p = 0; p < particle_count; p++) {
		const Color color = simulation_colors[source_of_particle[p]];
		pins.set(p, color.r > 0.5f ? 1.0f : 0.0f);
		distances.set(p, color.g * p_max_distance);
		particle_fabric.set(p, simulation_fabric[source_of_particle[p]]);
	}

	PackedInt32Array welded_triangles;
	for (int i = 0; i + 2 < simulation_indices.size(); i += 3) {
		const int a = welded_index[simulation_indices[i]];
		const int b = welded_index[simulation_indices[i + 1]];
		const int c = welded_index[simulation_indices[i + 2]];
		if (a != b && b != c && a != c) {
			welded_triangles.push_back(a);
			welded_triangles.push_back(b);
			welded_triangles.push_back(c);
		}
	}

	cloth->set_poses(poses);
	cloth->set_triangles(welded_triangles);
	cloth->set_pin_weights(pins);
	cloth->set_max_distances(distances);

	// Visible mesh: every vertex follows the three particles of the grid cell it lies in on the fabric.
	Ref<ArrayMesh> skinned;
	skinned.instantiate();
	skinned->set_blend_shape_mode(p_render_mesh->get_blend_shape_mode());
	for (int i = 0; i < p_render_mesh->get_blend_shape_count(); i++) {
		skinned->add_blend_shape(p_render_mesh->get_blend_shape_name(i));
	}

	for (int surface = 0; surface < p_render_mesh->get_surface_count(); surface++) {
		Array arrays = p_render_mesh->surface_get_arrays(surface);
		const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		const PackedVector2Array fabric = arrays[Mesh::ARRAY_TEX_UV2];
		ERR_FAIL_COND_V_MSG(fabric.size() != vertices.size(), Ref<ClothMesh>(), "Cloth render mesh must carry fabric coordinates in UV2.");
		PackedInt32Array bones;
		PackedFloat32Array weights;
		bones.resize(vertices.size() * 4);
		weights.resize(vertices.size() * 4);

		for (int v = 0; v < vertices.size(); v++) {
			float best_distance = FLT_MAX;
			int best_triangle = 0;
			Vector3 best_weights(1, 0, 0);
			// The cell holding the vertex has all weights non-negative; at the edge of the fabric
			// the cell with the least negative weight is the closest.
			for (int t = 0; t + 2 < welded_triangles.size(); t += 3) {
				const Vector3 barycentric = fabric_barycentric(fabric[v], particle_fabric[welded_triangles[t]], particle_fabric[welded_triangles[t + 1]], particle_fabric[welded_triangles[t + 2]]);
				const float outside = -MIN(barycentric.x, MIN(barycentric.y, barycentric.z));
				if (outside < best_distance) {
					best_distance = outside;
					best_triangle = t;
					best_weights = barycentric;
				}
			}
			const Vector3 clamped(MAX(best_weights.x, 0.0f), MAX(best_weights.y, 0.0f), MAX(best_weights.z, 0.0f));
			const real_t total = MAX(clamped.x + clamped.y + clamped.z, CMP_EPSILON);
			for (int corner = 0; corner < 3; corner++) {
				bones.set(v * 4 + corner, welded_triangles[best_triangle + corner]);
				weights.set(v * 4 + corner, clamped[corner] / total);
			}
			bones.set(v * 4 + 3, 0);
			weights.set(v * 4 + 3, 0.0f);
		}

		arrays[Mesh::ARRAY_BONES] = bones;
		arrays[Mesh::ARRAY_WEIGHTS] = weights;
		const TypedArray<Array> shapes = p_render_mesh->surface_get_blend_shape_arrays(surface);
		skinned->add_surface_from_arrays(p_render_mesh->surface_get_primitive_type(surface), arrays, shapes);
		skinned->surface_set_material(surface, p_render_mesh->surface_get_material(surface));
		skinned->surface_set_name(surface, p_render_mesh->surface_get_name(surface));
	}
	cloth->set_render_mesh(skinned);
	return cloth;
}
