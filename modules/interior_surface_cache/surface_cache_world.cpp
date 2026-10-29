// Fork(Lestoroer): native scene update loop for Interior Star Surface Cache.
#include "surface_cache_world.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_server.h"

#include <cstring>

using namespace SurfaceCache;
bool SurfaceCacheWorldRD::measure_gpu_time = false;
double SurfaceCacheWorldRD::gpu_time_ms = 0;
static const char *buffer_names[] = { "surfaces", "triangles", "materials", "material_ranks", "material_flags", "lights", "material_changes", "surface_flags" };
static constexpr uint32_t MATERIAL_BYTES = 48, TRIANGLE_BYTES = 144, SURFACE_BYTES = 32, LIGHT_BYTES = 64;
static constexpr uint32_t PAGE_BYTES = 2097152;
static uint32_t page_size(uint32_t size) {
	return size > PAGE_BYTES ? ((size + PAGE_BYTES - 1) / PAGE_BYTES) * PAGE_BYTES : size;
}
static uint32_t round_up(uint32_t value, uint32_t step) {
	return ((value + step - 1) / step) * step;
}

void SurfaceCacheWorldRD::_bind_methods() {
	BIND_CONSTANT(MATERIAL_RANK_CHUNK);
	BIND_CONSTANT(CAPTURE_TEXEL_BYTES);
	ClassDB::bind_method(D_METHOD("attach", "world", "settings"), &SurfaceCacheWorldRD::attach);
	ClassDB::bind_method(D_METHOD("update_profile", "settings"), &SurfaceCacheWorldRD::update_profile);
	ClassDB::bind_method(D_METHOD("set_rooms", "rooms"), &SurfaceCacheWorldRD::set_rooms);
	ClassDB::bind_method(D_METHOD("dispose"), &SurfaceCacheWorldRD::dispose);
	ClassDB::bind_method(D_METHOD("get_error"), &SurfaceCacheWorldRD::get_error);
	ClassDB::bind_method(D_METHOD("get_metrics"), &SurfaceCacheWorldRD::get_metrics);
	ClassDB::bind_method(D_METHOD("get_operations"), &SurfaceCacheWorldRD::get_operations);
	ClassDB::bind_method(D_METHOD("get_operation_ms"), &SurfaceCacheWorldRD::get_operation_ms);
	ClassDB::bind_method(D_METHOD("buffer_report"), &SurfaceCacheWorldRD::buffer_report);
	ClassDB::bind_method(D_METHOD("get_probe_resources"), &SurfaceCacheWorldRD::get_probe_resources);
	ClassDB::bind_static_method(get_class_static(), D_METHOD("set_measure_gpu_time", "enabled"), &SurfaceCacheWorldRD::set_measure_gpu_time);
	ClassDB::bind_static_method(get_class_static(), D_METHOD("get_gpu_time_ms"), &SurfaceCacheWorldRD::get_gpu_time_ms);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error"), "", "get_error");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "metrics"), "", "get_metrics");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "operations"), "", "get_operations");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "operation_ms"), "", "get_operation_ms");
}

void SurfaceCacheWorldRD::attach(RID world, const Ref<Resource> &settings) {
	ERR_FAIL_COND(initialized || disposed || settings.is_null());
	scenario = world;
	profile = settings;
	RenderingServer::get_singleton()->scenario_set_surface_cache_callback(world, callable_mp(this, &SurfaceCacheWorldRD::render_frame));
	RenderingServer::get_singleton()->scenario_set_surface_cache_view_callback(world, callable_mp(this, &SurfaceCacheWorldRD::render_view));
}

void SurfaceCacheWorldRD::update_profile(const Ref<Resource> &settings) {
	if (disposed) {
		return;
	}
	ERR_FAIL_COND(settings.is_null() || settings->call("layout_key") != profile->call("layout_key"));
	sky_changed |= settings->get("sky_radiance") != profile->get("sky_radiance") || settings->get("trace_distance") != profile->get("trace_distance");
	profile = settings;
}

void SurfaceCacheWorldRD::set_rooms(const Array &p_rooms) {
	rooms = p_rooms;
	rooms_dirty = true;
}

bool SurfaceCacheWorldRD::initialize_gpu() {
	ScopeTimer timer(counters.initialize_ms);
	device = RenderingServer::get_singleton()->get_rendering_device();
	if (!device || !device->has_feature(RD::SUPPORTS_RAY_QUERY)) {
		return fail("Surface Cache requires hardware ray tracing in the isolated Forward+ renderer");
	}
	const char *names[] = { "geometry_update", "material_resolve", "material_move", "material_pose", "cell_flags_clear", "material_ranks" };
	for (int i = 0; i < PASS_COUNT; ++i) {
		if (!kernels[i].initialize(device, names[i])) {
			return fail(String("Surface Cache shader compilation failed: ") + names[i] + ": " + kernels[i].error);
		}
	}
	world_hash = script_object("world_hash_cache");
	capture = script_object("surface_material");
	gather = script_object("final_gather");
	if (world_hash.is_null() || capture.is_null() || gather.is_null()) {
		return fail("Cannot initialize Surface Cache helpers");
	}
	if (!bool(world_hash->call("initialize", device))) {
		return fail(world_hash->get("error"));
	}
	if (!gather->initialize(device, world_hash)) {
		return fail(gather->get_error());
	}
	dummy = device->storage_buffer_create(64);
	device->buffer_clear(dummy, 0, 64);
	initialized = true;
	return true;
}

void SurfaceCacheWorldRD::render_frame() {
	if (disposed || !error.is_empty() || (!initialized && !initialize_gpu())) {
		return;
	}
	if (rooms_dirty) {
		world_hash->call("set_rooms", rooms);
		rooms_dirty = false;
	}
	if (measure_gpu_time) {
		read_gpu_time();
	}
	world_hash->call("begin_frame");
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	int64_t previous_frame = frame;
	update_world();
	cpu_submit_ms = milliseconds(start);
	counters.frame_ms += cpu_submit_ms;
	if (previous_frame == 0 && frame == 1) {
		print_line("SURFACE_CACHE_WORLD_READY " + JSON::stringify(get_metrics()));
	}
}

static String geometry_key(const Dictionary &change) {
	Transform3D transform = change["transform"];
	Vector3 scale = transform.basis.get_scale().abs();
	const int64_t scale_key = int64_t(Math::ceil(double(MAX(scale.x, MAX(scale.y, scale.z))) * 1000.0));
	return vformat("%s/%d/%d/%s", change["base"], change["mesh_revision"], scale_key, change["surface_sides"]);
}

void SurfaceCacheWorldRD::update_world() {
	frame_moves.clear();
	if (buffers[MATERIAL_CHANGES].is_valid()) {
		device->buffer_clear(buffers[MATERIAL_CHANGES], 0, 16);
	}
	const int capture_limit = frame > 0 ? 4 : 64;
	Array changes = take_changes(capture_limit);
	// Fork(Lestoroer): read each new source before uploading capture meshes and building BLAS.
	// The snapshot lives only for this update; changes in later frames are read afresh.
	Array selected;
	int captures = 0;
	for (int i = 0; i < changes.size(); ++i) {
		Dictionary change = changes[i];
		RID source = change["instance"];
		ChangeKind kind = change_kind(change);
		if (kind == ADD || kind == REPLACE || (kind == RECAPTURE && !instances[source]->deformed)) {
			if (captures == capture_limit) {
				deferred_changes.insert(source, change);
				continue;
			}
			++captures;
		}
		selected.push_back(change);
	}
	HashMap<RID, Vector<RenderingServerTypes::SurfaceData>> sources;
	{
		RenderingServer *server = RenderingServer::get_singleton();
		for (int i = 0; i < selected.size(); ++i) {
			Dictionary change = selected[i];
			ChangeKind kind = change_kind(change);
			if (kind != ADD && kind != REPLACE) {
				continue;
			}
			RID base = change["base"];
			if (sources.has(base) || geometries.has(geometry_key(change))) {
				continue;
			}
			ScopeTimer read_timer(counters.mesh_read_ms);
			ScopeTimer layout_timer(counters.layouts_ms);
			Vector<RenderingServerTypes::SurfaceData> &surfaces = sources[base];
			for (int surface = 0; surface < server->mesh_get_surface_count(base); ++surface) {
				surfaces.push_back(RendererRD::MeshStorage::get_singleton()->mesh_get_surface(base, surface, false));
			}
		}
	}
	bool geometry_changed = false;
	LocalVector<Instance *> dirty;
	for (int i = 0; i < selected.size(); ++i) {
		Dictionary change = selected[i];
		RID source = change["instance"];
		ChangeKind kind = change_kind(change);
		if (kind == REMOVE) {
			if (instances.has(source)) {
				remove_instance(source);
				geometry_changed = true;
			}
			if (lights.has(source)) {
				note_light_change(lights[source], PackedByteArray());
				lights.erase(source);
				lights_dirty = true;
				capacity_dirty = true;
			}
			continue;
		}
		if (kind == LIGHT) {
			PackedByteArray encoded = encode_light(change);
			if (encoded.is_empty()) {
				return;
			}
			if (!lights.has(source) || lights[source] != encoded) {
				note_light_change(lights.has(source) ? lights[source] : PackedByteArray(), encoded);
				lights.insert(source, encoded);
				lights_dirty = true;
				capacity_dirty = true;
			}
			continue;
		}
		if (bool(change["shader_displacement"])) {
			fail("Surface Cache does not support shader vertex displacement; use mesh, skin or morph geometry");
			return;
		}
		if (kind == ADD) {
			if (!add_instance(change, sources)) {
				return;
			}
			geometry_changed = true;
		} else {
			Instance &state = *instances[source];
			AABB bounds = change["bounds"];
			note_dirty(state.bounds.merge(bounds));
			state.bounds = bounds;
			if (kind == REPLACE) {
				remove_instance(source);
				if (!add_instance(change, sources)) {
					return;
				}
				geometry_changed = true;
			} else {
				if (kind == RECAPTURE) {
					state.capture_dirty = true;
				} else {
					moved.insert(source, true);
				}
				if (kind == MOVE_INSTANCE) {
					state.move_from = state.transform;
					state.has_move = true;
				}
				Transform3D transform = change["transform"];
				if (!state.transform.is_equal_approx(transform)) {
					frame_moves.push_back({ state.transform, transform, bounds });
				}
				state.transform = transform;
				int bits = change["changes"];
				state.geometry_dirty |= (bits & 10) != 0;
				state.pose_dirty |= (bits & 8) != 0;
				if (state.geometry_dirty) {
					tlas_updates.insert(source, true);
					geometry_changed = true;
				}
			}
		}
		dirty.push_back(instances[source]);
	}
	LocalVector<RID> settled;
	for (const KeyValue<RID, bool> &entry : moved) {
		Instance &state = *instances[entry.key];
		if ((state.geometry_dirty && !state.capture_dirty) || captures == capture_limit) {
			continue;
		}
		++captures;
		settled.push_back(entry.key);
		note_dirty(state.bounds);
		if (!state.capture_dirty) {
			state.capture_dirty = true;
			dirty.push_back(&state);
		}
	}
	for (RID source : settled) {
		moved.erase(source);
	}
	if (instances.is_empty()) {
		RenderingServer::get_singleton()->scenario_set_surface_cache_buffers(scenario, TypedArray<RID>());
		if (tlas.is_valid()) {
			device->free_rid(tlas);
			tlas = RID();
			tlas_capacity = 0;
		}
		tlas_instances.clear();
		tlas_owners.clear();
		tlas_added.clear();
		tlas_updates.clear();
		return;
	}
	if (!ensure_capacity()) {
		return;
	}
	mark("SurfaceCache update begin", true);
	for (Instance *state : dirty) {
		if (!state->ranks_written) {
			write_ranks(*state);
		}
		bool posed = state->deformed && state->geometry_dirty && !state->capture_dirty;
		if (state->geometry_dirty) {
			update_geometry(*state);
		}
		if (state->capture_dirty) {
			capture_material(*state);
		} else if (state->has_move) {
			move_material(*state);
		} else if (posed) {
			pose_material(*state);
		}
		if (!error.is_empty()) {
			return;
		}
	}
	if (Vector2i(capture->get("size")) != Vector2i() && frame - last_capture_frame > 120) {
		capture->call("dispose");
	}
	if (opacity_requested && !opacity_pending) {
		request_opacity();
	}
	mark("SurfaceCache capture end");
	if (geometry_changed || !tlas.is_valid() || !tlas_updates.is_empty()) {
		rebuild_tlas();
	}
	mark("SurfaceCache tlas end", true);
	if (lights_dirty) {
		upload_lights();
		lights_dirty = false;
	}
	if (!error.is_empty()) {
		return;
	}
	RenderingServer::get_singleton()->scenario_set_surface_cache_buffers(scenario, world_hash->call("engine_buffers"));
	world_hash->call("note_changes", dirty_bounds, light_changes, sun_changed || sky_changed);
	world_hash->set("anchor_margin", profile->get("texel_size"));
	dirty_bounds.clear();
	light_changes.clear();
	sun_changed = false;
	sky_changed = false;
	if (frame > 0 || deferred_changes.is_empty()) {
		++frame;
	}
}

Array SurfaceCacheWorldRD::take_changes(int limit) {
	Array fresh;
	Array polled = RenderingServer::get_singleton()->scenario_surface_cache_poll(scenario);
	for (int i = 0; i < polled.size(); ++i) {
		Dictionary change = polled[i];
		RID source = change["instance"];
		if (bool(change["active"]) && deferred_changes.has(source)) {
			change = change.duplicate();
			change["changes"] = int(change["changes"]) | int(deferred_changes[source]["changes"]);
			deferred_changes[source] = change;
		} else {
			deferred_changes.erase(source);
			fresh.push_back(change);
		}
	}
	Array result;
	LocalVector<RID> selected;
	for (const KeyValue<RID, Dictionary> &entry : deferred_changes) {
		if (int(selected.size()) == limit) {
			break;
		}
		selected.push_back(entry.key);
		result.push_back(entry.value);
	}
	for (RID source : selected) {
		deferred_changes.erase(source);
	}
	result.append_array(fresh);
	return result;
}

SurfaceCacheWorldRD::ChangeKind SurfaceCacheWorldRD::change_kind(const Dictionary &change) const {
	if (!bool(change["active"])) {
		return REMOVE;
	}
	if (int(change["type"]) == RSE::INSTANCE_LIGHT) {
		return LIGHT;
	}
	RID source = change["instance"];
	if (!instances.has(source)) {
		return ADD;
	}
	const Instance &state = *instances[source];
	Transform3D transform = change["transform"];
	if (state.base != RID(change["base"]) || state.mesh_revision != int64_t(change["mesh_revision"]) || state.sides != PackedInt32Array(change["surface_sides"]) || !state.transform.basis.get_scale().is_equal_approx(transform.basis.get_scale())) {
		return REPLACE;
	}
	int bits = change["changes"];
	if ((bits & ~10) == 0 && state.capture_serial > 0 && !state.capture_dirty) {
		if (state.deformed) {
			return POSE_INSTANCE;
		}
		if (bits == 2) {
			return MOVE_INSTANCE;
		}
	}
	return RECAPTURE;
}

String SurfaceCacheWorldRD::render_view(RenderData *data) {
	if (disposed || !error.is_empty() || !initialized || !tlas.is_valid() || instances.is_empty() || !data) {
		return String();
	}
	Ref<RenderSceneBuffers> view = data->get_render_scene_buffers();
	if (view.is_null()) {
		return String();
	}
	String suffix = " " + itos(int64_t(view->get_instance_id()));
	mark("SurfaceCache gather begin" + suffix, true);
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	RID resources[] = { buffers[SURFACES], buffers[TRIANGLES], buffers[MATERIALS], buffers[LIGHTS] };
	String result = gather->render(data, resources, buffers[MATERIAL_FLAGS], buffers[MATERIAL_RANKS], tlas, profile, lights.size(), buffers[MATERIAL_CHANGES], frame_moves);
	counters.gather_ms += milliseconds(start);
	mark("SurfaceCache gather end" + suffix, true);
	if (!gather->get_error().is_empty()) {
		fail(gather->get_error());
		return String();
	}
	return result;
}

LocalVector<RID> SurfaceCacheWorldRD::vertex_buffers(const Geometry &geometry) {
	LocalVector<RID> result;
	for (int i = 0; i < geometry.vertex_data.size(); ++i) {
		PackedByteArray data = geometry.vertex_data[i];
		result.push_back(device->vertex_buffer_create(data.size(), data, RD::BUFFER_CREATION_AS_STORAGE_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT));
	}
	return result;
}

RID SurfaceCacheWorldRD::create_blas(const LocalVector<RID> &vertices, const Geometry &geometry, bool build) {
	ScopeTimer timer(counters.blas_ms);
	counters.blas_builds += int(build);
	LocalVector<RD::AccelerationStructureGeometry> descriptions;
	for (uint32_t i = 0; i < vertices.size(); ++i) {
		RD::AccelerationStructureGeometry description;
		description.vertex_buffer = vertices[i];
		description.vertex_stride = 12;
		description.vertex_count = PackedByteArray(geometry.vertex_data[i]).size() / 12;
		description.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		descriptions.push_back(description);
	}
	RID blas = device->blas_create(descriptions, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	if (!blas.is_valid() || (build && device->blas_build(blas, false) != OK)) {
		fail("Surface Cache BLAS creation failed");
	}
	return blas;
}

bool SurfaceCacheWorldRD::add_instance(const Dictionary &change, const HashMap<RID, Vector<RenderingServerTypes::SurfaceData>> &sources) {
	Transform3D transform = change["transform"];
	Vector3 scale = transform.basis.get_scale().abs();
	double scale_bound = Math::ceil(double(MAX(scale.x, MAX(scale.y, scale.z))) * 1000.0) / 1000.0;
	if (scale_bound <= 0.0) {
		return fail("Surface Cache received a singular instance transform");
	}
	String key = geometry_key(change);
	if (!geometries.has(key)) {
		++counters.layouts;
		uint64_t start = OS::get_singleton()->get_ticks_usec();
		Vector<float> texel_sizes;
		texel_sizes.push_back(double(profile->get("texel_size")) / scale_bound);
		const Vector<RenderingServerTypes::SurfaceData> *source = sources.getptr(change["base"]);
		Dictionary prepared = source ? Mesh::surface_cache_prepare_from_surfaces(change["base"], *source, texel_sizes, change["surface_sides"]) : Mesh::surface_cache_prepare_from_rid(change["base"], texel_sizes, change["surface_sides"]);
		if (prepared.is_empty()) {
			return fail("Surface Cache geometry preparation failed");
		}
		Dictionary preparation = prepared["operation_ms"];
		counters.mesh_read_ms += double(preparation["mesh_read"]);
		counters.mesh_layout_ms += double(preparation["mesh_layout"]);
		counters.mesh_pack_ms += double(preparation["mesh_pack"]);
		counters.capture_mesh_ms += double(preparation["capture_mesh"]);
		counters.mesh_prepare_ms += milliseconds(start);
		counters.layouts_ms += milliseconds(start);
		Array layouts = prepared["layouts"];
		Dictionary layout = layouts[0];
		Geometry *geometry = memnew(Geometry);
		geometry->key = key;
		geometry->mesh = prepared["mesh"];
		geometry->size = layout["size"];
		geometry->sample_count = layout["sample_count"];
		geometry->triangles = prepared["triangle_data"];
		Array surfaces = layout["surfaces"];
		for (int i = 0; i < surfaces.size(); ++i) {
			Dictionary surface = surfaces[i];
			geometry->surfaces.push_back({ surface["offset"], surface["size"], int(PackedByteArray(geometry->triangles[i]).size() / TRIANGLE_BYTES) });
		}
		geometry->vertex_data = prepared["vertex_data"];
		geometry->coverage = prepared["material_coverage"];
		geometry->coverage_ranks = prepared["material_coverage_ranks"];
		geometry->vertices = vertex_buffers(*geometry);
		geometry->blas = create_blas(geometry->vertices, *geometry);
		geometry->coverage_buffer = device->storage_buffer_create(geometry->coverage.size() * 4, Span<uint8_t>(reinterpret_cast<const uint8_t *>(geometry->coverage.ptr()), geometry->coverage.size() * 4));
		geometry->ranks_buffer = device->storage_buffer_create(geometry->coverage_ranks.size() * 4, Span<uint8_t>(reinterpret_cast<const uint8_t *>(geometry->coverage_ranks.ptr()), geometry->coverage_ranks.size() * 4));
		capture_dimensions = capture_dimensions.max(geometry->size);
		geometries.insert(key, geometry);
	}
	Geometry &geometry = *geometries[key];
	++geometry.references;
	bool deformed = change["deformed"];
	if (deformed && geometry.local.is_empty()) {
		for (int i = 0; i < geometry.triangles.size(); ++i) {
			PackedByteArray data = geometry.triangles[i];
			geometry.local.push_back(device->storage_buffer_create(data.size(), data));
		}
	}
	Instance *state = memnew(Instance);
	state->generation = ++next_generation;
	state->mesh_revision = change["mesh_revision"];
	state->sides = change["surface_sides"];
	state->bounds = change["bounds"];
	state->base = change["base"];
	state->source = change["instance"];
	state->transform = transform;
	state->shared = &geometry;
	state->deformed = deformed;
	state->node_count = geometry.sample_count;
	state->node_offset = node_ranges.allocate(state->node_count);
	state->first_surface = surface_ranges.allocate(geometry.surfaces.size());
	for (const Geometry::Surface &surface : geometry.surfaces) {
		state->triangle_count += surface.triangles;
	}
	state->first_triangle = triangle_ranges.allocate(state->triangle_count);
	allocate_storage(*state);
	state->chart = RenderingServer::get_singleton()->instance_create();
	RenderingServer::get_singleton()->instance_set_base(state->chart, geometry.mesh->get_rid());
	PackedInt32Array ids;
	ids.resize(geometry.surfaces.size());
	for (int i = 0; i < ids.size(); ++i) {
		ids.set(i, state->first_surface + i);
	}
	if (deformed) {
		state->vertices = vertex_buffers(geometry);
		state->blas = create_blas(state->vertices, geometry, false);
		state->pose_cells = device->storage_buffer_create(state->node_count * 12);
	}
	RenderingServer::get_singleton()->instance_set_surface_cache_ids(state->source, ids);
	tlas_added.push_back(state->source);
	note_dirty(state->bounds);
	instances.insert(state->source, state);
	capacity_dirty = true;
	return error.is_empty();
}

bool SurfaceCacheWorldRD::ensure_capacity() {
	if (!capacity_dirty) {
		return true;
	}
	ScopeTimer timer(counters.capacity_ms);
	int material_count = MAX(material_cells, int(MAX(65536u, round_up(node_ranges.extent, 65536))));
	int storage = MAX(material_storage, int(MAX(65536u, round_up(storage_ranges.extent, 65536))));
	uint32_t sizes[BUFFER_COUNT] = {
		MAX(SURFACE_BYTES, Math::nearest_power_of_2_templated(uint32_t(surface_ranges.extent)) * SURFACE_BYTES),
		MAX(1u, round_up(triangle_ranges.extent, 16384) / 16384) * 16384 * TRIANGLE_BYTES,
		uint32_t(storage) * MATERIAL_BYTES, uint32_t(material_count / 32 * 8), uint32_t(material_count / 16 * 4),
		MAX(LIGHT_BYTES, Math::nearest_power_of_2_templated(uint32_t(lights.size())) * LIGHT_BYTES), 16,
		MAX(16u, Math::nearest_power_of_2_templated(uint32_t(surface_ranges.extent)) * 4)
	};
	uint64_t capture_bytes = uint64_t(capture_dimensions.x) * capture_dimensions.y * CAPTURE_TEXEL_BYTES;
	uint64_t requested = capture_bytes;
	for (uint32_t size : sizes) {
		requested += size;
	}
	uint64_t limit = uint64_t(int(profile->get("memory_limit_mib"))) * 1024 * 1024;
	if (requested > limit) {
		return fail(vformat("Surface Cache capacity exceeds the profile memory limit: %d MiB", (requested + 1048575) / 1048576));
	}
	uint32_t targets[BUFFER_COUNT];
	uint64_t total = capture_bytes;
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		targets[i] = capacities[i] >= sizes[i] ? capacities[i] : page_size(MAX(sizes[i], capacities[i] * 3 / 2));
		total += targets[i];
	}
	if (total > limit) {
		for (int i = 0; i < BUFFER_COUNT; ++i) {
			if (capacities[i] < sizes[i]) {
				targets[i] = page_size(sizes[i]);
			}
		}
	}
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		uint32_t size = targets[i], current = capacities[i];
		if (current >= size) {
			continue;
		}
		RID replacement = device->storage_buffer_create(size);
		if (!replacement.is_valid()) {
			return fail(String("Cannot allocate Surface Cache buffer: ") + buffer_names[i]);
		}
		if (buffers[i].is_valid()) {
			device->buffer_copy(buffers[i], replacement, 0, 0, current);
			device->free_rid(buffers[i]);
		}
		device->buffer_clear(replacement, current, size - current);
		buffers[i] = replacement;
		capacities[i] = size;
	}
	material_cells = material_count;
	material_storage = storage;
	buffer_memory_bytes = int64_t(world_hash->call("buffer_bytes"));
	for (uint32_t size : capacities) {
		buffer_memory_bytes += size;
	}
	capacity_dirty = false;
	return true;
}

static int bit_count(uint32_t bits) {
	int result = 0;
	while (bits) {
		bits &= bits - 1;
		++result;
	}
	return result;
}

int SurfaceCacheWorldRD::coverage_rank(const Geometry &geometry, int local) {
	int word = local >> 5;
	if (word >= geometry.coverage.size()) {
		return geometry.coverage.is_empty() ? 0 : geometry.coverage_ranks[geometry.coverage_ranks.size() - 1] + bit_count(geometry.coverage[geometry.coverage.size() - 1]);
	}
	return geometry.coverage_ranks[word] + bit_count(uint32_t(geometry.coverage[word]) & ((1u << (local & 31)) - 1));
}

void SurfaceCacheWorldRD::allocate_storage(Instance &state) {
	int first = state.node_offset, end = first + state.node_count;
	state.whole_first = (first + 31) / 32;
	int whole_end = MAX(state.whole_first, end / 32);
	state.whole_count = whole_end - state.whole_first;
	state.storage_rank = state.whole_count > 0 ? coverage_rank(*state.shared, state.whole_first * 32 - first) : 0;
	state.storage_count = state.whole_count > 0 ? coverage_rank(*state.shared, whole_end * 32 - first) - state.storage_rank : 0;
	state.storage_offset = state.storage_count > 0 ? storage_ranges.allocate(state.storage_count) : 0;
	// Only the first and last words can straddle another instance. Interior words are compact.
	int boundary_words[] = { first >> 5, (end - 1) >> 5 };
	for (int i = 0; i < 2; ++i) {
		int word = boundary_words[i];
		if ((i == 1 && word == boundary_words[0]) || (word >= state.whole_first && word < whole_end)) {
			continue;
		}
		if (!rank_chunks.has(word)) {
			RankChunk chunk;
			chunk.offset = storage_ranges.allocate(32);
			rank_chunks.insert(word, chunk);
		}
		++rank_chunks[word].references;
		state.chunks.push_back(word);
	}
}

void SurfaceCacheWorldRD::write_ranks(Instance &state) {
	if (state.whole_count > 0) {
		RID set = cached_buffers(kernels[RANKS], { state.shared->coverage_buffer, state.shared->ranks_buffer, buffers[MATERIAL_RANKS] });
		uint32_t push[] = { uint32_t(state.whole_first), uint32_t(state.whole_count), uint32_t(state.node_offset), uint32_t(state.storage_offset - state.storage_rank) };
		kernels[RANKS].elements(set, push, sizeof(push), state.whole_count);
	}
	for (int word : state.chunks) {
		RankChunk &chunk = rank_chunks[word];
		if (!chunk.written) {
			uint32_t data[] = { 0, uint32_t(chunk.offset) | MATERIAL_RANK_CHUNK };
			device->buffer_update(buffers[MATERIAL_RANKS], word * 8, sizeof(data), data);
			chunk.written = true;
		}
	}
	state.ranks_written = true;
}

void SurfaceCacheWorldRD::release_storage(Instance &state) {
	bool clear = buffers[MATERIALS].is_valid();
	if (state.storage_count > 0) {
		if (clear) {
			device->buffer_clear(buffers[MATERIALS], state.storage_offset * MATERIAL_BYTES, state.storage_count * MATERIAL_BYTES);
		}
		storage_ranges.release(state.storage_offset, state.storage_count);
	}
	if (clear && state.whole_count > 0) {
		device->buffer_clear(buffers[MATERIAL_RANKS], state.whole_first * 8, state.whole_count * 8);
	}
	int first = state.node_offset, end = first + state.node_count;
	for (int word : state.chunks) {
		RankChunk &chunk = rank_chunks[word];
		int low = MAX(first, word * 32) - word * 32;
		int high = MIN(end, word * 32 + 32) - word * 32;
		if (clear) {
			device->buffer_clear(buffers[MATERIALS], (chunk.offset + low) * MATERIAL_BYTES, (high - low) * MATERIAL_BYTES);
		}
		if (--chunk.references == 0) {
			if (clear) {
				device->buffer_clear(buffers[MATERIAL_RANKS], word * 8, 8);
			}
			storage_ranges.release(chunk.offset, 32);
			rank_chunks.erase(word);
		}
	}
}

void SurfaceCacheWorldRD::clear_flags(int first, int count) {
	int words = (first + count - 1) / 16 - first / 16 + 1;
	RID set = cached_buffers(kernels[CLEAR_FLAGS], { buffers[MATERIAL_FLAGS] });
	int32_t push[] = { first, count, 0, 0 };
	kernels[CLEAR_FLAGS].elements(set, push, sizeof(push), words);
}

void SurfaceCacheWorldRD::update_geometry(Instance &state) {
	ScopeTimer timer(counters.geometry_ms);
	++counters.geometry_updates;
	Geometry &geometry = *state.shared;
	int first = state.first_triangle;
	for (uint32_t surface = 0; surface < geometry.surfaces.size(); ++surface) {
		PackedByteArray triangles_data = geometry.triangles[surface];
		int triangles = geometry.surfaces[surface].triangles;
		Dictionary pose = state.deformed ? RenderingServer::get_singleton()->instance_get_deformed_surface(state.source, surface) : Dictionary();
		RID vertices = state.deformed ? state.vertices[surface] : geometry.vertices[surface];
		if (!state.triangles_uploaded) {
			device->buffer_update(buffers[TRIANGLES], first * TRIANGLE_BYTES, triangles_data.size(), triangles_data.ptr());
		}
		RID set;
		if (!state.deformed && uint32_t(surface) < state.geometry_sets.size() && device->uniform_set_is_valid(state.geometry_sets[surface])) {
			set = state.geometry_sets[surface];
		} else {
			set = cached_buffers(kernels[GEOMETRY], { state.deformed ? geometry.local[surface] : dummy, pose.get("vertex_buffer", dummy), buffers[TRIANGLES], vertices });
			if (!state.deformed) {
				state.geometry_sets.resize(MAX(state.geometry_sets.size(), uint32_t(surface + 1)));
				state.geometry_sets[surface] = set;
			}
		}
		struct Push {
			int32_t header[4];
			float transform[16];
			int32_t pose[4];
		} push = {};
		push.header[0] = triangles;
		push.header[1] = first;
		matrix(push.transform, Projection(state.transform));
		push.pose[0] = !pose.is_empty();
		push.pose[1] = int(pose.get("vertex_stride_words", 0));
		push.pose[2] = state.deformed;
		kernels[GEOMETRY].elements(set, &push, sizeof(push), triangles);
		if (!state.triangles_uploaded) {
			int32_t mapping[] = { first, state.node_offset, geometry.size.x, geometry.size.y, state.sides[surface], 0, 0, 0 };
			device->buffer_update(buffers[SURFACES], (state.first_surface + surface) * SURFACE_BYTES, sizeof(mapping), mapping);
		}
		first += triangles;
	}
	if (state.deformed && state.pose_dirty) {
		device->blas_build(state.blas);
		++counters.blas_builds;
	}
	state.triangles_uploaded = true;
	state.geometry_dirty = false;
	state.pose_dirty = false;
}

void SurfaceCacheWorldRD::capture_material(Instance &state) {
	++counters.captures;
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	last_capture_frame = frame;
	if (Vector2i(capture->get("size")) != capture_dimensions) {
		ScopeTimer allocation_timer(counters.capture_allocate_ms);
		capture->call("dispose");
		if (!bool(capture->call("allocate", device, capture_dimensions))) {
			fail("Surface Cache capture allocation failed");
			return;
		}
	}
	Geometry &geometry = *state.shared;
	device->buffer_clear(buffers[SURFACE_FLAGS], state.first_surface * 4, geometry.surfaces.size() * 4);
	LocalVector<RD::Uniform> bindings;
	for (RID rid : { buffers[TRIANGLES], buffers[MATERIALS], buffers[MATERIAL_CHANGES], buffers[SURFACE_FLAGS], buffers[MATERIAL_FLAGS], buffers[MATERIAL_RANKS], state.deformed ? state.pose_cells : dummy }) {
		bindings.push_back(uniform(bindings.size(), RD::UNIFORM_TYPE_STORAGE_BUFFER, rid));
	}
	TypedArray<RID> textures = capture->get("textures");
	for (int i = 0; i < textures.size(); ++i) {
		bindings.push_back(uniform(bindings.size(), RD::UNIFORM_TYPE_IMAGE, textures[i]));
	}
	RID set = cached(kernels[MATERIAL], bindings);
	int side_count = 0;
	for (int i = 0; i < state.sides.size(); ++i) {
		side_count = MAX(side_count, state.sides[i]);
	}
	for (int side = 0; side < side_count; ++side) {
		{
			ScopeTimer draw_timer(counters.capture_draw_ms);
			capture->call("capture", state.source, state.chart, 0, side, geometry.size);
		}
		int first = state.first_triangle;
		for (uint32_t surface = 0; surface < geometry.surfaces.size(); ++surface) {
			int triangles = geometry.surfaces[surface].triangles;
			if (side < state.sides[surface]) {
				const Geometry::Surface &layout = geometry.surfaces[surface];
				Vector2i offset = layout.offset, size = layout.size;
				int32_t push[] = { geometry.size.x, geometry.size.y, state.node_offset, int(state.deformed), first, state.first_surface + int(surface), triangles, side, offset.x, offset.y, size.x, size.y };
				kernels[MATERIAL].dispatch(set, push, sizeof(push), Vector3i(triangles, 1, 1));
			}
			first += triangles;
		}
	}
	state.capture_dirty = false;
	state.capture_serial = ++capture_serial;
	Array pending;
	pending.push_back(state.source);
	pending.push_back(state.generation);
	opacity_states.push_back(pending);
	opacity_requested = true;
	counters.captures_ms += milliseconds(start);
}

void SurfaceCacheWorldRD::move_material(Instance &state) {
	++counters.moves;
	Transform3D delta = state.transform * state.move_from.affine_inverse();
	state.has_move = false;
	struct Push {
		int32_t header[4];
		float transform[12];
	} push = {};
	push.header[0] = state.node_offset;
	push.header[1] = state.node_count;
	for (int column = 0; column < 3; ++column) {
		for (int row = 0; row < 3; ++row) {
			push.transform[column * 4 + row] = delta.basis.get_column(column)[row];
		}
		push.transform[column * 4 + 3] = delta.origin[column];
	}
	if (!device->uniform_set_is_valid(move_set)) {
		move_set = cached_buffers(kernels[MOVE], { buffers[MATERIALS], buffers[MATERIAL_RANKS] });
	}
	kernels[MOVE].elements(move_set, &push, sizeof(push), state.node_count);
}

void SurfaceCacheWorldRD::pose_material(Instance &state) {
	++counters.poses;
	RID set = cached_buffers(kernels[POSE], { buffers[TRIANGLES], buffers[MATERIALS], buffers[MATERIAL_RANKS], state.pose_cells });
	int32_t push[] = { state.node_offset, state.node_count, 0, 0 };
	kernels[POSE].elements(set, push, sizeof(push), state.node_count);
}

void SurfaceCacheWorldRD::rebuild_tlas() {
	++counters.tlas_builds;
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	for (RID source : tlas_added) {
		if (!instances.has(source) || instances[source]->tlas_index >= 0) {
			continue;
		}
		Instance &state = *instances[source];
		state.tlas_index = tlas_instances.size();
		tlas_instances.push_back(RD::AccelerationStructureInstance());
		tlas_owners.push_back(&state);
		write_tlas_instance(state);
	}
	for (const KeyValue<RID, bool> &entry : tlas_updates) {
		if (instances.has(entry.key) && instances[entry.key]->tlas_index >= 0) {
			write_tlas_instance(*instances[entry.key]);
		}
	}
	tlas_added.clear();
	tlas_updates.clear();
	if (int(tlas_instances.size()) > tlas_capacity) {
		if (tlas.is_valid()) {
			device->free_rid(tlas);
		}
		tlas_capacity = Math::nearest_power_of_2_templated(tlas_instances.size());
		tlas = device->tlas_create(tlas_capacity, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	}
	if (device->tlas_build(tlas, tlas_instances) != OK) {
		fail("Surface Cache TLAS update failed");
	}
	counters.tlas_ms += milliseconds(start);
}

void SurfaceCacheWorldRD::write_tlas_instance(Instance &state) {
	RD::AccelerationStructureInstance &instance = tlas_instances[state.tlas_index];
	instance.blas = state.deformed ? state.blas : state.shared->blas;
	instance.id = state.first_surface;
	instance.transform = state.transform;
	bool opaque = state.opaque.size() == state.shared->surfaces.size();
	for (int i = 0; opaque && i < state.opaque.size(); ++i) {
		opaque &= state.opaque[i] != 0;
	}
	instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT |
			(opaque ? RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT : RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT);
}

void SurfaceCacheWorldRD::remove_tlas_instance(Instance &state) {
	if (state.tlas_index < 0) {
		return;
	}
	int last = tlas_instances.size() - 1;
	if (state.tlas_index != last) {
		tlas_instances[state.tlas_index] = tlas_instances[last];
		tlas_owners[state.tlas_index] = tlas_owners[last];
		tlas_owners[state.tlas_index]->tlas_index = state.tlas_index;
	}
	tlas_instances.resize(last);
	tlas_owners.resize(last);
	state.tlas_index = -1;
}

void SurfaceCacheWorldRD::request_opacity() {
	opacity_requested = false;
	opacity_pending = true;
	Callable callback = callable_mp(this, &SurfaceCacheWorldRD::receive_opacity).bind(capture_serial, opacity_states);
	if (device->buffer_get_data_async(buffers[SURFACE_FLAGS], callback, 0, capacities[SURFACE_FLAGS]) != OK) {
		fail("Cannot read Surface Cache surface opacity asynchronously");
	}
	opacity_states = Array();
}

void SurfaceCacheWorldRD::receive_opacity(const PackedByteArray &data, uint64_t serial, const Array &states) {
	opacity_pending = false;
	if (disposed) {
		return;
	}
	for (int i = 0; i < states.size(); ++i) {
		Array entry = states[i];
		RID source = entry[0];
		if (!instances.has(source)) {
			continue;
		}
		Instance &state = *instances[source];
		if (state.generation != uint64_t(entry[1]) || state.capture_serial > serial || state.opacity_serial >= state.capture_serial) {
			continue;
		}
		state.opacity_serial = state.capture_serial;
		PackedByteArray opaque;
		opaque.resize(state.shared->surfaces.size());
		bool lost = false;
		for (int surface = 0; surface < opaque.size(); ++surface) {
			uint32_t flags;
			memcpy(&flags, data.ptr() + (state.first_surface + surface) * 4, 4);
			opaque.set(surface, flags == 0);
			lost |= surface < state.opaque.size() && state.opaque[surface] == 1 && opaque[surface] == 0;
		}
		if (opaque != state.opaque) {
			state.opaque = opaque;
			tlas_updates.insert(source, true);
			if (lost) {
				note_dirty(state.bounds);
			}
		}
	}
}

PackedByteArray SurfaceCacheWorldRD::encode_light(const Dictionary &light) {
	int type = light["light_type"];
	if (type == RSE::LIGHT_AREA) {
		fail("AreaLight transport is not implemented yet");
		return PackedByteArray();
	}
	Transform3D transform = light["transform"];
	Vector3 direction = -transform.basis.get_column(2).normalized();
	bool directional = type == RSE::LIGHT_DIRECTIONAL;
	Vector3 position = directional ? Vector3() : transform.origin;
	Color color = Color(light["color"]).srgb_to_linear();
	PackedFloat32Array params = light["light_params"];
	float values[] = {
		position.x, position.y, position.z, float(type), direction.x, direction.y, direction.z, directional ? 0.0f : params[RSE::LIGHT_PARAM_RANGE],
		color.r, color.g, color.b, params[RSE::LIGHT_PARAM_ENERGY] * params[RSE::LIGHT_PARAM_INDIRECT_ENERGY],
		params[RSE::LIGHT_PARAM_ATTENUATION], float(Math::cos(Math::deg_to_rad(double(params[RSE::LIGHT_PARAM_SPOT_ANGLE])))),
		params[RSE::LIGHT_PARAM_SPOT_ATTENUATION],
		directional ? float(Math::deg_to_rad(double(params[RSE::LIGHT_PARAM_SIZE])) * 0.5) : MAX(params[RSE::LIGHT_PARAM_SIZE], 0.08f)
	};
	PackedByteArray result;
	result.resize(sizeof(values));
	memcpy(result.ptrw(), values, sizeof(values));
	return result;
}

void SurfaceCacheWorldRD::note_light_change(const PackedByteArray &previous, const PackedByteArray &current) {
	for (const PackedByteArray *data : { &previous, &current }) {
		if (!data->is_empty()) {
			float type;
			memcpy(&type, data->ptr() + 12, 4);
			if (int(type) == RSE::LIGHT_DIRECTIONAL) {
				sun_changed = true;
				return;
			}
		}
	}
	if (previous.is_empty() && current.is_empty()) {
		return;
	}
	PackedByteArray pair;
	pair.resize(LIGHT_BYTES * 2);
	memset(pair.ptrw(), 0, pair.size());
	if (!previous.is_empty()) {
		memcpy(pair.ptrw(), previous.ptr(), LIGHT_BYTES);
	}
	if (!current.is_empty()) {
		memcpy(pair.ptrw() + LIGHT_BYTES, current.ptr(), LIGHT_BYTES);
	}
	light_changes.push_back(pair);
}

void SurfaceCacheWorldRD::note_dirty(const AABB &bounds) {
	dirty_bounds.push_back(bounds.grow(0.001));
}

void SurfaceCacheWorldRD::upload_lights() {
	PackedByteArray data;
	data.resize(MAX(uint32_t(LIGHT_BYTES), lights.size() * LIGHT_BYTES));
	int offset = 0;
	for (const KeyValue<RID, PackedByteArray> &entry : lights) {
		memcpy(data.ptrw() + offset, entry.value.ptr(), LIGHT_BYTES);
		offset += LIGHT_BYTES;
	}
	device->buffer_update(buffers[LIGHTS], 0, data.size(), data.ptr());
}

void SurfaceCacheWorldRD::remove_instance(RID source) {
	Instance *state = instances[source];
	moved.erase(source);
	tlas_updates.erase(source);
	note_dirty(state->bounds);
	RenderingServer::get_singleton()->instance_set_surface_cache_ids(source, PackedInt32Array());
	node_ranges.release(state->node_offset, state->node_count);
	release_storage(*state);
	triangle_ranges.release(state->first_triangle, state->triangle_count);
	surface_ranges.release(state->first_surface, state->shared->surfaces.size());
	if (buffers[MATERIALS].is_valid()) {
		clear_flags(state->node_offset, state->node_count);
	}
	if (state->blas.is_valid()) {
		device->free_rid(state->blas);
	}
	for (RID rid : state->vertices) {
		device->free_rid(rid);
	}
	if (state->pose_cells.is_valid()) {
		device->free_rid(state->pose_cells);
	}
	RenderingServer::get_singleton()->free_rid(state->chart);
	instances.erase(source);
	if (--state->shared->references == 0) {
		release_geometry(state->shared);
	}
	remove_tlas_instance(*state);
	memdelete(state);
	capacity_dirty = true;
}

void SurfaceCacheWorldRD::release_geometry(Geometry *geometry) {
	for (RID rid : { geometry->blas, geometry->coverage_buffer, geometry->ranks_buffer }) {
		if (rid.is_valid()) {
			device->free_rid(rid);
		}
	}
	for (RID rid : geometry->vertices) {
		device->free_rid(rid);
	}
	for (RID rid : geometry->local) {
		device->free_rid(rid);
	}
	geometries.erase(geometry->key);
	memdelete(geometry);
	capture_dimensions = Vector2i();
	for (const KeyValue<String, Geometry *> &entry : geometries) {
		capture_dimensions = capture_dimensions.max(entry.value->size);
	}
}

bool SurfaceCacheWorldRD::fail(const String &message) {
	RenderingServer::get_singleton()->scenario_set_surface_cache_buffers(scenario, TypedArray<RID>());
	error = message;
	ERR_PRINT(message);
	return false;
}

void SurfaceCacheWorldRD::mark(const String &name, bool boundary) {
	if (bool(profile->get("gpu_markers")) || (boundary && measure_gpu_time)) {
		device->capture_timestamp(name);
	}
}

void SurfaceCacheWorldRD::read_gpu_time() {
	uint64_t total = 0, begin = 0;
	bool active = false;
	for (uint32_t i = 0; i < device->get_captured_timestamps_count(); ++i) {
		String name = device->get_captured_timestamp_name(i);
		if (name == "SurfaceCache update begin" || name.begins_with("SurfaceCache gather begin ")) {
			begin = device->get_captured_timestamp_gpu_time(i);
			active = true;
		} else if (active && (name == "SurfaceCache tlas end" || name.begins_with("SurfaceCache gather end "))) {
			total += device->get_captured_timestamp_gpu_time(i) - begin;
			active = false;
		}
	}
	gpu_time_ms = double(total) / 1000000.0;
}

Dictionary SurfaceCacheWorldRD::get_metrics() const {
	Dictionary result;
	if (!initialized) {
		return result;
	}
	result["instances"] = instances.size();
	result["surfaces"] = surface_ranges.extent;
	result["triangles"] = triangle_ranges.extent;
	result["texel_capacity"] = material_cells;
	result["texel_addresses"] = node_ranges.extent;
	result["texel_storage"] = storage_ranges.extent;
	result["world_frames"] = frame;
	result["cpu_submit_ms"] = cpu_submit_ms;
	result["gpu_buffer_bytes"] = buffer_memory_bytes;
	Vector2i size = capture->get("size");
	result["capture_texture_bytes"] = uint64_t(size.x) * size.y * CAPTURE_TEXEL_BYTES;
	return result;
}
Dictionary SurfaceCacheWorldRD::get_operations() const {
	Dictionary result;
	result["captures"] = counters.captures;
	result["moves"] = counters.moves;
	result["poses"] = counters.poses;
	result["geometry_updates"] = counters.geometry_updates;
	result["blas_builds"] = counters.blas_builds;
	result["tlas_builds"] = counters.tlas_builds;
	result["layouts"] = counters.layouts;
	return result;
}
Dictionary SurfaceCacheWorldRD::get_operation_ms() const {
	Dictionary result;
	result["frame"] = counters.frame_ms;
	result["layouts"] = counters.layouts_ms;
	result["captures"] = counters.captures_ms;
	result["tlas_builds"] = counters.tlas_ms;
	result["gather"] = counters.gather_ms;
	result["initialize"] = counters.initialize_ms;
	result["mesh_read"] = counters.mesh_read_ms;
	result["mesh_layout"] = counters.mesh_layout_ms;
	result["mesh_pack"] = counters.mesh_pack_ms;
	result["capture_mesh"] = counters.capture_mesh_ms;
	result["capture_allocate"] = counters.capture_allocate_ms;
	result["capture_draw"] = counters.capture_draw_ms;

	result["mesh_prepare"] = counters.mesh_prepare_ms;
	result["blas_create_build"] = counters.blas_ms;
	result["buffer_capacity"] = counters.capacity_ms;
	result["geometry_upload"] = counters.geometry_ms;
	result["view_resources"] = gather.is_valid() ? gather->get_resource_ms() : 0.0;
	return result;
}
Dictionary SurfaceCacheWorldRD::buffer_report() const {
	Dictionary result;
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		result[buffer_names[i]] = capacities[i];
	}
	Vector2i size = capture.is_valid() ? Vector2i(capture->get("size")) : Vector2i();
	result["capture"] = uint64_t(size.x) * size.y * CAPTURE_TEXEL_BYTES;
	result["world_hash"] = world_hash.is_valid() ? world_hash->call("buffer_bytes") : Variant(0);
	return result;
}
Dictionary SurfaceCacheWorldRD::get_probe_resources() const {
	Dictionary resources, result;
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		resources[buffer_names[i]] = buffers[i];
	}
	result["buffers"] = resources;
	result["material_cells"] = material_cells;
	result["hash"] = world_hash;
	result["gather"] = gather;
	return result;
}
void SurfaceCacheWorldRD::dispose() {
	if (disposed) {
		return;
	}
	disposed = true;
	gpu_time_ms = 0;
	RenderingServer *server = RenderingServer::get_singleton();
	server->scenario_set_surface_cache_callback(scenario, Callable());
	server->scenario_set_surface_cache_view_callback(scenario, Callable());
	if (gather.is_valid()) {
		gather->dispose();
	}
	if (world_hash.is_valid()) {
		world_hash->call("dispose");
	}
	if (capture.is_valid()) {
		capture->call("dispose");
	}
	server->scenario_set_surface_cache_buffers(scenario, TypedArray<RID>());
	while (!instances.is_empty()) {
		remove_instance(instances.begin()->key);
	}
	if (!device) {
		return;
	}
	for (RID &rid : buffers) {
		if (rid.is_valid()) {
			device->free_rid(rid);
			rid = RID();
		}
	}
	for (Kernel &kernel : kernels) {
		kernel.dispose();
	}
	for (RID rid : { tlas, dummy }) {
		if (rid.is_valid()) {
			device->free_rid(rid);
		}
	}
	tlas = RID();
	dummy = RID();
}
