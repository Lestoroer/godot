// lestoroer#gi
#include "surface_cache_gather.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/sort_array.h"
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
	// Суммы по лучам зонда — подгруппами, если устройство их даёт в вычислительных шейдерах и подгруппа не меньше 4
	// потоков (world_probe_trace: до 16 подгрупп на группу из 64); иначе — деревом в общей памяти.
	const uint64_t subgroup_stages = p_device->limit_get(RD::LIMIT_SUBGROUP_IN_SHADERS);
	const uint64_t subgroup_operations = p_device->limit_get(RD::LIMIT_SUBGROUP_OPERATIONS);
	const uint64_t smallest_subgroup = MIN(p_device->limit_get(RD::LIMIT_SUBGROUP_MIN_SIZE), p_device->limit_get(RD::LIMIT_SUBGROUP_SIZE));
	const uint64_t needed_operations = RD::SUBGROUP_BASIC_BIT | RD::SUBGROUP_ARITHMETIC_BIT;
	subgroup_sums = (subgroup_stages & RD::SHADER_STAGE_COMPUTE_BIT) && (subgroup_operations & needed_operations) == needed_operations && smallest_subgroup >= 4;	// Ореол фильтра — вариант запроса, лучи и свет кадра — варианты одного шаблона.
	const char *names[PASS_COUNT] = { "reflection_gather", "reflection_temporal", "world_probe_request", "world_probe_request", "world_probe_check", "world_probe_trace", "world_probe_trace",
		"world_probe_filter", "world_probe_integrate", "world_probe_evict" };
	PackedStringArray reader = world_hash->call("reader_defines");
	for (int i = 0; i < PASS_COUNT; ++i) {
		PackedStringArray defines;
		if (i == REFLECTION || i >= WORLD_REQUEST) {
			defines = reader;
		}
		if (i >= WORLD_REQUEST) {
			defines.push_back("WORLD_REQUEST_STRIDE_PIXELS " + itos(WORLD_REQUEST_STRIDE));
			if (subgroup_sums) {
				defines.push_back("WORLD_SUBGROUP_ARITHMETIC 1");
			}
		}
		if (i == WORLD_HALO) {
			defines.push_back("WORLD_PROBE_HALO 1");
		}
		if (i == WORLD_SHADE) {
			defines.push_back("WORLD_PROBE_SHADE 1");
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
	const char *textures[] = { "light_0", "light_1", "depth_0", "depth_1", "normal_0", "normal_1", "diffuse_raw", "rough_radiance", "gi", "reflection_raw", "reflection_0", "reflection_1", "reflection_material_0", "reflection_material_1", "reflection" };
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
	// Номера кадров мировых зондов (кадр использования, проверки, изменения) идут от кадра вида: с ним начинается и
	// их кеш.
	r_view.world_reset = true;
	r_view.world_last_change = 0;
	r_view.sets[0] = Sets();
	r_view.sets[1] = Sets();
}

uint32_t SurfaceCacheGatherRD::world_pool_for(Vector2i p_size, float p_spacing) {
	const double cells = double(p_size.x) * p_size.y / (double(p_spacing) * p_spacing);
	return MAX(Math::nearest_power_of_2_templated(uint32_t(Math::ceil(cells * WORLD_POOL_PER_CELL))), WORLD_POOL_MINIMUM);
}

uint64_t SurfaceCacheGatherRD::world_buffer_bytes(uint32_t p_pool, uint32_t p_table) {
	const uint64_t per_probe = WORLD_INTERPOLATION_BYTES + WORLD_PIXEL_LIGHT_BYTES + WORLD_PROBE_BYTES + WORLD_RAYS * (WORLD_RAY_BYTES + WORLD_EMITTER_BYTES) + WORLD_LIGHT_BYTES + 12;
	return uint64_t(p_table) * WORLD_TABLE_SLOT_BYTES + uint64_t(p_pool) * per_probe + WORLD_HEADER_WORDS * 4 + 48 + sizeof(TransportParameters);
}

// Буферы мировых зондов вида — по его размеру и шагу зондов; при их смене — заново, с пустым кешем.
bool SurfaceCacheGatherRD::create_world_resources(View &r_view, float p_spacing, int64_t p_budget) {
	const uint32_t pool = world_pool_for(r_view.size, p_spacing);
	const uint32_t table = pool * WORLD_TABLE_PER_PROBE;
	const int64_t bytes = int64_t(world_buffer_bytes(pool, table));
	// Лимит проверяется каждый кадр: мир мог вырасти и после создания буферов зондов.
	if (bytes > p_budget) {
		error = vformat("Surface Cache probes exceed the profile memory limit: %d MiB more needed", (bytes - p_budget + 1048575) / 1048576);
		ERR_PRINT(error);
		return false;
	}
	if (r_view.world_tags.is_valid() && pool == r_view.world_pool && p_spacing == r_view.world_spacing) {
		return true;
	}
	ScopeTimer timer(resource_ms);
	free_world_resources(r_view);
	RID *buffers[] = { &r_view.world_tags, &r_view.world_slots, &r_view.world_keys, &r_view.world_probes, &r_view.world_rays, &r_view.world_emitters, &r_view.world_light, &r_view.world_work };
	// Таблица ключей, опоры зондов для пикселей, свет зондов для пикселей (half), зонды, записи лучей и выборок
	// источников, свет зондов кадра; работа — заголовок и списки лучей, света и свободных зондов.
	const uint64_t sizes[] = { uint64_t(table) * WORLD_TABLE_SLOT_BYTES, uint64_t(pool) * WORLD_INTERPOLATION_BYTES, uint64_t(pool) * WORLD_PIXEL_LIGHT_BYTES, uint64_t(pool) * WORLD_PROBE_BYTES,
		uint64_t(pool) * WORLD_RAYS * WORLD_RAY_BYTES, uint64_t(pool) * WORLD_RAYS * WORLD_EMITTER_BYTES, uint64_t(pool) * WORLD_LIGHT_BYTES, uint64_t(WORLD_HEADER_WORDS) * 4 + uint64_t(pool) * 12 };
	for (int i = 0; i < 8; ++i) {
		*buffers[i] = device->storage_buffer_create(sizes[i]);
		device->buffer_clear(*buffers[i], 0, sizes[i]);
	}
	r_view.world_args = device->storage_buffer_create(48, {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	r_view.world_transport = device->storage_buffer_create(sizeof(TransportParameters));
	r_view.world_pool = pool;
	r_view.world_table = table;
	r_view.world_spacing = p_spacing;
	r_view.world_reset = true;
	// Наборы ссылаются на прежние буферы.
	r_view.sets[0] = Sets();
	r_view.sets[1] = Sets();
	return true;
}

void SurfaceCacheGatherRD::free_world_resources(View &r_view) {
	for (RID *rid : { &r_view.world_tags, &r_view.world_slots, &r_view.world_keys, &r_view.world_probes, &r_view.world_rays, &r_view.world_emitters, &r_view.world_light, &r_view.world_work, &r_view.world_args, &r_view.world_transport }) {
		if (rid->is_valid()) {
			device->free_rid(*rid);
			*rid = RID();
		}
	}
	r_view.world_pool = 0;
	r_view.world_table = 0;
}

void SurfaceCacheGatherRD::receive_world_header(const PackedByteArray &p_data, ObjectID p_view) {
	View **found = views.getptr(p_view);
	if (!found || p_data.size() < int(WORLD_HEADER_WORDS * 4)) {
		return;
	}
	const uint32_t *words = reinterpret_cast<const uint32_t *>(p_data.ptr());
	View &v = **found;
	// Заголовок кадра до последнего вытеснения его ещё не видел.
	if (words[WORLD_WORD_FRAME] <= v.world_evict_frame) {
		return;
	}
	const uint32_t issued = MIN(words[WORLD_WORD_POOL], v.world_pool);
	const uint32_t free = words[WORLD_WORD_FREE] > words[WORLD_WORD_FREE_TAKEN] ? words[WORLD_WORD_FREE] - words[WORLD_WORD_FREE_TAKEN] : 0;
	// Запас — на новые зонды кадра.
	if (v.world_pool - issued + free < v.world_pool / 8) {
		v.world_evict = true;
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
	}
	r_sets.hash = world_hash->call("sets", script_uniforms(scene));
	replace(scene, uniform(14, RD::UNIFORM_TYPE_STORAGE_BUFFER, diagnostics ? v.stats : pass_stats[1]));
	// Общий набор проходов мировых зондов: каждый берёт из него свои привязки.
	LocalVector<RD::Uniform> world(scene);
	const RID world_buffers[] = { v.world_tags, v.world_slots, v.world_keys, v.world_probes, v.world_rays, v.world_emitters, v.world_light, v.world_work, v.world_transport };
	const uint32_t world_bindings[] = { 50, 51, 52, 53, 54, 55, 56, 58, 59 };
	for (int i = 0; i < 9; ++i) {
		world.push_back(uniform(world_bindings[i], RD::UNIFORM_TYPE_STORAGE_BUFFER, world_buffers[i]));
	}
	world.push_back(uniform(22, RD::UNIFORM_TYPE_IMAGE, texture("rough_radiance")));
	// История света — в сборке света зондов (world_probe_integrate: diffuse_history).
	const String history[] = { "light_" + prev, "depth_" + prev, "normal_" + prev, "light_" + cur, "gi" };
	for (int i = 0; i < 5; ++i) {
		world.push_back(uniform(60 + i, RD::UNIFORM_TYPE_IMAGE, texture(history[i])));
	}
	world.push_back(uniform(65, RD::UNIFORM_TYPE_STORAGE_BUFFER, material_changes));
	world.push_back(uniform(66, RD::UNIFORM_TYPE_STORAGE_BUFFER, v.emission_change));
	for (int pass = WORLD_REQUEST; pass < PASS_COUNT; ++pass) {
		r_sets.passes[pass] = cached(kernels[pass], world);
	}
	replace(scene, uniform(10, RD::UNIFORM_TYPE_IMAGE, texture("reflection_raw")));
	replace(scene, uniform(13, RD::UNIFORM_TYPE_IMAGE, texture("reflection_material_" + cur)));
	scene.push_back(uniform(22, RD::UNIFORM_TYPE_IMAGE, texture("rough_radiance")));
	replace(scene, uniform(14, RD::UNIFORM_TYPE_STORAGE_BUFFER, diagnostics ? v.stats : pass_stats[2]));
	r_sets.passes[REFLECTION] = cached(kernels[REFLECTION], scene);
	LocalVector<RD::Uniform> bindings;
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
	for (int i = 0; i < PASS_COUNT; ++i) {
		if (!device->uniform_set_is_valid(sets.passes[i])) {
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

namespace {
struct CenterLess {
	int axis = 0;
	bool operator()(const AABB &p_a, const AABB &p_b) const {
		return p_a.position[axis] + p_a.size[axis] * 0.5f < p_b.position[axis] + p_b.size[axis] * 0.5f;
	}
};

// Рамки p_boxes[0, p_count) — в p_groups общих рамок: половина рамок по центрам вдоль длинной оси их общей рамки —
// в половину групп, рекурсивно. Соседние в пространстве рамки попадают в одну группу; работа — O(n log n).
void group_boxes(AABB *p_boxes, uint32_t p_count, uint32_t p_groups, LocalVector<AABB> &r_groups) {
	AABB bounds = p_boxes[0];
	for (uint32_t i = 1; i < p_count; ++i) {
		bounds.merge_with(p_boxes[i]);
	}
	if (p_groups <= 1 || p_count <= 1) {
		r_groups.push_back(bounds);
		return;
	}
	const uint32_t left_groups = p_groups / 2;
	const uint32_t left = MAX(1u, uint32_t(uint64_t(p_count) * left_groups / p_groups));
	SortArray<AABB, CenterLess> sorter;
	sorter.compare.axis = bounds.get_longest_axis_index();
	sorter.nth_element(0, p_count, left, p_boxes);
	group_boxes(p_boxes, left, left_groups, r_groups);
	group_boxes(p_boxes + left, p_count - left, p_groups - left_groups, r_groups);
}
} // namespace

void SurfaceCacheGatherRD::write_transport(View &v, TransportParameters &p, const Transport &transport) {
	// Набор изменений мира — в журнал один раз, сколько бы видов ни рисовалось в его кадре.
	if (transport.serial != transport_serial) {
		transport_serial = transport.serial;
		for (const AABB &bounds : transport.changes) {
			transport_log.push_back({ ++transport_events, bounds });
		}
		if (transport_log.size() > uint32_t(TRANSPORT_LOG)) {
			const uint32_t extra = transport_log.size() - TRANSPORT_LOG;
			for (uint32_t i = 0; i + extra < transport_log.size(); ++i) {
				transport_log[i] = transport_log[i + extra];
			}
			transport_log.resize(TRANSPORT_LOG);
		}
	}
	p.counts[2] = transport.emitter_generation;
	// События, которые вид ещё не передал: старше начала журнала — потеряны, устарели все его зонды; рамки им не нужны.
	const uint64_t first = transport_log.is_empty() ? transport_events + 1 : transport_log[0].number;
	p.counts[3] = v.transport_consumed + 1 < first && v.transport_consumed < transport_events ? 1 : 0;
	LocalVector<AABB> fresh;
	for (const TransportEvent &event : transport_log) {
		if (event.number > v.transport_consumed && p.counts[3] == 0) {
			fresh.push_back(event.bounds);
		}
	}
	v.transport_consumed = transport_events;
	// Больше рамок, чем помещается, — объединяются в пространственные группы.
	if (fresh.size() > uint32_t(TRANSPORT_CHANGES)) {
		LocalVector<AABB> groups;
		group_boxes(fresh.ptr(), fresh.size(), TRANSPORT_CHANGES, groups);
		fresh = groups;
	}
	p.counts[0] = fresh.size();
	for (uint32_t i = 0; i < fresh.size(); ++i) {
		for (int axis = 0; axis < 3; ++axis) {
			p.changes[i][axis] = fresh[i].position[axis];
			p.changes[i][4 + axis] = fresh[i].get_end()[axis];
		}
	}
	p.counts[1] = v.world_last_change;
	if (!fresh.is_empty() || p.counts[3] != 0) {
		v.world_last_change = v.frame + 1;
	}
	p.serial[0] = uint32_t(transport.serial);
	p.serial[1] = transport.serial == v.transport_last_serial + 1 && p.counts[3] == 0 ? 1 : 0;
	v.transport_last_serial = transport.serial;
	p.sizes[0] = v.world_pool;
	p.sizes[1] = v.world_table;
	memcpy(&p.sizes[2], &v.world_spacing, sizeof(float));
}

String SurfaceCacheGatherRD::render(RenderData *data, Span<RID> resources, RID tlas, const Ref<Resource> &settings, int light_count, int emitter_count, RID material_changes, const LocalVector<Motion> &moves, const Transport &transport, uint64_t world_bytes) {
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
	bool probes = !(disabled & BLOCK_PROBES);
	bool reflection_history = v.frame > 0 && v.reflection_divisor == reflection_divisor && v.reflections;
	v.reflection_divisor = reflection_divisor;
	v.reflections = reflections;
	if (!buffers->has_texture(SNAME("surface_cache"), SNAME("light_0")) || v.size != size) {
		create_resources(v, buffers, size);
		v.light_scheme = -1;
	}
	// Буферы зондов этого вида — под лимитом памяти профиля вместе с буферами мира и зондами прочих видов; свои нынешние
	// буферы вид заменяет.
	const int64_t limit = int64_t(int(settings->get("memory_limit_mib"))) * 1024 * 1024;
	const int64_t own = v.world_tags.is_valid() ? int64_t(world_buffer_bytes(v.world_pool, v.world_table)) : 0;
	const int64_t others = int64_t(get_probe_buffer_bytes()) - own;
	if (!create_world_resources(v, float(settings->get("probe_spacing_pixels")), limit - int64_t(world_bytes) - others)) {
		return String();
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
	p.emitters[1] = emitter_count;
	p.roughness = settings->get("reflection_ray_roughness");
	p.reflection_history = reflection_history;
	write_changes(p);
	matrix(p.inverse_matrices, Projection(camera) * inverse_projection);
	matrix(p.inverse_matrices + 16, v.previous.inverse());
	write_moves(p, moves);
	p.limits[0] = settings->get("diffuse_history_seconds");
	// Сдвиг TAA в NDC: по нему точки пикселей берутся под центром пикселя без сдвига и не дрожат вместе с TAA.
	RenderSceneDataRD *scene_data = Object::cast_to<RenderSceneDataRD>(data->get_render_scene_data());
	if (scene_data) {
		p.limits[1] = scene_data->taa_jitter.x;
		p.limits[2] = scene_data->taa_jitter.y;
	}
	TransportParameters transport_parameters = {};
	write_transport(v, transport_parameters, transport);
	device->buffer_update(v.world_transport, 0, sizeof(transport_parameters), &transport_parameters);
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
	// Без пополнения кеш застывает: сведение без выборок оставляет ячейки как были.
	if (!(disabled & BLOCK_CACHE_UPDATE)) {
		world_hash->call("update", sets.hash, size);
	}
	mark("hash update end", id, markers);
	world_hash->call("resolve");
	mark("hash resolve end", id, markers);
	// Без зондов свет пикселя — мировой кеш движка: нулевая глубина итога (surface_cache_inc), у отражений нет света
	// зондов. При смене схемы история пикселя и отражений пуста: новая схема начинает со свежей оценки.
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
		// Мировые зонды: запрос клеток пикселями (новые — в список лучей, отмеченные — в список света), ореол фильтра,
		// проверка транспорта по изменениям мира, лучи, свет кадра из записей, фильтр.
		if (v.world_reset) {
			device->buffer_clear(v.world_tags, 0, uint64_t(v.world_table) * WORLD_TABLE_SLOT_BYTES);
			device->buffer_clear(v.world_keys, 0, uint64_t(v.world_pool) * WORLD_PIXEL_LIGHT_BYTES);
			device->buffer_clear(v.world_work, 0, WORLD_HEADER_WORDS * 4);
			v.world_reset = false;
			v.world_evict = false;
			v.world_evict_frame = 0;
		} else if (v.world_evict) {
			// Вытеснение: таблица — заново из зондов, использованных за возраст, остальные — в список свободных.
			const uint32_t frame = uint32_t(v.frame) + 1;
			const uint32_t since = frame - v.world_evict_frame;
			if (v.world_evict_frame != 0 && since < 2 * v.world_evict_age) {
				v.world_evict_age = MAX(v.world_evict_age / 2, 1u);
			} else if (v.world_evict_frame != 0 && since > 16 * v.world_evict_age) {
				v.world_evict_age = MIN(v.world_evict_age * 2, 1024u);
			}
			// Слова от WORLD_WORD_FREE до WORLD_WORD_EVICT_BEFORE: счётчики списка свободных — заново, кадр вида движок пишет
			// ниже, последнее — кадр вытеснения.
			static_assert(WORLD_WORD_EVICT_BEFORE == WORLD_WORD_FREE + 4);
			const uint32_t words[] = { 0, 0, 0, 0, frame > v.world_evict_age ? frame - v.world_evict_age : 0 };
			device->buffer_clear(v.world_tags, 0, uint64_t(v.world_table) * WORLD_TABLE_SLOT_BYTES);
			device->buffer_update(v.world_work, WORLD_WORD_FREE * 4, sizeof(words), words);
			kernels[WORLD_EVICT].elements(sets.passes[WORLD_EVICT], nullptr, 0, v.world_pool);
			v.world_evict = false;
			v.world_evict_frame = frame;
		}
		const uint32_t counts[] = { 0, 0 };
		// Слова 3–15: флаги кадра и три тройки групп (лучи, свет, фильтр) — строки групп по X, их число по Y, Z = 1.
		const uint32_t args[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0 };
		const uint32_t frame_word = uint32_t(v.frame) + 1;
		device->buffer_update(v.world_work, 0, sizeof(counts), counts);
		device->buffer_update(v.world_work, 12, sizeof(args), args);
		device->buffer_update(v.world_work, WORLD_WORD_FRAME * 4, 4, &frame_word);
		kernels[WORLD_REQUEST].pixels(sets.passes[WORLD_REQUEST], Vector2i((size.x + WORLD_REQUEST_STRIDE - 1) / WORLD_REQUEST_STRIDE, (size.y + WORLD_REQUEST_STRIDE - 1) / WORLD_REQUEST_STRIDE));
		mark("probe request end", id, markers);
		// Ореол фильтра — у зондов, отмеченных пикселями (их число — копия заголовка), по группам фильтра.
		device->buffer_copy(v.world_work, v.world_transport, 0, offsetof(TransportParameters, requested), sizeof(TransportParameters::requested));
		if (!(disabled & BLOCK_PROBE_FILTER)) {
			device->buffer_copy(v.world_work, v.world_args, 16, 0, 48);
			kernels[WORLD_HALO].dispatch_indirect(sets.passes[WORLD_HALO], v.world_args, 32);
		}
		mark("probe halo end", id, markers);
		// Проверка транспорта зондов кадра по изменениям мира: группа — зонд списка света.
		device->buffer_copy(v.world_work, v.world_args, 16, 0, 48);
		kernels[WORLD_CHECK].dispatch_indirect(sets.passes[WORLD_CHECK], v.world_args, 16);
		mark("probe check end", id, markers);
		device->buffer_copy(v.world_work, v.world_args, 16, 0, 48);
		kernels[WORLD_TRACE].dispatch_indirect(sets.passes[WORLD_TRACE], v.world_args, 0);
		mark("probe trace end", id, markers);
		kernels[WORLD_SHADE].dispatch_indirect(sets.passes[WORLD_SHADE], v.world_args, 16);
		mark("probe shade end", id, markers);
		// Без фильтра свет зонда пикселям пишет сам свет кадра (world_probe_trace).
		if (!(disabled & BLOCK_PROBE_FILTER)) {
			kernels[WORLD_FILTER].dispatch_indirect(sets.passes[WORLD_FILTER], v.world_args, 32);
		}
		mark("probe filter end", id, markers);
		Callable callback = callable_mp(this, &SurfaceCacheGatherRD::receive_world_header).bind(id);
		device->buffer_get_data_async(v.world_work, callback, 0, WORLD_HEADER_WORDS * 4);
	}
	// Свет пикселей с историей и их глубина с поверхностью; без зондов — только глубина и поверхность для отражений.
	kernels[WORLD_INTEGRATE].pixels(sets.passes[WORLD_INTEGRATE], size);
	mark("integrate end", id, markers);
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
	free_world_resources(*view);
	for (RID rid : { view->camera, view->emission_change, view->stats }) {
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

uint64_t SurfaceCacheGatherRD::get_probe_buffer_bytes() const {
	uint64_t total = 0;
	for (const KeyValue<ObjectID, View *> &entry : views) {
		if (entry.value->world_tags.is_valid()) {
			total += world_buffer_bytes(entry.value->world_pool, entry.value->world_table);
		}
	}
	return total;
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
		if (v.world_work.is_valid()) {
			state["world_work"] = v.world_work;
			state["world_probes"] = v.world_probes;
			state["world_rays"] = v.world_rays;
			state["world_emitters"] = v.world_emitters;
			state["world_pool"] = v.world_pool;
		}
		result.push_back(state);
	}
	return result;
}
