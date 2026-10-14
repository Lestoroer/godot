/**************************************************************************/
/*  cloth_body_3d.cpp                                                     */
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

#include "cloth_body_3d.h"

#include "../jolt_physics_server_3d.h"
#include "../misc/jolt_type_conversions.h"
#include "../objects/jolt_cloth_3d.h"
#include "../spaces/jolt_space_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

namespace {

// Margin around the particles for the visible mesh, which may stand off the coarse simulation surface.
constexpr real_t RENDER_MARGIN = 0.05;
// A cloth that starts away from its pose, like a cover laid on a bed, may leave it this much farther
// at first, so it flows into the pose instead of snapping to the allowed distance.
constexpr float REACH_RAMP_SECONDS = 1.2f;
constexpr float REACH_RAMP_SCALE = 25.0f;
// After falling asleep the cloth blends from where it rests into the exact pose.
constexpr float SETTLE_SECONDS = 0.3f;
// Fabric that settles from farther away, like a twisted curtain untwisting into its pose, moves no faster
// than this.
constexpr real_t SETTLE_SPEED = 0.5;
// The pose pulls the particles no faster than this. After a frame pause the pose of an opening curtain
// jumps by a whole frame of travel; pins moved that far in one step would fling the fabric next to them.
constexpr real_t POSE_SPEED_LIMIT = 3.0;
// The cloth falls asleep after it stayed still for REST_QUIET_SECONDS, checked every REST_WINDOW_SECONDS.
// Still: posed cloth moved less than REST_DRIFT_POSED and came back to its pose closer than
// REST_POSE_DISTANCE, or on average closer than REST_POSE_MEAN_DISTANCE where a few particles rest on
// something the pose goes into; a slow swing back to the pose is not cut short. Free cloth moved less than
// REST_DRIFT_FREE, or on average less than REST_DRIFT_MEAN_FREE: a few particles caught between the
// floor and another garment may keep trembling. Posed cloth that moved less than REST_DRIFT_POSED for
// REST_STUCK_SECONDS while the full return pulls it rests away from its pose, on something the pose goes
// into: a visible swing moves farther in that time, and before the return is full the cloth may just hang
// still away from its pose for a moment. Cloth that was not touched for REST_TIMEOUT_SECONDS falls asleep anyway, like a cover
// pressed between a folded towel and the mattress.
constexpr float REST_WINDOW_SECONDS = 0.25f;
constexpr float REST_QUIET_SECONDS = 0.5f;
constexpr float REST_STUCK_SECONDS = 1.5f;
constexpr float REST_TIMEOUT_SECONDS = 12.0f;
constexpr real_t REST_DRIFT_POSED = 0.003;
constexpr real_t REST_POSE_DISTANCE = 0.03;
constexpr real_t REST_POSE_MEAN_DISTANCE = 0.01;
constexpr real_t REST_DRIFT_FREE = 0.005;
constexpr real_t REST_DRIFT_MEAN_FREE = 0.0015;
// A resting cloth rebuilds the shape other cloth lies on only if it came to rest this much elsewhere.
constexpr real_t LYING_REBUILD_DISTANCE = 0.005;
// Cloth this close above a changed resting shape is woken to settle again.
constexpr real_t LYING_WAKE_MARGIN = 0.03;

AABB bounds_of(const LocalVector<Vector3> &p_points, real_t p_margin) {
	if (p_points.is_empty()) {
		return AABB();
	}
	AABB bounds(p_points[0], Vector3());
	for (const Vector3 &point : p_points) {
		bounds.expand_to(point);
	}
	return bounds.grow(p_margin);
}

} // namespace

void ClothBody3D::_rebuild() {
	_free_physics();
	_free_lying_collision(true);

	RenderingServer *rendering = RenderingServer::get_singleton();
	if (cloth_mesh.is_null() || cloth_mesh->get_render_mesh().is_null() || cloth_mesh->get_particle_count() == 0) {
		set_base(RID());
		return;
	}

	set_base(cloth_mesh->get_render_mesh()->get_rid());
	if (!skeleton.is_valid()) {
		skeleton = rendering->skeleton_create();
	}
	rendering->skeleton_allocate_data(skeleton, cloth_mesh->get_particle_count());
	const PackedFloat32Array pins = cloth_mesh->get_pin_weights();
	pinned.resize(pins.size());
	for (int i = 0; i < pins.size(); i++) {
		pinned[i] = pins[i] > 0.5f;
	}
	if (is_inside_world()) {
		rendering->instance_attach_skeleton(get_instance(), skeleton);
	}

	_update_pose();
	target_local = pose_local;
	previous_local = pose_local;
	current_local = pose_local;
	render_local = pose_local;
	_draw_pose();

	// A hidden cloth, like a folded garment, has no physics.
	if (is_inside_world() && is_visible_in_tree() && !Engine::get_singleton()->is_editor_hint()) {
		_create_physics();
	}
}

void ClothBody3D::_create_physics(const LocalVector<Vector3> *p_start_world) {
	JoltPhysicsServer3D *server = JoltPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_MSG(server, "ClothBody3D requires Jolt Physics as the 3D physics engine.");
	ERR_FAIL_COND_MSG(server->is_on_separate_thread(), "ClothBody3D does not support running physics on a separate thread.");
	JoltSpace3D *space = server->get_space(get_world_3d()->get_space());
	ERR_FAIL_NULL(space);

	const Transform3D transform = get_global_transform();
	JoltCloth3D::Setup setup;
	// The fabric rests in its pose wherever it starts: a garment crumpled on a bed edge still
	// rests flat.
	setup.positions.resize(pose_local.size());
	for (uint32_t i = 0; i < pose_local.size(); i++) {
		setup.positions[i] = transform.xform(pose_local[i]);
	}
	target_local = pose_local;
	if (p_start_world != nullptr) {
		setup.start_positions = *p_start_world;
	}
	const PackedInt32Array triangles = cloth_mesh->get_triangles();
	const PackedFloat32Array pins = cloth_mesh->get_pin_weights();
	const PackedFloat32Array distances = cloth_mesh->get_max_distances();
	for (int index : triangles) {
		setup.triangles.push_back(index);
	}
	for (float pin : pins) {
		setup.pin_weights.push_back(pin);
	}
	for (float distance : distances) {
		setup.max_distances.push_back(distance);
	}
	if (cloth_mesh->has_holds()) {
		for (float weight : cloth_mesh->get_hold_weights()) {
			setup.hold_weights.push_back(weight);
		}
	}
	setup.particle_mass = particle_mass;
	setup.stretch_compliance = stretch_compliance;
	setup.shear_compliance = shear_compliance;
	setup.bend_compliance = bend_compliance;
	setup.backstop_distance = backstop_distance;
	setup.backstop_radius = backstop_radius;
	setup.follow_targets = follow_pose;
	setup.dihedral_bends = dihedral_bends;
	setup.shear_across_folds = shear_across_folds;
	setup.crease_compliance = crease_compliance;

	cloth = memnew(JoltCloth3D);
	cloth->set_instance_id(get_instance_id());
	_update_motion();
	cloth->configure(setup);
	cloth->set_hold(hold);
	cloth->set_release_stamp(JoltCloth3D::next_stamp());
	cloth->set_space(space);
	if (p_start_world != nullptr && follow_pose) {
		reach_ramp = REACH_RAMP_SECONDS;
		cloth->set_max_distance_scale(max_distance_scale * REACH_RAMP_SCALE);
	}

	target_transform = transform;
	step_inverse = transform.affine_inverse();
	read_physics_frame = Engine::get_singleton()->get_physics_frames();
	was_active = false;
	targets_dirty = false;
	pose_reshaped = false;
	physics_started = p_start_world != nullptr;
	settle_left = 0.0f;
	if (p_start_world != nullptr) {
		current_local.resize(p_start_world->size());
		for (uint32_t i = 0; i < p_start_world->size(); i++) {
			current_local[i] = step_inverse.xform((*p_start_world)[i]);
		}
		previous_local = current_local;
	}
	if (carried) {
		carry_local = current_local;
		cloth->put_to_sleep();
	} else if (follow_pose) {
		// The pose is the resting shape: other cloth may lie on it before this cloth first sleeps.
		_update_lying_collision();
	}
}

void ClothBody3D::_free_physics() {
	if (cloth == nullptr) {
		return;
	}
	cloth->set_space(nullptr);
	memdelete(cloth);
	cloth = nullptr;
}

void ClothBody3D::_update_motion() {
	if (cloth == nullptr) {
		return;
	}
	cloth->set_collision_layer(collision_layer);
	cloth->set_collision_mask(collision_mask);
	_update_drive(false);
	cloth->set_gravity_scale(gravity_scale);
	cloth->set_linear_damping(linear_damping);
	cloth->set_friction(friction);
	cloth->set_vertex_radius(vertex_radius);
	cloth->set_max_distance_scale(max_distance_scale);
	cloth->set_iterations(iterations);
}

// Right after a touch the drive is weak and the cloth swings like a pendulum. Left alone, the drive
// grows over return_seconds after return_delay, with damping to match, and brings the cloth back to its
// pose: folds tangled by a swing or a pull come apart. While the pose moves, the cloth follows it
// closely and critically damped, so gathering folds do not cross. The pose is no touch: when it stops,
// the cloth settles into it with the full return at once instead of swinging on.
void ClothBody3D::_update_drive(bool p_pose_moved) {
	if (cloth == nullptr) {
		return;
	}
	if (return_stiffness <= drive_stiffness) {
		cloth->set_drive(drive_stiffness, drive_damping);
		return;
	}
	if (p_pose_moved) {
		untouched_time = MAX(untouched_time, return_delay + return_seconds);
		const float stiffness = travel_stiffness < 0.0f ? return_stiffness : travel_stiffness;
		cloth->set_drive(stiffness, 2.0f * Math::sqrt(stiffness));
		return;
	}
	const float t = CLAMP((untouched_time - return_delay) / MAX(return_seconds, 0.001f), 0.0f, 1.0f);
	const float weight = t * t * (3.0f - 2.0f * t);
	// Critical damping: the cloth eases into its pose without springing past it, like fabric settling.
	cloth->set_drive(Math::lerp(drive_stiffness, return_stiffness, weight), Math::lerp(drive_damping, 2.0f * Math::sqrt(return_stiffness), weight));
}

void ClothBody3D::_update_pose() {
	cloth_mesh->sample_pose(pose_position, pose_local);
	cloth_mesh->calculate_frames(pose_local, pose_frames);

	// Pose k > 0 is blend shape k - 1; the visible mesh blends the same two poses as the particles.
	const int pose_count = cloth_mesh->get_pose_count();
	const float position = CLAMP(pose_position, 0.0f, float(pose_count - 1));
	const int from = MIN(int(Math::floor(position)), pose_count - 1);
	const int to = MIN(from + 1, pose_count - 1);
	const float weight = position - float(from);
	RenderingServer *rendering = RenderingServer::get_singleton();
	for (int shape = 0; shape < pose_count - 1; shape++) {
		float shape_weight = 0.0f;
		if (shape == from - 1) {
			shape_weight += 1.0f - weight;
		}
		if (shape == to - 1) {
			shape_weight += weight;
		}
		rendering->instance_set_blend_shape_weight(get_instance(), shape, shape_weight);
	}

	targets_dirty = true;
	pose_reshaped = true;
	if (cloth != nullptr) {
		_targets_moved();
	} else {
		_draw_pose();
	}
}

void ClothBody3D::_push_targets() {
	if (cloth == nullptr || carried) {
		return;
	}
	physics_started = true;
	if (reach_ramp > 0.0f) {
		reach_ramp = MAX(reach_ramp - float(get_physics_process_delta_time()), 0.0f);
		const float weight = reach_ramp / REACH_RAMP_SECONDS;
		cloth->set_max_distance_scale(max_distance_scale * Math::lerp(1.0f, REACH_RAMP_SCALE, weight * weight));
	}

	const Transform3D transform = get_global_transform();
	// Reparenting recomputes the transform with rounding; that is no motion.
	if (follow_pose && (targets_dirty || !transform.is_equal_approx(target_transform))) {
		const bool pose_reached = _move_target_toward_pose();
		world_scratch.resize(target_local.size());
		for (uint32_t i = 0; i < target_local.size(); i++) {
			world_scratch[i] = transform.xform(target_local[i]);
		}
		cloth->set_targets(world_scratch, pose_reshaped);
		target_transform = transform;
		targets_dirty = !pose_reached;
		pose_reshaped = !pose_reached;
		targets_moving = true;
		_targets_moved();
	} else if (targets_moving) {
		// The pose stopped: other cloth may lie on it again without waiting for this cloth to sleep.
		targets_moving = false;
		_update_lying_collision();
	}
	// A hand holding the cloth touches it all the time: the return does not pull against the hand.
	untouched_time = cloth->get_grabbed_particle() >= 0 ? 0.0f : untouched_time + float(get_physics_process_delta_time());
	_update_drive(targets_moving);
	step_inverse = transform.affine_inverse();
}

// Moves the target shape toward the pose by at most a step of POSE_SPEED_LIMIT; true when it got there.
// The whole shape moves by one fraction, so its folds keep their form while it catches up.
bool ClothBody3D::_move_target_toward_pose() {
	real_t farthest = 0.0;
	for (uint32_t i = 0; i < pose_local.size(); i++) {
		farthest = MAX(farthest, target_local[i].distance_squared_to(pose_local[i]));
	}
	const real_t limit = POSE_SPEED_LIMIT * get_physics_process_delta_time();
	if (farthest <= limit * limit) {
		target_local = pose_local;
		return true;
	}
	const real_t fraction = limit / Math::sqrt(farthest);
	for (uint32_t i = 0; i < pose_local.size(); i++) {
		target_local[i] = target_local[i].lerp(pose_local[i], fraction);
	}
	return false;
}

void ClothBody3D::_read_simulation() {
	const uint64_t frame = Engine::get_singleton()->get_physics_frames();
	if (frame == read_physics_frame) {
		return;
	}
	const uint64_t steps = frame - read_physics_frame;
	read_physics_frame = frame;

	cloth->get_positions(world_scratch);
	previous_local = current_local;
	current_local.resize(world_scratch.size());
	for (uint32_t i = 0; i < world_scratch.size(); i++) {
		current_local[i] = step_inverse.xform(world_scratch[i]);
	}
	_check_rest(steps);
}

void ClothBody3D::_check_rest(uint64_t p_steps) {
	const float elapsed = float(p_steps) * float(get_physics_process_delta_time());
	awake_time += elapsed;
	if (rest_check_world.size() != world_scratch.size()) {
		rest_check_world = world_scratch;
		rest_check_time = 0.0f;
		rest_quiet_time = 0.0f;
		rest_stuck_time = 0.0f;
		return;
	}
	rest_check_time += elapsed;
	if (rest_check_time < REST_WINDOW_SECONDS) {
		return;
	}
	real_t drift_most = 0.0;
	real_t drift_total = 0.0;
	for (uint32_t i = 0; i < world_scratch.size(); i++) {
		const real_t drift = rest_check_world[i].distance_to(world_scratch[i]);
		drift_most = MAX(drift_most, drift);
		drift_total += drift;
	}
	real_t away_most = 0.0;
	real_t away_total = 0.0;
	if (follow_pose) {
		const Transform3D transform = get_global_transform();
		for (uint32_t i = 0; i < world_scratch.size(); i++) {
			const real_t away = transform.xform(pose_local[i]).distance_to(world_scratch[i]);
			away_most = MAX(away_most, away);
			away_total += away;
		}
	}
	const float window = rest_check_time;
	rest_check_world = world_scratch;
	rest_check_time = 0.0f;

	// Cloth under the cursor or in the hand stays awake for it.
	const bool touched = attended || cloth->get_grabbed_particle() >= 0;
	attended = false;
	if (touched) {
		awake_time = 0.0f;
		rest_quiet_time = 0.0f;
		rest_stuck_time = 0.0f;
		return;
	}
	const real_t count = real_t(MAX(world_scratch.size(), 1u));
	const bool returned = away_most < REST_POSE_DISTANCE || away_total / count < REST_POSE_MEAN_DISTANCE;
	const bool still = follow_pose ? drift_most < REST_DRIFT_POSED && returned
								   : drift_most < REST_DRIFT_FREE || drift_total / count < REST_DRIFT_MEAN_FREE;
	rest_quiet_time = still ? rest_quiet_time + window : 0.0f;
	const bool returning_fully = return_stiffness <= drive_stiffness || untouched_time >= return_delay + return_seconds;
	rest_stuck_time = follow_pose && returning_fully && drift_most < REST_DRIFT_POSED ? rest_stuck_time + window : 0.0f;
	if (rest_quiet_time >= REST_QUIET_SECONDS || rest_stuck_time >= REST_STUCK_SECONDS || awake_time >= REST_TIMEOUT_SECONDS) {
		// Posed cloth rests away from its pose only where something it touches keeps it away; fabric twisted
		// around itself would stay twisted, so it settles into the pose.
		settles_into_pose = follow_pose && (returned || !cloth->is_kept_from_targets(REST_POSE_DISTANCE));
		asleep_away = away_most;
		rest_quiet_time = 0.0f;
		rest_stuck_time = 0.0f;
		cloth->put_to_sleep();
	}
}

void ClothBody3D::_update_render() {
	if (cloth == nullptr || carried) {
		return;
	}

	const bool active = cloth->is_active();
	if (!active && !was_active) {
		if (settle_left > 0.0f) {
			settle_left = MAX(settle_left - float(get_process_delta_time()), 0.0f);
			if (settle_left > 0.0f) {
				_draw_settling(1.0f - settle_left / settle_seconds);
			} else {
				_draw_pose();
			}
		}
		return;
	}
	settle_left = 0.0f;
	_read_simulation();
	if (!active) {
		_fell_asleep();
		return;
	}
	if (!was_active) {
		rest_check_world.clear();
		awake_time = 0.0f;
		emit_signal(SNAME("sleeping_changed"), false);
	}
	was_active = true;
	drawn_in_pose = false;

	const float fraction = Engine::get_singleton()->get_physics_interpolation_fraction();
	render_local.resize(current_local.size());
	for (uint32_t i = 0; i < current_local.size(); i++) {
		render_local[i] = previous_local[i].lerp(current_local[i], fraction);
	}
	_draw_settling(0.0f);
}

void ClothBody3D::_draw_settling(float p_weight) {
	// p_weight blends the drawn shape from the simulation (0) into the exact pose (1).
	if (p_weight > 0.0f) {
		const float smooth = p_weight * p_weight * (3.0f - 2.0f * p_weight);
		for (uint32_t i = 0; i < render_local.size(); i++) {
			render_local[i] = current_local[i].lerp(pose_local[i], smooth);
		}
	}
	cloth_mesh->calculate_frames(render_local, render_frames);

	// Each particle is a bone that carries the posed mesh from the pose frame to the simulated frame.
	RenderingServer *rendering = RenderingServer::get_singleton();
	for (uint32_t i = 0; i < render_local.size(); i++) {
		const Basis rotation = pinned[i] ? Basis() : render_frames[i] * pose_frames[i].transposed();
		rendering->skeleton_bone_set_transform(skeleton, i, Transform3D(rotation, render_local[i] - rotation.xform(pose_local[i])));
	}
	rendering->instance_set_custom_aabb(get_instance(), bounds_of(render_local, RENDER_MARGIN));
}

void ClothBody3D::_draw_pose() {
	if (!skeleton.is_valid()) {
		return;
	}
	RenderingServer *rendering = RenderingServer::get_singleton();
	for (uint32_t i = 0; i < pose_local.size(); i++) {
		rendering->skeleton_bone_set_transform(skeleton, i, Transform3D());
	}
	real_t margin = RENDER_MARGIN;
	for (float distance : cloth_mesh->get_max_distances()) {
		margin = MAX(margin, distance * max_distance_scale);
	}
	rendering->instance_set_custom_aabb(get_instance(), bounds_of(pose_local, margin));
	drawn_in_pose = true;
}

void ClothBody3D::_fell_asleep() {
	was_active = false;
	if (settles_into_pose) {
		// The cloth blends into the exact pose so folds stay clean, and its particles rest there too: the
		// next touch moves the shape that is drawn. Cloth kept away from its pose is drawn where it lies.
		settle_seconds = MAX(SETTLE_SECONDS, float(asleep_away / SETTLE_SPEED));
		settle_left = settle_seconds;
		cloth->rest_in_targets();
	}
	_update_lying_collision();
	emit_signal(SNAME("sleeping_changed"), true);
}

// Resting cloth is solid for other settling cloth. A cloth pushed by the cursor keeps the shape
// it rested in until it rests again; only then the shape is rebuilt, if the cloth moved.
void ClothBody3D::_update_lying_collision() {
	if (lying_collision_layer == 0 || cloth == nullptr) {
		return;
	}
	if (follow_pose) {
		const Transform3D transform = get_global_transform();
		world_scratch.resize(pose_local.size());
		for (uint32_t i = 0; i < pose_local.size(); i++) {
			world_scratch[i] = transform.xform(pose_local[i]);
		}
	} else {
		cloth->get_positions(world_scratch);
	}
	const bool had_body = lying_body.is_valid();
	if (had_body && lying_positions.size() == world_scratch.size()) {
		real_t moved = 0.0;
		for (uint32_t i = 0; i < world_scratch.size(); i++) {
			moved = MAX(moved, lying_positions[i].distance_squared_to(world_scratch[i]));
		}
		if (moved < LYING_REBUILD_DISTANCE * LYING_REBUILD_DISTANCE) {
			return;
		}
	}
	// Cloth resting on the old shape is not woken: two cloths lying on each other would wake each
	// other forever. It is woken when the shape goes away. A rebuilt shape keeps the moment it first
	// appeared: cloth that lay on it before keeps lying on it.
	const uint64_t stamp = had_body ? JoltCloth3D::get_resting_shape_stamp(get_instance_id()) : JoltCloth3D::next_stamp();
	_free_lying_collision(false);
	JoltCloth3D::set_resting_shape_stamp(get_instance_id(), stamp);
	lying_positions = world_scratch;
	lying_bounds = bounds_of(lying_positions, 0.0);

	PackedVector3Array faces;
	for (int index : cloth_mesh->get_triangles()) {
		faces.push_back(lying_positions[index]);
	}
	Dictionary shape_data;
	shape_data["faces"] = faces;
	shape_data["backface_collision"] = true;

	PhysicsServer3D *physics = PhysicsServer3D::get_singleton();
	lying_shape = physics->concave_polygon_shape_create();
	physics->shape_set_data(lying_shape, shape_data);
	lying_body = physics->body_create();
	physics->body_set_mode(lying_body, PhysicsServer3D::BODY_MODE_STATIC);
	physics->body_add_shape(lying_body, lying_shape);
	physics->body_set_collision_layer(lying_body, lying_collision_layer);
	physics->body_set_collision_mask(lying_body, 0);
	physics->body_attach_object_instance_id(lying_body, get_instance_id());
	physics->body_set_space(lying_body, get_world_3d()->get_space());
}

void ClothBody3D::_free_lying_collision(bool p_wake_resting) {
	const bool had_body = lying_body.is_valid();
	PhysicsServer3D *physics = PhysicsServer3D::get_singleton();
	if (lying_body.is_valid()) {
		physics->free_rid(lying_body);
		lying_body = RID();
		JoltCloth3D::set_resting_shape_stamp(get_instance_id(), 0);
	}
	if (lying_shape.is_valid()) {
		physics->free_rid(lying_shape);
		lying_shape = RID();
	}
	lying_positions.clear();
	if (had_body && p_wake_resting) {
		_wake_resting_on(lying_bounds);
	}
}

// Cloth lying on a resting shape that changed or went away settles again instead of hanging in the air.
void ClothBody3D::_wake_resting_on(const AABB &p_bounds) {
	JoltPhysicsServer3D *server = JoltPhysicsServer3D::get_singleton();
	if (server == nullptr || !is_inside_world() || get_world_3d().is_null()) {
		return;
	}
	JoltSpace3D *space = server->get_space(get_world_3d()->get_space());
	if (space == nullptr) {
		return;
	}
	// Activation by area wakes this cloth as well; it stays as it was.
	const bool was_sleeping = cloth != nullptr && !cloth->is_active();
	const AABB box = p_bounds.grow(LYING_WAKE_MARGIN);
	space->get_body_iface().ActivateBodiesInAABox(JPH::AABox(to_jolt(box.position), to_jolt(box.get_end())), JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
	if (was_sleeping) {
		cloth->put_to_sleep();
	}
}

void ClothBody3D::_targets_moved() {
	// The pose moved away from the shape other cloth rests on.
	_free_lying_collision(true);
	cloth->wake_up();
}

void ClothBody3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			_rebuild();
			set_physics_process_internal(true);
			set_process_internal(true);
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			_free_physics();
			_free_lying_collision(true);
			set_physics_process_internal(false);
			set_process_internal(false);
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_inside_world()) {
				_rebuild();
			}
		} break;

		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_push_targets();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_update_render();
		} break;
	}
}

void ClothBody3D::set_cloth_mesh(const Ref<ClothMesh> &p_mesh) {
	cloth_mesh = p_mesh;
	if (is_inside_world()) {
		_rebuild();
	}
	update_gizmos();
}

void ClothBody3D::set_pose_position(float p_position) {
	pose_position = MAX(p_position, 0.0f);
	if (cloth_mesh.is_valid() && cloth_mesh->get_particle_count() > 0 && is_inside_world()) {
		_update_pose();
		if (cloth != nullptr && !physics_started) {
			reset_to_pose();
		}
	}
}

void ClothBody3D::set_follow_pose(bool p_follow) {
	if (follow_pose == p_follow) {
		return;
	}
	follow_pose = p_follow;
	// Before the first physics step the switch is the initial state, not a motion.
	if (cloth == nullptr || !is_inside_world() || !physics_started) {
		if (is_inside_world()) {
			_rebuild();
		}
		return;
	}
	// The cloth keeps its shape across the switch: a cover laid on a bed flows from how it was
	// carried into its pose.
	LocalVector<Vector3> start;
	if (carried) {
		const Transform3D transform = get_global_transform();
		start.resize(carry_local.size());
		for (uint32_t i = 0; i < carry_local.size(); i++) {
			start[i] = transform.xform(carry_local[i]);
		}
		carried = false;
	} else {
		cloth->get_positions(start);
	}
	_free_physics();
	_free_lying_collision(true);
	_create_physics(&start);
}

#define CLOTH_REBUILD_SETTER(m_name, m_type)    \
	void ClothBody3D::set_##m_name(m_type p_value) { \
		m_name = p_value;                         \
		if (cloth != nullptr) {                   \
			_rebuild();                           \
		}                                         \
	}

#define CLOTH_MOTION_SETTER(m_name, m_type)     \
	void ClothBody3D::set_##m_name(m_type p_value) { \
		m_name = p_value;                         \
		_update_motion();                         \
	}

CLOTH_REBUILD_SETTER(particle_mass, float)
CLOTH_REBUILD_SETTER(stretch_compliance, float)
CLOTH_REBUILD_SETTER(shear_compliance, float)
CLOTH_REBUILD_SETTER(bend_compliance, float)
CLOTH_REBUILD_SETTER(crease_compliance, float)

void ClothBody3D::set_hold(float p_hold) {
	hold = CLAMP(p_hold, 0.0f, 1.0f);
	if (cloth != nullptr) {
		cloth->set_hold(hold);
	}
}
CLOTH_REBUILD_SETTER(backstop_distance, float)
CLOTH_REBUILD_SETTER(backstop_radius, float)
CLOTH_REBUILD_SETTER(dihedral_bends, bool)
CLOTH_REBUILD_SETTER(shear_across_folds, bool)
CLOTH_MOTION_SETTER(linear_damping, float)
CLOTH_MOTION_SETTER(friction, float)
CLOTH_MOTION_SETTER(gravity_scale, float)
CLOTH_MOTION_SETTER(vertex_radius, float)
CLOTH_MOTION_SETTER(drive_stiffness, float)
CLOTH_MOTION_SETTER(drive_damping, float)
CLOTH_MOTION_SETTER(return_stiffness, float)
CLOTH_MOTION_SETTER(travel_stiffness, float)
CLOTH_MOTION_SETTER(return_delay, float)
CLOTH_MOTION_SETTER(return_seconds, float)
CLOTH_MOTION_SETTER(max_distance_scale, float)
CLOTH_MOTION_SETTER(iterations, int)
CLOTH_MOTION_SETTER(collision_layer, uint32_t)
CLOTH_MOTION_SETTER(collision_mask, uint32_t)

#undef CLOTH_REBUILD_SETTER
#undef CLOTH_MOTION_SETTER

void ClothBody3D::push(const Vector3 &p_point, const Vector3 &p_velocity, float p_radius) {
	if (cloth == nullptr || carried) {
		return;
	}
	attended = true;
	untouched_time = 0.0f;
	cloth->push(p_point, p_velocity * push_response, p_radius);
}

int ClothBody3D::grab(const Vector3 &p_point) {
	if (cloth == nullptr || carried) {
		return -1;
	}
	untouched_time = 0.0f;
	return cloth->grab(p_point);
}

void ClothBody3D::move_grab(const Vector3 &p_point) {
	untouched_time = 0.0f;
	if (cloth != nullptr) {
		cloth->move_grab(p_point);
	}
}

void ClothBody3D::release_grab() {
	untouched_time = 0.0f;
	if (cloth != nullptr) {
		cloth->release_grab();
		cloth->wake_up();
	}
}

bool ClothBody3D::is_grabbing() const {
	return cloth != nullptr && cloth->get_grabbed_particle() >= 0;
}

void ClothBody3D::set_carried(bool p_carried) {
	if (carried == p_carried) {
		return;
	}
	carried = p_carried;
	if (cloth == nullptr) {
		return;
	}

	if (carried) {
		// While carried the cloth keeps its shape and costs nothing: the instance moves, the particles
		// sleep where they were. Free cloth is held a little above where it lay; released, it falls
		// onto what lies under it, lying cloth included.
		_free_lying_collision(true);
		cloth->release_grab();
		cloth->put_to_sleep();
		carry_local = current_local;
		if (!follow_pose && drop_height > 0.0f) {
			const Vector3 lift = get_global_transform().affine_inverse().basis.xform(Vector3(0.0, drop_height, 0.0));
			for (Vector3 &point : carry_local) {
				point += lift;
			}
			render_local = carry_local;
			_draw_settling(0.0f);
		}
		return;
	}

	const Transform3D transform = get_global_transform();
	const LocalVector<Vector3> &shape = follow_pose ? pose_local : carry_local;
	world_scratch.resize(shape.size());
	for (uint32_t i = 0; i < shape.size(); i++) {
		world_scratch[i] = transform.xform(shape[i]);
	}
	cloth->set_targets(world_scratch, true);
	cloth->reset_to_targets();
	cloth->set_release_stamp(JoltCloth3D::next_stamp());
	target_local = pose_local;
	previous_local = shape;
	current_local = shape;
	target_transform = transform;
	step_inverse = transform.affine_inverse();
}

bool ClothBody3D::is_sleeping() const {
	return cloth == nullptr || !cloth->is_active();
}

void ClothBody3D::reset_to_pose() {
	if (cloth == nullptr) {
		return;
	}
	const Transform3D transform = get_global_transform();
	world_scratch.resize(pose_local.size());
	for (uint32_t i = 0; i < pose_local.size(); i++) {
		world_scratch[i] = transform.xform(pose_local[i]);
	}
	_free_lying_collision(true);
	cloth->set_targets(world_scratch, true);
	cloth->reset_to_targets();
	target_local = pose_local;
	previous_local = pose_local;
	current_local = pose_local;
	target_transform = transform;
	targets_dirty = false;
	pose_reshaped = false;
	_draw_pose();
	if (follow_pose) {
		// Exactly in its pose, the cloth rests in it: other cloth may lie on it right away.
		_update_lying_collision();
	}
}

PackedVector3Array ClothBody3D::get_particle_positions() const {
	PackedVector3Array positions;
	// A carried cloth is where it is drawn; its particles wait where it was picked up.
	const LocalVector<Vector3> *local = cloth == nullptr ? &pose_local : (carried ? &carry_local : nullptr);
	if (local != nullptr) {
		const Transform3D transform = get_global_transform();
		for (const Vector3 &point : *local) {
			positions.push_back(transform.xform(point));
		}
		return positions;
	}
	LocalVector<Vector3> world;
	cloth->get_positions(world);
	for (const Vector3 &point : world) {
		positions.push_back(point);
	}
	return positions;
}

AABB ClothBody3D::get_aabb() const {
	if (cloth_mesh.is_null() || cloth_mesh->get_render_mesh().is_null()) {
		return AABB();
	}
	return cloth_mesh->get_render_mesh()->get_aabb();
}

void ClothBody3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cloth_mesh", "mesh"), &ClothBody3D::set_cloth_mesh);
	ClassDB::bind_method(D_METHOD("get_cloth_mesh"), &ClothBody3D::get_cloth_mesh);
	ClassDB::bind_method(D_METHOD("set_pose_position", "position"), &ClothBody3D::set_pose_position);
	ClassDB::bind_method(D_METHOD("get_pose_position"), &ClothBody3D::get_pose_position);
	ClassDB::bind_method(D_METHOD("set_follow_pose", "follow"), &ClothBody3D::set_follow_pose);
	ClassDB::bind_method(D_METHOD("is_following_pose"), &ClothBody3D::is_following_pose);

	ClassDB::bind_method(D_METHOD("set_particle_mass", "mass"), &ClothBody3D::set_particle_mass);
	ClassDB::bind_method(D_METHOD("get_particle_mass"), &ClothBody3D::get_particle_mass);
	ClassDB::bind_method(D_METHOD("set_stretch_compliance", "compliance"), &ClothBody3D::set_stretch_compliance);
	ClassDB::bind_method(D_METHOD("get_stretch_compliance"), &ClothBody3D::get_stretch_compliance);
	ClassDB::bind_method(D_METHOD("set_shear_compliance", "compliance"), &ClothBody3D::set_shear_compliance);
	ClassDB::bind_method(D_METHOD("get_shear_compliance"), &ClothBody3D::get_shear_compliance);
	ClassDB::bind_method(D_METHOD("set_bend_compliance", "compliance"), &ClothBody3D::set_bend_compliance);
	ClassDB::bind_method(D_METHOD("get_bend_compliance"), &ClothBody3D::get_bend_compliance);
	ClassDB::bind_method(D_METHOD("set_crease_compliance", "compliance"), &ClothBody3D::set_crease_compliance);
	ClassDB::bind_method(D_METHOD("get_crease_compliance"), &ClothBody3D::get_crease_compliance);
	ClassDB::bind_method(D_METHOD("set_hold", "hold"), &ClothBody3D::set_hold);
	ClassDB::bind_method(D_METHOD("get_hold"), &ClothBody3D::get_hold);
	ClassDB::bind_method(D_METHOD("set_dihedral_bends", "dihedral"), &ClothBody3D::set_dihedral_bends);
	ClassDB::bind_method(D_METHOD("is_dihedral_bends"), &ClothBody3D::is_dihedral_bends);
	ClassDB::bind_method(D_METHOD("set_shear_across_folds", "shear"), &ClothBody3D::set_shear_across_folds);
	ClassDB::bind_method(D_METHOD("is_shear_across_folds"), &ClothBody3D::is_shear_across_folds);
	ClassDB::bind_method(D_METHOD("set_linear_damping", "damping"), &ClothBody3D::set_linear_damping);
	ClassDB::bind_method(D_METHOD("get_linear_damping"), &ClothBody3D::get_linear_damping);
	ClassDB::bind_method(D_METHOD("set_friction", "friction"), &ClothBody3D::set_friction);
	ClassDB::bind_method(D_METHOD("get_friction"), &ClothBody3D::get_friction);
	ClassDB::bind_method(D_METHOD("set_gravity_scale", "scale"), &ClothBody3D::set_gravity_scale);
	ClassDB::bind_method(D_METHOD("get_gravity_scale"), &ClothBody3D::get_gravity_scale);
	ClassDB::bind_method(D_METHOD("set_vertex_radius", "radius"), &ClothBody3D::set_vertex_radius);
	ClassDB::bind_method(D_METHOD("get_vertex_radius"), &ClothBody3D::get_vertex_radius);
	ClassDB::bind_method(D_METHOD("set_drive_stiffness", "stiffness"), &ClothBody3D::set_drive_stiffness);
	ClassDB::bind_method(D_METHOD("get_drive_stiffness"), &ClothBody3D::get_drive_stiffness);
	ClassDB::bind_method(D_METHOD("set_drive_damping", "damping"), &ClothBody3D::set_drive_damping);
	ClassDB::bind_method(D_METHOD("get_drive_damping"), &ClothBody3D::get_drive_damping);
	ClassDB::bind_method(D_METHOD("set_return_stiffness", "stiffness"), &ClothBody3D::set_return_stiffness);
	ClassDB::bind_method(D_METHOD("get_return_stiffness"), &ClothBody3D::get_return_stiffness);
	ClassDB::bind_method(D_METHOD("set_travel_stiffness", "stiffness"), &ClothBody3D::set_travel_stiffness);
	ClassDB::bind_method(D_METHOD("get_travel_stiffness"), &ClothBody3D::get_travel_stiffness);
	ClassDB::bind_method(D_METHOD("set_return_delay", "delay"), &ClothBody3D::set_return_delay);
	ClassDB::bind_method(D_METHOD("get_return_delay"), &ClothBody3D::get_return_delay);
	ClassDB::bind_method(D_METHOD("set_return_seconds", "seconds"), &ClothBody3D::set_return_seconds);
	ClassDB::bind_method(D_METHOD("get_return_seconds"), &ClothBody3D::get_return_seconds);
	ClassDB::bind_method(D_METHOD("set_max_distance_scale", "scale"), &ClothBody3D::set_max_distance_scale);
	ClassDB::bind_method(D_METHOD("get_max_distance_scale"), &ClothBody3D::get_max_distance_scale);
	ClassDB::bind_method(D_METHOD("set_backstop_distance", "distance"), &ClothBody3D::set_backstop_distance);
	ClassDB::bind_method(D_METHOD("get_backstop_distance"), &ClothBody3D::get_backstop_distance);
	ClassDB::bind_method(D_METHOD("set_backstop_radius", "radius"), &ClothBody3D::set_backstop_radius);
	ClassDB::bind_method(D_METHOD("get_backstop_radius"), &ClothBody3D::get_backstop_radius);
	ClassDB::bind_method(D_METHOD("set_iterations", "iterations"), &ClothBody3D::set_iterations);
	ClassDB::bind_method(D_METHOD("get_iterations"), &ClothBody3D::get_iterations);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &ClothBody3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &ClothBody3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &ClothBody3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &ClothBody3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_lying_collision_layer", "layer"), &ClothBody3D::set_lying_collision_layer);
	ClassDB::bind_method(D_METHOD("get_lying_collision_layer"), &ClothBody3D::get_lying_collision_layer);
	ClassDB::bind_method(D_METHOD("set_drop_height", "height"), &ClothBody3D::set_drop_height);
	ClassDB::bind_method(D_METHOD("get_drop_height"), &ClothBody3D::get_drop_height);
	ClassDB::bind_method(D_METHOD("set_push_response", "response"), &ClothBody3D::set_push_response);
	ClassDB::bind_method(D_METHOD("get_push_response"), &ClothBody3D::get_push_response);

	ClassDB::bind_method(D_METHOD("push", "point", "velocity", "radius"), &ClothBody3D::push);
	ClassDB::bind_method(D_METHOD("grab", "point"), &ClothBody3D::grab);
	ClassDB::bind_method(D_METHOD("move_grab", "point"), &ClothBody3D::move_grab);
	ClassDB::bind_method(D_METHOD("release_grab"), &ClothBody3D::release_grab);
	ClassDB::bind_method(D_METHOD("is_grabbing"), &ClothBody3D::is_grabbing);
	ClassDB::bind_method(D_METHOD("set_carried", "carried"), &ClothBody3D::set_carried);
	ClassDB::bind_method(D_METHOD("is_carried"), &ClothBody3D::is_carried);
	ClassDB::bind_method(D_METHOD("is_sleeping"), &ClothBody3D::is_sleeping);
	ClassDB::bind_method(D_METHOD("reset_to_pose"), &ClothBody3D::reset_to_pose);
	ClassDB::bind_method(D_METHOD("get_particle_positions"), &ClothBody3D::get_particle_positions);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cloth_mesh", PROPERTY_HINT_RESOURCE_TYPE, "ClothMesh"), "set_cloth_mesh", "get_cloth_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pose_position", PROPERTY_HINT_RANGE, "0,16,0.001,or_greater"), "set_pose_position", "get_pose_position");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_pose"), "set_follow_pose", "is_following_pose");

	ADD_GROUP("Fabric", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "particle_mass", PROPERTY_HINT_RANGE, "0.0001,1,0.0001,or_greater,suffix:kg"), "set_particle_mass", "get_particle_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stretch_compliance", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater"), "set_stretch_compliance", "get_stretch_compliance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shear_compliance", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater"), "set_shear_compliance", "get_shear_compliance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bend_compliance", PROPERTY_HINT_RANGE, "0,1,0.0001,or_greater"), "set_bend_compliance", "get_bend_compliance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "crease_compliance", PROPERTY_HINT_RANGE, "-1,1000,0.01,or_greater"), "set_crease_compliance", "get_crease_compliance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hold", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_hold", "get_hold");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dihedral_bends"), "set_dihedral_bends", "is_dihedral_bends");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shear_across_folds"), "set_shear_across_folds", "is_shear_across_folds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "linear_damping", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_linear_damping", "get_linear_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_friction", "get_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity_scale", PROPERTY_HINT_RANGE, "-2,2,0.01"), "set_gravity_scale", "get_gravity_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_radius", PROPERTY_HINT_RANGE, "0,0.05,0.0005,suffix:m"), "set_vertex_radius", "get_vertex_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "iterations", PROPERTY_HINT_RANGE, "1,32,1"), "set_iterations", "get_iterations");

	ADD_GROUP("Pose Drive", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drive_stiffness", PROPERTY_HINT_RANGE, "0,500,0.1,or_greater"), "set_drive_stiffness", "get_drive_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drive_damping", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater"), "set_drive_damping", "get_drive_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "return_stiffness", PROPERTY_HINT_RANGE, "0,500,0.1,or_greater"), "set_return_stiffness", "get_return_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "travel_stiffness", PROPERTY_HINT_RANGE, "-1,500,0.1,or_greater"), "set_travel_stiffness", "get_travel_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "return_delay", PROPERTY_HINT_RANGE, "0,10,0.05,suffix:s"), "set_return_delay", "get_return_delay");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "return_seconds", PROPERTY_HINT_RANGE, "0.05,10,0.05,suffix:s"), "set_return_seconds", "get_return_seconds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_distance_scale", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_max_distance_scale", "get_max_distance_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "push_response", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_push_response", "get_push_response");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "backstop_distance", PROPERTY_HINT_RANGE, "-1,1,0.001,suffix:m"), "set_backstop_distance", "get_backstop_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "backstop_radius", PROPERTY_HINT_RANGE, "0.01,10,0.01,suffix:m"), "set_backstop_radius", "get_backstop_radius");

	ADD_GROUP("Collision", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lying_collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_lying_collision_layer", "get_lying_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drop_height", PROPERTY_HINT_RANGE, "0,0.5,0.005,suffix:m"), "set_drop_height", "get_drop_height");

	ADD_SIGNAL(MethodInfo("sleeping_changed", PropertyInfo(Variant::BOOL, "sleeping")));
}

ClothBody3D::ClothBody3D() {
}

ClothBody3D::~ClothBody3D() {
	_free_physics();
	_free_lying_collision(false);
	if (skeleton.is_valid()) {
		RenderingServer::get_singleton()->free_rid(skeleton);
	}
}
