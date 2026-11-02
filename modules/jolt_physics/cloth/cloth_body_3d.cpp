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

// lestoroer@physics

#include "cloth_body_3d.h"

#include "../jolt_physics_server_3d.h"
#include "../objects/jolt_cloth_3d.h"
#include "../spaces/jolt_space_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

namespace {

// Margin around the particles for the visible mesh, which may stand off the coarse simulation surface.
constexpr real_t RENDER_MARGIN = 0.05;
// After falling asleep the cloth blends from where it rests into the exact pose.
constexpr float SETTLE_SECONDS = 0.3f;
// Fabric untwisting into its pose from far away moves no faster than this.
constexpr real_t SETTLE_SPEED = 0.5;
// After a frame pause the pose jumps by a whole frame of travel; pins moved that far in one step would
// fling the fabric next to them.
constexpr real_t POSE_SPEED_LIMIT = 3.0;
// Still means back near the pose, not merely slow: a slow swing back to the pose is not cut short. Only the full
// return can tell that cloth rests away from its pose, because a visible swing moves farther in REST_STUCK_SECONDS.
constexpr float REST_WINDOW_SECONDS = 0.25f;
constexpr float REST_QUIET_SECONDS = 0.5f;
constexpr float REST_STUCK_SECONDS = 1.5f;
constexpr float REST_TIMEOUT_SECONDS = 12.0f;
constexpr real_t REST_DRIFT = 0.003;
constexpr real_t REST_POSE_DISTANCE = 0.03;
constexpr real_t REST_POSE_MEAN_DISTANCE = 0.01;

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

	RenderingServer *rendering = RenderingServer::get_singleton();
	if (cloth_mesh.is_null() || cloth_mesh->get_render_mesh().is_null() || cloth_mesh->get_particle_count() == 0) {
		set_base(RID());
		return;
	}
	ERR_FAIL_COND_MSG(!cloth_mesh->has_pins(), "ClothBody3D simulates hanging cloth: its ClothMesh must have pinned particles.");

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

	// Hidden cloth has no physics.
	if (is_inside_world() && is_visible_in_tree() && !Engine::get_singleton()->is_editor_hint()) {
		_create_physics();
	}
}

void ClothBody3D::_create_physics() {
	JoltPhysicsServer3D *server = JoltPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_MSG(server, "ClothBody3D requires Jolt Physics as the 3D physics engine.");
	ERR_FAIL_COND_MSG(server->is_on_separate_thread(), "ClothBody3D does not support running physics on a separate thread.");
	JoltSpace3D *space = server->get_space(get_world_3d()->get_space());
	ERR_FAIL_NULL(space);

	const Transform3D transform = get_global_transform();
	JoltCloth3D::Setup setup;
	setup.positions.resize(pose_local.size());
	for (uint32_t i = 0; i < pose_local.size(); i++) {
		setup.positions[i] = transform.xform(pose_local[i]);
	}
	target_local = pose_local;
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
	setup.particle_mass = particle_mass;
	setup.stretch_compliance = stretch_compliance;
	setup.shear_compliance = shear_compliance;
	setup.bend_compliance = bend_compliance;
	setup.crease_compliance = crease_compliance;

	cloth = memnew(JoltCloth3D);
	cloth->set_instance_id(get_instance_id());
	_update_motion();
	cloth->configure(setup);
	cloth->set_space(space);

	target_transform = transform;
	step_inverse = transform.affine_inverse();
	read_physics_frame = Engine::get_singleton()->get_physics_frames();
	was_active = false;
	targets_dirty = false;
	pose_reshaped = false;
	physics_started = false;
	settle_left = 0.0f;
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

// A touched cloth swings on a weak drive; left alone, the drive grows and folds tangled by the swing come apart.
// A moving pose is followed closely and critically damped, so gathering folds do not cross, and when it stops
// the cloth settles at once instead of swinging on.
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
		cloth->wake_up();
	} else {
		_draw_pose();
	}
}

void ClothBody3D::_push_targets() {
	if (cloth == nullptr) {
		return;
	}
	physics_started = true;

	const Transform3D transform = get_global_transform();
	// Reparenting recomputes the transform with rounding; that is no motion.
	if (targets_dirty || !transform.is_equal_approx(target_transform)) {
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
		cloth->wake_up();
	} else {
		targets_moving = false;
	}
	untouched_time += float(get_physics_process_delta_time());
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
	for (uint32_t i = 0; i < world_scratch.size(); i++) {
		drift_most = MAX(drift_most, rest_check_world[i].distance_to(world_scratch[i]));
	}
	real_t away_most = 0.0;
	real_t away_total = 0.0;
	const Transform3D transform = get_global_transform();
	for (uint32_t i = 0; i < world_scratch.size(); i++) {
		const real_t away = transform.xform(pose_local[i]).distance_to(world_scratch[i]);
		away_most = MAX(away_most, away);
		away_total += away;
	}
	const float window = rest_check_time;
	rest_check_world = world_scratch;
	rest_check_time = 0.0f;

	// Cloth under the cursor stays awake for it.
	if (attended) {
		attended = false;
		awake_time = 0.0f;
		rest_quiet_time = 0.0f;
		rest_stuck_time = 0.0f;
		return;
	}
	const real_t count = real_t(MAX(world_scratch.size(), 1u));
	const bool returned = away_most < REST_POSE_DISTANCE || away_total / count < REST_POSE_MEAN_DISTANCE;
	rest_quiet_time = drift_most < REST_DRIFT && returned ? rest_quiet_time + window : 0.0f;
	const bool returning_fully = return_stiffness <= drive_stiffness || untouched_time >= return_delay + return_seconds;
	rest_stuck_time = returning_fully && drift_most < REST_DRIFT ? rest_stuck_time + window : 0.0f;
	if (rest_quiet_time >= REST_QUIET_SECONDS || rest_stuck_time >= REST_STUCK_SECONDS || awake_time >= REST_TIMEOUT_SECONDS) {
		asleep_away = away_most;
		rest_quiet_time = 0.0f;
		rest_stuck_time = 0.0f;
		cloth->put_to_sleep();
	}
}

void ClothBody3D::_update_render() {
	if (cloth == nullptr) {
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
	}
	was_active = true;

	const float fraction = Engine::get_singleton()->get_physics_interpolation_fraction();
	render_local.resize(current_local.size());
	for (uint32_t i = 0; i < current_local.size(); i++) {
		render_local[i] = previous_local[i].lerp(current_local[i], fraction);
	}
	_draw_settling(0.0f);
}

// p_weight blends the drawn shape from the simulation (0) into the exact pose (1).
void ClothBody3D::_draw_settling(float p_weight) {
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
}

// The particles rest in the pose too: the next touch moves the shape that is drawn, and fabric twisted around
// itself does not stay twisted.
void ClothBody3D::_fell_asleep() {
	was_active = false;
	settle_seconds = MAX(SETTLE_SECONDS, float(asleep_away / SETTLE_SPEED));
	settle_left = settle_seconds;
	cloth->rest_in_targets();
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

#define CLOTH_REBUILD_SETTER(m_name, m_type)         \
	void ClothBody3D::set_##m_name(m_type p_value) { \
		m_name = p_value;                            \
		if (cloth != nullptr) {                      \
			_rebuild();                              \
		}                                            \
	}

#define CLOTH_MOTION_SETTER(m_name, m_type)          \
	void ClothBody3D::set_##m_name(m_type p_value) { \
		m_name = p_value;                            \
		_update_motion();                            \
	}

CLOTH_REBUILD_SETTER(particle_mass, float)
CLOTH_REBUILD_SETTER(stretch_compliance, float)
CLOTH_REBUILD_SETTER(shear_compliance, float)
CLOTH_REBUILD_SETTER(bend_compliance, float)
CLOTH_REBUILD_SETTER(crease_compliance, float)
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
	if (cloth == nullptr) {
		return;
	}
	attended = true;
	untouched_time = 0.0f;
	cloth->push(p_point, p_velocity * push_response, p_radius);
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
	cloth->set_targets(world_scratch, true);
	cloth->reset_to_targets();
	target_local = pose_local;
	previous_local = pose_local;
	current_local = pose_local;
	target_transform = transform;
	targets_dirty = false;
	pose_reshaped = false;
	_draw_pose();
}

PackedVector3Array ClothBody3D::get_particle_positions() const {
	PackedVector3Array positions;
	if (cloth == nullptr) {
		const Transform3D transform = get_global_transform();
		for (const Vector3 &point : pose_local) {
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
	ClassDB::bind_method(D_METHOD("set_iterations", "iterations"), &ClothBody3D::set_iterations);
	ClassDB::bind_method(D_METHOD("get_iterations"), &ClothBody3D::get_iterations);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &ClothBody3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &ClothBody3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &ClothBody3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &ClothBody3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_push_response", "response"), &ClothBody3D::set_push_response);
	ClassDB::bind_method(D_METHOD("get_push_response"), &ClothBody3D::get_push_response);

	ClassDB::bind_method(D_METHOD("push", "point", "velocity", "radius"), &ClothBody3D::push);
	ClassDB::bind_method(D_METHOD("is_sleeping"), &ClothBody3D::is_sleeping);
	ClassDB::bind_method(D_METHOD("reset_to_pose"), &ClothBody3D::reset_to_pose);
	ClassDB::bind_method(D_METHOD("get_particle_positions"), &ClothBody3D::get_particle_positions);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cloth_mesh", PROPERTY_HINT_RESOURCE_TYPE, "ClothMesh"), "set_cloth_mesh", "get_cloth_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pose_position", PROPERTY_HINT_RANGE, "0,16,0.001,or_greater"), "set_pose_position", "get_pose_position");

	ADD_GROUP("Fabric", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "particle_mass", PROPERTY_HINT_RANGE, "0.0001,1,0.0001,or_greater,suffix:kg"), "set_particle_mass", "get_particle_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stretch_compliance", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater"), "set_stretch_compliance", "get_stretch_compliance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shear_compliance", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater"), "set_shear_compliance", "get_shear_compliance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bend_compliance", PROPERTY_HINT_RANGE, "0,1,0.0001,or_greater"), "set_bend_compliance", "get_bend_compliance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "crease_compliance", PROPERTY_HINT_RANGE, "-1,1000,0.01,or_greater"), "set_crease_compliance", "get_crease_compliance");
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

	ADD_GROUP("Collision", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
}

ClothBody3D::ClothBody3D() {
}

ClothBody3D::~ClothBody3D() {
	_free_physics();
	if (skeleton.is_valid()) {
		RenderingServer::get_singleton()->free_rid(skeleton);
	}
}
