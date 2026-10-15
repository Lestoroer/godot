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

#include "core/templates/hash_set.h"

#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

namespace {

// Long range attachments hold every particle at most as far from its pin as in the current pose:
// under gravity the fabric hangs at its full length and swings around the pins like a pendulum.
constexpr float LRA_SLACK = 1.0f;
// Bends folded in the pose by this angle or more are creases of the folds and get their own compliance.
constexpr float CREASE_ANGLE = 0.35f;
// The hand holding the fabric follows the cursor at most this fast. Let go, the fabric keeps the hand's
// motion up to GRIP_RELEASE_SPEED: a flick of the mouse does not throw it.
constexpr float GRIP_SPEED = 3.0f;
constexpr float GRIP_RELEASE_SPEED = 0.8f;
// The held patch is this many times heavier than the fabric, so the fabric's weight does not pull it from
// the hand. It is not weightless: Jolt collides only particles with mass, and a weightless hand would drag
// the fabric through walls.
constexpr float GRIP_MASS_SCALE = 1000.0f;
// Brushing adds to the fabric's motion, but never past this speed along the brush: a flick of the
// mouse swings the curtain, it does not throw it.
constexpr float PUSH_SPEED_LIMIT = 0.8f;
// A held particle leaves its target by at most this, like fabric in the cuff of a tied curtain. While
// a hold eases in or out, the allowed distance goes from a meter to this.
constexpr float HOLD_REACH = 0.01f;
constexpr float HOLD_EASE_REACH = 1.0f;
// A closing holder brings the fabric it gathers no faster than this: a sudden hold would yank the
// fabric into it.
constexpr float HOLD_CLOSE_SPEED = 0.4f;
// A particle allowed to leave its pose by less than this follows the pose exactly: next to pins it
// barely moves anyway, and as a free particle it fights the pins and the bends and never sleeps.
constexpr float HARD_REACH = 0.035f;
// A particle pushed out of surfaces facing opposite ways in two steps in a row is squeezed between them.
constexpr float SQUEEZE_FACING = -0.5f;
// Squeezed fabric slips out once the fabric next to it is stretched by this share.
constexpr float SQUEEZE_SLIP_STRETCH = 0.1f;

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
	// A new body has the particles' own masses: a hand from before holds nothing, nothing is squeezed.
	_forget_grip();
	squeezed.clear();
	contact_normals.resize(setup.positions.size());
	previous_contact_normals.resize(setup.positions.size());
	for (uint32_t i = 0; i < contact_normals.size(); i++) {
		contact_normals[i] = Vector3();
		previous_contact_normals[i] = Vector3();
	}
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
	if (setup.shear_across_folds) {
		_shear_cell_diagonals(*shared);
	}
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

// Woven fabric does not stretch along its threads but shears easily across them. Jolt takes the diagonal of a
// cell for a shear edge only where the cell is nearly flat; across the folds of a curtain the diagonals would
// be as stiff as the threads, the fabric could not shear, and a pull would stretch it like rubber instead.
// Here every cell diagonal, the longest edge of both its triangles, shears, and so does the other diagonal.
// In a bent cell both diagonals also hold its bend: fabric with soft shear bends anyway, but nearly rigid
// shear would keep the cell from bending, so cloth turns this on with its shear soft.
void JoltCloth3D::_shear_cell_diagonals(JPH::SoftBodySharedSettings &p_shared) const {
	const auto edge_key = [](uint32_t p_a, uint32_t p_b) {
		return (uint64_t(MIN(p_a, p_b)) << 32) | uint64_t(MAX(p_a, p_b));
	};
	struct Side {
		uint32_t opposite = 0;
		bool longest = false;
	};
	HashMap<uint64_t, LocalVector<Side>> sides;
	HashSet<uint64_t> threads;
	for (uint32_t i = 0; i + 2 < setup.triangles.size(); i += 3) {
		for (uint32_t k = 0; k < 3; k++) {
			const uint32_t a = setup.triangles[i + k];
			const uint32_t b = setup.triangles[i + (k + 1) % 3];
			const uint32_t c = setup.triangles[i + (k + 2) % 3];
			const real_t length = setup.positions[a].distance_to(setup.positions[b]);
			const bool longest = length >= setup.positions[b].distance_to(setup.positions[c]) && length >= setup.positions[c].distance_to(setup.positions[a]);
			sides[edge_key(a, b)].push_back({ c, longest });
			threads.insert(edge_key(a, b));
		}
	}
	HashSet<uint64_t> diagonals;
	for (const KeyValue<uint64_t, LocalVector<Side>> &edge : sides) {
		if (edge.value.size() == 2 && edge.value[0].longest && edge.value[1].longest) {
			diagonals.insert(edge.key);
			diagonals.insert(edge_key(edge.value[0].opposite, edge.value[1].opposite));
		}
	}

	HashSet<uint64_t> present;
	for (JPH::SoftBodySharedSettings::Edge &edge : p_shared.mEdgeConstraints) {
		const uint64_t key = edge_key(edge.mVertex[0], edge.mVertex[1]);
		present.insert(key);
		if (diagonals.has(key)) {
			edge.mCompliance = setup.shear_compliance;
		} else if (threads.has(key)) {
			edge.mCompliance = setup.stretch_compliance;
		}
	}
	for (const uint64_t key : diagonals) {
		const uint32_t a = uint32_t(key >> 32);
		const uint32_t b = uint32_t(key & 0xffffffff);
		if (!present.has(key) && (p_shared.mVertices[a].mInvMass > 0.0f || p_shared.mVertices[b].mInvMass > 0.0f)) {
			p_shared.mEdgeConstraints.push_back(JPH::SoftBodySharedSettings::Edge(a, b, setup.shear_compliance));
		}
	}
	p_shared.CalculateEdgeLengths();
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
	_keep_attachments_from_pulling();
}

// When the pose brings fabric closer to its pin, like a curtain released from its cuff that falls under its
// rings, the long range attachment would shorten faster than the fabric follows and jerk it toward the pin
// every step: the fabric shivers in ripples. An attachment shortens no further than its particle is from the
// pin now; it still keeps the fabric from stretching, and at rest it is the pose distance again.
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

// Jolt pushes a particle out of one surface at a time. Fabric squeezed between two, like a cover under a folded
// sweater on the mattress, would jump from one to the other every step and shiver. A squeezed particle stands
// still, as if held by the friction of both, until the fabric next to it pulls it out.
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
		// Only particles with their own mass: the hand holds its patch, pins and squeezed particles do not collide.
		if (vertex.mInvMass > 0.0f && vertex.mInvMass == particle_inverse_masses[i] && contact_normals[i].dot(previous_contact_normals[i]) < SQUEEZE_FACING) {
			vertex.mInvMass = 0.0f;
			vertex.mVelocity = JPH::Vec3::sZero();
			squeezed.push_back(i);
		}
	}
}

// The pose leaves or the hand pulls: the fabric next to the squeezed particle stretches.
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

// Air holds the fabric back across its surface, not along it: light fabric floats down and its hem does not
// whip, but it still slides and swings along itself. The drag grows with the square of the speed across the
// fabric and never turns the motion back.
void JoltCloth3D::_apply_air(float p_step) {
	if (air_resistance <= 0.0f) {
		return;
	}
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	air_normals.resize(vertices.size());
	for (JPH::Vec3 &normal : air_normals) {
		normal = JPH::Vec3::sZero();
	}
	for (uint32_t i = 0; i + 2 < setup.triangles.size(); i += 3) {
		const uint32_t a = setup.triangles[i];
		const uint32_t b = setup.triangles[i + 1];
		const uint32_t c = setup.triangles[i + 2];
		const JPH::Vec3 normal = (vertices[b].mPosition - vertices[a].mPosition).Cross(vertices[c].mPosition - vertices[a].mPosition);
		air_normals[a] += normal;
		air_normals[b] += normal;
		air_normals[c] += normal;
	}
	for (uint32_t i = 0; i < vertices.size(); i++) {
		JPH::SoftBodyVertex &vertex = vertices[i];
		if (vertex.mInvMass == 0.0f) {
			continue;
		}
		const JPH::Vec3 normal = air_normals[i].NormalizedOr(JPH::Vec3::sZero());
		const float across = vertex.mVelocity.Dot(normal);
		const float slowdown = MIN(Math::abs(across), air_resistance * across * across * p_step);
		vertex.mVelocity -= normal * (across > 0.0f ? slowdown : -slowdown);
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
		if (!hangs && contact_normals[i] != Vector3()) {
			// The drive does not press a particle into what it touches: a cover over a pillow rests on
			// the pillow instead of trembling between it and the pose, and comes back when it is gone.
			// Hanging cloth is pulled through: a fold caught behind a holder or a rod comes free.
			const JPH::Vec3 normal = to_jolt(contact_normals[i]);
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

void JoltCloth3D::_apply_hold(float p_step) {
	if (!setup.follow_targets || setup.hold_weights.is_empty()) {
		return;
	}
	const JPH::SoftBodyMotionProperties *motion = static_cast<const JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	const JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const JPH::RVec3 origin = jolt_body->GetCenterOfMassPosition();
	hold_closing = false;
	for (JPH::SoftBodySharedSettings::Skinned &skinned : shared_settings->mSkinnedConstraints) {
		const uint32_t particle = skinned.mVertex;
		if (_is_kinematic(particle)) {
			continue;
		}
		const float free = setup.max_distances[particle] < 0.0f ? FLT_MAX : setup.max_distances[particle];
		const float held = hold * setup.hold_weights[particle];
		if (held <= 0.0f) {
			skinned.mMaxDistance = free;
			continue;
		}
		float reach = Math::lerp(MIN(free, HOLD_EASE_REACH), HOLD_REACH, held);
		const float away = (vertices[particle].mPosition - JPH::Vec3(to_jolt_r(targets[particle]) - origin)).Length() - HOLD_CLOSE_SPEED * p_step;
		if (away > reach) {
			reach = away;
			hold_closing = true;
		}
		skinned.mMaxDistance = reach;
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
	// A few rounds bring the target within reach of every anchor at once. Anchors are where they stay, at
	// their targets: an anchor the hand pulls along would not hold the hand back.
	for (int round = 0; round < 3; round++) {
		for (uint32_t k = 0; k < grip_anchors.size(); k++) {
			const Vector3 anchor = targets[grip_anchors[k]];
			const Vector3 offset = grab_target - anchor;
			if (offset.length() > grip_reaches[k]) {
				grab_target = anchor + offset.normalized() * grip_reaches[k];
			}
		}
	}
	// The heavy patch reaches the hand within the step: the fabric next to it follows instead of stretching,
	// and walls stop it like any particle.
	const float inverse_step = 1.0f / p_step;
	for (uint32_t k = 0; k < grip_particles.size(); k++) {
		JPH::SoftBodyVertex &vertex = vertices[grip_particles[k]];
		vertex.mVelocity = to_jolt((grab_target + grip_offsets[k] - (origin + to_godot(vertex.mPosition))) * inverse_step);
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
	_forget_grip();
	_find_neighbors();
	particle_inverse_masses.resize(particle_count);
	const float inverse_mass = 1.0f / MAX(setup.particle_mass, 0.0001f);
	hangs = false;
	for (uint32_t i = 0; i < particle_count; i++) {
		particle_inverse_masses[i] = _is_kinematic(i) ? 0.0f : inverse_mass;
		hangs = hangs || _is_kinematic(i);
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

// A cover on a pillow its pose goes into is kept from its pose: the way of its particles to their targets
// leads into what they touch. Fabric twisted around itself or caught by friction is not kept, nor is hanging
// cloth: a fold caught behind its holder is a tangle.
bool JoltCloth3D::is_kept_from_targets(float p_distance) const {
	if (!in_space() || hangs) {
		return false;
	}
	const JPH::SoftBodyMotionProperties *motion = static_cast<const JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	const JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const JPH::RVec3 origin = jolt_body->GetCenterOfMassPosition();
	for (uint32_t i = 0; i < vertices.size(); i++) {
		const JPH::SoftBodyVertex &vertex = vertices[i];
		const JPH::Vec3 to_target = JPH::Vec3(to_jolt_r(targets[i]) - origin) - vertex.mPosition;
		const float distance = to_target.Length();
		if (distance > p_distance && to_jolt(contact_normals[i]).Dot(to_target) < -0.5f * distance) {
			return true;
		}
	}
	return false;
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

void JoltCloth3D::set_air_resistance(float p_resistance) {
	air_resistance = MAX(p_resistance, 0.0f);
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

void JoltCloth3D::set_hold_weights(const LocalVector<float> &p_weights) {
	ERR_FAIL_COND(p_weights.size() != setup.positions.size());
	setup.hold_weights = p_weights;
	hold_dirty = true;
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
	if (!in_space()) {
		return -1;
	}
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
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	for (uint32_t particle : grip_particles) {
		// The hand pulls squeezed fabric out.
		squeezed.erase(particle);
		grip_offsets.push_back(positions[particle] - grab_target);
		vertices[particle].mInvMass = particle_inverse_masses[particle] / GRIP_MASS_SCALE;
	}
	_find_grip_anchors();
	wake_up();
	return nearest;
}

// Fabric does not stretch: the hand takes it no farther than it reaches from its pin and from the fabric a
// closed holder keeps. A stiffer grip would stretch it like a rubber band, and it would snap back on release.
void JoltCloth3D::_find_grip_anchors() {
	if (shared_settings.GetPtr() == nullptr) {
		return;
	}
	for (const JPH::SoftBodySharedSettings::LRA &attachment : shared_settings->mLRAConstraints) {
		if (attachment.mVertex[1] == uint32_t(grabbed_particle)) {
			grip_anchors.push_back(attachment.mVertex[0]);
			grip_reaches.push_back(attachment.mMaxDistance);
		}
	}
	if (shared_settings->mLRAConstraints.empty()) {
		// Nothing holds free cloth but the hand: it keeps the fabric from stretching instead of the pins.
		_measure_along_fabric(grabbed_particle, grip_tethers);
		return;
	}
	if (hold <= 0.0f || setup.hold_weights.is_empty()) {
		return;
	}
	// The held fabric nearest along the fabric, and the length of fabric between: pulled taut, the fabric
	// straightens its curves and folds, so the hand reaches as far as that length, not the straight line.
	// Only fabric the holder holds fully stays put; partly held fabric may go far.
	LocalVector<real_t> along;
	_measure_along_fabric(grabbed_particle, along);
	int held = -1;
	for (uint32_t i = 0; i < along.size(); i++) {
		if (hold * setup.hold_weights[i] >= 0.99f && (held < 0 || along[i] < along[held])) {
			held = i;
		}
	}
	if (held >= 0) {
		grip_anchors.push_back(held);
		grip_reaches.push_back(along[held]);
	}
}

// Shortest distances along the fabric from p_from to every particle, over the triangle edges of the targets:
// fabric of another fold may be close in space but is far along the fabric.
void JoltCloth3D::_measure_along_fabric(uint32_t p_from, LocalVector<real_t> &r_along) const {
	const uint32_t particle_count = targets.size();
	r_along.resize(particle_count);
	for (uint32_t i = 0; i < particle_count; i++) {
		r_along[i] = Math::INF;
	}
	r_along[p_from] = 0.0;
	// A binary heap of (distance, particle); a particle may sit in it more than once, the stale entries are skipped.
	struct Entry {
		real_t distance;
		uint32_t particle;
	};
	LocalVector<Entry> heap;
	const auto sift_up = [&heap](uint32_t p_index) {
		while (p_index > 0) {
			const uint32_t parent = (p_index - 1) / 2;
			if (heap[parent].distance <= heap[p_index].distance) {
				break;
			}
			SWAP(heap[parent], heap[p_index]);
			p_index = parent;
		}
	};
	const auto pop = [&heap]() {
		const Entry top = heap[0];
		heap[0] = heap[heap.size() - 1];
		heap.resize(heap.size() - 1);
		uint32_t index = 0;
		while (true) {
			const uint32_t left = index * 2 + 1;
			const uint32_t right = left + 1;
			uint32_t smallest = index;
			if (left < heap.size() && heap[left].distance < heap[smallest].distance) {
				smallest = left;
			}
			if (right < heap.size() && heap[right].distance < heap[smallest].distance) {
				smallest = right;
			}
			if (smallest == index) {
				break;
			}
			SWAP(heap[index], heap[smallest]);
			index = smallest;
		}
		return top;
	};
	heap.push_back({ 0.0, p_from });
	while (!heap.is_empty()) {
		const Entry entry = pop();
		if (entry.distance > r_along[entry.particle]) {
			continue;
		}
		for (uint32_t k = neighbor_starts[entry.particle]; k < neighbor_starts[entry.particle + 1]; k++) {
			const uint32_t neighbor = neighbor_list[k];
			const real_t distance = entry.distance + targets[entry.particle].distance_to(targets[neighbor]);
			if (distance < r_along[neighbor]) {
				r_along[neighbor] = distance;
				heap.push_back({ distance, neighbor });
				sift_up(heap.size() - 1);
			}
		}
	}
}

// Every particle of free cloth stays within its length of fabric from the hand: hanging from the hand it does not
// stretch like rubber, and its hem does not whip. A tether does not pull a particle into what it touches: fabric
// over the edge of a box is longer than the straight line to the hand, and pulled along that line it would
// stretch over the edge; it slides along the surface instead.
void JoltCloth3D::_apply_tethers(float p_step) {
	if (grip_tethers.is_empty()) {
		return;
	}
	JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
	JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
	const JPH::Vec3 hand = JPH::Vec3(to_jolt_r(grab_target + grip_offsets[0]) - jolt_body->GetCenterOfMassPosition());
	const float inverse_step = 1.0f / p_step;
	for (uint32_t i = 0; i < vertices.size(); i++) {
		JPH::SoftBodyVertex &vertex = vertices[i];
		if (vertex.mInvMass == 0.0f) {
			continue;
		}
		const JPH::Vec3 predicted = vertex.mPosition + vertex.mVelocity * p_step;
		const JPH::Vec3 from_hand = predicted - hand;
		const float reach = float(grip_tethers[i]);
		const float distance = from_hand.Length();
		if (distance > reach && distance > 0.0f) {
			JPH::Vec3 pull = (hand + from_hand * (reach / distance) - vertex.mPosition) * inverse_step - vertex.mVelocity;
			if (contact_normals[i] != Vector3()) {
				const JPH::Vec3 normal = to_jolt(contact_normals[i]);
				pull -= normal * MIN(pull.Dot(normal), 0.0f);
			}
			vertex.mVelocity += pull;
		}
	}
}

void JoltCloth3D::move_grab(const Vector3 &p_world_position) {
	grab_requested = p_world_position;
	wake_up();
}

void JoltCloth3D::release_grab() {
	if (in_space()) {
		JPH::SoftBodyMotionProperties *motion = static_cast<JPH::SoftBodyMotionProperties *>(jolt_body->GetMotionPropertiesUnchecked());
		JPH::Array<JPH::SoftBodyVertex> &vertices = motion->GetVertices();
		for (uint32_t particle : grip_particles) {
			JPH::SoftBodyVertex &vertex = vertices[particle];
			vertex.mInvMass = particle_inverse_masses[particle];
			const float speed = vertex.mVelocity.Length();
			if (speed > GRIP_RELEASE_SPEED) {
				vertex.mVelocity *= GRIP_RELEASE_SPEED / speed;
			}
		}
	}
	_forget_grip();
}

void JoltCloth3D::_forget_grip() {
	grabbed_particle = -1;
	grip_tethers.clear();
	grip_particles.clear();
	grip_offsets.clear();
	grip_anchors.clear();
	grip_reaches.clear();
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

	if (hold_dirty || hold_closing) {
		_apply_hold(p_step);
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
	_hold_squeezed();
	_apply_gravity(p_step);
	_apply_air(p_step);
	_apply_drive(p_step);
	_apply_pushes();
	_apply_grab(p_step);
	_apply_tethers(p_step);
	previous_targets = targets;
	// The coming step records its own contacts.
	previous_contact_normals = contact_normals;
	for (Vector3 &normal : contact_normals) {
		normal = Vector3();
	}
}
