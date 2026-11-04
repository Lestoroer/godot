// lestoroer#local-lights
#include "local_lights_world.h"

#include "local_lights.h"

#include "core/config/engine.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

namespace {

const Vector3i CELL_DIMS(20, 20, 20);
const double CELL_SIZE_EXACT = 1.6;
const float CELL_SIZE = float(CELL_SIZE_EXACT);
const float RECENTER_CELLS = 4.0f;
const uint32_t DATA_WIDTH = 2048;
const uint32_t SLOT_TEXELS = 8;
const uint32_t SLOT_FLOATS = SLOT_TEXELS * 4;
const uint32_t BYTES_PER_DATA_TEXEL = 8;
const int MAP_CAPACITY_STEP = 8;
const int BATCH_TILES = 8;
const float NEAR_PLANE = 0.03f;
const float SPOT_HOTSPOT = 0.5f;
// Снимок спота шире конуса: выборка у края конуса не выходит за плитку.
const float SPOT_MAP_MARGIN_DEGREES = 8.0f;
const float SPOT_MAP_MAX_HALF_ANGLE_DEGREES = 75.0f;
// Запас грани куба сверх её ребра, тексели: gather 2×2 не выходит за плитку. Контракт с local_light_ratio.glsl.
const float CUBE_GUARD_TEXELS = 4.0f;
const uint32_t NOISE_FRAME_PERIOD = 1024;

// Оси граней куба. Контракт с выбором грани в local_light_ratio.glsl и local_lights_inc.gdshaderinc.
const Vector3 FACE_RIGHT[6] = { Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 0) };
const Vector3 FACE_UP[6] = { Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) };
const Vector3 FACE_FORWARD[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(0, 1, 0) };

RendererSceneCull *scene_cull() {
	return static_cast<RendererSceneCull *>(RSG::scene);
}

// Карта рисует только позиции и не исполняет материал: материал прохода прозрачности тень не отбрасывает, альфа-вырез отбрасывает целиком.
bool material_casts_in_maps(RID p_material) {
	typedef RendererSceneRenderImplementation::SceneShaderForwardClustered SceneShader;
	const SceneShader::MaterialData *data = static_cast<const SceneShader::MaterialData *>(RendererRD::MaterialStorage::get_singleton()->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
	return !data || !data->shader_data || !data->shader_data->uses_alpha_pass();
}

Basis rows_of(const Vector3 &p_right, const Vector3 &p_up, const Vector3 &p_forward) {
	Basis rows;
	rows.rows[0] = p_right;
	rows.rows[1] = p_up;
	rows.rows[2] = p_forward;
	return rows;
}

RD::Uniform sampled_uniform(int p_binding, RID p_sampler, RID p_texture) {
	RD::Uniform uniform;
	uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	uniform.binding = p_binding;
	uniform.append_id(p_sampler);
	uniform.append_id(p_texture);
	return uniform;
}

RD::Uniform image_uniform(int p_binding, RID p_texture) {
	RD::Uniform uniform;
	uniform.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	uniform.binding = p_binding;
	uniform.append_id(p_texture);
	return uniform;
}

RD::Uniform buffer_uniform(int p_binding, RID p_buffer) {
	RD::Uniform uniform;
	uniform.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	uniform.binding = p_binding;
	uniform.append_id(p_buffer);
	return uniform;
}

// Плоскости снимка: точка внутри, если скалярное произведение с нормалью неотрицательно.
bool box_outside_planes(const Vector3 *p_normals, int p_count, const Vector3 &p_center, const Vector3 &p_half) {
	for (int i = 0; i < p_count; i++) {
		const Vector3 &n = p_normals[i];
		if (n.dot(p_center) + Math::abs(n.x) * p_half.x + Math::abs(n.y) * p_half.y + Math::abs(n.z) * p_half.z < 0.0f) {
			return true;
		}
	}
	return false;
}

void pyramid_normals(const Vector3 &p_right, const Vector3 &p_up, const Vector3 &p_forward, float p_extent, Vector3 *r_normals) {
	r_normals[0] = (p_right + p_forward * p_extent).normalized();
	r_normals[1] = (-p_right + p_forward * p_extent).normalized();
	r_normals[2] = (p_up + p_forward * p_extent).normalized();
	r_normals[3] = (-p_up + p_forward * p_extent).normalized();
}

} // namespace

LocalLightsWorld::LocalLightsWorld(RID p_scenario) {
	scenario = p_scenario;
	rd = RD::get_singleton();
	Vector<String> modes;
	modes.push_back("");
	caster_shader.initialize(modes);
	caster_shader_version = caster_shader.version_create();
	caster_rd_shader = caster_shader.version_get_shader(caster_shader_version, 0);
	ratio_shader.initialize(modes);
	ratio_shader_version = ratio_shader.version_create();
	ratio_rd_shader = ratio_shader.version_get_shader(ratio_shader_version, 0);
	ratio_pipeline = rd->compute_pipeline_create(ratio_rd_shader);
	blur_shader.initialize(modes);
	blur_shader_version = blur_shader.version_create();
	blur_rd_shader = blur_shader.version_get_shader(blur_shader_version, 0);
	blur_pipeline = rd->compute_pipeline_create(blur_rd_shader);
	params_buffer = rd->uniform_buffer_create(sizeof(ScreenParams));

	const uint32_t cell_total = CELL_DIMS.x * CELL_DIMS.y * CELL_DIMS.z;
	cell_omni.resize(cell_total);
	cell_spot.resize(cell_total);
	cell_next.resize(cell_total);
	memset(cell_omni.ptr(), 0, cell_total * sizeof(uint32_t));
	memset(cell_spot.ptr(), 0, cell_total * sizeof(uint32_t));
	grid_bytes.resize(cell_total * 4 * sizeof(float));
	memset(grid_bytes.ptrw(), 0, grid_bytes.size());

	RD::TextureFormat grid_format;
	grid_format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	grid_format.width = CELL_DIMS.x;
	grid_format.height = CELL_DIMS.y * CELL_DIMS.z;
	grid_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	grid_rd = rd->texture_create(grid_format, RD::TextureView());
	grid_texture = _wrap_texture(grid_rd, RSE::TEXTURE_LAYERED_2D_ARRAY);
}

LocalLightsWorld::~LocalLightsWorld() {
	_free_retired_textures();
	_free_wrapped(grid_texture);
	_free_wrapped(data_texture);
	_free_wrapped(omni_atlas_texture);
	_free_wrapped(spot_atlas_texture);
	_free_wrapped(blurred_ratio_texture);
	for (const KeyValue<RD::VertexFormatID, RID> &entry : caster_pipelines) {
		rd->free_rid(entry.value);
	}
	for (const KeyValue<int, DepthStrip> &entry : depth_strips) {
		rd->free_rid(entry.value.framebuffer);
		rd->free_rid(entry.value.depth);
	}
	for (const RID &rid : { omni_atlas, spot_atlas, grid_rd, data_rd, blur_radius, ratio_and_normal, blurred_ratio, params_buffer, ratio_pipeline, blur_pipeline }) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	caster_shader.version_free(caster_shader_version);
	ratio_shader.version_free(ratio_shader_version);
	blur_shader.version_free(blur_shader_version);
}

RID LocalLightsWorld::_wrap_texture(RID p_rd_texture, RSE::TextureLayeredType p_type) const {
	RID texture = RSG::texture_storage->texture_allocate();
	RendererRD::TextureStorage::get_singleton()->texture_rd_initialize(texture, p_rd_texture, p_type);
	return texture;
}

void LocalLightsWorld::_free_wrapped(RID &r_texture) const {
	if (r_texture.is_valid()) {
		RSG::texture_storage->texture_free(r_texture);
		r_texture = RID();
	}
}

void LocalLightsWorld::_retire_texture(RID &r_rd_texture, RID &r_wrapped) {
	if (r_wrapped.is_valid()) {
		retired_wrapped_textures.push_back(r_wrapped);
		r_wrapped = RID();
	}
	if (r_rd_texture.is_valid()) {
		retired_rd_textures.push_back(r_rd_texture);
		r_rd_texture = RID();
	}
}

void LocalLightsWorld::_free_retired_textures() {
	for (RID &wrapped : retired_wrapped_textures) {
		_free_wrapped(wrapped);
	}
	retired_wrapped_textures.clear();
	for (const RID &texture : retired_rd_textures) {
		rd->free_rid(texture);
	}
	retired_rd_textures.clear();
}

void LocalLightsWorld::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		return;
	}
	enabled = p_enabled;
	refresh_all = true;
	if (!enabled) {
		for (Lamp &lamp : lamps) {
			lamp.in_grid = false;
		}
		repack_needed = true;
		globals_published = false;
		RSG::material_storage->global_shader_parameter_set("local_light_dims", Vector3i());
		frame_metrics = LocalLightsFrameMetrics();
		LocalLights::get_singleton()->publish_frame_metrics(frame_metrics);
	}
	scene_cull()->scenario_queue_lights_for_surface_cache(scenario);
}

void LocalLightsWorld::prepare_view(const LocalVector<Instance *> &p_lights) {
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame != current_frame) {
		_begin_frame(frame);
	}
	frame_metrics.views++;
	uint64_t started = OS::get_singleton()->get_ticks_usec();
	_take_view_lamps(p_lights, frame);
	frame_metrics.track_usec += OS::get_singleton()->get_ticks_usec() - started;
	_serve_queue(frame);
	started = OS::get_singleton()->get_ticks_usec();
	_update_grid();
	_publish_globals();
	frame_metrics.grid_usec += OS::get_singleton()->get_ticks_usec() - started;
}

void LocalLightsWorld::_begin_frame(uint64_t p_frame) {
	current_frame = p_frame;
	_free_retired_textures();
	LocalLights::get_singleton()->publish_frame_metrics(frame_metrics);
	frame_metrics = LocalLightsFrameMetrics();
	for (uint32_t i = 0; i < lamps.size(); i++) {
		Lamp &lamp = lamps[i];
		if (!lamp.active) {
			continue;
		}
		const Instance *instance = scene_cull()->instance_owner.get_or_null(lamp.instance);
		if (!instance || instance->base_type != RSE::INSTANCE_LIGHT) {
			_release_lamp(i);
			continue;
		}
		// Лампа ушла из всех видов: её вклад никто не считает, изменения дождутся возвращения в кадр.
		if (lamp.in_grid && lamp.last_visible_frame + 1 < p_frame) {
			lamp.in_grid = false;
			repack_needed = true;
		}
	}
}

void LocalLightsWorld::_take_view_lamps(const LocalVector<Instance *> &p_lights, uint64_t p_frame) {
	frame_metrics.lamps_in_view = MAX(frame_metrics.lamps_in_view, p_lights.size());
	bool form_changed = false;
	for (Instance *instance : p_lights) {
		const uint32_t *existing = lamp_of_instance.getptr(instance->self);
		uint32_t index;
		bool is_new = false;
		if (existing) {
			index = *existing;
		} else {
			if (!free_lamps.is_empty()) {
				index = free_lamps[free_lamps.size() - 1];
				free_lamps.resize(free_lamps.size() - 1);
			} else {
				index = lamps.size();
				lamps.resize(index + 1);
			}
			lamps[index] = Lamp();
			lamps[index].active = true;
			lamps[index].instance = instance->self;
			lamp_of_instance.insert(instance->self, index);
			is_new = true;
		}
		Lamp &lamp = lamps[index];
		lamp.last_visible_frame = p_frame;
		InstanceLightData *light = static_cast<InstanceLightData *>(instance->base_data);
		if (is_new || refresh_all || light->is_shadow_dirty()) {
			const Vector3 previous_position = lamp.position;
			const float previous_range = lamp.range;
			_refresh_lamp(lamp, instance, p_frame);
			light->clear_shadow_dirty();
			form_changed = form_changed || is_new || refresh_all || previous_position != lamp.position || previous_range != lamp.range;
		}
	}
	refresh_all = false;
	if (form_changed) {
		_update_window();
	}
}

void LocalLightsWorld::_refresh_lamp(Lamp &r_lamp, const Instance *p_instance, uint64_t p_frame) {
	const RID base = p_instance->base;
	auto *storage = RSG::light_storage;
	const Transform3D &transform = p_instance->transform;
	const float range = storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
	const bool spot = storage->light_get_type(base) == RSE::LIGHT_SPOT;
	const float angle = spot ? storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE) : 0.0f;
	const bool form_changed = r_lamp.range != range || r_lamp.angle != angle || r_lamp.transform != transform || r_lamp.cells.is_empty();
	r_lamp.spot = spot;
	r_lamp.transform = transform;
	r_lamp.position = transform.origin;
	r_lamp.axis = spot ? -transform.basis.get_column(2).normalized() : Vector3();
	r_lamp.range = range;
	r_lamp.angle = angle;
	r_lamp.linear_color = storage->light_get_color(base).srgb_to_linear();
	r_lamp.energy = storage->light_get_param(base, RSE::LIGHT_PARAM_ENERGY);
	r_lamp.decay = storage->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
	r_lamp.cone_attenuation = spot ? 1.0f / storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION) : 1.0f;
	r_lamp.specular_amount = storage->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f;
	r_lamp.shadow_bias = storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_BIAS);
	r_lamp.shadow_normal_bias = storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_NORMAL_BIAS);
	r_lamp.caster_mask = storage->light_get_shadow_caster_mask(base);
	r_lamp.casts = storage->light_has_shadow(base);
	r_lamp.data_dirty = true;
	if (form_changed && origin_valid) {
		LocalVector<uint32_t> cells;
		_compute_cells(r_lamp, cells);
		if (cells.size() != r_lamp.cells.size() || (cells.size() > 0 && memcmp(cells.ptr(), r_lamp.cells.ptr(), cells.size() * sizeof(uint32_t)) != 0)) {
			r_lamp.cells = cells;
			if (r_lamp.in_grid) {
				repack_needed = true;
			}
		}
	}
	if (r_lamp.casts) {
		if (r_lamp.queued_order == 0) {
			r_lamp.queued_order = next_queue_order++;
			r_lamp.queued_frame = p_frame;
		}
	} else {
		r_lamp.queued_order = 0;
		_release_map(r_lamp);
	}
}

// Окно следует за лампами: центр их охвата, снап к ячейке. Сдвиг окна заново раскладывает все лампы.
void LocalLightsWorld::_update_window() {
	AABB bounds;
	bool first = true;
	for (const Lamp &lamp : lamps) {
		if (!lamp.active) {
			continue;
		}
		if (first) {
			bounds = AABB(lamp.position, Vector3());
			first = false;
		} else {
			bounds.expand_to(lamp.position);
		}
	}
	if (first) {
		return;
	}
	const Vector3 center = bounds.get_center();
	const Vector3 drift = (center - window_center).abs();
	if (origin_valid && MAX(drift.x, MAX(drift.y, drift.z)) <= RECENTER_CELLS * CELL_SIZE) {
		// Лампа могла появиться до первого окна: её ячейки посчитаны в _refresh_lamp.
		for (Lamp &lamp : lamps) {
			if (lamp.active && lamp.cells.is_empty()) {
				_compute_cells(lamp, lamp.cells);
			}
		}
		return;
	}
	const Vector3 corner = center - Vector3(CELL_DIMS) * 0.5f * CELL_SIZE;
	origin_cell = Vector3i((corner / CELL_SIZE).floor());
	origin = Vector3(origin_cell) * CELL_SIZE;
	window_center = origin + Vector3(CELL_DIMS) * 0.5f * CELL_SIZE;
	origin_valid = true;
	globals_published = false;
	for (Lamp &lamp : lamps) {
		if (lamp.active) {
			_compute_cells(lamp, lamp.cells);
			lamp.data_dirty = true;
		}
	}
	repack_needed = true;
}

// Ячейки окна в охвате лампы: коробка охвата, внутри неё хорда сферы, у спота ещё конус.
void LocalLightsWorld::_compute_cells(Lamp &r_lamp, LocalVector<uint32_t> &r_cells) const {
	r_cells.clear();
	const float reach_range = r_lamp.range;
	AABB box(r_lamp.position - Vector3(1, 1, 1) * reach_range, Vector3(1, 1, 1) * reach_range * 2.0f);
	const float outer_degrees = MIN(r_lamp.angle, 89.0f);
	const float cos_outer = Math::cos(Math::deg_to_rad(outer_degrees));
	if (r_lamp.spot) {
		const Vector3 base = r_lamp.position + r_lamp.axis * reach_range;
		const float base_radius = reach_range * Math::tan(Math::deg_to_rad(outer_degrees));
		const Vector3 extent(base_radius * Math::sqrt(MAX(1.0f - r_lamp.axis.x * r_lamp.axis.x, 0.0f)),
				base_radius * Math::sqrt(MAX(1.0f - r_lamp.axis.y * r_lamp.axis.y, 0.0f)),
				base_radius * Math::sqrt(MAX(1.0f - r_lamp.axis.z * r_lamp.axis.z, 0.0f)));
		const Vector3 low(MIN(r_lamp.position.x, base.x - extent.x), MIN(r_lamp.position.y, base.y - extent.y), MIN(r_lamp.position.z, base.z - extent.z));
		const Vector3 high(MAX(r_lamp.position.x, base.x + extent.x), MAX(r_lamp.position.y, base.y + extent.y), MAX(r_lamp.position.z, base.z + extent.z));
		box = AABB(low, high - low);
	}
	const Vector3 window = Vector3(CELL_DIMS) * CELL_SIZE;
	const Vector3 lo = box.position - origin;
	const Vector3 hi = box.get_end() - origin;
	if (hi.x < 0.0f || hi.y < 0.0f || hi.z < 0.0f || lo.x > window.x || lo.y > window.y || lo.z > window.z) {
		return;
	}
	const auto clamp_cell = [](float p_value, int p_dim) { return CLAMP(int(Math::floor(p_value / CELL_SIZE)), 0, p_dim - 1); };
	const Vector3i cell_min(clamp_cell(lo.x, CELL_DIMS.x), clamp_cell(lo.y, CELL_DIMS.y), clamp_cell(lo.z, CELL_DIMS.z));
	const Vector3i cell_max(clamp_cell(hi.x, CELL_DIMS.x), clamp_cell(hi.y, CELL_DIMS.y), clamp_cell(hi.z, CELL_DIMS.z));
	const float sin_outer = Math::sqrt(MAX(1.0f - cos_outer * cos_outer, 0.0f));
	const float cell_bound = CELL_SIZE * 0.5f * Math::sqrt(3.0f);
	const float reach = reach_range + cell_bound;
	const float reach_sq = reach * reach;
	const Vector3 first_center = origin + Vector3(0.5f, 0.5f, 0.5f) * CELL_SIZE;
	const float vx_base = first_center.x - r_lamp.position.x;
	const int dx = CELL_DIMS.x;
	const int dxy = CELL_DIMS.x * CELL_DIMS.y;
	for (int cz = cell_min.z; cz <= cell_max.z; cz++) {
		const float vz = first_center.z + float(cz) * CELL_SIZE - r_lamp.position.z;
		const float d2_z = vz * vz;
		if (d2_z > reach_sq) {
			continue;
		}
		for (int cy = cell_min.y; cy <= cell_max.y; cy++) {
			const float vy = first_center.y + float(cy) * CELL_SIZE - r_lamp.position.y;
			const float d2_yz = d2_z + vy * vy;
			const float span_sq = reach_sq - d2_yz;
			if (span_sq <= 0.0f) {
				continue;
			}
			const float span = Math::sqrt(span_sq);
			const int cx_lo = MAX(int(Math::ceil((-span - vx_base) / CELL_SIZE)), cell_min.x);
			const int cx_hi = MIN(int(Math::floor((span - vx_base) / CELL_SIZE)), cell_max.x);
			const int row = cz * dxy + cy * dx;
			if (!r_lamp.spot) {
				for (int cx = cx_lo; cx <= cx_hi; cx++) {
					r_cells.push_back(row + cx);
				}
				continue;
			}
			const float dot_yz = vy * r_lamp.axis.y + vz * r_lamp.axis.z;
			for (int cx = cx_lo; cx <= cx_hi; cx++) {
				const float vx = vx_base + float(cx) * CELL_SIZE;
				const float axial = vx * r_lamp.axis.x + dot_yz;
				if (axial > reach || axial < -cell_bound) {
					continue;
				}
				const float d2 = vx * vx + d2_yz;
				const float side = cos_outer * Math::sqrt(MAX(d2 - axial * axial, 0.0f)) - axial * sin_outer;
				if (side > cell_bound) {
					continue;
				}
				r_cells.push_back(row + cx);
			}
		}
	}
}

void LocalLightsWorld::_release_map(Lamp &r_lamp) {
	if (r_lamp.map_index >= 0) {
		(r_lamp.spot ? free_spot_maps : free_omni_maps).push_back(r_lamp.map_index);
	}
	if (r_lamp.map_index >= 0 || r_lamp.map_valid) {
		r_lamp.data_dirty = true;
	}
	r_lamp.map_index = -1;
	r_lamp.map_valid = false;
}

void LocalLightsWorld::_release_lamp(uint32_t p_index) {
	Lamp &lamp = lamps[p_index];
	_release_map(lamp);
	if (lamp.in_grid) {
		repack_needed = true;
	}
	lamp_of_instance.erase(lamp.instance);
	lamp = Lamp();
	free_lamps.push_back(p_index);
}

RID LocalLightsWorld::_create_atlas(int p_size, int p_layers, RID p_previous, int p_previous_layers) {
	RD::TextureFormat format;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.format = RD::DATA_FORMAT_D16_UNORM;
	format.width = p_size;
	format.height = p_size;
	format.array_layers = p_layers;
	// Копия между текстурами требует одного аспекта, а аспект глубины RD выводит из этого флага.
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	RID atlas = rd->texture_create(format, RD::TextureView());
	if (p_previous.is_valid()) {
		for (int layer = 0; layer < p_previous_layers; layer++) {
			rd->texture_copy(p_previous, atlas, Vector3(), Vector3(), Vector3(p_size, p_size, 1), 0, 0, layer, layer);
		}
	}
	return atlas;
}

void LocalLightsWorld::_ensure_atlases(int p_omni_needed, int p_spot_needed) {
	const LocalLightsSettings &settings = LocalLights::get_singleton()->get_settings();
	if (settings.omni_face_size != omni_size || settings.spot_tile_size != spot_size) {
		// Другое разрешение: снятые карты не переносятся.
		for (Lamp &lamp : lamps) {
			if (lamp.active && lamp.map_index >= 0) {
				_release_map(lamp);
				lamp.in_grid = false;
				repack_needed = true;
				if (lamp.casts && lamp.queued_order == 0) {
					lamp.queued_order = next_queue_order++;
					lamp.queued_frame = current_frame;
				}
			}
		}
		_retire_texture(omni_atlas, omni_atlas_texture);
		_retire_texture(spot_atlas, spot_atlas_texture);
		omni_capacity = 0;
		spot_capacity = 0;
		free_omni_maps.clear();
		free_spot_maps.clear();
		omni_size = settings.omni_face_size;
		spot_size = settings.spot_tile_size;
	}
	const auto grow = [&](int p_needed, int p_size, int p_layers_per_map, int &r_capacity, RID &r_atlas, RID &r_texture, LocalVector<int> &r_free) {
		int in_use = r_capacity - int(r_free.size());
		if (in_use + p_needed <= r_capacity) {
			return;
		}
		int capacity = in_use + p_needed;
		capacity = ((capacity + MAP_CAPACITY_STEP - 1) / MAP_CAPACITY_STEP) * MAP_CAPACITY_STEP;
		RID atlas = _create_atlas(p_size, capacity * p_layers_per_map, r_atlas, r_capacity * p_layers_per_map);
		_retire_texture(r_atlas, r_texture);
		r_atlas = atlas;
		r_texture = _wrap_texture(atlas, RSE::TEXTURE_LAYERED_2D_ARRAY);
		for (int map = capacity - 1; map >= r_capacity; map--) {
			r_free.push_back(map);
		}
		r_capacity = capacity;
		globals_published = false;
	};
	grow(p_omni_needed, omni_size, 6, omni_capacity, omni_atlas, omni_atlas_texture, free_omni_maps);
	grow(p_spot_needed, spot_size, 1, spot_capacity, spot_atlas, spot_atlas_texture, free_spot_maps);
}

LocalLightsWorld::DepthStrip &LocalLightsWorld::_strip_for(int p_size) {
	DepthStrip *existing = depth_strips.getptr(p_size);
	if (existing) {
		return *existing;
	}
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_D16_UNORM;
	format.width = p_size;
	format.height = p_size * BATCH_TILES;
	format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	DepthStrip strip;
	strip.depth = rd->texture_create(format, RD::TextureView());
	Vector<RID> attachments;
	attachments.push_back(strip.depth);
	strip.framebuffer = rd->framebuffer_create(attachments);
	depth_strips.insert(p_size, strip);
	return *depth_strips.getptr(p_size);
}

RID LocalLightsWorld::_pipeline_for(RD::VertexFormatID p_format) {
	const RID *existing = caster_pipelines.getptr(p_format);
	if (existing) {
		return *existing;
	}
	RD::PipelineRasterizationState raster;
	raster.cull_mode = RD::POLYGON_CULL_DISABLED;
	RD::PipelineDepthStencilState depth;
	depth.enable_depth_test = true;
	depth.enable_depth_write = true;
	depth.depth_compare_operator = RD::COMPARE_OP_LESS;
	RD::PipelineColorBlendState blend;
	const RID framebuffer = _strip_for(omni_size).framebuffer;
	RID pipeline = rd->render_pipeline_create(caster_rd_shader, rd->framebuffer_get_format(framebuffer), p_format, RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), depth, blend);
	caster_pipelines.insert(p_format, pipeline);
	return pipeline;
}

void LocalLightsWorld::_gather_casters(Lamp &r_lamp, const Instance *p_instance) {
	const LocalLightsSettings &settings = LocalLights::get_singleton()->get_settings();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	r_lamp.caster_first = caster_pool.size();
	const InstanceLightData *light = static_cast<const InstanceLightData *>(p_instance->base_data);
	const uint64_t position_mask = 1ull << RSE::ARRAY_VERTEX;
	const float range_sq = r_lamp.range * r_lamp.range;
	for (const Instance *geometry : light->geometries) {
		if (geometry->base_type != RSE::INSTANCE_MESH || geometry->cast_shadows == RSE::SHADOW_CASTING_SETTING_OFF || !(geometry->layer_mask & r_lamp.caster_mask)) {
			continue;
		}
		const uint32_t surface_count = mesh_storage->mesh_get_surface_count(geometry->base);
		if (geometry->mesh_instance.is_valid()) {
			mesh_storage->mesh_instance_check_for_update(geometry->mesh_instance);
		}
		for (uint32_t index = 0; index < surface_count; index++) {
			void *surface = mesh_storage->mesh_get_surface(geometry->base, index);
			if (mesh_storage->mesh_surface_get_primitive(surface) != RSE::PRIMITIVE_TRIANGLES) {
				continue;
			}
			RID material = geometry->material_override;
			if (material.is_null()) {
				material = index < uint32_t(geometry->materials.size()) && geometry->materials[index].is_valid() ? geometry->materials[index] : mesh_storage->mesh_surface_get_material(geometry->base, index);
			}
			if (material.is_valid() && !material_casts_in_maps(material)) {
				continue;
			}
			const AABB local_bounds = mesh_storage->mesh_surface_get_aabb(surface);
			const AABB bounds = geometry->transform.xform(local_bounds);
			if (settings.caster_culling) {
				const Vector3 nearest = r_lamp.position.clamp(bounds.position, bounds.get_end());
				if (nearest.distance_squared_to(r_lamp.position) > range_sq) {
					frame_metrics.surfaces_culled++;
					continue;
				}
			}
			CasterSurface caster;
			caster.instance = geometry;
			caster.bounds = bounds;
			if (geometry->mesh_instance.is_valid()) {
				mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(geometry->mesh_instance, index, position_mask, false, false, caster.vertex_array, caster.format);
			} else {
				mesh_storage->mesh_surface_get_vertex_arrays_and_format(surface, position_mask, false, false, caster.vertex_array, caster.format);
			}
			caster.index_array = mesh_storage->mesh_surface_get_index_array(surface, 0);
			const bool compressed = mesh_storage->mesh_surface_get_format(surface) & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
			caster.decode_position = compressed ? local_bounds.position : Vector3();
			caster.decode_size = compressed ? local_bounds.size : Vector3(1, 1, 1);
			caster.triangles = mesh_storage->mesh_surface_get_vertices_drawn_count(surface) / 3;
			caster_pool.push_back(caster);
		}
	}
	r_lamp.caster_count = caster_pool.size() - r_lamp.caster_first;
}

void LocalLightsWorld::_serve_queue(uint64_t p_frame) {
	const LocalLightsSettings &settings = LocalLights::get_singleton()->get_settings();
	serve_order.clear();
	for (uint32_t i = 0; i < lamps.size(); i++) {
		const Lamp &lamp = lamps[i];
		if (lamp.active && lamp.queued_order != 0 && lamp.last_visible_frame == p_frame) {
			serve_order.push_back(i);
		}
	}
	frame_metrics.queue = 0;
	if (serve_order.is_empty()) {
		return;
	}
	struct ByOrder {
		const LocalVector<Lamp> *lamps;
		bool operator()(uint32_t a, uint32_t b) const { return (*lamps)[a].queued_order < (*lamps)[b].queued_order; }
	};
	SortArray<uint32_t, ByOrder> sorter;
	sorter.compare.lamps = &lamps;
	sorter.sort(serve_order.ptr(), serve_order.size());
	const uint32_t budget = settings.update_cap > 0 ? MIN(uint32_t(settings.update_cap), serve_order.size()) : serve_order.size();

	uint64_t started = OS::get_singleton()->get_ticks_usec();
	int omni_new = 0;
	int spot_new = 0;
	for (uint32_t i = 0; i < budget; i++) {
		const Lamp &lamp = lamps[serve_order[i]];
		if (lamp.map_index < 0) {
			(lamp.spot ? spot_new : omni_new)++;
		}
	}
	_ensure_atlases(omni_new, spot_new);
	caster_pool.clear();
	tiles.clear();
	LocalVector<Tile> spot_tiles;
	for (uint32_t i = 0; i < budget; i++) {
		Lamp &lamp = lamps[serve_order[i]];
		const Instance *instance = scene_cull()->instance_owner.get_or_null(lamp.instance);
		ERR_CONTINUE(!instance);
		LocalVector<int> &free_maps = lamp.spot ? free_spot_maps : free_omni_maps;
		if (lamp.map_index < 0) {
			lamp.map_index = free_maps[free_maps.size() - 1];
			free_maps.resize(free_maps.size() - 1);
		}
		_gather_casters(lamp, instance);
		if (lamp.spot) {
			Tile tile;
			tile.lamp = serve_order[i];
			tile.layer = lamp.map_index;
			spot_tiles.push_back(tile);
		} else {
			for (int face = 0; face < 6; face++) {
				Tile tile;
				tile.lamp = serve_order[i];
				tile.face = face;
				tile.layer = lamp.map_index * 6 + face;
				tiles.push_back(tile);
			}
		}
		frame_metrics.served_wait_frames = MAX(frame_metrics.served_wait_frames, uint32_t(p_frame - lamp.queued_frame));
	}
	RendererRD::MeshStorage::get_singleton()->update_mesh_instances();
	frame_metrics.gather_usec += OS::get_singleton()->get_ticks_usec() - started;

	started = OS::get_singleton()->get_ticks_usec();
	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights maps");
	}
	if (!tiles.is_empty()) {
		_record_tiles(tiles, omni_size, settings);
	}
	if (!spot_tiles.is_empty()) {
		_record_tiles(spot_tiles, spot_size, settings);
	}
	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights maps end");
	}
	frame_metrics.record_usec += OS::get_singleton()->get_ticks_usec() - started;

	for (uint32_t i = 0; i < budget; i++) {
		Lamp &lamp = lamps[serve_order[i]];
		lamp.queued_order = 0;
		lamp.map_valid = true;
		lamp.baked_origin = lamp.position;
		lamp.baked_inverse_range = 1.0f / lamp.range;
		if (lamp.spot) {
			const Vector3 helper = Math::abs(lamp.axis.y) < 0.999f ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
			const Vector3 right = helper.cross(lamp.axis).normalized();
			lamp.baked_forward = lamp.axis;
			lamp.baked_up = lamp.axis.cross(right);
			const float half_angle = Math::deg_to_rad(MIN(lamp.angle + SPOT_MAP_MARGIN_DEGREES, SPOT_MAP_MAX_HALF_ANGLE_DEGREES));
			lamp.baked_inverse_tangent = 1.0f / Math::tan(half_angle);
		}
		lamp.data_dirty = true;
		if (!lamp.in_grid) {
			repack_needed = true;
		}
	}
	frame_metrics.lamps_served += budget;
	uint32_t oldest = 0;
	for (uint32_t i = budget; i < serve_order.size(); i++) {
		oldest = MAX(oldest, uint32_t(p_frame - lamps[serve_order[i]].queued_frame));
	}
	frame_metrics.oldest_wait_frames = MAX(frame_metrics.oldest_wait_frames, oldest);
	frame_metrics.queue = serve_order.size() - budget;
}

void LocalLightsWorld::_record_tiles(const LocalVector<Tile> &p_tiles, int p_size, const LocalLightsSettings &p_settings) {
	const DepthStrip &strip = _strip_for(p_size);
	const float guard_extent = 1.0f + CUBE_GUARD_TEXELS / float(p_size);
	Vector3 face_normals[6][4];
	for (int face = 0; face < 6; face++) {
		pyramid_normals(FACE_RIGHT[face], FACE_UP[face], FACE_FORWARD[face], guard_extent, face_normals[face]);
	}
	for (uint32_t first = 0; first < p_tiles.size(); first += BATCH_TILES) {
		const uint32_t count = MIN(uint32_t(BATCH_TILES), p_tiles.size() - first);
		RD::DrawListID list = rd->draw_list_begin(strip.framebuffer, RD::DRAW_CLEAR_DEPTH, VectorView<Color>(), 1.0f, 0, Rect2(0, 0, p_size, count * p_size));
		RD::VertexFormatID bound_format = 0;
		for (uint32_t row = 0; row < count; row++) {
			const Tile &tile = p_tiles[first + row];
			const Lamp &lamp = lamps[tile.lamp];
			rd->draw_list_set_viewport(list, Rect2(0, row * p_size, p_size, p_size));
			rd->draw_list_disable_scissor(list);
			Vector3 right, up, forward;
			float extent;
			Vector3 normals[4];
			if (lamp.spot) {
				forward = lamp.axis;
				const Vector3 helper = Math::abs(forward.y) < 0.999f ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
				right = helper.cross(forward).normalized();
				up = forward.cross(right);
				extent = Math::tan(Math::deg_to_rad(MIN(lamp.angle + SPOT_MAP_MARGIN_DEGREES, SPOT_MAP_MAX_HALF_ANGLE_DEGREES)));
				// Лучи вне конуса не читаются: кастер отсекается по конусу с небольшим запасом, плитка шире.
				const float cone_extent = 1.0f / Math::tan(Math::deg_to_rad(MIN(lamp.angle + 1.0f, SPOT_MAP_MAX_HALF_ANGLE_DEGREES)));
				pyramid_normals(right, up, forward, 1.0f / cone_extent, normals);
			} else {
				right = FACE_RIGHT[tile.face];
				up = FACE_UP[tile.face];
				forward = FACE_FORWARD[tile.face];
				extent = guard_extent;
				for (int i = 0; i < 4; i++) {
					normals[i] = face_normals[tile.face][i];
				}
			}
			const Basis rows = rows_of(right, up, forward);
			const Transform3D light_from_world(rows, rows.xform(-lamp.position));
			const float inverse_range = 1.0f / lamp.range;
			CasterPush push;
			push.projection[0] = extent;
			push.projection[1] = extent;
			push.projection[2] = 0.0f;
			push.projection[3] = 0.0f;
			push.extra[0] = lamp.range / (lamp.range - NEAR_PLANE);
			push.extra[1] = -NEAR_PLANE * lamp.range / (lamp.range - NEAR_PLANE);
			push.extra[2] = inverse_range;
			push.extra[3] = 0.0f;
			push.decode_position[3] = 0.0f;
			push.decode_size[3] = 0.0f;
			for (uint32_t i = 0; i < lamp.caster_count; i++) {
				const CasterSurface &caster = caster_pool[lamp.caster_first + i];
				if (p_settings.caster_culling) {
					const Vector3 half = caster.bounds.size * 0.5f;
					const Vector3 center = caster.bounds.position + half - lamp.position;
					if (box_outside_planes(normals, 4, center, half)) {
						frame_metrics.surfaces_culled++;
						continue;
					}
				}
				if (caster.format != bound_format) {
					rd->draw_list_bind_render_pipeline(list, _pipeline_for(caster.format));
					bound_format = caster.format;
				}
				rd->draw_list_bind_vertex_array(list, caster.vertex_array);
				if (caster.index_array.is_valid()) {
					rd->draw_list_bind_index_array(list, caster.index_array);
				}
				RendererRD::MaterialStorage::store_transform(light_from_world * caster.instance->transform, push.to_light);
				for (int axis = 0; axis < 3; axis++) {
					push.decode_position[axis] = caster.decode_position[axis];
					push.decode_size[axis] = caster.decode_size[axis];
				}
				rd->draw_list_set_push_constant(list, &push, sizeof(CasterPush));
				rd->draw_list_draw(list, caster.index_array.is_valid(), 1);
				frame_metrics.surfaces_drawn++;
				frame_metrics.triangles_drawn += caster.triangles;
			}
			frame_metrics.faces_baked++;
		}
		rd->draw_list_end();
		for (uint32_t row = 0; row < count; row++) {
			const Tile &tile = p_tiles[first + row];
			RID atlas = lamps[tile.lamp].spot ? spot_atlas : omni_atlas;
			rd->texture_copy(strip.depth, atlas, Vector3(0, row * p_size, 0), Vector3(), Vector3(p_size, p_size, 1), 0, 0, 0, tile.layer);
		}
	}
}

void LocalLightsWorld::_update_grid() {
	for (Lamp &lamp : lamps) {
		if (!lamp.active) {
			continue;
		}
		const bool wanted = lamp.last_visible_frame + 1 >= current_frame && (!lamp.casts || lamp.map_valid) && !lamp.cells.is_empty();
		if (wanted != lamp.in_grid) {
			lamp.in_grid = wanted;
			repack_needed = true;
		}
	}
	if (repack_needed) {
		_repack();
	}
	uint32_t in_grid = 0;
	for (Lamp &lamp : lamps) {
		if (!lamp.active || !lamp.in_grid) {
			continue;
		}
		in_grid++;
		if (lamp.data_dirty) {
			_write_lamp_data(lamp);
			lamp.data_dirty = false;
			data_changed = true;
		}
	}
	frame_metrics.lamps_in_grid = in_grid;
	if (grid_changed) {
		rd->texture_update(grid_rd, 0, grid_bytes);
		grid_changed = false;
		frame_metrics.grid_uploads++;
		frame_metrics.uploaded_bytes += grid_bytes.size();
	}
	if (data_changed && data_rd.is_valid()) {
		rd->texture_update(data_rd, 0, data_bytes);
		data_changed = false;
		frame_metrics.data_uploads++;
		frame_metrics.uploaded_bytes += data_bytes.size();
	}
}

// Блоки ячеек: сначала Omni, затем Spot. Данные лампы копируются в её слот каждой ячейки со смещением от центра ячейки.
void LocalLightsWorld::_repack() {
	repack_needed = false;
	frame_metrics.repacks++;
	for (uint32_t cell : filled_cells) {
		cell_omni[cell] = 0;
		cell_spot[cell] = 0;
	}
	filled_cells.clear();
	float *grid = reinterpret_cast<float *>(grid_bytes.ptrw());
	memset(grid, 0, grid_bytes.size());
	for (const Lamp &lamp : lamps) {
		if (!lamp.active || !lamp.in_grid) {
			continue;
		}
		for (uint32_t cell : lamp.cells) {
			if (cell_omni[cell] + cell_spot[cell] == 0) {
				filled_cells.push_back(cell);
			}
			(lamp.spot ? cell_spot : cell_omni)[cell]++;
		}
	}
	uint32_t list_end = 0;
	for (uint32_t cell : filled_cells) {
		grid[cell * 4] = float(list_end);
		grid[cell * 4 + 1] = float(cell_omni[cell]);
		grid[cell * 4 + 2] = float(cell_spot[cell]);
		cell_next[cell] = list_end;
		list_end += cell_omni[cell] + cell_spot[cell];
	}
	// Omni занимают начало блока: первый проход их, второй Spot.
	for (int pass = 0; pass < 2; pass++) {
		for (Lamp &lamp : lamps) {
			if (!lamp.active || !lamp.in_grid || lamp.spot != (pass == 1)) {
				continue;
			}
			lamp.cell_slots.resize(lamp.cells.size());
			for (uint32_t i = 0; i < lamp.cells.size(); i++) {
				lamp.cell_slots[i] = cell_next[lamp.cells[i]]++;
			}
			lamp.data_dirty = true;
		}
	}
	const uint32_t needed_rows = MAX(1u, (list_end * SLOT_TEXELS + DATA_WIDTH - 1) / DATA_WIDTH);
	if (needed_rows > data_rows || !data_rd.is_valid()) {
		data_rows = Math::next_power_of_2(needed_rows);
		_retire_texture(data_rd, data_texture);
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		format.width = DATA_WIDTH;
		format.height = data_rows;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		data_rd = rd->texture_create(format, RD::TextureView());
		data_texture = _wrap_texture(data_rd, RSE::TEXTURE_LAYERED_2D_ARRAY);
		data_bytes.resize(DATA_WIDTH * data_rows * BYTES_PER_DATA_TEXEL);
		memset(data_bytes.ptrw(), 0, data_bytes.size());
		globals_published = false;
	}
	grid_changed = true;
	data_changed = true;
}

void LocalLightsWorld::_write_lamp_data(const Lamp &p_lamp) {
	float values[SLOT_FLOATS] = {};
	// Половинная точность отбрасывает лишние биты: ошибка одинарного округления у значения на границе шага сдвигала бы край охвата на целый шаг.
	values[3] = float(1.0 / (double(p_lamp.range) * double(p_lamp.range)));
	values[8] = p_lamp.linear_color.r * p_lamp.energy;
	values[9] = p_lamp.linear_color.g * p_lamp.energy;
	values[10] = p_lamp.linear_color.b * p_lamp.energy;
	values[11] = p_lamp.decay;
	values[14] = p_lamp.cone_attenuation;
	values[15] = p_lamp.specular_amount;
	if (p_lamp.spot) {
		const float outer = Math::deg_to_rad(MIN(p_lamp.angle, 89.0f));
		const float cos_outer = Math::cos(outer);
		const float cos_inner = Math::cos(outer * SPOT_HOTSPOT);
		values[4] = p_lamp.axis.x;
		values[5] = p_lamp.axis.y;
		values[6] = p_lamp.axis.z;
		values[7] = cos_outer * cos_outer;
		values[12] = cos_outer;
		values[13] = 1.0f / MAX(cos_inner - cos_outer, 1e-4f);
	}
	values[27] = -1.0f;
	if (p_lamp.casts && p_lamp.map_valid) {
		values[16] = p_lamp.shadow_bias;
		values[17] = p_lamp.shadow_normal_bias;
		values[18] = p_lamp.spot ? float(p_lamp.map_index) : 0.0f;
		values[19] = p_lamp.baked_inverse_range;
		values[20] = p_lamp.spot ? p_lamp.baked_forward.x : 0.0f;
		values[21] = p_lamp.spot ? p_lamp.baked_forward.y : 0.0f;
		values[22] = p_lamp.spot ? p_lamp.baked_forward.z : 1.0f;
		values[23] = p_lamp.spot ? p_lamp.baked_inverse_tangent : 0.0f;
		const Vector3 delta = p_lamp.position - p_lamp.baked_origin;
		values[24] = delta.x;
		values[25] = delta.y;
		values[26] = delta.z;
		values[27] = float(p_lamp.spot ? p_lamp.map_index : p_lamp.map_index * 6);
		values[28] = p_lamp.spot ? p_lamp.baked_up.x : 0.0f;
		values[29] = p_lamp.spot ? p_lamp.baked_up.y : 1.0f;
		values[30] = p_lamp.spot ? p_lamp.baked_up.z : 0.0f;
	}
	uint16_t halves[SLOT_FLOATS];
	for (uint32_t i = 3; i < SLOT_FLOATS; i++) {
		halves[i] = Math::make_half_float(values[i]);
	}
	uint8_t *data = data_bytes.ptrw();
	const int dx = CELL_DIMS.x;
	const int dy = CELL_DIMS.y;
	for (uint32_t i = 0; i < p_lamp.cells.size(); i++) {
		const uint32_t cell = p_lamp.cells[i];
		const int cx = cell % dx;
		const int cy = (cell / dx) % dy;
		const int cz = cell / (dx * dy);
		// Центр ячейки считается от целых номеров: по той же причине, что и 1/r², смещение на границе шага не должно зависеть от положения окна.
		halves[0] = Math::make_half_float(float(double(p_lamp.position.x) - (double(origin_cell.x + cx) + 0.5) * CELL_SIZE_EXACT));
		halves[1] = Math::make_half_float(float(double(p_lamp.position.y) - (double(origin_cell.y + cy) + 0.5) * CELL_SIZE_EXACT));
		halves[2] = Math::make_half_float(float(double(p_lamp.position.z) - (double(origin_cell.z + cz) + 0.5) * CELL_SIZE_EXACT));
		memcpy(data + uint64_t(p_lamp.cell_slots[i]) * SLOT_TEXELS * BYTES_PER_DATA_TEXEL, halves, sizeof(halves));
	}
}

void LocalLightsWorld::_publish_globals() {
	if (globals_published) {
		return;
	}
	globals_published = true;
	RendererRD::MaterialStorage *materials = RendererRD::MaterialStorage::get_singleton();
	materials->global_shader_parameter_set("local_light_origin", origin);
	materials->global_shader_parameter_set("local_light_cell_size", CELL_SIZE);
	materials->global_shader_parameter_set("local_light_inverse_cell_size", 1.0f / CELL_SIZE);
	materials->global_shader_parameter_set("local_light_dims", CELL_DIMS);
	materials->global_shader_parameter_set("local_light_grid", grid_texture);
	if (data_texture.is_valid()) {
		materials->global_shader_parameter_set("local_light_data", data_texture);
	}
	if (omni_atlas_texture.is_valid()) {
		materials->global_shader_parameter_set("local_light_omni_atlas", omni_atlas_texture);
	}
	if (spot_atlas_texture.is_valid()) {
		materials->global_shader_parameter_set("local_light_spot_atlas", spot_atlas_texture);
	}
	if (blurred_ratio_texture.is_valid()) {
		materials->global_shader_parameter_set("local_light_shadow_ratio", blurred_ratio_texture);
	}
}

void LocalLightsWorld::_ensure_screen_textures(const Size2i &p_size) {
	if (p_size.x <= screen_size.x && p_size.y <= screen_size.y && blurred_ratio.is_valid()) {
		return;
	}
	screen_size = Size2i(MAX(p_size.x, screen_size.x), MAX(p_size.y, screen_size.y));
	_retire_texture(blurred_ratio, blurred_ratio_texture);
	RID no_wrapper;
	_retire_texture(blur_radius, no_wrapper);
	_retire_texture(ratio_and_normal, no_wrapper);
	RD::TextureFormat format;
	format.width = screen_size.x;
	format.height = screen_size.y;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	format.format = RD::DATA_FORMAT_R16_SFLOAT;
	blur_radius = rd->texture_create(format, RD::TextureView());
	format.format = RD::DATA_FORMAT_R16G16B16A16_UNORM;
	ratio_and_normal = rd->texture_create(format, RD::TextureView());
	format.format = RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32;
	format.usage_bits |= RD::TEXTURE_USAGE_SAMPLING_BIT;
	blurred_ratio = rd->texture_create(format, RD::TextureView());
	blurred_ratio_texture = _wrap_texture(blurred_ratio, RSE::TEXTURE_LAYERED_2D_ARRAY);
	RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set("local_light_shadow_ratio", blurred_ratio_texture);
}

void LocalLightsWorld::render_screen_passes(RenderDataRD *p_render_data) {
	if (frame_metrics.lamps_in_grid == 0) {
		return;
	}
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	const LocalLightsSettings &settings = LocalLights::get_singleton()->get_settings();
	Ref<RenderSceneBuffersRD> buffers = p_render_data->render_buffers;
	ERR_FAIL_COND(buffers.is_null());
	const Size2i size = buffers->get_internal_size();
	_ensure_screen_textures(size);

	RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const Projection projection = scene_data->get_view_projection(0);
	ScreenParams params;
	RendererRD::MaterialStorage::store_camera(projection.inverse(), params.inverse_projection);
	RendererRD::MaterialStorage::store_camera(projection, params.projection);
	RendererRD::MaterialStorage::store_transform(scene_data->get_cam_transform(), params.view_to_world);
	params.origin_and_step[0] = origin.x;
	params.origin_and_step[1] = origin.y;
	params.origin_and_step[2] = origin.z;
	params.origin_and_step[3] = CELL_SIZE;
	params.dims[0] = CELL_DIMS.x;
	params.dims[1] = CELL_DIMS.y;
	params.dims[2] = CELL_DIMS.z;
	params.dims[3] = 0;
	params.blur[0] = settings.softness_per_meter;
	params.blur[1] = float(settings.fixed_noise_frame >= 0 ? uint64_t(settings.fixed_noise_frame) : Engine::get_singleton()->get_frames_drawn() % NOISE_FRAME_PERIOD);
	params.blur[2] = settings.several_lamps_growth;
	params.blur[3] = settings.show_hard_mask ? 1.0f : 0.0f;
	params.view_size[0] = size.x;
	params.view_size[1] = size.y;
	params.view_size[2] = 0;
	params.view_size[3] = 0;
	rd->buffer_update(params_buffer, 0, sizeof(ScreenParams), &params);

	RendererRD::MaterialStorage *materials = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *textures = RendererRD::TextureStorage::get_singleton();
	const RID sampler = materials->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID empty_array = textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);
	const RID depth = buffers->get_depth_texture(0);
	const Vector2i groups((size.x + 7) / 8, (size.y + 7) / 8);
	UniformSetCacheRD *uniform_sets = UniformSetCacheRD::get_singleton();

	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights ratio");
	}
	RID ratio_set = uniform_sets->get_cache(ratio_rd_shader, 0, sampled_uniform(0, sampler, depth), sampled_uniform(1, sampler, grid_rd), sampled_uniform(2, sampler, data_rd),
			sampled_uniform(3, sampler, omni_atlas.is_valid() ? omni_atlas : empty_array), image_uniform(4, blur_radius), buffer_uniform(5, params_buffer),
			sampled_uniform(6, sampler, spot_atlas.is_valid() ? spot_atlas : empty_array), image_uniform(7, ratio_and_normal));
	RD::ComputeListID compute = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute, ratio_pipeline);
	rd->compute_list_bind_uniform_set(compute, ratio_set, 0);
	rd->compute_list_dispatch(compute, groups.x, groups.y, 1);
	rd->compute_list_end();

	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights blur");
	}
	RID blur_set = uniform_sets->get_cache(blur_rd_shader, 0, sampled_uniform(0, sampler, depth), image_uniform(1, blur_radius), image_uniform(2, blurred_ratio), buffer_uniform(3, params_buffer), image_uniform(4, ratio_and_normal));
	compute = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute, blur_pipeline);
	rd->compute_list_bind_uniform_set(compute, blur_set, 0);
	rd->compute_list_dispatch(compute, groups.x, groups.y, 1);
	rd->compute_list_end();
	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights screen end");
	}
	frame_metrics.screen_usec += OS::get_singleton()->get_ticks_usec() - started;
}

Dictionary LocalLightsWorld::memory_report() const {
	Dictionary report;
	report["omniAtlasBytes"] = uint64_t(omni_capacity) * 6 * omni_size * omni_size * 2;
	report["spotAtlasBytes"] = uint64_t(spot_capacity) * spot_size * spot_size * 2;
	uint64_t strips = 0;
	for (const KeyValue<int, DepthStrip> &entry : depth_strips) {
		strips += uint64_t(entry.key) * entry.key * BATCH_TILES * 2;
	}
	report["stripBytes"] = strips;
	report["screenBytes"] = uint64_t(screen_size.x) * screen_size.y * (2 + 8 + 4);
	report["gridBytes"] = uint64_t(grid_bytes.size());
	report["dataBytes"] = uint64_t(data_bytes.size());
	report["omniCapacity"] = omni_capacity;
	report["spotCapacity"] = spot_capacity;
	uint64_t cpu = uint64_t(grid_bytes.size()) + uint64_t(data_bytes.size());
	cpu += lamps.get_capacity() * sizeof(Lamp);
	for (const Lamp &lamp : lamps) {
		cpu += (lamp.cells.get_capacity() + lamp.cell_slots.get_capacity()) * sizeof(uint32_t);
	}
	cpu += (cell_omni.get_capacity() + cell_spot.get_capacity() + cell_next.get_capacity() + filled_cells.get_capacity() + serve_order.get_capacity()) * sizeof(uint32_t);
	cpu += caster_pool.get_capacity() * sizeof(CasterSurface) + tiles.get_capacity() * sizeof(Tile);
	cpu += (free_omni_maps.get_capacity() + free_spot_maps.get_capacity()) * sizeof(int);
	report["cpuBytes"] = cpu;
	return report;
}

Dictionary LocalLightsWorld::read_screen_buffers() const {
	Dictionary result;
	result["size"] = screen_size;
	if (blurred_ratio.is_valid()) {
		result["hard"] = rd->texture_get_data(ratio_and_normal, 0);
		result["surface"] = rd->texture_get_data(blur_radius, 0);
		result["blurred"] = rd->texture_get_data(blurred_ratio, 0);
	}
	return result;
}
