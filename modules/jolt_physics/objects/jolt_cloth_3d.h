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

#include "core/templates/local_vector.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

class JoltCloth3D final : public JoltObject3D {
public:
	// Particle data in world space at the moment the cloth enters the space.
	struct Setup {
		LocalVector<Vector3> positions;
		LocalVector<int32_t> triangles;
		LocalVector<float> pin_weights;
		// Distance a particle may leave its target, in meters; negative means unlimited.
		LocalVector<float> max_distances;
		float particle_mass = 0.01f;
		float stretch_compliance = 0.0f;
		float shear_compliance = 0.0f;
		float bend_compliance = 0.0001f;
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

	Setup setup;
	LocalVector<Vector3> targets;
	LocalVector<Vector3> previous_targets;
	LocalVector<Push> pushes;
	LocalVector<float> particle_inverse_masses;
	JPH::Array<JPH::Mat44> joint_matrices;

	float drive_stiffness = 0.0f;
	float drive_damping = 0.0f;
	float gravity_scale = 1.0f;
	float linear_damping = 0.1f;
	float friction = 0.4f;
	float vertex_radius = 0.005f;
	float max_distance_scale = 1.0f;
	int iterations = 6;

	int grabbed_particle = -1;
	Vector3 grab_target;
	bool targets_valid = false;
	bool hard_reset_requested = false;

	virtual JPH::BroadPhaseLayer _get_broad_phase_layer() const override;
	virtual JPH::ObjectLayer _get_object_layer() const override;
	virtual void _add_to_space() override;

	JPH::SoftBodySharedSettings *_create_shared_settings() const;
	void _skin_to_targets(bool p_hard_reset);
	void _apply_drive(float p_step);
	void _apply_pushes();
	void _apply_grab();
	void _update_motion_settings();

public:
	JoltCloth3D();
	virtual ~JoltCloth3D() override;

	void configure(const Setup &p_setup);
	int get_particle_count() const { return (int)setup.positions.size(); }

	void set_targets(const LocalVector<Vector3> &p_world_targets);
	void reset_to_targets();

	void set_drive(float p_stiffness, float p_damping);
	void set_gravity_scale(float p_scale);
	void set_linear_damping(float p_damping);
	void set_friction(float p_friction);
	void set_vertex_radius(float p_radius);
	void set_max_distance_scale(float p_scale);
	void set_iterations(int p_iterations);

	void push(const Vector3 &p_point, const Vector3 &p_velocity, float p_radius);
	void grab(int p_particle, const Vector3 &p_world_position);
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
