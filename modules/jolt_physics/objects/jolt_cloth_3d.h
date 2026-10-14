/**************************************************************************/
/*  jolt_cloth_3d.h                                                       */
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

// Fork(Lestoroer): cloth that follows an authored pose. See modules/jolt_physics/cloth/README.md.

#pragma once

#include "jolt_object_3d.h"

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

class JoltCloth3D final : public JoltObject3D {
public:
	// Particle data in world space at the moment the cloth enters the space.
	struct Setup {
		// Rest shape of the fabric: edge lengths and bends are measured here.
		LocalVector<Vector3> positions;
		// Where the particles start; empty means at rest.
		LocalVector<Vector3> start_positions;
		LocalVector<int32_t> triangles;
		LocalVector<float> pin_weights;
		// Distance a particle may leave its target, in meters; negative means unlimited.
		LocalVector<float> max_distances;
		// How firmly a hold keeps each particle at its target, from 0 to 1; empty: nothing is held.
		LocalVector<float> hold_weights;
		float particle_mass = 0.01f;
		float stretch_compliance = 0.0f;
		float shear_compliance = 0.0f;
		float bend_compliance = 0.0001f;
		// Dihedral bends keep hanging folds crisp; distance bends cost several times less.
		bool dihedral_bends = true;
		// Every cell of a grid shears with both its diagonals, also across folds; see _shear_cell_diagonals.
		bool shear_across_folds = false;
		// Compliance of bends folded in the pose by a crease angle or more; negative: as other bends.
		float crease_compliance = -1.0f;
		float backstop_distance = -1.0f;
		float backstop_radius = 0.5f;
		bool follow_targets = true;
	};

private:
	struct Push {
		Vector3 point;
		Vector3 velocity;
		float radius = 0.0f;
	};

	// When each resting shape other cloth lies on appeared, by the instance that left it. Written on the
	// main thread between steps, read by the collision filter during steps.
	static HashMap<ObjectID, uint64_t> resting_shape_stamps;
	static uint64_t stamp_counter;

	Setup setup;
	uint64_t release_stamp = 0;
	LocalVector<Vector3> targets;
	LocalVector<Vector3> previous_targets;
	LocalVector<Push> pushes;
	LocalVector<float> particle_inverse_masses;
	JPH::Array<JPH::Mat44> joint_matrices;
	JPH::Ref<JPH::SoftBodySharedSettings> shared_settings;

	float drive_stiffness = 0.0f;
	float drive_damping = 0.0f;
	float gravity_scale = 1.0f;
	float linear_damping = 0.1f;
	float friction = 0.4f;
	float vertex_radius = 0.005f;
	float max_distance_scale = 1.0f;
	float hold = 0.0f;
	bool hold_dirty = false;
	// Held fabric is still coming to its holder.
	bool hold_closing = false;
	int iterations = 6;

	// A hand holds a patch of particles: the grabbed one and its neighbors.
	int grabbed_particle = -1;
	LocalVector<uint32_t> grip_particles;
	LocalVector<Vector3> grip_offsets;
	// What holds the grabbed fabric, its pin and the fabric in a closed holder, and how far the hand
	// may take the grabbed particle from each.
	LocalVector<uint32_t> grip_anchors;
	LocalVector<float> grip_reaches;
	Vector3 grab_target;
	Vector3 grab_requested;
	bool targets_valid = false;
	bool hard_reset_requested = false;
	bool rest_shape_dirty = false;

	virtual JPH::BroadPhaseLayer _get_broad_phase_layer() const override;
	virtual JPH::ObjectLayer _get_object_layer() const override;
	virtual void _add_to_space() override;

	bool _is_kinematic(uint32_t p_particle) const;
	JPH::SoftBodySharedSettings *_create_shared_settings() const;
	void _shear_cell_diagonals(JPH::SoftBodySharedSettings &p_shared) const;
	void _skin_to_targets(bool p_hard_reset);
	void _reshape_rest();
	void _keep_attachments_from_pulling();
	void _apply_gravity(float p_step);
	void _apply_drive(float p_step);
	void _apply_pushes();
	void _apply_grab(float p_step);
	void _find_grip_anchors();
	void _forget_grip();
	void _apply_hold(float p_step);
	void _apply_crease_compliance(JPH::SoftBodySharedSettings &p_shared) const;
	void _update_motion_settings();

public:
	JoltCloth3D();
	virtual ~JoltCloth3D() override;

	// The targets start at the rest shape.
	void configure(const Setup &p_setup);
	int get_particle_count() const { return (int)setup.positions.size(); }

	// Stamps order releases of cloth and the appearance of resting shapes.
	static uint64_t next_stamp() { return ++stamp_counter; }
	// p_stamp 0 removes the resting shape of the instance.
	static void set_resting_shape_stamp(ObjectID p_instance, uint64_t p_stamp);
	static uint64_t get_resting_shape_stamp(ObjectID p_instance);
	// The cloth was let go to move on its own: put down, unfolded, created.
	void set_release_stamp(uint64_t p_stamp) { release_stamp = p_stamp; }

	// p_reshape: the targets form a new pose, not the same pose moved; the fabric then rests in it.
	void set_targets(const LocalVector<Vector3> &p_world_targets, bool p_reshape);
	void reset_to_targets();
	// Puts the particles of a sleeping cloth exactly at its targets, at rest, without waking it.
	void rest_in_targets();

	void set_drive(float p_stiffness, float p_damping);
	void set_gravity_scale(float p_scale);
	void set_linear_damping(float p_damping);
	void set_friction(float p_friction);
	void set_vertex_radius(float p_radius);
	void set_max_distance_scale(float p_scale);
	// How firmly the hold acts now, from 0 to 1.
	void set_hold(float p_hold);
	void set_iterations(int p_iterations);

	void push(const Vector3 &p_point, const Vector3 &p_velocity, float p_radius);
	// Grabs the free particle nearest to the point with its neighbors; returns it, or -1.
	int grab(const Vector3 &p_world_position);
	void move_grab(const Vector3 &p_world_position);
	void release_grab();
	int get_grabbed_particle() const { return grabbed_particle; }

	bool is_active() const;
	void wake_up();
	void put_to_sleep();

	// Copies particle positions in world space.
	void get_positions(LocalVector<Vector3> &r_positions) const;

	bool interacts_with(const JoltObject3D &p_other) const;

	virtual bool can_interact_with(const JoltBody3D &p_other) const override;
	virtual bool can_interact_with(const JoltSoftBody3D &p_other) const override;
	virtual bool can_interact_with(const JoltArea3D &p_other) const override;
	virtual bool reports_contacts() const override { return false; }
	virtual Vector3 get_velocity_at_position(const Vector3 &p_position) const override { return Vector3(); }

	virtual void pre_step(float p_step) override;
};
