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

// lestoroer@physics

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

// Under gravity the fabric hangs at its full length in the pose and swings around the pins like a pendulum.
constexpr float LRA_SLACK = 1.0f;
constexpr float CREASE_ANGLE = 0.35f;
// A flick of the mouse swings the curtain, it does not throw it.
constexpr float PUSH_SPEED_LIMIT = 0.8f;
// A free particle this close to the pose only fights the pins and the bends and never sleeps.
constexpr float HARD_REACH = 0.035f;
constexpr float SQUEEZE_FACING = -0.5f;
constexpr float SQUEEZE_SLIP_STRETCH = 0.1f;

float push_falloff(float p_distance, float p_radius) {
	const float t = p_distance / p_radius;
	return Math::exp(-2.0f * t * t);
}

} // namespace

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
	squeezed.clear();
	contact_normals.resize(setup.positions.size());
	previous_contact_normals.resize(setup.positions.size());
	for (uint32_t i = 0; i < contact_normals.size(); i++) {
		contact_normals[i] = Vector3();
		previous_contact_normals[i] = Vector3();
	}
	JPH::SoftBodyCreationSettings settings(shared_settings.GetPtr(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), _get_object_layer());
	settings.mUserData = reinterpret_cast<JPH::uint64>(this);
	settings.mCollisionGroup = JPH::CollisionGroup(JoltGroupFilter::instance, group_id, sub_group_id);
	settings.mNumIterations = (JPH::uint32)iterations;
	settings.mLinearDamping = linear_damping;
	settings.mFriction = friction;
	// The space has no gravity of its own; the cloth applies it before each step, like Godot soft bodies.
	settings.mGravityFactor = 0.0f;
	settings.mVertexRadius = vertex_radius;
	settings.mMaxLinearVelocity = JoltProjectSettings::max_linear_velocity;
	settings.mUpdatePosition = false;
	// ClothBody3D decides when the cloth sleeps: Jolt would stop a slow swing on its way back to the pose.
	settings.mAllowSleeping = false;
	settings.mFacesDoubleSided = true;

	JPH::Body *new_jolt_body = space->add_object(*this, settings);
	if (new_jolt_body == nullptr) {
		return;
	}
	jolt_body = new_jolt_body;

	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	motion->SetSkinnedMaxDistanceMultiplier(max_distance_scale);
	hard_reset_requested = targets_valid;
}

bool JoltCloth3D::_is_kinematic(uint32_t p_particle) const {
	return setup.pin_weights[p_particle] > 0.5f || setup.max_distances[p_particle] < HARD_REACH;
}

JPH::SoftBodySharedSettings *JoltCloth3D::_create_shared_settings() const {
	JPH::SoftBodySharedSettings *shared = new JPH::SoftBodySharedSettings();
	const uint32_t particle_count = setup.positions.size();
	const float inverse_mass = 1.0f / MAX(setup.particle_mass, 0.0001f);

	shared->mVertices.reserve(particle_count);
	for (uint32_t i = 0; i < particle_count; i++) {
		JPH::Float3 position;
		to_jolt(setup.positions[i]).StoreFloat3(&position);
		shared->mVertices.push_back(JPH::SoftBodySharedSettings::Vertex(position, JPH::Float3(0, 0, 0), _is_kinematic(i) ? 0.0f : inverse_mass));
	}

	for (uint32_t i = 0; i + 2 < setup.triangles.size(); i += 3) {
		const JPH::SoftBodySharedSettings::Face face(setup.triangles[i], setup.triangles[i + 1], setup.triangles[i + 2]);
		if (!face.IsDegenerate()) {
			shared->AddFace(face);
		}
	}

	// Long range attachments keep fabric from stretching under its pins.
	const JPH::SoftBodySharedSettings::VertexAttributes attributes(setup.stretch_compliance, setup.shear_compliance, setup.bend_compliance, JPH::SoftBodySharedSettings::ELRAType::EuclideanDistance, LRA_SLACK);
	shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Dihedral);
	_apply_crease_compliance(*shared);

	// One joint per particle: the joint places the particle's skinned position at its pose target.
	shared->mInvBindMatrices.reserve(particle_count);
	shared->mSkinnedConstraints.reserve(particle_count);
	for (uint32_t i = 0; i < particle_count; i++) {
		const float max_distance = _is_kinematic(i) ? 0.0f : setup.max_distances[i];
		shared->mInvBindMatrices.push_back(JPH::SoftBodySharedSettings::InvBind(i, JPH::Mat44::sTranslation(-to_jolt(setup.positions[i]))));
		JPH::SoftBodySharedSettings::Skinned skinned(i, max_distance, FLT_MAX, 0.0f);
		skinned.mWeights[0] = JPH::SoftBodySharedSettings::SkinWeight(i, 1.0f);
		shared->mSkinnedConstraints.push_back(skinned);
	}
	shared->CalculateSkinnedConstraintNormals();

	shared->Optimize();
	return shared;
}

void JoltCloth3D::_find_neighbors() {
	const uint32_t particle_count = setup.positions.size();
	LocalVector<LocalVector<uint32_t>> neighbors;
	neighbors.resize(particle_count);
	for (uint32_t i = 0; i + 2 < setup.triangles.size(); i += 3) {
		for (uint32_t k = 0; k < 3; k++) {
			const uint32_t a = setup.triangles[i + k];
			const uint32_t b = setup.triangles[i + (k + 1) % 3];
			if (!neighbors[a].has(b)) {
				neighbors[a].push_back(b);
				neighbors[b].push_back(a);
			}
		}
	}
	neighbor_starts.resize(particle_count + 1);
	neighbor_list.clear();
	for (uint32_t i = 0; i < particle_count; i++) {
		neighbor_starts[i] = neighbor_list.size();
		for (uint32_t neighbor : neighbors[i]) {
			neighbor_list.push_back(neighbor);
		}
	}
	neighbor_starts[particle_count] = neighbor_list.size();
}

void JoltCloth3D::_skin_to_targets(bool p_hard_reset) {
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	const JPH::RVec3 origin = jolt_body->GetCenterOfMassPosition();
	const uint32_t particle_count = targets.size();

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
	_keep_attachments_from_pulling();
}

// When the pose brings fabric closer to its pin, the long range attachment would shorten faster than the
// fabric follows and jerk it toward the pin every step: the fabric shivers in ripples. An attachment shortens
// no further than its particle is from the pin now; it still keeps the fabric from stretching, and at rest it
// is the pose distance again.
void JoltCloth3D::_keep_attachments_from_pulling() {
	const JPH::SoftBodyMotionProperties *motion = static_cast<const JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	const JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	for (JPH::SoftBodySharedSettings::LRA &attachment : shared_settings->mLRAConstraints) {
		const float current = (vertices[attachment.mVertex[0]].mPosition - vertices[attachment.mVertex[1]].mPosition).Length();
		attachment.mMaxDistance = MAX(attachment.mMaxDistance, current);
	}
}

void JoltCloth3D::_apply_crease_compliance(JPH::SoftBodySharedSettings &p_shared) const {
	if (setup.crease_compliance < 0.0f) {
		return;
	}
	for (JPH::SoftBodySharedSettings::DihedralBend &bend : p_shared.mDihedralBendConstraints) {
		bend.mCompliance = Math::abs(bend.mInitialAngle) >= CREASE_ANGLE ? setup.crease_compliance : setup.bend_compliance;
	}
}

// Jolt pushes a particle out of one surface at a time. Fabric squeezed between two, like a curtain between the
// wall and a piece of furniture, would jump from one to the other every step and shiver. A squeezed particle
// stands still, as if held by the friction of both, until the fabric next to it pulls it out.
void JoltCloth3D::_hold_squeezed() {
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	for (uint32_t k = 0; k < squeezed.size();) {
		const uint32_t particle = squeezed[k];
		if (_is_pulled_out(particle, vertices)) {
			vertices[particle].mInvMass = particle_inverse_masses[particle];
			squeezed.remove_at_unordered(k);
			continue;
		}
		vertices[particle].mVelocity = JPH::Vec3::sZero();
		k++;
	}
	for (uint32_t i = 0; i < vertices.size(); i++) {
		JPH::SoftBodyVertex &vertex = vertices[i];
		// Only particles with their own mass: pins and squeezed particles do not collide.
		if (vertex.mInvMass > 0.0f && vertex.mInvMass == particle_inverse_masses[i] && contact_normals[i].dot(previous_contact_normals[i]) < SQUEEZE_FACING) {
			vertex.mInvMass = 0.0f;
			vertex.mVelocity = JPH::Vec3::sZero();
			squeezed.push_back(i);
		}
	}
}

// The pose leaves: the fabric next to the squeezed particle stretches.
bool JoltCloth3D::_is_pulled_out(uint32_t p_particle, const JPH::Array<JPH::SoftBodyVertex> &p_vertices) const {
	for (uint32_t k = neighbor_starts[p_particle]; k < neighbor_starts[p_particle + 1]; k++) {
		const uint32_t neighbor = neighbor_list[k];
		const float rest = float(targets[p_particle].distance_to(targets[neighbor]));
		if ((p_vertices[p_particle].mPosition - p_vertices[neighbor].mPosition).Length() > rest * (1.0f + SQUEEZE_SLIP_STRETCH)) {
			return true;
		}
	}
	return false;
}

void JoltCloth3D::_free_squeezed() {
	if (in_space()) {
		JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
		JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
		for (uint32_t particle : squeezed) {
			vertices[particle].mInvMass = particle_inverse_masses[particle];
		}
	}
	squeezed.clear();
}

void JoltCloth3D::_apply_gravity(float p_step) {
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
	if (drive_stiffness <= 0.0f) {
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
		const JPH::Vec3 acceleration = drive_stiffness * (target - vertex.mPosition) + drive_damping * (target_velocity - vertex.mVelocity);
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
			Vector3 velocity =to_godot(vertex.mVelocity) + push.velocity * push_falloff(distance, push.radius);
			const real_t along = velocity.dot(direction);
			if (along > PUSH_SPEED_LIMIT) {
				velocity -= direction * (along - PUSH_SPEED_LIMIT);
			}
			vertex.mVelocity = to_jolt(velocity);
		}
	}
	pushes.clear();
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
	ERR_FAIL_COND_MSG(p_setup.triangles.size() % 3 != 0, "Cloth triangles must come in groups of three indices.");

	setup = p_setup;
	_find_neighbors();
	particle_inverse_masses.resize(particle_count);
	const float inverse_mass = 1.0f / MAX(setup.particle_mass, 0.0001f);
	for (uint32_t i = 0; i < particle_count; i++) {
		particle_inverse_masses[i] = _is_kinematic(i) ? 0.0f : inverse_mass;
	}
	targets = setup.positions;
	previous_targets = setup.positions;
	targets_valid = true;

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
	_free_squeezed();
	hard_reset_requested = true;
	previous_targets = targets;
	wake_up();
}

void JoltCloth3D::rest_in_targets() {
	if (!in_space()) {
		return;
	}
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const JPH::RVec3 origin = jolt_body->GetCenterOfMassPosition();
	for (uint32_t i = 0; i < vertices.size(); i++) {
		vertices[i].mPosition = JPH::Vec3(to_jolt_r(targets[i]) - origin);
		vertices[i].mPreviousPosition = vertices[i].mPosition;
		vertices[i].mVelocity = JPH::Vec3::sZero();
	}
}

void JoltCloth3D::record_contacts(const JPH::SoftBodyManifold &p_manifold) {
	const JPH::Array<JPH::SoftBodyVertex> &vertices = p_manifold.GetVertices();
	if (vertices.size() != contact_normals.size()) {
		return;
	}
	for (uint32_t i = 0; i < vertices.size(); i++) {
		if (p_manifold.HasContact(vertices[i])) {
			contact_normals[i] = to_godot(vertices[i].mCollisionPlane.GetNormal());
		}
	}
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

// Cloth collides with rigid bodies only; areas, soft bodies and other cloth pass through it.
bool JoltCloth3D::interacts_with(const JoltObject3D &p_other) const {
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

	if (rest_shape_dirty) {
		_reshape_rest();
	}
	rest_shape_dirty = false;
	if (targets_valid) {
		_skin_to_targets(hard_reset_requested);
		hard_reset_requested = false;
	}
	_hold_squeezed();
	_apply_gravity(p_step);
	_apply_drive(p_step);
	_apply_pushes();
	previous_targets = targets;
	// The coming step records its own contacts.
	previous_contact_normals = contact_normals;
	for (Vector3 &normal : contact_normals) {
		normal = Vector3();
	}
}
