/**************************************************************************/
/*  cloth_mesh.h                                                          */
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

// Fork(Lestoroer): cloth asset. See modules/jolt_physics/cloth/README.md.

#pragma once

#include "core/io/resource.h"
#include "scene/resources/mesh.h"

// Simulation particles of a cloth, its authored poses and the visible mesh skinned to the particles.
// Pose 0 is the rest pose of the visible mesh; pose k > 0 is its blend shape k - 1.
class ClothMesh : public Resource {
	GDCLASS(ClothMesh, Resource);

	PackedInt32Array triangles;
	PackedFloat32Array pin_weights;
	PackedFloat32Array max_distances;
	TypedArray<PackedVector3Array> poses;
	Ref<ArrayMesh> render_mesh;

	// Adjacency for particle frames: for every particle its triangles and one neighbor.
	mutable LocalVector<int32_t> frame_triangle_offsets;
	mutable LocalVector<int32_t> frame_triangles;
	mutable LocalVector<int32_t> frame_neighbors;
	mutable bool frames_ready = false;

	void _prepare_frames() const;

protected:
	static void _bind_methods();

public:
	void set_triangles(const PackedInt32Array &p_triangles);
	PackedInt32Array get_triangles() const { return triangles; }

	void set_pin_weights(const PackedFloat32Array &p_weights) { pin_weights = p_weights; }
	PackedFloat32Array get_pin_weights() const { return pin_weights; }
	// A pinned cloth hangs on its support: it is pulled by a point, not carried.
	bool has_pins() const;

	void set_max_distances(const PackedFloat32Array &p_distances) { max_distances = p_distances; }
	PackedFloat32Array get_max_distances() const { return max_distances; }

	void set_poses(const TypedArray<PackedVector3Array> &p_poses);
	TypedArray<PackedVector3Array> get_poses() const { return poses; }
	int get_pose_count() const { return poses.size(); }

	void set_render_mesh(const Ref<ArrayMesh> &p_mesh) { render_mesh = p_mesh; }
	Ref<ArrayMesh> get_render_mesh() const { return render_mesh; }

	int get_particle_count() const;

	// Particle positions of a pose position between 0 and pose count - 1.
	void sample_pose(float p_position, LocalVector<Vector3> &r_positions) const;
	// Rotation of the surface at every particle: X along a neighbor, Y along the normal.
	void calculate_frames(const LocalVector<Vector3> &p_positions, LocalVector<Basis> &r_frames) const;

	// Builds a cloth from a visible mesh and a coarse simulation mesh in the same space.
	// The simulation mesh carries pins in the red vertex color and the allowed distance from the pose,
	// as a fraction of p_max_distance, in the green vertex color. Both meshes carry the same blend shapes.
	static Ref<ClothMesh> build(const Ref<ArrayMesh> &p_render_mesh, const Ref<ArrayMesh> &p_simulation_mesh, float p_max_distance);
};
