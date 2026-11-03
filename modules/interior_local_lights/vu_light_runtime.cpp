// lestoroer#local-lights
#include "vu_light_runtime.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cstring>

static RendererSceneCull *scene_cull() {
	return static_cast<RendererSceneCull *>(RSG::scene);
}
static double elapsed(uint64_t start) {
	return (OS::get_singleton()->get_ticks_usec() - start) * 0.001;
}
static constexpr int WORDS = 8, STRIDE = 32, FILTER_BATCH = 4;
static const Basis faces_basis[6] = {
	Basis(Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(-1, 0, 0)),
	Basis(Vector3(0, 0, -1), Vector3(0, 1, 0), Vector3(1, 0, 0)),
	Basis(Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, -1, 0)),
	Basis(Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(0, 1, 0)),
	Basis(Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, -1)),
	Basis(Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1))
};

void VuLightRuntime::_bind_methods() {
	ClassDB::bind_method(D_METHOD("attach", "scenario", "sources", "world", "tile_size", "cap", "memory_mib", "filter_mode"), &VuLightRuntime::attach, DEFVAL(256), DEFVAL(16), DEFVAL(128), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("update", "camera"), &VuLightRuntime::update);
	ClassDB::bind_method(D_METHOD("set_cap", "cap"), &VuLightRuntime::set_cap);
	ClassDB::bind_method(D_METHOD("set_gpu_timestamps", "enabled"), &VuLightRuntime::set_gpu_timestamps);
	ClassDB::bind_method(D_METHOD("set_softness", "softness"), &VuLightRuntime::set_softness);
	ClassDB::bind_method(D_METHOD("set_shadows_enabled", "enabled"), &VuLightRuntime::set_shadows_enabled);
	ClassDB::bind_method(D_METHOD("set_light_enabled", "instance", "enabled"), &VuLightRuntime::set_light_enabled);
	ClassDB::bind_method(D_METHOD("set_interactive", "instance"), &VuLightRuntime::set_interactive);
	ClassDB::bind_method(D_METHOD("add_light", "instance"), &VuLightRuntime::add_light);
	ClassDB::bind_method(D_METHOD("dispose"), &VuLightRuntime::dispose);
	ClassDB::bind_method(D_METHOD("get_error"), &VuLightRuntime::get_error);
	ClassDB::bind_method(D_METHOD("diagnostics"), &VuLightRuntime::diagnostics);
	ClassDB::bind_method(D_METHOD("get_grid_texture"), &VuLightRuntime::get_grid_texture);
	ClassDB::bind_method(D_METHOD("owns_light", "instance"), &VuLightRuntime::owns_light);
}

void VuLightRuntime::fail(const String &p_error) {
	if (error.is_empty()) {
		error = p_error;
		ERR_PRINT(error);
	}
}

void VuLightRuntime::attach(RID p_scenario, const TypedArray<RID> &p_sources, const AABB &p_world, int p_tile_size, int p_cap, int p_memory_mib, int p_filter_mode) {
	ERR_FAIL_COND(ready || scenario.is_valid() || disposed);
	if (p_world.size.x <= 0 || p_world.size.y <= 0 || p_world.size.z <= 0 || p_tile_size < 32 || p_tile_size > 1024 || p_memory_mib < 8 || p_cap < 0) {
		fail("Invalid native local-light configuration");
		return;
	}
	if (!RenderingServer::get_singleton()->is_on_render_thread()) {
		fail("Native local-light attach requires the render thread");
		return;
	}
	filter_mode = p_filter_mode;
	if (filter_mode < 0 || filter_mode > 1) {
		fail("Invalid shadow filter");
		return;
	}
	scenario = p_scenario;
	world = p_world;
	tile_size = p_tile_size;
	cap = p_cap;
	memory_limit = uint64_t(p_memory_mib) << 20;
	dimensions = Vector3i(Math::ceil(world.size.x / cell_size), Math::ceil(world.size.y / cell_size), Math::ceil(world.size.z / cell_size));
	grid.resize(dimensions.x * dimensions.y * dimensions.z * WORDS);
	grid.fill(0);
	records.resize(max_sources * STRIDE);
	records.fill(0);
	lights.resize(max_sources);
	for (int i = 0; i < max_sources; i++) {
		records.write[i * STRIDE + 23] = -1;
	}
	rd = RenderingServer::get_singleton()->get_rendering_device();
	if (!rd) {
		fail("Native local lights require RenderingDevice");
		return;
	}
	scene_cull()->local_light_observe(scenario, true);
	ready = true;
	for (int i = 0; i < p_sources.size(); i++) {
		if (!add_light(p_sources[i])) {
			fail("Invalid source or native source capacity exceeded");
			return;
		}
	}
	consume_changes();
	int required_layers = 0;
	for (const Light &light : lights) {
		if (light.instance.is_valid()) {
			required_layers += light.spot ? 1 : 6;
		}
	}
	uint64_t working_bytes = 8 * 1024 * 1024 + uint64_t(tile_size) * tile_size * (4 + (filter_mode == 1 ? 96 * FILTER_BATCH : 0));
	if (working_bytes >= memory_limit) {
		fail("Native light scratch buffers exceed the memory budget");
		return;
	}
	int layer_limit = MIN(int((memory_limit - working_bytes) / (uint64_t(tile_size) * tile_size * (filter_mode == 1 ? 16 : 4))), int(rd->limit_get(RD::LIMIT_MAX_TEXTURE_ARRAY_LAYERS)));
	if (required_layers > layer_limit) {
		fail("Requested native shadow atlas exceeds the memory budget");
		return;
	}
	layers = MIN(layer_limit, ((required_layers + 11) / 12 + 1) * 12);
	if (initialize_gpu()) {
		RenderingServer::get_singleton()->connect("frame_pre_draw", callable_mp(this, &VuLightRuntime::render_frame));
	}
}

bool VuLightRuntime::initialize_gpu() {
	Ref<RDShaderFile> source = ResourceLoader::load("res://features/lighting/vu_lights/native_shadow_caster.glsl");
	if (source.is_null()) {
		fail("Missing native_shadow_caster.glsl");
		return false;
	}
	Ref<RDShaderSPIRV> spirv = source->get_spirv();
	caster_shader = rd->shader_create_from_spirv(spirv->get_stages());
	if (!caster_shader.is_valid()) {
		fail("Cannot compile native shadow caster");
		return false;
	}
	RD::TextureFormat format;
	format.width = dimensions.x * 4;
	format.height = dimensions.y * dimensions.z;
	format.format = RD::DATA_FORMAT_R16G16B16A16_UINT;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	grid_rd = rd->texture_create(format, RD::TextureView());
	grid_texture.instantiate();
	grid_texture->set_texture_rd_rid(grid_rd);
	format.width = STRIDE / 4;
	format.height = max_sources;
	format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	data_rd = rd->texture_create(format, RD::TextureView());
	data_texture.instantiate();
	data_texture->set_texture_rd_rid(data_rd);
	format.width = tile_size;
	format.height = tile_size;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.array_layers = layers;
	format.format = filter_mode == 1 ? RD::DATA_FORMAT_R32G32B32A32_SFLOAT : RD::DATA_FORMAT_R16G16_UNORM;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
	if (filter_mode == 1) {
		format.usage_bits |= RD::TEXTURE_USAGE_STORAGE_BIT;
	}
	atlas_rd = rd->texture_create(format, RD::TextureView());
	atlas_texture.instantiate();
	atlas_texture->set_texture_rd_rid(atlas_rd);
	format.array_layers = 1;
	format.texture_type = RD::TEXTURE_TYPE_2D;
	format.format = RD::DATA_FORMAT_D32_SFLOAT;
	format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	format.is_discardable = true;
	depth_rd = rd->texture_create(format, RD::TextureView());
	layer_owner.resize(layers);
	layer_owner.fill(-1);
	slices.resize(layers);
	framebuffers.resize(layers);
	RD::SamplerState sampling;
	sampling.min_filter = RD::SAMPLER_FILTER_LINEAR;
	sampling.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler = rd->sampler_create(sampling);
	format.width = 1;
	format.height = 1;
	format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	format.is_discardable = false;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
	Vector<uint8_t> white;
	white.resize(4);
	white.fill(255);
	Vector<Vector<uint8_t>> initial;
	initial.push_back(white);
	white_texture = rd->texture_create(format, RD::TextureView(), initial);
	memory_bytes = uint64_t(tile_size) * tile_size * (uint64_t(layers) * (filter_mode == 1 ? 16 : 4) + 4 + (filter_mode == 1 ? 96 * FILTER_BATCH : 0)) + grid.size() * 8 + records.size() * 8 + lights.size() * sizeof(Light);
	if (memory_bytes > memory_limit) {
		fail("Native local-light memory budget exceeded");
		return false;
	}
	if (filter_mode == 1) {
		Ref<RDShaderFile> source_filter = ResourceLoader::load("res://features/lighting/vu_lights/native_moment_filter.glsl");
		if (source_filter.is_null()) {
			fail("Missing native moment filter");
			return false;
		}
		filter_shader = rd->shader_create_from_spirv(source_filter->get_spirv()->get_stages());
		filter_pipeline = rd->compute_pipeline_create(filter_shader);
		format.width = tile_size;
		format.height = tile_size;
		format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		format.array_layers = 6 * FILTER_BATCH;
		format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		filter_scratch = rd->texture_create(format, RD::TextureView());
		for (int axis = 0; axis < 2; axis++) {
			RD::Uniform source;
			source.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			source.binding = 0;
			source.append_id(sampler);
			source.append_id(axis == 0 ? atlas_rd : filter_scratch);
			RD::Uniform target;
			target.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			target.binding = 1;
			target.append_id(axis == 0 ? filter_scratch : atlas_rd);
			Vector<RD::Uniform> bindings;
			bindings.push_back(source);
			bindings.push_back(target);
			RID set = rd->uniform_set_create(bindings, filter_shader, 0);
			if (axis == 0) {
				filter_horizontal = set;
			} else {
				filter_vertical = set;
			}
		}
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_add("vu_native_data", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, data_texture);
	rs->global_shader_parameter_add("vu_native_atlas", RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY, atlas_texture);
	rs->global_shader_parameter_add("vu_native_origin", RSE::GLOBAL_VAR_TYPE_VEC3, world.position);
	rs->global_shader_parameter_add("vu_native_dims", RSE::GLOBAL_VAR_TYPE_IVEC3, dimensions);
	rs->global_shader_parameter_add("vu_native_settings", RSE::GLOBAL_VAR_TYPE_VEC4, shader_settings());
	return true;
}

Vector4 VuLightRuntime::shader_settings() const {
	int blocks = 1;
	for (const KeyValue<RID, int> &entry : light_slots) {
		blocks = MAX(blocks, entry.value / 64 + 1);
	}
	return Vector4(1 / cell_size, softness, tile_size, blocks);
}

// σ ядра карты в радианах. Границы задают выборки на текселе: шаг угла равен 2/tile_size, в фильтре до 12 шагов на сторону.
float VuLightRuntime::moment_filter_sigma() const {
	return CLAMP(softness / 3.0f, 1.0f / tile_size, 9.0f / tile_size);
}

bool VuLightRuntime::add_light(RID instance) {
	if (!ready || disposed || !scene_cull()->local_light_can_manage(instance, scenario)) {
		return false;
	}
	if (light_slots.has(instance)) {
		return true;
	}
	for (int i = 0; i < lights.size(); i++) {
		if (!lights[i].instance.is_valid()) {
			lights.write[i] = Light();
			lights.write[i].instance = instance;
			lights.write[i].queued = frame;
			light_slots.insert(instance, i);
			scene_cull()->local_light_set_owner(instance, true);
			if (data_texture.is_valid()) {
				RenderingServer::get_singleton()->global_shader_parameter_set("vu_native_settings", shader_settings());
			}
			return true;
		}
	}
	return false;
}
void VuLightRuntime::dirty(Light &light) {
	if (!light.dirty) {
		light.queued = frame;
	}
	light.dirty = true;
}
void VuLightRuntime::clear_membership(int slot) {
	Light &light = lights.write[slot];
	for (int cell : light.cells) {
		grid.write[cell * WORDS + slot / 32] &= ~(1u << (slot % 32));
	}
	if (!light.cells.is_empty()) {
		grid_dirty = true;
	}
	light.cells.clear();
	light.cell_range = -1;
}
void VuLightRuntime::update_membership(int slot) {
	Light &light = lights.write[slot];
	if (!light.enabled || !light.base.is_valid()) {
		clear_membership(slot);
		return;
	}
	Vector3 position = (light.shadows && shadows_enabled && light.valid) ? light.baked_transform.origin : light.transform.origin;
	float range = (light.shadows && shadows_enabled && light.valid) ? light.baked_range : light.range;
	// Conservative guard: reuse membership until the sphere leaves its cached envelope.
	const float guard = cell_size * 0.125f;
	if (light.cell_range >= range && position.distance_squared_to(light.cell_position) <= Math::pow(light.cell_range - range, 2)) {
		return;
	}
	clear_membership(slot);
	light.cell_position = position;
	light.cell_range = range + guard;
	range += guard;
	Vector3 lo = (position - Vector3(range, range, range) - world.position) / cell_size;
	Vector3 hi = (position + Vector3(range, range, range) - world.position) / cell_size;
	Vector3i first(MAX(0, int(Math::floor(lo.x))), MAX(0, int(Math::floor(lo.y))), MAX(0, int(Math::floor(lo.z))));
	Vector3i last(MIN(dimensions.x - 1, int(Math::floor(hi.x))), MIN(dimensions.y - 1, int(Math::floor(hi.y))), MIN(dimensions.z - 1, int(Math::floor(hi.z))));
	for (int z = first.z; z <= last.z; z++) {
		for (int y = first.y; y <= last.y; y++) {
			for (int x = first.x; x <= last.x; x++) {
				Vector3 closest = position.clamp(world.position + Vector3(x, y, z) * cell_size, world.position + Vector3(x + 1, y + 1, z + 1) * cell_size);
				if (closest.distance_squared_to(position) > range * range) {
					continue;
				}
				int cell = x + dimensions.x * (y + dimensions.y * z);
				grid.write[cell * WORDS + slot / 32] |= 1u << (slot % 32);
				light.cells.push_back(cell);
			}
		}
	}
	grid_dirty = true;
}

void VuLightRuntime::consume_changes() {
	scene_cull()->local_light_poll(scenario, changes);
	for (const RendererSceneCull::LocalLightChange &change : changes) {
		int *slot_ptr = light_slots.getptr(change.instance);
		if (slot_ptr) {
			int slot = *slot_ptr;
			Light &light = lights.write[slot];
			if (!change.active) {
				clear_membership(slot);
				for (int i = 0; i < layer_owner.size(); i++) {
					if (layer_owner[i] == slot) {
						layer_owner.write[i] = -1;
					}
				}
				for (Light &waiting : lights) {
					waiting.atlas_denied = false;
				}
				records.write[slot * STRIDE + 3] = 0;
				light.layer = -1;
				light.valid = false;
				light.base = RID();
				light.enabled = false;
				dirty(light);
				if (!change.exists) {
					light = Light();
					light_slots.erase(change.instance);
				}
				data_dirty = true;
				continue;
			}
			auto *storage = RSG::light_storage;
			float range = storage->light_get_param(change.base, RSE::LIGHT_PARAM_RANGE), angle = storage->light_get_param(change.base, RSE::LIGHT_PARAM_SPOT_ANGLE);
			bool shadows = storage->light_has_shadow(change.base);
			uint32_t mask = storage->light_get_shadow_caster_mask(change.base);
			bool geometry_change = light.base != change.base || light.transform != change.transform || light.range != range || light.angle != angle || light.caster_mask != mask;
			bool shadow_change = light.shadows != shadows;
			light.base = change.base;
			light.transform = change.transform;
			light.range = range;
			light.angle = angle;
			if (shadows && !light.shadows && (light.dirty || geometry_change)) {
				light.valid = false;
			}
			light.shadows = shadows;
			light.caster_mask = mask;
			if (light.enabled != change.enabled) {
				light.enabled = change.enabled;
				if (light.enabled) {
					light.valid = false;
					dirty(light);
				}
				update_membership(slot);
			}
			light.spot = storage->light_get_type(change.base) == RSE::LIGHT_SPOT;
			light.color = storage->light_get_color(change.base).srgb_to_linear();
			light.energy = storage->light_get_param(change.base, RSE::LIGHT_PARAM_ENERGY);
			light.decay = storage->light_get_param(change.base, RSE::LIGHT_PARAM_ATTENUATION);
			light.cone_decay = 1.0f / MAX(.001f, storage->light_get_param(change.base, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
			light.specular = 2.0f * storage->light_get_param(change.base, RSE::LIGHT_PARAM_SPECULAR);
			if (geometry_change) {
				dirty(light);
			}
			if (geometry_change || shadow_change) {
				update_membership(slot);
			}
			publish(slot);
			continue;
		}
		Geometry *old = geometry.getptr(change.instance);
		AABB changed_bounds = old ? old->bounds : change.bounds;
		if (change.active && !change.light) {
			changed_bounds = changed_bounds.merge(change.bounds);
			Geometry value;
			value.base = change.base;
			value.mesh_instance = change.mesh_instance;
			value.material_override = change.material_override;
			value.materials = change.materials;
			value.transform = change.transform;
			value.bounds = change.bounds;
			value.layers = change.layers;
			geometry.insert(change.instance, value);
			if (value.mesh_instance.is_valid()) {
				RendererRD::MeshStorage::get_singleton()->mesh_instance_check_for_update(value.mesh_instance);
			}
		} else if (old) {
			geometry.erase(change.instance);
		} else {
			continue;
		}
		if (change.flags & 5) {
			for (const KeyValue<RID, MaterialData> &entry : materials) {
				if (rd->uniform_set_is_valid(entry.value.uniform)) {
					rd->free_rid(entry.value.uniform);
				}
			}
			materials.clear();
		}
		for (int slot = 0; slot < lights.size(); slot++) {
			Light &light = lights.write[slot];
			if (!light.instance.is_valid() || !light.enabled) {
				continue;
			}
			Vector3 nearest = light.transform.origin.clamp(changed_bounds.position, changed_bounds.position + changed_bounds.size);
			if (nearest.distance_squared_to(light.transform.origin) <= light.range * light.range) {
				dirty(light);
			}
		}
	}
	RendererRD::MeshStorage::get_singleton()->update_mesh_instances();
}

void VuLightRuntime::publish(int slot) {
	Light &light = lights.write[slot];
	float data[STRIDE] = {};
	bool shadow = shadows_enabled && light.shadows;
	bool visible = light.enabled && light.base.is_valid() && (!shadow || light.valid);
	Transform3D pose = shadow && light.valid ? light.baked_transform : light.transform;
	float range = shadow && light.valid ? light.baked_range : light.range, angle = shadow && light.valid ? light.baked_angle : light.angle;
	Vector3 axis = -pose.basis.get_column(2).normalized();
	data[0] = pose.origin.x;
	data[1] = pose.origin.y;
	data[2] = pose.origin.z;
	data[3] = visible ? 1 / (range * range) : 0;
	data[4] = axis.x;
	data[5] = axis.y;
	data[6] = axis.z;
	data[7] = light.spot ? Math::cos(Math::deg_to_rad(angle)) : -2;
	data[8] = light.color.r * light.energy;
	data[9] = light.color.g * light.energy;
	data[10] = light.color.b * light.energy;
	data[11] = light.decay;
	data[12] = light.cone_decay;
	data[13] = light.specular;
	data[14] = softness;
	data[15] = range;
	Vector3 x = pose.basis.get_column(0).normalized(), y = pose.basis.get_column(1).normalized();
	data[16] = x.x;
	data[17] = x.y;
	data[18] = x.z;
	data[19] = 1 / Math::tan(Math::deg_to_rad(MIN(170.0f, 2 * angle + 6) * 0.5f));
	data[20] = y.x;
	data[21] = y.y;
	data[22] = y.z;
	data[23] = shadow && light.valid ? light.layer : -1;
	if (memcmp(records.ptr() + slot * STRIDE, data, sizeof(data)) != 0) {
		memcpy(records.ptrw() + slot * STRIDE, data, sizeof(data));
		data_dirty = true;
	}
}

void VuLightRuntime::render_frame() {
	if (!ready || disposed || !error.is_empty()) {
		return;
	}
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	frame++;
	served_age = uploaded_bytes = updates = faces = triangles = draws = 0;
	if (gpu_timestamps) {
		rd->capture_timestamp("VU Native Begin");
	}
	consume_changes();
	uint64_t shadow_start = OS::get_singleton()->get_ticks_usec();
	if (gpu_timestamps) {
		rd->capture_timestamp("VU Native Shadows Begin");
	}
	pending_filters[0].clear();
	pending_filters[1].clear();
	int budget = cap == 0 ? max_sources : cap;
	while (shadows_enabled && budget-- > 0) {
		int selected = -1;
		double best = -1;
		for (int slot = 0; slot < lights.size(); slot++) {
			const Light &light = lights[slot];
			if (!light.instance.is_valid() || !light.base.is_valid() || !light.enabled || !light.shadows || !light.dirty || light.atlas_denied) {
				continue;
			}
			double distance = light.transform.origin.distance_squared_to(camera_position);
			double score = (light.instance == interactive ? 1e12 : 0) + (frame - light.queued) * 4 + 100 / (1 + distance);
			if (!light.valid) {
				score += 1000;
			}
			if (score > best) {
				best = score;
				selected = slot;
			}
		}
		if (selected < 0) {
			break;
		}
		if (!render_shadow(selected)) {
			continue;
		}
		Light &light = lights.write[selected];
		served_age = MAX(served_age, frame - light.queued);
		light.dirty = false;
		light.valid = true;
		light.updated = frame;
		light.baked_transform = light.transform;
		light.baked_range = light.range;
		light.baked_angle = light.angle;
		update_membership(selected);
		publish(selected);
		updates++;
		if (filter_mode == 1) {
			LocalVector<int> &batch = pending_filters[light.spot ? 1 : 0];
			batch.push_back(selected);
			if (batch.size() == FILTER_BATCH) {
				filter_moments(batch);
				batch.clear();
			}
		}
	}
	for (const LocalVector<int> &batch : pending_filters) {
		filter_moments(batch);
	}
	if (gpu_timestamps) {
		rd->capture_timestamp("VU Native Shadows End");
	}
	shadow_ms = elapsed(shadow_start);
	upload();
	cpu_ms = elapsed(start);
	if (gpu_timestamps) {
		rd->capture_timestamp("VU Native End");
	}
	total_uploads += uploaded_bytes;
	total_updates += updates;
	total_faces += faces;
	total_triangles += triangles;
	total_draws += draws;
	total_shadow_ms += shadow_ms;
}
void VuLightRuntime::upload() {
	Vector<uint8_t> bytes;
	if (grid_dirty) {
		bytes.resize(grid.size() * 4);
		memcpy(bytes.ptrw(), grid.ptr(), bytes.size());
		rd->texture_update(grid_rd, 0, bytes);
		uploaded_bytes += bytes.size();
		grid_dirty = false;
		total_grid_uploads++;
	}
	if (data_dirty) {
		bytes.resize(records.size() * 4);
		memcpy(bytes.ptrw(), records.ptr(), bytes.size());
		rd->texture_update(data_rd, 0, bytes);
		uploaded_bytes += bytes.size();
		data_dirty = false;
		total_data_uploads++;
	}
}
void VuLightRuntime::set_cap(int value) {
	ERR_FAIL_COND(value < 0);
	cap = value;
}
void VuLightRuntime::set_softness(float value) {
	ERR_FAIL_COND(value < 0);
	if (softness == value) {
		return;
	}
	softness = value;
	RenderingServer::get_singleton()->global_shader_parameter_set("vu_native_settings", shader_settings());
	for (const KeyValue<RID, int> &entry : light_slots) {
		publish(entry.value);
		if (filter_mode == 1) {
			dirty(lights.write[entry.value]);
		}
	}
}
void VuLightRuntime::set_shadows_enabled(bool value) {
	if (!ready || disposed || shadows_enabled == value) {
		return;
	}
	if (value) {
		consume_changes();
	}
	shadows_enabled = value;
	for (const KeyValue<RID, int> &entry : light_slots) {
		Light &light = lights.write[entry.value];
		if (value && light.dirty) {
			light.valid = false;
		}
		update_membership(entry.value);
		publish(entry.value);
	}
}
void VuLightRuntime::set_light_enabled(RID instance, bool enabled) {
	if (disposed) { return; }
	int *slot = light_slots.getptr(instance);
	ERR_FAIL_NULL(slot);
	scene_cull()->local_light_set_enabled(instance, enabled);
	Light &light = lights.write[*slot];
	if (light.enabled == enabled) {
		return;
	}
	light.enabled = enabled;
	if (enabled) {
		light.valid = false;
		dirty(light);
	}
	update_membership(*slot);
	publish(*slot);
}

RID VuLightRuntime::pipeline(RD::VertexFormatID format) {
	RID *existing = pipelines.getptr(format);
	if (existing) {
		return *existing;
	}
	RD::PipelineRasterizationState raster;
	raster.cull_mode = RD::POLYGON_CULL_DISABLED;
	RD::PipelineDepthStencilState depth;
	depth.enable_depth_test = true;
	depth.enable_depth_write = true;
	depth.depth_compare_operator = RD::COMPARE_OP_GREATER;
	RD::PipelineColorBlendState blend;
	blend.attachments.push_back(RD::PipelineColorBlendState::Attachment());
	RID result = rd->render_pipeline_create(caster_shader, rd->framebuffer_get_format(framebuffers[0]), format, RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), depth, blend);
	pipelines.insert(format, result);
	return result;
}

VuLightRuntime::MaterialData VuLightRuntime::material_data(RID material) {
	MaterialData *cached = materials.getptr(material);
	if (cached && rd->uniform_set_is_valid(cached->uniform)) {
		return *cached;
	}
	MaterialData data;
	RID texture = white_texture;
	if (material.is_valid()) {
		auto *storage = RendererRD::MaterialStorage::get_singleton();
		auto *shader = storage->material_get_shader_data(material);
		data.casts = shader ? shader->casts_shadows() : true;
		if (shader && shader->get_default_parameter("alpha_scissor_threshold").get_type() != Variant::NIL) {
			Variant threshold = storage->material_get_param(material, "alpha_scissor_threshold");
			data.threshold = threshold.get_type() == Variant::NIL ? 0.5f : float(threshold);
			Variant color = storage->material_get_param(material, "albedo_color");
			if (color.get_type() == Variant::COLOR) {
				data.alpha = Color(color).a;
			}
			if (bool(storage->material_get_param(material, "use_albedo_texture"))) {
				RID source = storage->material_get_param(material, "albedo_texture");
				RID rd_texture = RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(source);
				if (rd_texture.is_valid()) {
					texture = rd_texture;
				}
			}
			Variant scale = storage->material_get_param(material, "uv_scale"), offset = storage->material_get_param(material, "uv_offset");
			if (scale.get_type() == Variant::VECTOR2) {
				Vector2 v = scale;
				data.uv.x = v.x;
				data.uv.y = v.y;
			}
			if (offset.get_type() == Variant::VECTOR2) {
				Vector2 v = offset;
				data.uv.z = v.x;
				data.uv.w = v.y;
			}
		}
	}
	RD::Uniform uniform;
	uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	uniform.binding = 0;
	uniform.append_id(sampler);
	uniform.append_id(texture);
	Vector<RD::Uniform> uniforms;
	uniforms.push_back(uniform);
	data.uniform = rd->uniform_set_create(uniforms, caster_shader, 0);
	materials.insert(material, data);
	return data;
}

bool VuLightRuntime::render_shadow(int slot) {
	Light &light = lights.write[slot];
	int count = light.spot ? 1 : 6;
	if (light.layer < 0) {
		if (light.spot) {
			for (int i = layers - 1; i >= 0; i--) {
				if (layer_owner[i] < 0) {
					light.layer = i;
					break;
				}
			}
		} else {
			for (int i = 0; i + 6 <= layers; i += 6) {
				bool free = true;
				for (int j = 0; j < 6; j++) {
					if (layer_owner[i + j] >= 0) {
						free = false;
						break;
					}
				}
				if (free) {
					light.layer = i;
					break;
				}
			}
		}
		if (light.layer < 0) {
			light.atlas_denied = true;
			ERR_PRINT_ONCE("Native shadow atlas full: waiting sources remain dark; existing lights continue. See atlas_denied diagnostics.");
			return false;
		}
		for (int i = 0; i < count; i++) {
			layer_owner.write[light.layer + i] = slot;
		}
	}
	auto *mesh = RendererRD::MeshStorage::get_singleton();
	float fov = light.spot ? MIN(170.0f, 2 * light.angle + 6) : 90.0f;
	Projection projection = Projection::create_perspective(fov, 1, 0.01, MAX(0.02f, light.range));
	Projection correction = Projection::create_depth_correction(true);
	LocalVector<const Geometry *> candidates;
	for (const KeyValue<RID, Geometry> &entry : geometry) {
		const Geometry &caster = entry.value;
		if ((caster.layers & light.caster_mask) && light.transform.origin.distance_squared_to(light.transform.origin.clamp(caster.bounds.position, caster.bounds.position + caster.bounds.size)) <= light.range * light.range) {
			candidates.push_back(&caster);
		}
	}
	for (int face = 0; face < count; face++) {
		int layer = light.layer + face;
		if (!framebuffers[layer].is_valid()) {
			slices.write[layer] = rd->texture_create_shared_from_slice(RD::TextureView(), atlas_rd, layer, 0);
			Vector<RID> attachments;
			attachments.push_back(slices[layer]);
			attachments.push_back(depth_rd);
			framebuffers.write[layer] = rd->framebuffer_create(attachments);
		}
		if (!framebuffers[0].is_valid()) {
			slices.write[0] = rd->texture_create_shared_from_slice(RD::TextureView(), atlas_rd, 0, 0);
			Vector<RID> attachments;
			attachments.push_back(slices[0]);
			attachments.push_back(depth_rd);
			framebuffers.write[0] = rd->framebuffer_create(attachments);
		}
		Transform3D pose = light.spot ? light.transform.orthonormalized() : Transform3D(faces_basis[face], light.transform.origin);
		Projection view = projection * Projection(pose.affine_inverse());
		Vector<Plane> planes = projection.get_projection_planes(pose);
		Color clear(1, 1, 1, 1);
		RD::DrawListID list = rd->draw_list_begin(framebuffers[layer], RD::DRAW_CLEAR_ALL, VectorView<Color>(&clear, 1), 0);
		Projection corrected_view = correction * view;
		for (const Geometry *candidate : candidates) {
			const Geometry &caster = *candidate;
			bool outside = false;
			for (const Plane &plane : planes) {
				if (plane.distance_to(caster.bounds.get_support(-plane.normal)) > 0) {
					outside = true;
					break;
				}
			}
			if (outside) {
				continue;
			}
			int surfaces = mesh->mesh_get_surface_count(caster.base);
			Projection mvp = corrected_view * Projection(caster.transform);
			for (int i = 0; i < surfaces; i++) {
				void *surface = mesh->mesh_get_surface(caster.base, uint32_t(i));
				if (mesh->mesh_surface_get_primitive(surface) != RSE::PRIMITIVE_TRIANGLES) {
					continue;
				}
				uint64_t flags = mesh->mesh_surface_get_format(surface);
				RID material = caster.material_override.is_valid() ? caster.material_override : (i < caster.materials.size() && caster.materials[i].is_valid() ? caster.materials[i] : mesh->mesh_surface_get_material(caster.base, i));
				MaterialData mat = material_data(material);
				if (!mat.casts) {
					continue;
				}
				uint64_t mask = (1u << RSE::ARRAY_VERTEX) | (1u << RSE::ARRAY_TEX_UV);
				RID vertex_array;
				RD::VertexFormatID format;
				if (caster.mesh_instance.is_valid()) {
					mesh->mesh_instance_surface_get_vertex_arrays_and_format(caster.mesh_instance, i, mask, false, false, vertex_array, format);
				} else {
					mesh->mesh_surface_get_vertex_arrays_and_format(surface, mask, false, false, vertex_array, format);
				}
				DrawPush push = {};
				for (int c = 0; c < 4; c++) {
					for (int r = 0; r < 4; r++) {
						push.mvp[c * 4 + r] = mvp[c][r];
					}
				}
				AABB bounds = mesh->mesh_surface_get_aabb(surface);
				bool compressed = flags & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
				for (int k = 0; k < 3; k++) {
					push.decode_position[k] = compressed ? bounds.position[k] : 0;
					push.decode_size[k] = compressed ? bounds.size[k] : 1;
				}
				push.decode_position[3] = (filter_mode == 1 ? -1 : 1) / light.range;
				push.decode_size[3] = 1 / projection[0][0];
				push.options[0] = mat.threshold;
				push.options[1] = mat.alpha;
				Vector4 uv_decode = mesh->mesh_surface_get_uv_scale(surface);
				push.options[2] = uv_decode.x;
				push.options[3] = uv_decode.y;
				for (int k = 0; k < 4; k++) {
					push.uv_transform[k] = mat.uv[k];
				}
				RID index = mesh->mesh_surface_get_index_array(surface, 0);
				rd->draw_list_bind_render_pipeline(list, pipeline(format));
				rd->draw_list_bind_uniform_set(list, mat.uniform, 0);
				rd->draw_list_bind_vertex_array(list, vertex_array);
				if (index.is_valid()) {
					rd->draw_list_bind_index_array(list, index);
				}
				rd->draw_list_set_push_constant(list, &push, sizeof(push));
				rd->draw_list_draw(list, index.is_valid(), 1);
				draws++;
				triangles += mesh->mesh_surface_get_vertices_drawn_count(surface) / 3;
			}
		}
		rd->draw_list_end();
		faces++;
	}
	return true;
}

void VuLightRuntime::filter_moments(const LocalVector<int> &sources) {
	struct Push {
		int32_t layers[FILTER_BATCH][4];
		float settings[FILTER_BATCH][4];
	};
	const float sigma = moment_filter_sigma();
	for (uint32_t first = 0; first < sources.size(); first += FILTER_BATCH) {
		const int count = MIN(uint32_t(FILTER_BATCH), sources.size() - first);
		const bool spot = lights[sources[first]].spot;
		Push push = {};
		for (int axis = 0; axis < (spot ? 2 : 4); axis++) {
			for (int i = 0; i < count; i++) {
				const Light &light = lights[sources[first + i]];
				push.layers[i][0] = axis % 2 == 0 ? light.layer : i * 6;
				push.layers[i][1] = axis % 2 == 0 ? i * 6 : light.layer;
				push.layers[i][2] = spot ? 1 : 6;
				push.layers[i][3] = axis;
				const float half_tan = spot ? Math::tan(Math::deg_to_rad(MIN(170.0f, 2 * light.angle + 6) * .5f)) : 1.0f;
				push.settings[i][0] = tile_size;
				// Тот же угол в шагах карты: для куба шаг 2/tile_size, для спота в центре карты 2·tan/tile_size.
				push.settings[i][1] = CLAMP(sigma * tile_size / (2.0f * half_tan), 0.5f, 4.5f);
			}
			RD::ComputeListID list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(list, filter_pipeline);
			rd->compute_list_bind_uniform_set(list, axis % 2 == 0 ? filter_horizontal : filter_vertical, 0);
			rd->compute_list_set_push_constant(list, &push, sizeof(push));
			rd->compute_list_dispatch(list, (tile_size + 7) / 8, (tile_size + 7) / 8, count * (spot ? 1 : 6));
			rd->compute_list_end();
		}
	}
}

Dictionary VuLightRuntime::diagnostics() const {
	Dictionary result;
	int active = 0, omni = 0, spot = 0, valid = 0, queue = 0, casting = 0, used_layers = 0, denied = 0;
	uint64_t age = 0;
	for (const Light &light : lights) {
		if (light.instance.is_valid() && light.enabled) {
			active++;
			if (light.atlas_denied) { denied++; }
			light.spot ? spot++ : omni++;
			if (light.shadows && shadows_enabled) {
				casting++;
				if (light.valid) {
					valid++;
				}
			}
			if (light.layer >= 0) {
				used_layers += light.spot ? 1 : 6;
			}
			if (light.dirty && light.shadows && shadows_enabled) {
				queue++;
				age = MAX(age, frame - light.queued);
			}
		}
	}
	result["lights"] = active;
	result["omni"] = omni;
	result["spots"] = spot;
	result["valid_shadows"] = valid;
	result["queue"] = queue;
	result["casting"] = casting;
	result["atlas_denied"] = denied;
	result["used_layers"] = used_layers;
	result["served_age_frames"] = served_age;
	result["total_uploads"] = total_uploads;
	result["total_updates"] = total_updates;
	result["total_faces"] = total_faces;
	result["total_triangles"] = total_triangles;
	result["total_draws"] = total_draws;
	result["total_shadow_ms"] = total_shadow_ms;
	result["total_grid_uploads"] = total_grid_uploads;
	result["total_data_uploads"] = total_data_uploads;
	result["max_age_frames"] = age;
	result["updates"] = updates;
	result["faces"] = faces;
	result["triangles"] = triangles;
	result["draws"] = draws;
	result["cpu_ms"] = cpu_ms;
	result["shadow_submit_ms"] = shadow_ms;
	result["uploaded_bytes"] = uploaded_bytes;
	result["memory_bytes"] = memory_bytes;
	result["filter_mode"] = filter_mode;
	result["atlas_layers"] = layers;
	result["tile_size"] = tile_size;
	result["cap"] = cap;
	result["casters"] = geometry.size();
	result["error"] = error;
	const int *interactive_slot = light_slots.getptr(interactive);
	result["interactive_updated"] = interactive_slot && lights[*interactive_slot].updated == frame;
	return result;
}
bool VuLightRuntime::owns_light(RID instance) const {
	return !disposed && light_slots.has(instance) && scene_cull()->local_light_is_managed(instance);
}
void VuLightRuntime::dispose() {
	if (disposed) {
		return;
	}
	disposed = true;
	Callable callback = callable_mp(this, &VuLightRuntime::render_frame);
	if (RenderingServer::get_singleton()->is_connected("frame_pre_draw", callback)) {
		RenderingServer::get_singleton()->disconnect("frame_pre_draw", callback);
	}
	if (ready) {
		for (const KeyValue<RID, int> &entry : light_slots) {
			scene_cull()->local_light_set_enabled(entry.key, true);
			scene_cull()->local_light_set_owner(entry.key, false);
		}
		scene_cull()->local_light_observe(scenario, false);
		for (const char *name : { "vu_native_data", "vu_native_atlas", "vu_native_origin", "vu_native_dims", "vu_native_settings" }) {
			RenderingServer::get_singleton()->global_shader_parameter_remove(name);
		}
	}
	if (!rd) {
		return;
	}
	for (const KeyValue<RID, MaterialData> &entry : materials) {
		if (rd->uniform_set_is_valid(entry.value.uniform)) {
			rd->free_rid(entry.value.uniform);
		}
	}
	for (const KeyValue<uint64_t, RID> &entry : pipelines) {
		rd->free_rid(entry.value);
	}
	for (RID rid : framebuffers) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	for (RID rid : slices) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	if (grid_texture.is_valid()) {
		grid_texture->set_texture_rd_rid(RID());
	}
	if (data_texture.is_valid()) {
		data_texture->set_texture_rd_rid(RID());
	}
	if (atlas_texture.is_valid()) {
		atlas_texture->set_texture_rd_rid(RID());
	}
	for (RID rid : { filter_horizontal, filter_vertical, filter_pipeline, filter_shader, filter_scratch, grid_rd, data_rd, atlas_rd, depth_rd, caster_shader, sampler, white_texture }) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
}
VuLightRuntime::~VuLightRuntime() {
	dispose();
}
