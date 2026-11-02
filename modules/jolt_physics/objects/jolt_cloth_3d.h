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

// lestoroer@physics

#pragma once

#include "jolt_object_3d.h"

#include "core/templates/local_vector.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

class JoltCloth3D final : public JoltObject3D {
public:
	// Particle data in world space at the moment the cloth enters the space.
	struct Setup {
		// Rest shape of the fabric: edge lengths and bends are measured here.
		LocalVector<Vector3> positions;
		LocalVector<int32_t> triangles;
		LocalVector<float> pin_weights;
		// Distance a particle may leave its target, in meters.
		LocalVector<float> max_distances;
		float particle_mass = 0.01f;
		float stretch_compliance = 0.0f;
		float shear_compliance = 0.0f;
		float bend_compliance = 0.0001f;
		// Compliance of bends folded in the pose by a crease angle or more; negative: as other bends.
		float crease_compliance = -1.0f;
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
	JPH::Ref<JPH::SoftBodySharedSettings> shared_settings;
	// Neighbors of particle i along the triangle edges: neighbor_list from neighbor_starts[i] to neighbor_starts[i + 1].
	LocalVector<uint32_t> neighbor_starts;
	LocalVector<uint32_t> neighbor_list;
	// What each particle touched in the last step and in the step before it: the normal of the surface, facing
	// the particle; zero without contact. Jolt forgets contacts before the next step, so the cloth keeps them.
	LocalVector<Vector3> contact_normals;
	LocalVector<Vector3> previous_contact_normals;
	// Particles squeezed between two surfaces stand still; see _hold_squeezed.
	LocalVector<uint32_t> squeezed;

	float drive_stiffness = 0.0f;
	float drive_damping = 0.0f;
	float gravity_scale = 1.0f;
	float linear_damping = 0.1f;
	float friction = 0.4f;
	float vertex_radius = 0.005f;
	float max_distance_scale = 1.0f;
	int iterations = 6;

	bool targets_valid = false;
	bool hard_reset_requested = false;
	bool rest_shape_dirty = false;

	virtual JPH::BroadPhaseLayer _get_broad_phase_layer() const override;
	virtual JPH::ObjectLayer _get_object_layer() const override;
	virtual void _add_to_space() override;

	bool _is_kinematic(uint32_t p_particle) const;
	JPH::SoftBodySharedSettings *_create_shared_settings() const;
	void _find_neighbors();
	void _skin_to_targets(bool p_hard_reset);
	void _reshape_rest();
	void _keep_attachments_from_pulling();
	void _hold_squeezed();
	bool _is_pulled_out(uint32_t p_particle, const JPH::Array<JPH::SoftBodyVertex> &p_vertices) const;
	void _free_squeezed();
	void _apply_gravity(float p_step);
	void _apply_drive(float p_step);
	void _apply_pushes();
	void _apply_crease_compliance(JPH::SoftBodySharedSettings &p_shared) const;
	void _update_motion_settings();

public:
	JoltCloth3D();
	virtual ~JoltCloth3D() override;

	// The targets start at the rest shape.
	void configure(const Setup &p_setup);
	int get_particle_count() const { return (int)setup.positions.size(); }

	// p_reshape: the targets form a new pose, not the same pose moved; the fabric then rests in it.
	void set_targets(const LocalVector<Vector3> &p_world_targets, bool p_reshape);
	void reset_to_targets();
	// Puts the particles of a sleeping cloth exactly at its targets, at rest, without waking it.
	void rest_in_targets();
	// Called by the contact listener at the end of a step in which particles touched something.
	void record_contacts(const JPH::SoftBodyManifold &p_manifold);

	void set_drive(float p_stiffness, float p_damping);
	void set_gravity_scale(float p_scale);
	void set_linear_damping(float p_damping);
	void set_friction(float p_friction);
	void set_vertex_radius(float p_radius);
	void set_max_distance_scale(float p_scale);
	void set_iterations(int p_iterations);

	void push(const Vector3 &p_point, const Vector3 &p_velocity, float p_radius);

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
