// lestoroer#gi
#include "surface_cache_gather.h"

#include "core/object/class_db.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/storage/render_scene_data.h"

using namespace SurfaceCache;

void SurfaceCacheGatherRD::_bind_methods() {
	ClassDB::bind_method(D_METHOD("dispose"), &SurfaceCacheGatherRD::dispose);
	ClassDB::bind_method(D_METHOD("get_error"), &SurfaceCacheGatherRD::get_error);
	ClassDB::bind_method(D_METHOD("get_probe_views"), &SurfaceCacheGatherRD::get_probe_views);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error"), "", "get_error");
	BIND_CONSTANT(VIEW_BYTES);
	BIND_CONSTANT(VIEW_CHANGES);
	BIND_CONSTANT(VIEW_MOVED);
}

bool SurfaceCacheGatherRD::initialize(RenderingDevice *p_device, const Ref<RefCounted> &p_world_hash) {
	device = p_device;
	world_hash = p_world_hash;
	const char *names[PASS_COUNT] = { "screen_probe_place", "screen_probe_trace", "screen_probe_filter", "screen_probe_integrate", "screen_contact", "screen_contact_filter", "gather_temporal", "reflection_gather", "reflection_temporal" };
	PackedStringArray reader = world_hash->call("reader_defines");
	for (int i = 0; i < PASS_COUNT; ++i) {
		PackedStringArray defines;
		if (i == PLACE || i == TRACE || i == INTEGRATE || i == CONTACT || i == REFLECTION) {
			defines = reader;
		}
		if (!kernels[i].initialize(device, names[i], defines)) {
			error = String("Final gather shader compilation failed: ") + names[i] + ": " + kernels[i].error;
			return false;
		}
	}
	sampler = device->sampler_create(RD::SamplerState());
	for (RID &rid : pass_stats) {
		rid = device->storage_buffer_create(16);
	}
	return true;
}

void SurfaceCacheGatherRD::create_resources(View &r_view, const Ref<RenderSceneBuffersRD> &p_buffers, Vector2i p_size) {
	ScopeTimer timer(resource_ms);
	p_buffers->clear_context(SNAME("surface_cache"));
	const char *textures[] = { "light_0", "light_1", "depth_0", "depth_1", "normal_0", "normal_1", "diffuse_raw", "rough_radiance", "contact_raw", "contact", "contact_light", "gi", "reflection_raw", "reflection_0", "reflection_1", "reflection_material_0", "reflection_material_1", "reflection" };
	for (const char *name : textures) {
		String texture_name(name);
		RD::DataFormat format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		if (texture_name == "light_0" || texture_name == "light_1" || texture_name == "reflection_0" || texture_name == "reflection_1") {
			format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		} else if (texture_name.begins_with("depth_")) {
			format = RD::DATA_FORMAT_R32_SFLOAT;
		}
		RID texture = p_buffers->create_texture(SNAME("surface_cache"), StringName(name), format,
				RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT,
				RD::TEXTURE_SAMPLES_1, p_size, 1, 1, true, false);
		device->texture_clear(texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	r_view.size = p_size;
	r_view.frame = 0;
	r_view.sets[0] = Sets();
	r_view.sets[1] = Sets();
	Vector2i grid((p_size.x + 15) / 16, (p_size.y + 15) / 16);
	if (grid != r_view.grid) {
		int count = grid.x * grid.y;
		RID *buffers[] = { &r_view.meta[0], &r_view.meta[1], &r_view.light, &r_view.filtered };
		uint32_t sizes[] = { count * PROBE_META_BYTES, count * PROBE_META_BYTES, count * PROBE_LIGHT_BYTES, count * PROBE_LIGHT_BYTES };
		for (int i = 0; i < 4; ++i) {
			if (buffers[i]->is_valid()) {
				device->free_rid(*buffers[i]);
			}
			*buffers[i] = device->storage_buffer_create(sizes[i]);
			device->buffer_clear(*buffers[i], 0, sizes[i]);
		}
		r_view.grid = grid;
	}
}

void SurfaceCacheGatherRD::build_sets(Sets &r_sets, View &v, const Ref<RenderSceneBuffersRD> &buffers, int current, Span<RID> resources, RID tlas, RID material_changes, bool diagnostics) {
	const String cur = itos(current);
	const String prev = itos(1 - current);
	auto texture = [&](const String &name) { return buffers->get_texture(SNAME("surface_cache"), name); };
	auto images = [&](LocalVector<RD::Uniform> &bindings, std::initializer_list<String> names) {
		for (const String &name : names) {
			bindings.push_back(uniform(bindings.size(), RD::UNIFORM_TYPE_IMAGE, texture(name)));
		}
	};
	LocalVector<RD::Uniform> scene;
	for (int i = 0; i < resources.size(); ++i) {
		scene.push_back(uniform(i, RD::UNIFORM_TYPE_STORAGE_BUFFER, resources[i]));
	}
	scene.push_back(uniform(6, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.camera));
	scene.push_back(uniform(7, RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, tlas));
	scene.push_back(sampled(8, sampler, buffers->get_depth_texture(0)));
	RID normals = buffers->get_texture(SNAME("forward_clustered"), SNAME("normal_roughness"));
	scene.push_back(sampled(9, sampler, normals));
	const String outputs[] = { "diffuse_raw", "depth_" + cur, "normal_" + cur, "reflection_material_" + cur };
	for (int i = 0; i < 4; ++i) {
		scene.push_back(uniform(10 + i, RD::UNIFORM_TYPE_IMAGE, texture(outputs[i])));
	}
	scene.push_back(uniform(14, RD::UNIFORM_TYPE_STORAGE_BUFFER, diagnostics ? v.stats : pass_stats[0]));
	TypedArray<RDUniform> hash_uniforms = world_hash->call("uniforms");
	LocalVector<RD::Uniform> hash_bindings;
	for (int i = 0; i < hash_uniforms.size(); ++i) {
		Ref<RDUniform> entry = hash_uniforms[i];
		RD::Uniform converted;
		converted.binding = entry->get_binding();
		converted.uniform_type = entry->get_uniform_type();
		TypedArray<RID> ids = entry->get_ids();
		for (int j = 0; j < ids.size(); ++j) {
			converted.append_id(ids[j]);
		}
		scene.push_back(converted);
		hash_bindings.push_back(converted);
	}
	r_sets.hash = world_hash->call("sets", script_uniforms(scene));
	replace(scene, uniform(14, RD::UNIFORM_TYPE_STORAGE_BUFFER, diagnostics ? v.stats : pass_stats[1]));
	LocalVector<RD::Uniform> probe(scene);
	probe.push_back(uniform(39, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.meta[current]));
	probe.push_back(uniform(40, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.light));
	probe.push_back(uniform(42, RD::UNIFORM_TYPE_IMAGE, texture("contact")));
	r_sets.passes[TRACE] = cached(kernels[TRACE], probe);
	// Расстановка оставляет зонд прошлого кадра, пока поверхность под ним не сменилась.
	probe.push_back(uniform(43, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.meta[1 - current]));
	r_sets.passes[PLACE] = cached(kernels[PLACE], probe);
	LocalVector<RD::Uniform> contact(scene);
	contact.push_back(uniform(41, RD::UNIFORM_TYPE_IMAGE, texture("contact_raw")));
	r_sets.passes[CONTACT] = cached(kernels[CONTACT], contact);
	LocalVector<RD::Uniform> bindings;
	bindings.push_back(uniform(0, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.camera));
	images(bindings, { "contact_raw", "depth_" + cur, "normal_" + cur, "contact", "contact_light" });
	r_sets.passes[CONTACT_FILTER] = cached(kernels[CONTACT_FILTER], bindings);
	r_sets.passes[FILTER] = cached_buffers(kernels[FILTER], { v.camera, v.meta[current], v.light, v.filtered });
	bindings.clear();
	bindings.push_back(uniform(0, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.camera));
	bindings.push_back(sampled(1, sampler, normals));
	bindings.push_back(uniform(2, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.meta[current]));
	bindings.push_back(uniform(3, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.filtered));
	images(bindings, { "depth_" + cur, "normal_" + cur, "diffuse_raw", "rough_radiance" });
	bindings.push_back(uniform(9, RD::UNIFORM_TYPE_IMAGE, texture("contact")));
	// Пиксель с малым весом зондов добирает свет мирового кеша.
	for (const RD::Uniform &entry : hash_bindings) {
		bindings.push_back(entry);
	}
	r_sets.passes[INTEGRATE] = cached(kernels[INTEGRATE], bindings);
	bindings.clear();
	bindings.push_back(uniform(0, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.camera));
	images(bindings, { "diffuse_raw", "depth_" + cur, "normal_" + cur, "light_" + prev, "depth_" + prev, "normal_" + prev, "light_" + cur, "contact", "gi" });
	bindings.push_back(uniform(10, RD::UNIFORM_TYPE_STORAGE_BUFFER, material_changes));
	bindings.push_back(uniform(11, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.emission_change));
	images(bindings, { "contact_light" });
	r_sets.passes[TEMPORAL] = cached(kernels[TEMPORAL], bindings);
	replace(scene, uniform(10, RD::UNIFORM_TYPE_IMAGE, texture("reflection_raw")));
	replace(scene, uniform(13, RD::UNIFORM_TYPE_IMAGE, texture("reflection_material_" + cur)));
	scene.push_back(uniform(22, RD::UNIFORM_TYPE_IMAGE, texture("rough_radiance")));
	replace(scene, uniform(14, RD::UNIFORM_TYPE_STORAGE_BUFFER, diagnostics ? v.stats : pass_stats[2]));
	r_sets.passes[REFLECTION] = cached(kernels[REFLECTION], scene);
	bindings.clear();
	bindings.push_back(uniform(0, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.camera));
	bindings.push_back(uniform(1, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.emission_change));
	images(bindings, { "reflection_raw", "depth_" + cur, "normal_" + cur, "reflection_material_" + cur, "reflection_" + prev, "depth_" + prev, "normal_" + prev, "reflection_material_" + prev, "reflection_" + cur, "reflection" });
	bindings.push_back(sampled(12, sampler, normals));
	bindings.push_back(uniform(13, RD::UNIFORM_TYPE_IMAGE, texture("rough_radiance")));
	r_sets.passes[REFLECTION_TEMPORAL] = cached(kernels[REFLECTION_TEMPORAL], bindings);
	r_sets.ready = true;
}

void SurfaceCacheGatherRD::take_changes(double now) {
	TypedArray<AABB> fresh = world_hash->call("pending_changes");
	if (fresh.size() > VIEW_CHANGES) {
		AABB merged = fresh[0];
		for (int i = 0; i < fresh.size(); ++i) {
			merged = merged.merge(fresh[i]);
		}
		changes.push_back({ merged, now });
	} else {
		for (int i = 0; i < fresh.size(); ++i) {
			changes.push_back({ AABB(fresh[i]), now });
		}
	}
	if (bool(world_hash->call("sun_change_pending"))) {
		world_light_time = now;
	}
	uint32_t write = 0;
	for (uint32_t i = 0; i < changes.size(); ++i) {
		if (now - changes[i].time < CHANGE_SECONDS) {
			changes[write++] = changes[i];
		}
	}
	changes.resize(write);
	while (changes.size() > VIEW_CHANGES) {
		uint32_t first = 0, second = 1;
		double smallest = INFINITY;
		for (uint32_t i = 0; i < changes.size(); ++i) {
			for (uint32_t j = i + 1; j < changes.size(); ++j) {
				double volume = changes[i].bounds.merge(changes[j].bounds).get_volume();
				if (volume < smallest) {
					smallest = volume;
					first = i;
					second = j;
				}
			}
		}
		Change merged = { changes[first].bounds.merge(changes[second].bounds), MAX(changes[first].time, changes[second].time) };
		changes.remove_at(second);
		changes[first] = merged;
	}
}

void SurfaceCacheGatherRD::write_changes(Parameters &p) {
	int64_t frame = world_hash->get("frame");
	if (changes_frame != frame) {
		changes_frame = frame;
		clock += RSG::rasterizer->get_frame_delta_time();
		take_changes(clock);
	}
	p.time[1] = clock - world_light_time;
	p.change_count[0] = changes.size();
	for (uint32_t i = 0; i < changes.size(); ++i) {
		const Change &change = changes[i];
		for (int axis = 0; axis < 3; ++axis) {
			p.changes[i][axis] = change.bounds.position[axis];
			p.changes[i][4 + axis] = change.bounds.get_end()[axis];
		}
	}
}

void SurfaceCacheGatherRD::write_moves(Parameters &p, const LocalVector<Motion> &moves) {
	struct Move {
		Transform3D back;
		AABB bounds;
	};
	Move groups[VIEW_MOVED];
	int count = 0;
	for (uint32_t i = 0; i < moves.size(); ++i) {
		const Motion &move = moves[i];
		Transform3D back = move.from * move.to.affine_inverse();
		bool joined = false;
		for (int j = 0; j < count; ++j) {
			if (groups[j].back.is_equal_approx(back)) {
				groups[j].bounds = groups[j].bounds.merge(move.bounds);
				joined = true;
				break;
			}
		}
		if (!joined && count < VIEW_MOVED) {
			groups[count++] = { back, move.bounds };
		}
	}
	p.move_count[0] = count;
	for (int i = 0; i < count; ++i) {
		AABB bounds = groups[i].bounds.grow(0.01);
		const Transform3D &back = groups[i].back;
		p.moves[i][3] = back.basis.get_rotation_quaternion().get_angle() > Math::PI / 90.0 ? 1.0f : 0.0f;
		for (int axis = 0; axis < 3; ++axis) {
			p.moves[i][axis] = bounds.position[axis];
			p.moves[i][4 + axis] = bounds.get_end()[axis];
			for (int column = 0; column < 3; ++column) {
				p.moves[i][8 + axis * 4 + column] = back.basis.get_column(column)[axis];
			}
			p.moves[i][11 + axis * 4] = back.origin[axis];
		}
	}
}

bool SurfaceCacheGatherRD::sets_valid(const Sets &sets) const {
	if (!sets.ready) {
		return false;
	}
	for (RID pass : sets.passes) {
		if (!device->uniform_set_is_valid(pass)) {
			return false;
		}
	}
	return device->uniform_set_is_valid(sets.hash["update"]);
}

void SurfaceCacheGatherRD::mark(const char *name, ObjectID view, bool enabled) {
	if (enabled) {
		device->capture_timestamp(String("SurfaceCache ") + name + " " + itos(int64_t(view)));
	}
}

String SurfaceCacheGatherRD::render(RenderData *data, Span<RID> resources, RID tlas, const Ref<Resource> &settings, int light_count, int emitter_count, RID material_changes, const LocalVector<Motion> &moves) {
	ERR_FAIL_COND_V(!data || !device || settings.is_null() || resources.size() != 4, String());
	Ref<RenderSceneBuffersRD> buffers = data->get_render_scene_buffers();
	if (buffers.is_null()) {
		return String();
	}
	if (buffers->get_view_count() != 1) {
		error = "Surface Cache final gather does not yet support multiview";
		ERR_PRINT(error);
		return String();
	}
	Vector2i size = buffers->get_internal_size();
	if (size.x <= 0 || size.y <= 0) {
		return String();
	}
	LocalVector<ObjectID> expired;
	for (const KeyValue<ObjectID, View *> &entry : views) {
		if (!ObjectDB::get_instance(entry.key)) {
			expired.push_back(entry.key);
		}
	}
	for (ObjectID id : expired) {
		free_view(views[id]);
		views.erase(id);
	}
	ObjectID id = buffers->get_instance_id();
	if (!views.has(id)) {
		View *view = memnew(View);
		view->camera = device->storage_buffer_create(sizeof(Parameters));
		view->emission_change = device->storage_buffer_create(16);
		view->stats = device->storage_buffer_create(4096);
		device->buffer_clear(view->emission_change, 0, 16);
		views.insert(id, view);
	}
	View &v = *views[id];
	int reflection_divisor = 1 << int(settings->get("reflection_resolution"));
	int disabled = settings->get("disabled_blocks");
	bool reflections = !(disabled & BLOCK_REFLECTIONS);
	bool probes = !(disabled & BLOCK_SCREEN_PROBES);
	bool reflection_history = v.frame > 0 && v.reflection_divisor == reflection_divisor && v.reflections;
	v.reflection_divisor = reflection_divisor;
	v.reflections = reflections;
	if (!buffers->has_texture(SNAME("surface_cache"), SNAME("light_0")) || v.size != size) {
		create_resources(v, buffers, size);
		v.light_scheme = -1;
	}
	Projection projection = data->get_render_scene_data()->get_cam_projection();
	Transform3D camera = data->get_render_scene_data()->get_cam_transform();
	Projection previous_projection = v.previous * Projection(v.previous_camera);
	Vector4 lens(projection[0][0], projection[1][1], projection[2][2], projection[3][2]);
	Vector4 old_lens(previous_projection[0][0], previous_projection[1][1], previous_projection[2][2], previous_projection[3][2]);
	bool camera_moved = !lens.is_equal_approx(old_lens) || !camera.is_equal_approx(v.previous_camera);
	Projection inverse_projection = projection.inverse();
	Parameters p = {};
	matrix(p.matrices, inverse_projection);
	matrix(p.matrices + 16, Projection(camera));
	matrix(p.matrices + 32, v.previous);
	p.dimensions[0] = size.x;
	p.dimensions[1] = size.y;
	p.dimensions[2] = v.frame;
	p.dimensions[3] = reflection_divisor;
	Color sky = settings->get("sky_radiance");
	p.sky[0] = sky.r;
	p.sky[1] = sky.g;
	p.sky[2] = sky.b;
	bool diagnostics = settings->get("diagnostics");
	bool markers = settings->get("gpu_markers");
	// Первый кадр ещё без шага; пределы истории отражений шейдер ведёт в кадрах этой длительности.
	double delta = RSG::rasterizer->get_frame_delta_time();
	if (delta <= 0.0) {
		delta = 1.0 / 60.0;
	}
	p.time[0] = delta;
	p.history[0] = light_count;
	p.history[1] = v.frame > 0;
	p.history[3] = diagnostics;
	p.reflection[0] = disabled;
	p.reflection[1] = settings->get("reflection_samples");
	p.reflection[2] = settings->get("reflection_bounces");
	p.reflection[3] = camera_moved;
	p.probe_settings[0] = settings->get("probe_ray_passes");
	p.probe_settings[1] = emitter_count;
	p.roughness = settings->get("reflection_ray_roughness");
	p.reflection_history = reflection_history;
	p.probes[0] = 16;
	p.probes[1] = v.grid.x;
	p.probes[2] = v.grid.y;
	p.probes[3] = 64;
	write_changes(p);
	matrix(p.inverse_matrices, Projection(camera) * inverse_projection);
	matrix(p.inverse_matrices + 16, v.previous.inverse());
	write_moves(p, moves);
	p.limits[0] = settings->get("diffuse_history_seconds");
	// Сдвиг TAA в NDC: по нему зонды берут точку под центром пикселя без сдвига и не дрожат вместе с TAA.
	RenderSceneDataRD *scene_data = Object::cast_to<RenderSceneDataRD>(data->get_render_scene_data());
	if (scene_data) {
		p.limits[1] = scene_data->taa_jitter.x;
		p.limits[2] = scene_data->taa_jitter.y;
	}
	device->buffer_update(v.camera, 0, sizeof(p), &p);
	Sets &sets = v.sets[v.frame % 2];
	RID key[] = { resources[0], resources[1], resources[2], resources[3], tlas, material_changes, buffers->get_depth_texture(0), buffers->get_texture(SNAME("forward_clustered"), SNAME("normal_roughness")) };
	bool changed = sets.diagnostics != diagnostics;
	for (int i = 0; i < 8; ++i) {
		changed |= sets.resources[i] != key[i];
	}
	if (changed || !sets_valid(sets)) {
		build_sets(sets, v, buffers, v.frame % 2, resources, tlas, material_changes, diagnostics);
		for (int i = 0; i < 8; ++i) {
			sets.resources[i] = key[i];
		}
		sets.diagnostics = diagnostics;
	}
	if (diagnostics) {
		device->buffer_clear(v.stats, 0, 4096);
	}
	auto texture = [&](const char *name) { return buffers->get_texture(SNAME("surface_cache"), name); };
	// Без контакта проход пишет только глубину и поверхность пикселя; видимость без лучей — ноль лучей.
	kernels[CONTACT].pixels(sets.passes[CONTACT], size);
	mark("contact end", id, markers);
	if ((disabled & BLOCK_CONTACT) || !probes) {
		device->texture_clear(texture("contact"), Color(1, 0, 0, 0), 0, 1, 0, 1);
		device->texture_clear(texture("contact_light"), Color(0, 0, 0, 0), 0, 1, 0, 1);
	} else {
		kernels[CONTACT_FILTER].pixels(sets.passes[CONTACT_FILTER], size);
	}
	mark("contact filter end", id, markers);
	// Без пополнения кеш застывает: сведение без выборок оставляет ячейки как были.
	if (!(disabled & BLOCK_CACHE_UPDATE)) {
		world_hash->call("update", sets.hash, size);
	}
	mark("hash update end", id, markers);
	world_hash->call("resolve");
	mark("hash resolve end", id, markers);
	int probe_count = v.grid.x * v.grid.y;
	// Без экранных зондов свет пикселя — мировой кеш движка: нулевая глубина итога (surface_cache_inc), у отражений
	// нет света зондов. При смене схемы история пикселя и отражений пуста: новая схема начинает со свежей оценки.
	if (v.light_scheme != int(probes)) {
		device->texture_clear(texture("gi"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		device->texture_clear(texture("light_0"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		device->texture_clear(texture("light_1"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		device->texture_clear(texture("rough_radiance"), Color(0, 0, 0, probes ? 0 : -1), 0, 1, 0, 1);
		device->texture_clear(texture("reflection_0"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		device->texture_clear(texture("reflection_1"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		v.light_scheme = int(probes);
	}
	if (probes) {
		kernels[PLACE].pixels(sets.passes[PLACE], v.grid);
		mark("probe place end", id, markers);
		kernels[TRACE].dispatch(sets.passes[TRACE], nullptr, 0, Vector3i(probe_count, 1, 1));
		mark("probe trace end", id, markers);
		if (disabled & BLOCK_PROBE_FILTER) {
			device->buffer_copy(v.light, v.filtered, 0, 0, probe_count * PROBE_LIGHT_BYTES);
		} else {
			kernels[FILTER].elements(sets.passes[FILTER], nullptr, 0, probe_count);
		}
		mark("probe filter end", id, markers);
		kernels[INTEGRATE].dispatch(sets.passes[INTEGRATE], nullptr, 0, Vector3i(v.grid.x, v.grid.y, 1));
		mark("integrate end", id, markers);
		kernels[TEMPORAL].pixels(sets.passes[TEMPORAL], size);
	} else {
		mark("probe place end", id, markers);
		mark("probe trace end", id, markers);
		mark("probe filter end", id, markers);
		mark("integrate end", id, markers);
	}
	mark("diffuse temporal end", id, markers);
	mark("reflections begin", id, markers);
	// Без отражений GI нулевая глубина итога отдаёт отражения движку.
	if (reflections) {
		kernels[REFLECTION].pixels(sets.passes[REFLECTION], Vector2i((size.x + reflection_divisor - 1) / reflection_divisor, (size.y + reflection_divisor - 1) / reflection_divisor));
		mark("reflection trace end", id, markers);
		kernels[REFLECTION_TEMPORAL].pixels(sets.passes[REFLECTION_TEMPORAL], size);
	} else {
		device->texture_clear(texture("reflection"), Color(0, 0, 0, 0), 0, 1, 0, 1);
		mark("reflection trace end", id, markers);
	}
	mark("reflection temporal end", id, markers);
	mark("reflections end", id, markers);
	v.previous_camera = camera;
	v.previous = projection * Projection(camera.affine_inverse());
	++v.frame;
	return "gi";
}

void SurfaceCacheGatherRD::free_view(View *view) {
	for (RID rid : { view->camera, view->emission_change, view->stats, view->meta[0], view->meta[1], view->light, view->filtered }) {
		if (rid.is_valid()) {
			device->free_rid(rid);
		}
	}
	memdelete(view);
}

void SurfaceCacheGatherRD::dispose() {
	for (const KeyValue<ObjectID, View *> &entry : views) {
		Ref<RenderSceneBuffersRD> buffers = Object::cast_to<RenderSceneBuffersRD>(ObjectDB::get_instance(entry.key));
		if (buffers.is_valid()) {
			buffers->clear_context(SNAME("surface_cache"));
		}
		free_view(entry.value);
	}
	views.clear();
	for (Kernel &kernel : kernels) {
		kernel.dispose();
	}
	for (RID &rid : pass_stats) {
		if (rid.is_valid()) {
			device->free_rid(rid);
			rid = RID();
		}
	}
	if (sampler.is_valid()) {
		device->free_rid(sampler);
		sampler = RID();
	}
}

Array SurfaceCacheGatherRD::get_probe_views() const {
	Array result;
	for (const KeyValue<ObjectID, View *> &entry : views) {
		Ref<RenderSceneBuffersRD> buffers = Object::cast_to<RenderSceneBuffersRD>(ObjectDB::get_instance(entry.key));
		if (buffers.is_null()) {
			continue;
		}
		const View &v = *entry.value;
		Dictionary state;
		state["buffers"] = buffers;
		state["frame"] = v.frame;
		state["size"] = v.size;
		state["stats"] = v.stats;
		result.push_back(state);
	}
	return result;
}
