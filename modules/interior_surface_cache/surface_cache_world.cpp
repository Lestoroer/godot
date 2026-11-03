// lestoroer#gi
#include "surface_cache_world.h"
#include "surface_cache_geometry.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_server.h"
// lestoroer#local-lights
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cstring>

using namespace SurfaceCache;
bool SurfaceCacheWorldRD::measure_gpu_time = false;
double SurfaceCacheWorldRD::gpu_time_ms = 0;
static const char *buffer_names[] = { "surfaces", "orientations", "materials", "lights", "material_changes", "surface_flags" };
static constexpr uint32_t MATERIAL_BYTES = 64, SURFACE_BYTES = 32, LIGHT_BYTES = 64;
static constexpr uint32_t PAGE_BYTES = 2097152;
// При загрузке допустим больший бюджет: меньший лишь продлевает ожидание; после загрузки работа не должна задерживать игру.
static constexpr double STARTUP_BUDGET_MS = 33.0, UPDATE_BUDGET_MS = 4.0;
static constexpr int UPDATE_CAPTURE_LIMIT = 4;
static uint32_t round_up(uint32_t value, uint32_t step) {
	return ((value + step - 1) / step) * step;
}
// Размер кратен 16 байтам: иначе очистка добавленной части буфера отвергает диапазон.
static uint32_t page_size(uint32_t size) {
	return round_up(size, size > PAGE_BYTES ? PAGE_BYTES : 16);
}

void SurfaceCacheWorldRD::_bind_methods() {
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
	// lestoroer#local-lights
	if (settings->get("local_lights_enabled") != profile->get("local_lights_enabled")) {
		static_cast<RendererSceneCull *>(RSG::scene)->local_light_invalidate_gi(scenario);
	}
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
	if (!device->has_feature(RD::SUPPORTS_RAY_TRACING_POSITION_FETCH)) {
		return fail("Surface Cache requires ray tracing position fetch (VK_KHR_ray_tracing_position_fetch)");
	}
	const char *names[] = { "pose_vertices", "material_resolve" };
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
	return vformat("%s/%d/%s", change["base"], change["mesh_revision"], change["surface_sides"]);
}

void SurfaceCacheWorldRD::update_world() {
	frame_moves.clear();
	if (buffers[MATERIAL_CHANGES].is_valid()) {
		device->buffer_clear(buffers[MATERIAL_CHANGES], 0, 16);
	}
	const int capture_limit = frame > 0 ? UPDATE_CAPTURE_LIMIT : INT_MAX;
	const double budget_ms = frame > 0 ? UPDATE_BUDGET_MS : STARTUP_BUDGET_MS;
	Array changes = take_changes();
	// Все readback выполняются до upload, чтобы чтение не ждало только что отправленных данных.
	struct Pending {
		Dictionary change;
		real_t distance;
		bool operator<(const Pending &p_other) const { return distance < p_other.distance; }
	};
	LocalVector<Pending> pending;
	Array selected;
	for (int i = 0; i < changes.size(); ++i) {
		Dictionary change = changes[i];
		RID source = change["instance"];
		ChangeKind kind = change_kind(change);
		if (kind == ADD || kind == REPLACE || (kind == RECAPTURE && !instances[source]->deformed)) {
			const AABB bounds = change["bounds"];
			pending.push_back({ change, has_view ? bounds.get_center().distance_squared_to(view_origin) : real_t(0) });
		} else {
			selected.push_back(change);
		}
	}
	pending.sort();
	int captures = 0;
	double estimate_ms = 0;
	HashSet<RID> estimated;
	for (const Pending &entry : pending) {
		const Dictionary &change = entry.change;
		RID base = change["base"];
		ChangeKind kind = change_kind(change);
		const bool prepares = (kind == ADD || kind == REPLACE) && !geometries.has(geometry_key(change)) && !estimated.has(base);
		if (captures == capture_limit || (captures > 0 && prepares && estimate_ms >= budget_ms)) {
			deferred_changes.insert(change["instance"], change);
			continue;
		}
		++captures;
		if (prepares) {
			estimated.insert(base);
			estimate_ms += triangle_count(base) * prepare_ms_per_triangle;
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
		if (!state->surfaces_written) {
			write_surfaces(*state);
		}
		if (state->geometry_dirty) {
			update_geometry(*state);
		}
		if (state->capture_dirty) {
			capture_material(*state);
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
	dirty_bounds.clear();
	light_changes.clear();
	sun_changed = false;
	sky_changed = false;
	if (frame == 0 && deferred_changes.is_empty()) {
		fit_capacity();
	}
	if (frame > 0 || deferred_changes.is_empty()) {
		++frame;
	}
}

int SurfaceCacheWorldRD::triangle_count(RID p_base) const {
	RendererRD::MeshStorage *storage = RendererRD::MeshStorage::get_singleton();
	int corners = 0;
	for (int surface = 0; surface < storage->mesh_get_surface_count(p_base); ++surface) {
		corners += storage->mesh_surface_get_vertices_drawn_count(storage->mesh_get_surface(p_base, uint32_t(surface)));
	}
	return corners / 3;
}

Array SurfaceCacheWorldRD::take_changes() {
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
	for (const KeyValue<RID, Dictionary> &entry : deferred_changes) {
		result.push_back(entry.value);
	}
	deferred_changes.clear();
	result.append_array(fresh);
	return result;
}

SurfaceCacheWorldRD::ChangeKind SurfaceCacheWorldRD::change_kind(const Dictionary &change) const {
	if (!bool(change["active"])) {
		return REMOVE;
	}
	if (int(change["type"]) == RSE::INSTANCE_LIGHT) {
		// lestoroer#local-lights
		if (bool(change.get("local_light", false)) && !bool(profile->get("local_lights_enabled"))) return REMOVE;
		return LIGHT;
	}
	RID source = change["instance"];
	if (!instances.has(source)) {
		return ADD;
	}
	const Instance &state = *instances[source];
	if (state.base != RID(change["base"]) || state.mesh_revision != int64_t(change["mesh_revision"]) || state.sides != PackedInt32Array(change["surface_sides"])) {
		return REPLACE;
	}
	int bits = change["changes"];
	if ((bits & ~10) == 0 && state.capture_serial > 0 && !state.capture_dirty && (state.deformed || bits == 2)) {
		return MOVE_OR_POSE;
	}
	return RECAPTURE;
}

String SurfaceCacheWorldRD::render_view(RenderData *data) {
	if (data && data->get_render_scene_data()) {
		view_origin = data->get_render_scene_data()->get_cam_transform().origin;
		has_view = true;
	}
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
	RID resources[] = { buffers[SURFACES], buffers[ORIENTATIONS], buffers[MATERIALS], buffers[LIGHTS] };
	String result = gather->render(data, resources, tlas, profile, lights.size(), buffers[MATERIAL_CHANGES], frame_moves);
	counters.gather_ms += milliseconds(start);
	mark("SurfaceCache gather end" + suffix, true);
	if (!gather->get_error().is_empty()) {
		fail(gather->get_error());
		return String();
	}
	return result;
}

static constexpr uint32_t RAY_GEOMETRY_BITS = RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;
static constexpr uint32_t POSED_VERTEX_BITS = RD::BUFFER_CREATION_AS_STORAGE_BIT | RAY_GEOMETRY_BITS;

LocalVector<RID> SurfaceCacheWorldRD::vertex_buffers(const TypedArray<PackedByteArray> &p_vertex_data) {
	LocalVector<RID> result;
	for (int i = 0; i < p_vertex_data.size(); ++i) {
		PackedByteArray data = p_vertex_data[i];
		result.push_back(device->vertex_buffer_create(data.size(), data, RAY_GEOMETRY_BITS));
	}
	return result;
}

LocalVector<RID> SurfaceCacheWorldRD::index_buffers(const TypedArray<PackedByteArray> &p_index_data) {
	LocalVector<RID> result;
	for (int i = 0; i < p_index_data.size(); ++i) {
		PackedByteArray data = p_index_data[i];
		result.push_back(device->index_buffer_create(data.size() / 4, RD::INDEX_BUFFER_FORMAT_UINT32, data, false, RAY_GEOMETRY_BITS));
	}
	return result;
}

LocalVector<RID> SurfaceCacheWorldRD::copy_vertex_buffers(const Geometry &geometry) {
	LocalVector<RID> result;
	for (uint32_t i = 0; i < geometry.surfaces.size(); ++i) {
		const uint32_t size = geometry.surfaces[i].vertices * 12;
		RID copy = device->vertex_buffer_create(size, Span<uint8_t>(), POSED_VERTEX_BITS);
		device->buffer_copy(geometry.vertices[i], copy, 0, 0, size);
		result.push_back(copy);
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
		description.vertex_count = geometry.surfaces[i].vertices;
		description.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		description.index_buffer = geometry.indices[i];
		description.index_count = geometry.surfaces[i].triangles * 3;
		descriptions.push_back(description);
	}
	RID blas = device->blas_create(descriptions, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT | RD::ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT);
	if (!blas.is_valid() || (build && device->blas_build(blas, false) != OK)) {
		fail("Surface Cache BLAS creation failed");
	}
	return blas;
}

bool SurfaceCacheWorldRD::add_instance(const Dictionary &change, const HashMap<RID, Vector<RenderingServerTypes::SurfaceData>> &sources) {
	String key = geometry_key(change);
	if (!geometries.has(key)) {
		++counters.layouts;
		uint64_t start = OS::get_singleton()->get_ticks_usec();
		const Vector<RenderingServerTypes::SurfaceData> *source = sources.getptr(change["base"]);
		Dictionary prepared = source ? SurfaceCacheGeometry::prepare_from_surfaces(change["base"], *source, change["surface_sides"]) : SurfaceCacheGeometry::prepare_from_rid(change["base"], change["surface_sides"]);
		if (prepared.is_empty()) {
			return fail("Surface Cache geometry preparation failed");
		}
		Dictionary preparation = prepared["operation_ms"];
		counters.mesh_read_ms += double(preparation["mesh_read"]);
		counters.mesh_layout_ms += double(preparation["mesh_layout"]);
		counters.mesh_pack_ms += double(preparation["mesh_pack"]);
		counters.capture_mesh_ms += double(preparation["capture_mesh"]);
		const double prepare_ms = milliseconds(start);
		counters.mesh_prepare_ms += prepare_ms;
		counters.layouts_ms += prepare_ms;
		prepared_triangles += triangle_count(change["base"]);
		prepare_ms_per_triangle = counters.mesh_prepare_ms / MAX(prepared_triangles, int64_t(1));
		Dictionary layout = prepared["layout"];
		Geometry *geometry = memnew(Geometry);
		geometry->key = key;
		geometry->mesh = prepared["mesh"];
		geometry->size = layout["size"];
		geometry->square = layout["capture_size"];
		geometry->columns = geometry->size.x / (geometry->square + 2);
		Array layout_surfaces = layout["surfaces"];
		TypedArray<PackedByteArray> vertex_data = prepared["vertex_data"];
		TypedArray<PackedByteArray> index_data = prepared["index_data"];
		geometry->orientation_bits = prepared["orientation_data"];
		for (int i = 0; i < vertex_data.size(); ++i) {
			const Dictionary surface = layout_surfaces[i];
			geometry->surfaces.push_back({ int(PackedByteArray(index_data[i]).size() / 12), int(PackedByteArray(vertex_data[i]).size() / 12), int(surface["orientation"]), geometry->orientation_words * 32 });
			geometry->orientation_words += PackedByteArray(geometry->orientation_bits[i]).size() / 4;
		}
		if (geometry->orientation_words > 0) {
			geometry->orientation_offset = orientation_ranges.allocate(geometry->orientation_words);
			for (Geometry::Surface &surface : geometry->surfaces) {
				surface.orientation_bit += geometry->orientation_offset * 32;
			}
		}
		geometry->vertices = vertex_buffers(vertex_data);
		geometry->indices = index_buffers(index_data);
		geometry->blas = create_blas(geometry->vertices, *geometry);
		capture_dimensions = capture_dimensions.max(geometry->size);
		geometries.insert(key, geometry);
	}
	Geometry &geometry = *geometries[key];
	++geometry.references;
	bool deformed = change["deformed"];
	Instance *state = memnew(Instance);
	state->generation = ++next_generation;
	state->mesh_revision = change["mesh_revision"];
	state->sides = change["surface_sides"];
	state->bounds = change["bounds"];
	state->base = change["base"];
	state->source = change["instance"];
	state->transform = change["transform"];
	state->shared = &geometry;
	state->deformed = deformed;
	state->first_surface = surface_ranges.allocate(geometry.surfaces.size());
	for (const Geometry::Surface &surface : geometry.surfaces) {
		state->triangle_count += surface.triangles;
	}
	triangle_total += state->triangle_count;
	state->chart = RenderingServer::get_singleton()->instance_create();
	RenderingServer::get_singleton()->instance_set_base(state->chart, geometry.mesh->get_rid());
	PackedInt32Array ids;
	ids.resize(geometry.surfaces.size());
	for (int i = 0; i < ids.size(); ++i) {
		ids.set(i, state->first_surface + i);
	}
	if (deformed) {
		state->vertices = copy_vertex_buffers(geometry);
		state->blas = create_blas(state->vertices, geometry, false);
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
	uint32_t sizes[BUFFER_COUNT];
	required_sizes(sizes);
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
	buffer_memory_bytes = int64_t(world_hash->call("buffer_bytes"));
	for (uint32_t size : capacities) {
		buffer_memory_bytes += size;
	}
	capacity_dirty = false;
	return true;
}

void SurfaceCacheWorldRD::required_sizes(uint32_t *r_sizes) const {
	const uint32_t surfaces = Math::nearest_power_of_2_templated(MAX(1u, uint32_t(surface_ranges.extent)));
	const uint32_t sizes[BUFFER_COUNT] = {
		surfaces * SURFACE_BYTES,
		MAX(16u, uint32_t(orientation_ranges.extent) * 4),
		surfaces * MATERIAL_BYTES,
		MAX(LIGHT_BYTES, Math::nearest_power_of_2_templated(uint32_t(lights.size())) * LIGHT_BYTES), 16,
		MAX(16u, surfaces * 4)
	};
	memcpy(r_sizes, sizes, sizeof(sizes));
}

void SurfaceCacheWorldRD::fit_capacity() {
	ScopeTimer timer(counters.capacity_ms);
	uint32_t sizes[BUFFER_COUNT];
	required_sizes(sizes);
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		const uint32_t size = page_size(sizes[i]);
		if (!buffers[i].is_valid() || capacities[i] <= size) {
			continue;
		}
		RID replacement = device->storage_buffer_create(size);
		if (!replacement.is_valid()) {
			fail(String("Cannot allocate Surface Cache buffer: ") + buffer_names[i]);
			return;
		}
		device->buffer_copy(buffers[i], replacement, 0, 0, size);
		device->free_rid(buffers[i]);
		buffers[i] = replacement;
		capacities[i] = size;
	}
	buffer_memory_bytes = int64_t(world_hash->call("buffer_bytes"));
	for (uint32_t size : capacities) {
		buffer_memory_bytes += size;
	}
}

void SurfaceCacheWorldRD::update_geometry(Instance &state) {
	ScopeTimer timer(counters.geometry_ms);
	++counters.geometry_updates;
	state.geometry_dirty = false;
	if (!state.deformed || !state.pose_dirty) {
		return;
	}
	Geometry &geometry = *state.shared;
	for (uint32_t surface = 0; surface < geometry.surfaces.size(); ++surface) {
		Dictionary pose = RenderingServer::get_singleton()->instance_get_deformed_surface(state.source, surface);
		if (pose.is_empty()) {
			continue;
		}
		RID set = cached_buffers(kernels[POSE], { RID(pose["vertex_buffer"]), state.vertices[surface] });
		const int32_t push[] = { geometry.surfaces[surface].vertices, int(pose["vertex_stride_words"]), 0, 0 };
		kernels[POSE].elements(set, push, sizeof(push), geometry.surfaces[surface].vertices);
	}
	device->blas_build(state.blas);
	++counters.blas_builds;
	state.pose_dirty = false;
}

void SurfaceCacheWorldRD::write_surfaces(Instance &state) {
	Geometry &geometry = *state.shared;
	if (!geometry.orientation_bits.is_empty()) {
		int written = 0;
		for (int surface = 0; surface < geometry.orientation_bits.size(); ++surface) {
			PackedByteArray bits = geometry.orientation_bits[surface];
			device->buffer_update(buffers[ORIENTATIONS], (geometry.orientation_offset + written) * 4, bits.size(), bits.ptr());
			written += bits.size() / 4;
		}
		geometry.orientation_bits.clear();
	}
	for (uint32_t surface = 0; surface < geometry.surfaces.size(); ++surface) {
		const Geometry::Surface &record = geometry.surfaces[surface];
		const int32_t mapping[] = { record.orientation, record.orientation_bit, 0, 0, state.sides[surface], 0, 0, 0 };
		device->buffer_update(buffers[SURFACES], (state.first_surface + surface) * SURFACE_BYTES, sizeof(mapping), mapping);
	}
	state.surfaces_written = true;
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
	const int surface_count = geometry.surfaces.size();
	device->buffer_clear(buffers[SURFACE_FLAGS], state.first_surface * 4, surface_count * 4);
	LocalVector<RD::Uniform> bindings;
	for (RID rid : { buffers[MATERIALS], buffers[MATERIAL_CHANGES], buffers[SURFACE_FLAGS] }) {
		bindings.push_back(uniform(bindings.size(), RD::UNIFORM_TYPE_STORAGE_BUFFER, rid));
	}
	TypedArray<RID> textures = capture->get("textures");
	for (int i = 0; i < textures.size(); ++i) {
		bindings.push_back(uniform(bindings.size(), RD::UNIFORM_TYPE_IMAGE, textures[i]));
	}
	RID set = cached(kernels[MATERIAL], bindings);
	{
		ScopeTimer draw_timer(counters.capture_draw_ms);
		capture->call("capture", state.source, state.chart, geometry.size);
	}
	int32_t push[] = { state.first_surface, surface_count, geometry.columns, geometry.square };
	kernels[MATERIAL].dispatch(set, push, sizeof(push), Vector3i(surface_count, 1, 1));
	state.capture_dirty = false;
	state.capture_serial = ++capture_serial;
	Array pending;
	pending.push_back(state.source);
	pending.push_back(state.generation);
	opacity_states.push_back(pending);
	opacity_requested = true;
	counters.captures_ms += milliseconds(start);
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
	tlas_updates.erase(source);
	note_dirty(state->bounds);
	RenderingServer::get_singleton()->instance_set_surface_cache_ids(source, PackedInt32Array());
	triangle_total -= state->triangle_count;
	surface_ranges.release(state->first_surface, state->shared->surfaces.size());
	if (state->blas.is_valid()) {
		device->free_rid(state->blas);
	}
	for (RID rid : state->vertices) {
		device->free_rid(rid);
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
	if (geometry->blas.is_valid()) {
		device->free_rid(geometry->blas);
	}
	for (RID rid : geometry->vertices) {
		device->free_rid(rid);
	}
	for (RID rid : geometry->indices) {
		device->free_rid(rid);
	}
	if (geometry->orientation_words > 0) {
		orientation_ranges.release(geometry->orientation_offset, geometry->orientation_words);
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
	result["triangles"] = triangle_total;
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
	result["tlas"] = tlas;
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
