/**************************************************************************/
/*  jolt_cloth_3d.cpp                                                     */
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

#include "jolt_cloth_3d.h"

#include "../jolt_project_settings.h"
#include "../misc/jolt_type_conversions.h"
#include "../spaces/jolt_broad_phase_layer.h"
#include "../spaces/jolt_space_3d.h"
#include "jolt_area_3d.h"
#include "jolt_body_3d.h"
#include "jolt_group_filter.h"

#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

namespace {

// Long range attachments hold every particle at most as far from its pin as in the current pose:
// under gravity the fabric hangs at its full length and swings around the pins like a pendulum.
constexpr float LRA_SLACK = 1.0f;
// Bends folded in the pose by this angle or more are creases of the folds and get their own compliance.
constexpr float CREASE_ANGLE = 0.35f;
// The hand holding the fabric: its target follows the cursor at most this fast, pulls the held patch
// with a damped spring and never further than the leash. Released, the fabric does not jerk.
constexpr float GRIP_SPEED = 3.0f;
constexpr float GRIP_STIFFNESS = 6400.0f;
constexpr float GRIP_DAMPING = 60.0f;
constexpr float GRIP_LEASH = 0.08f;
// Brushing adds to the fabric's motion, but never past this speed along the brush: a flick of the
// mouse swings the curtain, it does not throw it.
constexpr float PUSH_SPEED_LIMIT = 0.8f;
// A held particle leaves its target by at most this, like fabric in the cuff of a tied curtain. While
// a hold eases in or out, the allowed distance goes from a meter to this.
constexpr float HOLD_REACH = 0.01f;
constexpr float HOLD_EASE_REACH = 1.0f;
// A particle allowed to leave its pose by less than this follows the pose exactly: next to pins it
// barely moves anyway, and as a free particle it fights the pins and the bends and never sleeps.
constexpr float HARD_REACH = 0.035f;

// Gaussian falloff of a cursor push with distance.
float push_falloff(float p_distance, float p_radius) {
	const float t = p_distance / p_radius;
	return Math::exp(-2.0f * t * t);
}

} // namespace

HashMap<ObjectID, uint64_t> JoltCloth3D::resting_shape_stamps;
uint64_t JoltCloth3D::stamp_counter = 0;

void JoltCloth3D::set_resting_shape_stamp(ObjectID p_instance, uint64_t p_stamp) {
	if (p_stamp == 0) {
		resting_shape_stamps.erase(p_instance);
	} else {
		resting_shape_stamps[p_instance] = p_stamp;
	}
}

uint64_t JoltCloth3D::get_resting_shape_stamp(ObjectID p_instance) {
	const uint64_t *stamp = resting_shape_stamps.getptr(p_instance);
	return stamp != nullptr ? *stamp : 0;
}

JPH::BroadPhaseLayer JoltCloth3D::_get_broad_phase_layer() const {
	return JoltBroadPhaseLayer::BODY_DYNAMIC;
}

JPH::ObjectLayer JoltCloth3D::_get_object_layer() const {
	ERR_FAIL_NULL_V(space, 0);
	return space->map_to_object_layer(_get_broad_phase_layer(), collision_layer, collision_mask);
}

void JoltCloth3D::_add_to_space() {
	if (unlikely(space == nullptr || setup.positions.is_empty())) {
		return;
	}

	JPH::CollisionGroup::GroupID group_id = 0;
	JPH::CollisionGroup::SubGroupID sub_group_id = 0;
	JoltGroupFilter::encode_object(this, group_id, sub_group_id);

	// The body stays at the origin, so particle positions are world positions.
	shared_settings = _create_shared_settings();
	hold_dirty = true;
	JPH::SoftBodyCreationSettings settings(shared_settings.GetPtr(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), _get_object_layer());
	settings.mUserData = reinterpret_cast<JPH::uint64>(this);
	// Jolt asks the group filter whether the cloth touches a body; without it the cloth would also
	// collide with the shape it leaves for other cloth to rest on.
	settings.mCollisionGroup = JPH::CollisionGroup(JoltGroupFilter::instance, group_id, sub_group_id);
	settings.mNumIterations = (JPH::uint32)iterations;
	settings.mLinearDamping = linear_damping;
	settings.mFriction = friction;
	// The space has no gravity of its own; the cloth applies it before each step, like Godot soft bodies.
	settings.mGravityFactor = 0.0f;
	settings.mVertexRadius = vertex_radius;
	settings.mMaxLinearVelocity = JoltProjectSettings::max_linear_velocity;
	settings.mUpdatePosition = false;
	// ClothBody3D decides when posed cloth sleeps: Jolt would stop a slow swing on its way back to
	// the pose. Free cloth also sleeps by Jolt once it is slow.
	settings.mAllowSleeping = !setup.follow_targets;
	settings.mFacesDoubleSided = true;

	JPH::Body *new_jolt_body = space->add_object(*this, settings);
	if (new_jolt_body == nullptr) {
		return;
	}
	jolt_body = new_jolt_body;

	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	motion->SetSkinnedMaxDistanceMultiplier(max_distance_scale);
	if (setup.start_positions.is_empty()) {
		hard_reset_requested = targets_valid;
		return;
	}
	hard_reset_requested = false;
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const JPH::RVec3 origin = jolt_body->GetCenterOfMassPosition();
	for (uint32_t i = 0; i < vertices.size(); i++) {
		vertices[i].mPosition = JPH::Vec3(to_jolt_r(setup.start_positions[i]) - origin);
		vertices[i].mPreviousPosition = vertices[i].mPosition;
		vertices[i].mVelocity = JPH::Vec3::sZero();
	}
	// Re-entering a space later starts at rest.
	setup.start_positions.clear();
}

bool JoltCloth3D::_is_kinematic(uint32_t p_particle) const {
	if (setup.pin_weights[p_particle] > 0.5f) {
		return true;
	}
	return setup.follow_targets && setup.max_distances[p_particle] >= 0.0f && setup.max_distances[p_particle] < HARD_REACH;
}

JPH::SoftBodySharedSettings *JoltCloth3D::_create_shared_settings() const {
	JPH::SoftBodySharedSettings *shared = new JPH::SoftBodySharedSettings();
	const uint32_t particle_count = setup.positions.size();
	const float inverse_mass = 1.0f / MAX(setup.particle_mass, 0.0001f);

	bool has_pins = false;
	shared->mVertices.reserve(particle_count);
	for (uint32_t i = 0; i < particle_count; i++) {
		const bool pinned = _is_kinematic(i);
		has_pins = has_pins || pinned;
		JPH::Float3 position;
		to_jolt(setup.positions[i]).StoreFloat3(&position);
		shared->mVertices.push_back(JPH::SoftBodySharedSettings::Vertex(position, JPH::Float3(0, 0, 0), pinned ? 0.0f : inverse_mass));
	}

	for (uint32_t i = 0; i + 2 < setup.triangles.size(); i += 3) {
		const JPH::SoftBodySharedSettings::Face face(setup.triangles[i], setup.triangles[i + 1], setup.triangles[i + 2]);
		if (!face.IsDegenerate()) {
			shared->AddFace(face);
		}
	}

	// Long range attachments keep fabric from stretching under its pins.
	const JPH::SoftBodySharedSettings::ELRAType lra = has_pins ? JPH::SoftBodySharedSettings::ELRAType::EuclideanDistance : JPH::SoftBodySharedSettings::ELRAType::None;
	const JPH::SoftBodySharedSettings::VertexAttributes attributes(setup.stretch_compliance, setup.shear_compliance, setup.bend_compliance, lra, LRA_SLACK);
	shared->CreateConstraints(&attributes, 1, setup.dihedral_bends ? JPH::SoftBodySharedSettings::EBendType::Dihedral : JPH::SoftBodySharedSettings::EBendType::Distance);
	_apply_crease_compliance(*shared);

	if (setup.follow_targets) {
		// One joint per particle: the joint places the particle's skinned position at its pose target.
		shared->mInvBindMatrices.reserve(particle_count);
		shared->mSkinnedConstraints.reserve(particle_count);
		const float backstop_distance = setup.backstop_distance < 0.0f ? FLT_MAX : setup.backstop_distance;
		for (uint32_t i = 0; i < particle_count; i++) {
			const bool pinned = _is_kinematic(i);
			const float max_distance = pinned ? 0.0f : (setup.max_distances[i] < 0.0f ? FLT_MAX : setup.max_distances[i]);
			shared->mInvBindMatrices.push_back(JPH::SoftBodySharedSettings::InvBind(i, JPH::Mat44::sTranslation(-to_jolt(setup.positions[i]))));
			JPH::SoftBodySharedSettings::Skinned skinned(i, max_distance, backstop_distance, setup.backstop_radius);
			skinned.mWeights[0] = JPH::SoftBodySharedSettings::SkinWeight(i, 1.0f);
			shared->mSkinnedConstraints.push_back(skinned);
		}
		shared->CalculateSkinnedConstraintNormals();
	}

	shared->Optimize();
	return shared;
}

void JoltCloth3D::_skin_to_targets(bool p_hard_reset) {
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	const JPH::RVec3 origin = jolt_body->GetCenterOfMassPosition();
	const uint32_t particle_count = targets.size();

	if (!setup.follow_targets) {
		if (p_hard_reset) {
			JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
			for (uint32_t i = 0; i < particle_count; i++) {
				vertices[i].mPosition = JPH::Vec3(to_jolt_r(targets[i]) - origin);
				vertices[i].mPreviousPosition = vertices[i].mPosition;
				vertices[i].mVelocity = JPH::Vec3::sZero();
			}
		}
		return;
	}

	joint_matrices.resize(particle_count);
	for (uint32_t i = 0; i < particle_count; i++) {
		joint_matrices[i] = JPH::Mat44::sTranslation(JPH::Vec3(to_jolt_r(targets[i]) - origin));
	}
	motion->SkinVertices(jolt_body->GetCenterOfMassTransform(), joint_matrices.data(), particle_count, p_hard_reset, space->get_temp_allocator());
}

void JoltCloth3D::_reshape_rest() {
	// Edges and bends rest in the current pose, so a gathered curtain does not push its folds apart.
	for (uint32_t i = 0; i < targets.size(); i++) {
		const JPH::Vec3 position = to_jolt(targets[i]);
		position.StoreFloat3(&shared_settings->mVertices[i].mPosition);
		// Skinning starts from the bind position, so the inverse bind moves with it.
		shared_settings->mInvBindMatrices[i].mInvBind = JPH::Mat44::sTranslation(-position);
	}
	shared_settings->CalculateEdgeLengths();
	shared_settings->CalculateBendConstraintConstants();
	_apply_crease_compliance(*shared_settings);
	shared_settings->CalculateLRALengths(LRA_SLACK);
}

void JoltCloth3D::_apply_crease_compliance(JPH::SoftBodySharedSettings &p_shared) const {
	if (setup.crease_compliance < 0.0f) {
		return;
	}
	for (JPH::SoftBodySharedSettings::DihedralBend &bend : p_shared.mDihedralBendConstraints) {
		bend.mCompliance = Math::abs(bend.mInitialAngle) >= CREASE_ANGLE ? setup.crease_compliance : setup.bend_compliance;
	}
}

void JoltCloth3D::_apply_gravity(float p_step) {
	// Cloth that follows its pose hangs under gravity too: the pins and long range attachments hold
	// it at the length of the pose, and the weak drive only removes slow drift.
	if (gravity_scale == 0.0f) {
		return;
	}

	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	const Vector3 gravity = space->get_default_area()->compute_gravity(to_godot(jolt_body->GetCenterOfMassPosition()));
	const JPH::Vec3 velocity_change = to_jolt(gravity * (gravity_scale * p_step));
	for (JPH::SoftBodyVertex &vertex : motion->GetVertices()) {
		if (vertex.mInvMass != 0.0f) {
			vertex.mVelocity += velocity_change;
		}
	}
}

void JoltCloth3D::_apply_drive(float p_step) {
	if (!setup.follow_targets || drive_stiffness <= 0.0f) {
		return;
	}

	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const JPH::RVec3 origin = jolt_body->GetCenterOfMassPosition();
	const float inverse_step = 1.0f / p_step;

	for (uint32_t i = 0; i < targets.size(); i++) {
		JPH::SoftBodyVertex &vertex = vertices[i];
		if (vertex.mInvMass == 0.0f) {
			continue;
		}
		const JPH::Vec3 target = JPH::Vec3(to_jolt_r(targets[i]) - origin);
		const JPH::Vec3 target_velocity = to_jolt((targets[i] - previous_targets[i]) * inverse_step);
		JPH::Vec3 acceleration = drive_stiffness * (target - vertex.mPosition) + drive_damping * (target_velocity - vertex.mVelocity);
		if (vertex.mHasContact) {
			// The drive does not press a particle into what it touches: a cover over a pillow rests on
			// the pillow instead of trembling between it and the pose, and comes back when it is gone.
			const JPH::Vec3 normal = vertex.mCollisionPlane.GetNormal();
			const float into = acceleration.Dot(normal);
			if (into < 0.0f) {
				acceleration -= into * normal;
			}
		}
		vertex.mVelocity += acceleration * p_step;
	}
}

void JoltCloth3D::_apply_pushes() {
	if (pushes.is_empty()) {
		return;
	}

	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const Vector3 origin = to_godot(jolt_body->GetCenterOfMassPosition());

	for (const Push &push : pushes) {
		const real_t speed = push.velocity.length();
		if (speed == 0.0) {
			continue;
		}
		const Vector3 direction = push.velocity / speed;
		for (JPH::SoftBodyVertex &vertex : vertices) {
			if (vertex.mInvMass == 0.0f) {
				continue;
			}
			const float distance = (origin + to_godot(vertex.mPosition)).distance_to(push.point);
			if (distance >= 2.0f * push.radius) {
				continue;
			}
			// Brushing again and again swings the fabric more, but a cursor moving over free cloth
			// does not fling it.
			Vector3 velocity = to_godot(vertex.mVelocity) + push.velocity * push_falloff(distance, push.radius);
			const real_t along = velocity.dot(direction);
			if (along > PUSH_SPEED_LIMIT) {
				velocity -= direction * (along - PUSH_SPEED_LIMIT);
			}
			vertex.mVelocity = to_jolt(velocity);
		}
	}
	pushes.clear();
}

void JoltCloth3D::_apply_hold() {
	if (!setup.follow_targets || setup.hold_weights.is_empty()) {
		return;
	}
	for (JPH::SoftBodySharedSettings::Skinned &skinned : shared_settings->mSkinnedConstraints) {
		const uint32_t particle = skinned.mVertex;
		if (_is_kinematic(particle)) {
			continue;
		}
		const float free = setup.max_distances[particle] < 0.0f ? FLT_MAX : setup.max_distances[particle];
		const float held = hold * setup.hold_weights[particle];
		skinned.mMaxDistance = held > 0.0f ? Math::lerp(MIN(free, HOLD_EASE_REACH), HOLD_REACH, held) : free;
	}
}

void JoltCloth3D::_apply_grab(float p_step) {
	if (grabbed_particle < 0) {
		return;
	}

	grab_target = grab_target.move_toward(grab_requested, GRIP_SPEED * p_step);
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const Vector3 origin = to_godot(jolt_body->GetCenterOfMassPosition());
	// An implicit damped spring stays stable at the physics step for any stiffness.
	const float denominator = 1.0f + GRIP_DAMPING * p_step + GRIP_STIFFNESS * p_step * p_step;
	for (uint32_t k = 0; k < grip_particles.size(); k++) {
		JPH::SoftBodyVertex &vertex = vertices[grip_particles[k]];
		const Vector3 gap = (grab_target + grip_offsets[k] - (origin + to_godot(vertex.mPosition))).limit_length(GRIP_LEASH);
		vertex.mVelocity = to_jolt((to_godot(vertex.mVelocity) + gap * (GRIP_STIFFNESS * p_step)) / denominator);
	}
}

void JoltCloth3D::_update_motion_settings() {
	if (!in_space()) {
		return;
	}

	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	motion->SetNumIterations((JPH::uint32)iterations);
	motion->SetLinearDamping(linear_damping);
	motion->SetVertexRadius(vertex_radius);
	motion->SetSkinnedMaxDistanceMultiplier(max_distance_scale);
	jolt_body->SetFriction(friction);
}

JoltCloth3D::JoltCloth3D() :
		JoltObject3D(OBJECT_TYPE_CLOTH) {
}

JoltCloth3D::~JoltCloth3D() = default;

void JoltCloth3D::configure(const Setup &p_setup) {
	const uint32_t particle_count = p_setup.positions.size();
	ERR_FAIL_COND_MSG(p_setup.pin_weights.size() != particle_count || p_setup.max_distances.size() != particle_count, "Cloth particle attributes must match the particle count.");
	ERR_FAIL_COND_MSG(!p_setup.start_positions.is_empty() && p_setup.start_positions.size() != particle_count, "Cloth start positions must match the particle count.");
	ERR_FAIL_COND_MSG(!p_setup.hold_weights.is_empty() && p_setup.hold_weights.size() != particle_count, "Cloth hold weights must match the particle count.");
	ERR_FAIL_COND_MSG(p_setup.triangles.size() % 3 != 0, "Cloth triangles must come in groups of three indices.");

	setup = p_setup;
	particle_inverse_masses.resize(particle_count);
	const float inverse_mass = 1.0f / MAX(setup.particle_mass, 0.0001f);
	for (uint32_t i = 0; i < particle_count; i++) {
		particle_inverse_masses[i] = _is_kinematic(i) ? 0.0f : inverse_mass;
	}
	targets = setup.positions;
	previous_targets = setup.positions;
	targets_valid = true;
	grabbed_particle = -1;

	if (space != nullptr) {
		_reset_space();
	}
}

void JoltCloth3D::set_targets(const LocalVector<Vector3> &p_world_targets, bool p_reshape) {
	ERR_FAIL_COND(p_world_targets.size() != setup.positions.size());
	targets = p_world_targets;
	targets_valid = true;
	rest_shape_dirty = rest_shape_dirty || p_reshape;
}

void JoltCloth3D::reset_to_targets() {
	hard_reset_requested = true;
	previous_targets = targets;
	wake_up();
}

void JoltCloth3D::set_drive(float p_stiffness, float p_damping) {
	drive_stiffness = MAX(p_stiffness, 0.0f);
	drive_damping = MAX(p_damping, 0.0f);
}

void JoltCloth3D::set_gravity_scale(float p_scale) {
	gravity_scale = p_scale;
	_update_motion_settings();
}

void JoltCloth3D::set_linear_damping(float p_damping) {
	linear_damping = MAX(p_damping, 0.0f);
	_update_motion_settings();
}

void JoltCloth3D::set_friction(float p_friction) {
	friction = MAX(p_friction, 0.0f);
	_update_motion_settings();
}

void JoltCloth3D::set_vertex_radius(float p_radius) {
	vertex_radius = MAX(p_radius, 0.0f);
	_update_motion_settings();
}

void JoltCloth3D::set_hold(float p_hold) {
	const float clamped = CLAMP(p_hold, 0.0f, 1.0f);
	if (clamped != hold) {
		hold = clamped;
		hold_dirty = true;
	}
}

void JoltCloth3D::set_max_distance_scale(float p_scale) {
	max_distance_scale = MAX(p_scale, 0.0f);
	_update_motion_settings();
}

void JoltCloth3D::set_iterations(int p_iterations) {
	iterations = MAX(p_iterations, 1);
	_update_motion_settings();
}

void JoltCloth3D::push(const Vector3 &p_point, const Vector3 &p_velocity, float p_radius) {
	ERR_FAIL_COND(p_radius <= 0.0f);
	pushes.push_back({ p_point, p_velocity, p_radius });
	wake_up();
}

int JoltCloth3D::grab(const Vector3 &p_world_position) {
	release_grab();
	LocalVector<Vector3> positions;
	get_positions(positions);
	int nearest = -1;
	real_t nearest_distance = Math::INF;
	for (uint32_t i = 0; i < positions.size(); i++) {
		const real_t distance = positions[i].distance_squared_to(p_world_position);
		if (particle_inverse_masses[i] > 0.0f && distance < nearest_distance) {
			nearest_distance = distance;
			nearest = i;
		}
	}
	if (nearest < 0) {
		return -1;
	}
	// The hand holds the particle with its free neighbors, so the fabric does not stretch into a tent.
	grabbed_particle = nearest;
	grip_particles.push_back(nearest);
	for (uint32_t i = 0; i + 2 < setup.triangles.size(); i += 3) {
		for (int corner = 0; corner < 3; corner++) {
			if (setup.triangles[i + corner] != nearest) {
				continue;
			}
			for (int other = 1; other < 3; other++) {
				const uint32_t neighbor = setup.triangles[i + (corner + other) % 3];
				if (particle_inverse_masses[neighbor] > 0.0f && !grip_particles.has(neighbor)) {
					grip_particles.push_back(neighbor);
				}
			}
		}
	}
	grab_target = positions[nearest];
	grab_requested = p_world_position;
	for (uint32_t particle : grip_particles) {
		grip_offsets.push_back(positions[particle] - grab_target);
	}
	wake_up();
	return nearest;
}

void JoltCloth3D::move_grab(const Vector3 &p_world_position) {
	grab_requested = p_world_position;
	wake_up();
}

void JoltCloth3D::release_grab() {
	grabbed_particle = -1;
	grip_particles.clear();
	grip_offsets.clear();
}

bool JoltCloth3D::is_active() const {
	return in_space() && jolt_body->IsActive();
}

void JoltCloth3D::wake_up() {
	if (in_space() && jolt_body->IsInBroadPhase()) {
		space->get_body_iface().ActivateBody(jolt_body->GetID());
	}
}

void JoltCloth3D::put_to_sleep() {
	if (in_space() && jolt_body->IsInBroadPhase()) {
		space->get_body_iface().DeactivateBody(jolt_body->GetID());
	}
}

void JoltCloth3D::get_positions(LocalVector<Vector3> &r_positions) const {
	if (!in_space()) {
		r_positions = targets;
		return;
	}

	const JPH::SoftBodyMotionProperties *motion = static_cast<const JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	const JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const Vector3 origin = to_godot(jolt_body->GetCenterOfMassPosition());
	r_positions.resize(vertices.size());
	for (uint32_t i = 0; i < vertices.size(); i++) {
		r_positions[i] = origin + to_godot(vertices[i].mPosition);
	}
}

bool JoltCloth3D::interacts_with(const JoltObject3D &p_other) const {
	// Cloth collides with rigid bodies only; areas, soft bodies and other cloth pass through it.
	// The body a resting cloth leaves for other cloth to lie on carries the cloth's instance; the
	// cloth itself moves through it.
	if (p_other.get_instance_id() == get_instance_id()) {
		return false;
	}
	// A resting shape that appeared after this cloth was let go is not solid for it: both cloths
	// moved and went through each other, and this one would get stuck inside the other.
	const uint64_t *resting_stamp = resting_shape_stamps.getptr(p_other.get_instance_id());
	if (resting_stamp != nullptr && *resting_stamp > release_stamp) {
		return false;
	}
	return p_other.is_body() && (can_collide_with(p_other) || p_other.can_collide_with(*this));
}

bool JoltCloth3D::can_interact_with(const JoltBody3D &p_other) const {
	return interacts_with(static_cast<const JoltObject3D &>(p_other));
}

bool JoltCloth3D::can_interact_with(const JoltSoftBody3D &p_other) const {
	return false;
}

bool JoltCloth3D::can_interact_with(const JoltArea3D &p_other) const {
	return false;
}

void JoltCloth3D::pre_step(float p_step) {
	JoltObject3D::pre_step(p_step);

	if (hold_dirty) {
		_apply_hold();
		hold_dirty = false;
	}
	if (rest_shape_dirty && setup.follow_targets) {
		_reshape_rest();
	}
	rest_shape_dirty = false;
	if (targets_valid) {
		_skin_to_targets(hard_reset_requested);
		hard_reset_requested = false;
	}
	_apply_gravity(p_step);
	_apply_drive(p_step);
	_apply_pushes();
	_apply_grab(p_step);
	previous_targets = targets;
}
