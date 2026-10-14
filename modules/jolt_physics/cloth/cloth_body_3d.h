/**************************************************************************/
/*  cloth_body_3d.h                                                       */
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

// Fork(Lestoroer): cloth node. See modules/jolt_physics/cloth/README.md.

#pragma once

#include "cloth_mesh.h"

#include "scene/3d/visual_instance_3d.h"

class JoltCloth3D;

// Visible cloth that follows its pose with physics. The pose is the state; the simulated shape only
// adds motion around it, and while the cloth sleeps it is drawn exactly in the pose.
class ClothBody3D : public GeometryInstance3D {
	GDCLASS(ClothBody3D, GeometryInstance3D);

	Ref<ClothMesh> cloth_mesh;
	JoltCloth3D *cloth = nullptr;
	RID skeleton;

	float pose_position = 0.0f;
	bool follow_pose = true;
	bool carried = false;

	float particle_mass = 0.01f;
	float stretch_compliance = 0.0f;
	float shear_compliance = 0.0001f;
	float bend_compliance = 0.01f;
	float crease_compliance = -1.0f;
	bool dihedral_bends = true;
	float linear_damping = 0.3f;
	float friction = 0.5f;
	float gravity_scale = 1.0f;
	float vertex_radius = 0.005f;
	float drive_stiffness = 20.0f;
	float drive_damping = 4.0f;
	float max_distance_scale = 1.0f;
	float backstop_distance = -1.0f;
	float backstop_radius = 0.5f;
	int iterations = 8;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	uint32_t lying_collision_layer = 0;
	float drop_height = 0.0f;
	float push_response = 1.0f;

	LocalVector<Vector3> pose_local;
	LocalVector<Basis> pose_frames;
	LocalVector<Vector3> previous_local;
	LocalVector<Vector3> current_local;
	LocalVector<Vector3> render_local;
	LocalVector<Basis> render_frames;
	LocalVector<Vector3> world_scratch;
	LocalVector<Vector3> carry_local;

	Transform3D target_transform;
	Transform3D step_inverse;
	uint64_t read_physics_frame = 0;
	bool targets_dirty = true;
	// The targets moved on the last physics step.
	bool targets_moving = false;
	bool pose_reshaped = false;
	// Until the first physics step a pose change is the initial state, not a motion.
	bool physics_started = false;
	// Seconds left while a cloth that started away from its pose may leave it further than usual.
	float reach_ramp = 0.0f;
	// Seconds left of the blend from the simulated shape into the exact pose after falling asleep.
	float settle_left = 0.0f;
	bool was_active = false;
	bool drawn_in_pose = true;
	// Where the particles were at the start of the current rest check, in world space.
	LocalVector<Vector3> rest_check_world;
	float rest_check_time = 0.0f;
	// How long the cloth has been still in a row, and awake since it was woken or last touched.
	float rest_quiet_time = 0.0f;
	float awake_time = 0.0f;
	// The cursor or a hand touched the cloth since the last rest check.
	bool attended = false;

	// Static body other cloth lies on while this cloth rests, and the particle positions it was built from.
	RID lying_body;
	RID lying_shape;
	LocalVector<Vector3> lying_positions;
	AABB lying_bounds;

	void _rebuild();
	void _create_physics(const LocalVector<Vector3> *p_start_world = nullptr);
	void _draw_settling(float p_weight);
	void _free_physics();
	void _update_pose();
	void _push_targets();
	void _read_simulation();
	void _check_rest(uint64_t p_steps);
	void _update_render();
	void _draw_pose();
	void _fell_asleep();
	void _update_lying_collision();
	void _free_lying_collision(bool p_wake_resting);
	void _wake_resting_on(const AABB &p_bounds);
	void _targets_moved();
	void _update_motion();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_cloth_mesh(const Ref<ClothMesh> &p_mesh);
	Ref<ClothMesh> get_cloth_mesh() const { return cloth_mesh; }

	void set_pose_position(float p_position);
	float get_pose_position() const { return pose_position; }

	void set_follow_pose(bool p_follow);
	bool is_following_pose() const { return follow_pose; }

	void set_particle_mass(float p_mass);
	float get_particle_mass() const { return particle_mass; }
	void set_stretch_compliance(float p_compliance);
	float get_stretch_compliance() const { return stretch_compliance; }
	void set_shear_compliance(float p_compliance);
	float get_shear_compliance() const { return shear_compliance; }
	void set_bend_compliance(float p_compliance);
	float get_bend_compliance() const { return bend_compliance; }
	void set_crease_compliance(float p_compliance);
	float get_crease_compliance() const { return crease_compliance; }
	void set_dihedral_bends(bool p_dihedral);
	bool is_dihedral_bends() const { return dihedral_bends; }
	void set_linear_damping(float p_damping);
	float get_linear_damping() const { return linear_damping; }
	void set_friction(float p_friction);
	float get_friction() const { return friction; }
	void set_gravity_scale(float p_scale);
	float get_gravity_scale() const { return gravity_scale; }
	void set_vertex_radius(float p_radius);
	float get_vertex_radius() const { return vertex_radius; }
	void set_drive_stiffness(float p_stiffness);
	float get_drive_stiffness() const { return drive_stiffness; }
	void set_drive_damping(float p_damping);
	float get_drive_damping() const { return drive_damping; }
	void set_max_distance_scale(float p_scale);
	float get_max_distance_scale() const { return max_distance_scale; }
	void set_backstop_distance(float p_distance);
	float get_backstop_distance() const { return backstop_distance; }
	void set_backstop_radius(float p_radius);
	float get_backstop_radius() const { return backstop_radius; }
	void set_iterations(int p_iterations);
	int get_iterations() const { return iterations; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_lying_collision_layer(uint32_t p_layer) { lying_collision_layer = p_layer; }
	uint32_t get_lying_collision_layer() const { return lying_collision_layer; }
	void set_drop_height(float p_height) { drop_height = MAX(p_height, 0.0f); }
	float get_drop_height() const { return drop_height; }
	void set_push_response(float p_response) { push_response = MAX(p_response, 0.0f); }
	float get_push_response() const { return push_response; }

	void push(const Vector3 &p_point, const Vector3 &p_velocity, float p_radius);
	int grab(const Vector3 &p_point);
	void move_grab(const Vector3 &p_point);
	void release_grab();
	bool is_grabbing() const;

	void set_carried(bool p_carried);
	bool is_carried() const { return carried; }

	bool is_sleeping() const;
	void reset_to_pose();
	// Particle positions in world space, for tests and tools.
	PackedVector3Array get_particle_positions() const;

	virtual AABB get_aabb() const override;

	ClothBody3D();
	~ClothBody3D();
};
