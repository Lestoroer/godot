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
const float CELL_SIZE = 1.6f;
const float RECENTER_CELLS = 4.0f;
// Таблица ламп: восемь текселей на лампу, в строке 256 ламп. Список ламп ячеек: индекс лампы в текселе. Контракт с local_light_ratio.glsl и local_lights_inc.gdshaderinc.
const uint32_t DATA_WIDTH = 2048;
const uint32_t SLOT_TEXELS = 8;
const uint32_t SLOT_FLOATS = SLOT_TEXELS * 4;
const uint32_t LAMPS_PER_DATA_ROW = DATA_WIDTH / SLOT_TEXELS;
const uint32_t BYTES_PER_DATA_TEXEL = 16;
const uint32_t LIST_WIDTH = 2048;
const int ATLAS_WIDTH = 8192;
const int ATLAS_HEIGHT_STEP = 512;
const int ATLAS_MAX_HEIGHT = 8192;
const int MAP_SIDES[] = { 64, 96, 128, 192, 256, 384, 512 };
// Снимок спота шире конуса: выборка у края конуса не выходит за плитку.
const float SPOT_MAP_MARGIN_DEGREES = 8.0f;
const float SPOT_MAP_MAX_HALF_ANGLE_DEGREES = 75.0f;
// Запас грани куба за её ребром, тексели в сумме, по половине с каждой стороны: gather 2×2 не выходит за плитку. Контракт с local_light_ratio.glsl.
const float CUBE_GUARD_TEXELS = 4.0f;
const uint32_t NOISE_FRAME_PERIOD = 1024;

// Оси граней куба. Контракт с выбором грани в local_light_ratio.glsl и local_lights_inc.gdshaderinc.
const Vector3 FACE_RIGHT[6] = { Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 0) };
const Vector3 FACE_UP[6] = { Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) };
const Vector3 FACE_FORWARD[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(0, 1, 0) };

RendererSceneCull *scene_cull() {
	return static_cast<RendererSceneCull *>(RSG::scene);
}

typedef RendererSceneRenderImplementation::SceneShaderForwardClustered SceneShader;

const SceneShader::ShaderData *spatial_shader_data(RID p_material) {
	const SceneShader::MaterialData *data = static_cast<const SceneShader::MaterialData *>(RendererRD::MaterialStorage::get_singleton()->material_get_data(p_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
	return data ? data->shader_data : nullptr;
}

// Значение параметра материала, а если он его не задаёт, значение по умолчанию из шейдера. Имена параметров — граница с материалом.
Variant material_value(RID p_material, const SceneShader::ShaderData *p_shader_data, const StringName &p_name) {
	const Variant value = RSG::material_storage->material_get_param(p_material, p_name);
	return value.get_type() != Variant::NIL ? value : p_shader_data->get_default_parameter(p_name);
}

// Первое имя — мастер-материал, второе — BaseMaterial3D.
Variant material_value(RID p_material, const SceneShader::ShaderData *p_shader_data, const StringName &p_master_name, const StringName &p_base_name) {
	const Variant value = material_value(p_material, p_shader_data, p_master_name);
	return value.get_type() != Variant::NIL ? value : material_value(p_material, p_shader_data, p_base_name);
}

// Ближняя плоскость карты: как можно ближе к лампе, но шаг 16 бит на дальнем конце не больше 12,5 мм: половина шага входит в запас сравнения.
float map_near_plane(float p_range) {
	return MAX(MIN(p_range / 64.0f, 0.03f), p_range * p_range / (65536.0f * 0.0125f));
}

struct SpotMapAxes {
	Vector3 right;
	Vector3 up;
	// Тангенс половины угла снимка.
	float extent = 0.0f;
};

SpotMapAxes spot_map_axes(const Vector3 &p_forward, float p_angle) {
	SpotMapAxes axes;
	const Vector3 helper = Math::abs(p_forward.y) < 0.999f ? Vector3(0, 1, 0) : Vector3(0, 0, 1);
	axes.right = helper.cross(p_forward).normalized();
	axes.up = p_forward.cross(axes.right);
	axes.extent = Math::tan(Math::deg_to_rad(MIN(p_angle + SPOT_MAP_MARGIN_DEGREES, SPOT_MAP_MAX_HALF_ANGLE_DEGREES)));
	return axes;
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
	Vector<String> caster_modes;
	caster_modes.push_back("");
	caster_modes.push_back("\n#define MAP_CUTOUT\n");
	caster_shader.initialize(caster_modes);
	caster_shader_version = caster_shader.version_create();
	caster_rd_shader = caster_shader.version_get_shader(caster_shader_version, 0);
	cutout_rd_shader = caster_shader.version_get_shader(caster_shader_version, 1);
	Vector<String> modes;
	modes.push_back("");
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
	_free_wrapped(list_texture);
	_free_wrapped(map_atlas_texture);
	_free_wrapped(blurred_ratio_texture);
	for (const KeyValue<RD::VertexFormatID, RID> &entry : caster_pipelines) {
		rd->free_rid(entry.value);
	}
	for (const KeyValue<RD::VertexFormatID, RID> &entry : cutout_pipelines) {
		rd->free_rid(entry.value);
	}
	for (const RID &rid : { map_atlas, grid_rd, data_rd, list_rd, ratio_surface, blurred_ratio, params_buffer, ratio_pipeline, blur_pipeline }) {
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
	if (bake_all_lamps) {
		_collect_scenario_lamps();
		_take_view_lamps(scenario_lamps, frame);
		scenario_lamps.clear();
	}
	frame_metrics.track_usec += OS::get_singleton()->get_ticks_usec() - started;
	_serve_queue(frame);
	bake_all_lamps = false;
	started = OS::get_singleton()->get_ticks_usec();
	_update_grid();
	_publish_globals();
	frame_metrics.grid_usec += OS::get_singleton()->get_ticks_usec() - started;
}

void LocalLightsWorld::_collect_scenario_lamps() {
	RendererSceneCull::Scenario *scenario_data = scene_cull()->scenario_owner.get_or_null(scenario);
	ERR_FAIL_NULL(scenario_data);
	for (SelfList<Instance> *element = scenario_data->instances.first(); element; element = element->next()) {
		Instance *instance = element->self();
		if (instance->base_type == RSE::INSTANCE_LIGHT && instance->visible && RSG::light_storage->light_get_type(instance->base) != RSE::LIGHT_DIRECTIONAL) {
			scenario_lamps.push_back(instance);
		}
	}
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
	origin = (corner / CELL_SIZE).floor() * CELL_SIZE;
	window_center = origin + Vector3(CELL_DIMS) * 0.5f * CELL_SIZE;
	origin_valid = true;
	globals_published = false;
	for (Lamp &lamp : lamps) {
		if (lamp.active) {
			_compute_cells(lamp, lamp.cells);
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

void LocalLightsWorld::_free_map_place(const Lamp &p_lamp) {
	const uint32_t rect_class = uint32_t(p_lamp.map_side) * 8 + uint32_t(p_lamp.map_faces);
	if (!free_map_rects.has(rect_class)) {
		free_map_rects.insert(rect_class, LocalVector<Vector2i>());
	}
	free_map_rects[rect_class].push_back(p_lamp.map_origin);
}

void LocalLightsWorld::_release_map(Lamp &r_lamp) {
	if (r_lamp.map_faces > 0) {
		_free_map_place(r_lamp);
	}
	if (r_lamp.map_faces > 0 || r_lamp.map_valid) {
		r_lamp.data_dirty = true;
	}
	r_lamp.map_side = 0;
	r_lamp.map_faces = 0;
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

void LocalLightsWorld::_apply_map_sizes(const LocalLightsSettings &p_settings) {
	if (p_settings.omni_face_size == omni_size && p_settings.spot_tile_size == spot_size && p_settings.map_texel_size == map_texel_size) {
		return;
	}
	// Другое разрешение: снятые карты не переносятся.
	for (Lamp &lamp : lamps) {
		if (lamp.active && lamp.map_faces > 0) {
			_release_map(lamp);
			lamp.in_grid = false;
			repack_needed = true;
			if (lamp.casts && lamp.queued_order == 0) {
				lamp.queued_order = next_queue_order++;
				lamp.queued_frame = current_frame;
			}
		}
	}
	map_framebuffer = RID();
	_retire_texture(map_atlas, map_atlas_texture);
	atlas_height = 0;
	atlas_used_height = 0;
	map_shelves.clear();
	free_map_rects.clear();
	omni_size = p_settings.omni_face_size;
	spot_size = p_settings.spot_tile_size;
	map_texel_size = p_settings.map_texel_size;
}

int LocalLightsWorld::_map_side_for(const Lamp &p_lamp) const {
	const int largest = p_lamp.spot ? spot_size : omni_size;
	if (map_texel_size <= 0.0f) {
		return largest;
	}
	const float half_span = p_lamp.spot ? Math::tan(Math::deg_to_rad(MIN(p_lamp.angle + SPOT_MAP_MARGIN_DEGREES, SPOT_MAP_MAX_HALF_ANGLE_DEGREES))) : 1.0f;
	const float wanted = 2.0f * p_lamp.range * half_span / map_texel_size;
	for (const int side : MAP_SIDES) {
		if (float(side) >= wanted) {
			return MIN(side, largest);
		}
	}
	return largest;
}

// Старое место лампы освобождается только после выдачи нового: при нехватке места лампа остаётся со старой картой.
bool LocalLightsWorld::_place_map(Lamp &r_lamp, int p_side) {
	const int faces = r_lamp.spot ? 1 : 6;
	Lamp previous_place;
	previous_place.map_origin = r_lamp.map_origin;
	previous_place.map_side = r_lamp.map_side;
	previous_place.map_faces = r_lamp.map_faces;
	LocalVector<Vector2i> *free_rects = free_map_rects.getptr(uint32_t(p_side) * 8 + uint32_t(faces));
	if (free_rects && !free_rects->is_empty()) {
		r_lamp.map_origin = (*free_rects)[free_rects->size() - 1];
		free_rects->resize(free_rects->size() - 1);
	} else {
		const int width = p_side * faces;
		MapShelf *shelf = nullptr;
		for (MapShelf &candidate : map_shelves) {
			if (candidate.height == p_side && candidate.next_x + width <= ATLAS_WIDTH) {
				shelf = &candidate;
				break;
			}
		}
		if (!shelf) {
			if (atlas_used_height + p_side > ATLAS_MAX_HEIGHT) {
				ERR_PRINT_ONCE("В атласе локального света нет места: лампа без места не светит, пока место не освободится.");
				return false;
			}
			MapShelf new_shelf;
			new_shelf.y = atlas_used_height;
			new_shelf.height = p_side;
			atlas_used_height += p_side;
			map_shelves.push_back(new_shelf);
			shelf = &map_shelves[map_shelves.size() - 1];
		}
		r_lamp.map_origin = Vector2i(shelf->next_x, shelf->y);
		shelf->next_x += width;
	}
	r_lamp.map_side = p_side;
	r_lamp.map_faces = faces;
	if (previous_place.map_faces > 0) {
		_free_map_place(previous_place);
	}
	return true;
}

void LocalLightsWorld::_ensure_atlas_height() {
	if (atlas_used_height <= atlas_height) {
		return;
	}
	const int height = ((atlas_used_height + ATLAS_HEIGHT_STEP - 1) / ATLAS_HEIGHT_STEP) * ATLAS_HEIGHT_STEP;
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_D16_UNORM;
	format.width = ATLAS_WIDTH;
	format.height = height;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID atlas = rd->texture_create(format, RD::TextureView());
	if (map_atlas.is_valid()) {
		rd->texture_copy(map_atlas, atlas, Vector3(), Vector3(), Vector3(ATLAS_WIDTH, atlas_height, 1), 0, 0, 0, 0);
	}
	_retire_texture(map_atlas, map_atlas_texture);
	map_atlas = atlas;
	map_atlas_texture = _wrap_texture(map_atlas, RSE::TEXTURE_LAYERED_2D_ARRAY);
	Vector<RID> attachments;
	attachments.push_back(map_atlas);
	map_framebuffer = rd->framebuffer_create(attachments);
	atlas_height = height;
	globals_published = false;
}

RID LocalLightsWorld::_pipeline_for(HashMap<RD::VertexFormatID, RID> &r_pipelines, RID p_shader, RD::VertexFormatID p_format) {
	const RID *existing = r_pipelines.getptr(p_format);
	if (existing) {
		return *existing;
	}
	RD::PipelineRasterizationState raster;
	raster.cull_mode = RD::POLYGON_CULL_DISABLED;
	// Кастер ближе ближней плоскости перекрывает всё за ней, а не выпадает из карты: лампа стоит вплотную к преграде.
	raster.enable_depth_clamp = true;
	RD::PipelineDepthStencilState depth;
	depth.enable_depth_test = true;
	depth.enable_depth_write = true;
	depth.depth_compare_operator = RD::COMPARE_OP_LESS;
	RID pipeline = rd->render_pipeline_create(p_shader, rd->framebuffer_get_format(map_framebuffer), p_format, RD::RENDER_PRIMITIVE_TRIANGLES, raster, RD::PipelineMultisampleState(), depth, RD::PipelineColorBlendState());
	r_pipelines.insert(p_format, pipeline);
	return pipeline;
}

// Альфа мастер-материала: альфа цвета × альфа карты × альфа вершины. Другой шейдер с вырезом отбрасывает тень целиком и сообщает об этом.
bool LocalLightsWorld::_read_cutout(RID p_material, const void *p_shader_data, void *p_surface, CasterCutout &r_cutout) const {
	const SceneShader::ShaderData *shader_data = static_cast<const SceneShader::ShaderData *>(p_shader_data);
	static const StringName alpha_scissor_threshold = "alpha_scissor_threshold";
	static const StringName albedo_texture = "albedo_texture";
	static const StringName texture_albedo = "texture_albedo";
	static const StringName use_albedo_texture = "use_albedo_texture";
	static const StringName albedo_color = "albedo_color";
	static const StringName albedo = "albedo";
	static const StringName use_vertex_color = "use_vertex_color";
	static const StringName uv_scale = "uv_scale";
	static const StringName uv1_scale = "uv1_scale";
	static const StringName uv_offset = "uv_offset";
	static const StringName uv1_offset = "uv1_offset";
	const Variant threshold = material_value(p_material, shader_data, alpha_scissor_threshold);
	if (threshold.get_type() == Variant::NIL) {
		WARN_PRINT_ONCE("Local lights: the shader of an alpha cutout material has no alpha_scissor_threshold parameter, its shadow has no holes.");
		return false;
	}
	r_cutout.threshold = threshold;
	const Variant color = material_value(p_material, shader_data, albedo_color, albedo);
	r_cutout.alpha = color.get_type() == Variant::COLOR ? Color(color).a : 1.0f;
	r_cutout.vertex_alpha = bool(material_value(p_material, shader_data, use_vertex_color));
	const Variant scale = material_value(p_material, shader_data, uv_scale, uv1_scale);
	r_cutout.uv_scale = scale.get_type() != Variant::NIL ? Vector2(scale) : Vector2(1, 1);
	const Variant offset = material_value(p_material, shader_data, uv_offset, uv1_offset);
	r_cutout.uv_offset = offset.get_type() != Variant::NIL ? Vector2(offset) : Vector2();
	const Vector4 mesh_uv_scale = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_uv_scale(p_surface);
	r_cutout.mesh_uv_decode = Vector2(mesh_uv_scale.x, mesh_uv_scale.y);
	RendererRD::TextureStorage *textures = RendererRD::TextureStorage::get_singleton();
	const Variant use_texture = material_value(p_material, shader_data, use_albedo_texture);
	RID alpha_texture;
	if (use_texture.get_type() == Variant::NIL || bool(use_texture)) {
		const Variant texture = material_value(p_material, shader_data, albedo_texture, texture_albedo);
		if (texture.get_type() == Variant::RID) {
			alpha_texture = textures->texture_get_rd_texture(RID(texture));
		}
	}
	if (alpha_texture.is_null()) {
		alpha_texture = textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	}
	const RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
	r_cutout.uniform_set = UniformSetCacheRD::get_singleton()->get_cache(cutout_rd_shader, 0, sampled_uniform(0, sampler, alpha_texture));
	return true;
}

void LocalLightsWorld::_draw_cutout_surface(RD::DrawListID p_list, const CasterSurface &p_caster, const Transform3D &p_light_from_world, float p_extent, const float *p_depth) {
	const CasterCutout &cutout = cutout_pool[p_caster.cutout];
	rd->draw_list_bind_render_pipeline(p_list, _pipeline_for(cutout_pipelines, cutout_rd_shader, p_caster.format));
	rd->draw_list_bind_uniform_set(p_list, cutout.uniform_set, 0);
	rd->draw_list_bind_vertex_array(p_list, p_caster.vertex_array);
	if (p_caster.index_array.is_valid()) {
		rd->draw_list_bind_index_array(p_list, p_caster.index_array);
	}
	CutoutPush push;
	RendererRD::MaterialStorage::store_transform_transposed_3x4(p_light_from_world * p_caster.instance->transform, push.to_light);
	push.projection_and_cut[0] = p_extent;
	push.projection_and_cut[1] = p_extent;
	push.projection_and_cut[2] = cutout.threshold;
	push.projection_and_cut[3] = cutout.alpha;
	push.extra[0] = p_depth[0];
	push.extra[1] = p_depth[1];
	push.extra[2] = cutout.mesh_uv_decode.x;
	push.extra[3] = cutout.mesh_uv_decode.y;
	for (int axis = 0; axis < 3; axis++) {
		push.decode_position[axis] = p_caster.decode_position[axis];
		push.decode_size[axis] = p_caster.decode_size[axis];
	}
	push.decode_position[3] = cutout.vertex_alpha ? 1.0f : 0.0f;
	push.decode_size[3] = 0.0f;
	push.uv_transform[0] = cutout.uv_scale.x;
	push.uv_transform[1] = cutout.uv_scale.y;
	push.uv_transform[2] = cutout.uv_offset.x;
	push.uv_transform[3] = cutout.uv_offset.y;
	rd->draw_list_set_push_constant(p_list, &push, sizeof(CutoutPush));
	rd->draw_list_draw(p_list, p_caster.index_array.is_valid(), 1);
}

void LocalLightsWorld::_gather_casters(Lamp &r_lamp, const Instance *p_instance) {
	const LocalLightsSettings &settings = LocalLights::get_singleton()->get_settings();
	r_lamp.caster_first = lamp_casters.size();
	const InstanceLightData *light = static_cast<const InstanceLightData *>(p_instance->base_data);
	const float range_sq = r_lamp.range * r_lamp.range;
	for (const Instance *geometry : light->geometries) {
		if (geometry->base_type != RSE::INSTANCE_MESH || geometry->cast_shadows == RSE::SHADOW_CASTING_SETTING_OFF || !(geometry->layer_mask & r_lamp.caster_mask)) {
			continue;
		}
		const InstanceCasters &surfaces = _instance_surfaces(geometry);
		for (uint32_t index = surfaces.first; index < surfaces.first + surfaces.count; index++) {
			if (settings.caster_culling) {
				const AABB &bounds = caster_pool[index].bounds;
				const Vector3 nearest = r_lamp.position.clamp(bounds.position, bounds.get_end());
				if (nearest.distance_squared_to(r_lamp.position) > range_sq) {
					frame_metrics.surfaces_culled++;
					continue;
				}
			}
			if (caster_pool[index].cutout >= 0) {
				lamp_cutout_casters.push_back(index);
			} else {
				lamp_casters.push_back(index);
			}
		}
	}
	// Вырез рисуется последним: его отбрасывание выключает ранний тест, а готовая глубина отсекает закрытое до чтения карты.
	for (const uint32_t index : lamp_cutout_casters) {
		lamp_casters.push_back(index);
	}
	lamp_cutout_casters.clear();
	r_lamp.caster_count = lamp_casters.size() - r_lamp.caster_first;
}

const LocalLightsWorld::InstanceCasters &LocalLightsWorld::_instance_surfaces(const Instance *p_geometry) {
	const uint32_t slot = p_geometry->self.get_local_index();
	if (slot >= instance_casters.size()) {
		instance_casters.resize(slot + 1);
	}
	InstanceCasters &entry = instance_casters[slot];
	if (entry.gather_pass == gather_pass) {
		return entry;
	}
	entry.gather_pass = gather_pass;
	entry.first = caster_pool.size();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const uint64_t position_mask = 1ull << RSE::ARRAY_VERTEX;
	const uint64_t cutout_mask = position_mask | (1ull << RSE::ARRAY_COLOR) | (1ull << RSE::ARRAY_TEX_UV);
	const uint32_t surface_count = mesh_storage->mesh_get_surface_count(p_geometry->base);
	if (p_geometry->mesh_instance.is_valid()) {
		mesh_storage->mesh_instance_check_for_update(p_geometry->mesh_instance);
	}
	for (uint32_t index = 0; index < surface_count; index++) {
		void *surface = mesh_storage->mesh_get_surface(p_geometry->base, index);
		if (mesh_storage->mesh_surface_get_primitive(surface) != RSE::PRIMITIVE_TRIANGLES) {
			continue;
		}
		RID material = p_geometry->material_override;
		if (material.is_null()) {
			material = index < uint32_t(p_geometry->materials.size()) && p_geometry->materials[index].is_valid() ? p_geometry->materials[index] : mesh_storage->mesh_surface_get_material(p_geometry->base, index);
		}
		const SceneShader::ShaderData *shader_data = material.is_valid() ? spatial_shader_data(material) : nullptr;
		// Материал прохода прозрачности тень не отбрасывает.
		if (shader_data && shader_data->uses_alpha_pass()) {
			continue;
		}
		const AABB local_bounds = mesh_storage->mesh_surface_get_aabb(surface);
		CasterSurface caster;
		caster.instance = p_geometry;
		caster.bounds = p_geometry->transform.xform(local_bounds);
		CasterCutout cutout;
		if (shader_data && shader_data->uses_alpha_clip && _read_cutout(material, shader_data, surface, cutout)) {
			caster.cutout = cutout_pool.size();
			cutout_pool.push_back(cutout);
		}
		const uint64_t attributes = caster.cutout >= 0 ? cutout_mask : position_mask;
		if (p_geometry->mesh_instance.is_valid()) {
			mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(p_geometry->mesh_instance, index, attributes, false, false, caster.vertex_array, caster.format);
		} else {
			mesh_storage->mesh_surface_get_vertex_arrays_and_format(surface, attributes, false, false, caster.vertex_array, caster.format);
		}
		caster.index_array = mesh_storage->mesh_surface_get_index_array(surface, 0);
		const bool compressed = mesh_storage->mesh_surface_get_format(surface) & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
		caster.decode_position = compressed ? local_bounds.position : Vector3();
		caster.decode_size = compressed ? local_bounds.size : Vector3(1, 1, 1);
		caster.triangles = mesh_storage->mesh_surface_get_vertices_drawn_count(surface) / 3;
		caster_pool.push_back(caster);
	}
	entry.count = caster_pool.size() - entry.first;
	return entry;
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
	_assign_queue_tiers();
	struct ByTierAndOrder {
		const LocalVector<Lamp> *lamps;
		bool operator()(uint32_t a, uint32_t b) const {
			const Lamp &first = (*lamps)[a];
			const Lamp &second = (*lamps)[b];
			return first.queue_tier != second.queue_tier ? first.queue_tier < second.queue_tier : first.queued_order < second.queued_order;
		}
	};
	SortArray<uint32_t, ByTierAndOrder> sorter;
	sorter.compare.lamps = &lamps;
	sorter.sort(serve_order.ptr(), serve_order.size());
	const uint32_t budget = settings.update_cap > 0 && !bake_all_lamps ? MIN(uint32_t(settings.update_cap), serve_order.size()) : serve_order.size();
	_choose_lamps_to_serve(budget, p_frame);

	uint64_t started = OS::get_singleton()->get_ticks_usec();
	_apply_map_sizes(settings);
	caster_pool.clear();
	cutout_pool.clear();
	lamp_casters.clear();
	gather_pass++;
	for (uint32_t i = 0; i < budget; i++) {
		Lamp &lamp = lamps[serve_order[i]];
		const int side = _map_side_for(lamp);
		if (lamp.map_side != side) {
			_place_map(lamp, side);
		}
	}
	_ensure_atlas_height();
	for (uint32_t i = 0; i < budget; i++) {
		Lamp &lamp = lamps[serve_order[i]];
		if (lamp.map_faces == 0) {
			continue;
		}
		const Instance *instance = scene_cull()->instance_owner.get_or_null(lamp.instance);
		ERR_CONTINUE(!instance);
		_gather_casters(lamp, instance);
		frame_metrics.served_wait_frames = MAX(frame_metrics.served_wait_frames, uint32_t(p_frame - lamp.queued_frame));
	}
	RendererRD::MeshStorage::get_singleton()->update_mesh_instances();
	frame_metrics.gather_usec += OS::get_singleton()->get_ticks_usec() - started;

	started = OS::get_singleton()->get_ticks_usec();
	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights maps");
	}
	for (uint32_t i = 0; i < budget; i++) {
		const Lamp &lamp = lamps[serve_order[i]];
		if (lamp.map_faces > 0) {
			_record_lamp(lamp, settings);
		}
	}
	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights maps end");
	}
	frame_metrics.record_usec += OS::get_singleton()->get_ticks_usec() - started;

	for (uint32_t i = 0; i < budget; i++) {
		Lamp &lamp = lamps[serve_order[i]];
		if (lamp.map_faces == 0) {
			continue;
		}
		lamp.queued_order = 0;
		lamp.map_valid = true;
		lamp.baked_origin = lamp.position;
		lamp.baked_range = lamp.range;
		if (lamp.spot) {
			const SpotMapAxes axes = spot_map_axes(lamp.axis, lamp.angle);
			lamp.baked_forward = lamp.axis;
			lamp.baked_up = axes.up;
			lamp.baked_inverse_tangent = 1.0f / axes.extent;
		}
		lamp.data_dirty = true;
		if (!lamp.in_grid) {
			repack_needed = true;
		}
	}
	frame_metrics.lamps_served += budget;
	for (uint32_t i = 0; i < budget; i++) {
		const QueueTier tier = lamps[serve_order[i]].queue_tier;
		frame_metrics.lamps_served_held += tier == QUEUE_TIER_HELD ? 1 : 0;
		frame_metrics.lamps_served_influenced += tier == QUEUE_TIER_INFLUENCED ? 1 : 0;
	}
	uint32_t oldest = 0;
	for (uint32_t i = budget; i < serve_order.size(); i++) {
		oldest = MAX(oldest, uint32_t(p_frame - lamps[serve_order[i]].queued_frame));
	}
	frame_metrics.oldest_wait_frames = MAX(frame_metrics.oldest_wait_frames, oldest);
	frame_metrics.queue = serve_order.size() - budget;
	for (uint32_t i = 0; i < budget; i++) {
		if (lamps[serve_order[i]].map_faces == 0) {
			frame_metrics.queue++;
		}
	}
}

void LocalLightsWorld::set_held_instances(const Vector<RID> &p_instances) {
	held_instances.clear();
	for (const RID &instance : p_instances) {
		held_instances.push_back(instance);
	}
}

void LocalLightsWorld::_assign_queue_tiers() {
	for (Lamp &lamp : lamps) {
		lamp.queue_tier = QUEUE_TIER_OTHER;
	}
	for (const RID &held : held_instances) {
		const Instance *instance = scene_cull()->instance_owner.get_or_null(held);
		if (!instance) {
			continue;
		}
		if (instance->base_type == RSE::INSTANCE_LIGHT) {
			const uint32_t *index = lamp_of_instance.getptr(held);
			if (index) {
				lamps[*index].queue_tier = QUEUE_TIER_HELD;
			}
			continue;
		}
		if (!((1 << instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) || !instance->base_data) {
			continue;
		}
		const AABB &bounds = instance->transformed_aabb;
		const RendererSceneCull::InstanceGeometryData *geometry = static_cast<const RendererSceneCull::InstanceGeometryData *>(instance->base_data);
		for (const Instance *light : geometry->lights) {
			const uint32_t *index = lamp_of_instance.getptr(light->self);
			if (!index) {
				continue;
			}
			Lamp &lamp = lamps[*index];
			const Vector3 nearest = lamp.position.clamp(bounds.position, bounds.get_end());
			if (lamp.queue_tier == QUEUE_TIER_OTHER && nearest.distance_squared_to(lamp.position) <= lamp.range * lamp.range) {
				lamp.queue_tier = QUEUE_TIER_INFLUENCED;
			}
		}
	}
}

// Движущийся предмет ставит свои лампы в очередь каждый кадр и мог бы занять весь cap: остальным всегда остаётся четверть.
// При cap 1 они идут через кадр, а при cap от 2 лампа в руках обновляется каждый кадр.
void LocalLightsWorld::_choose_lamps_to_serve(uint32_t p_budget, uint64_t p_frame) {
	uint32_t others_first = 0;
	while (others_first < serve_order.size() && lamps[serve_order[others_first]].queue_tier != QUEUE_TIER_OTHER) {
		others_first++;
	}
	if (others_first == 0 || others_first == serve_order.size() || others_first <= p_budget / 2) {
		// Либо очередь однородна, либо приоритетные и так оставляют остальным больше четверти.
		return;
	}
	uint32_t reserved = p_budget >= 2 ? MAX(1u, p_budget / 4) : (p_frame % 2);
	reserved = MIN(reserved, serve_order.size() - others_first);
	const uint32_t priority_served = MIN(others_first, p_budget - reserved);
	for (uint32_t i = 0; i < reserved && priority_served + i < others_first; i++) {
		SWAP(serve_order[priority_served + i], serve_order[others_first + i]);
	}
}

void LocalLightsWorld::_record_lamp(const Lamp &p_lamp, const LocalLightsSettings &p_settings) {
	const int side = p_lamp.map_side;
	const float guard_extent = 1.0f + CUBE_GUARD_TEXELS / float(side);
	const Rect2 region(p_lamp.map_origin, Vector2i(side * p_lamp.map_faces, side));
	RD::DrawListID list = rd->draw_list_begin(map_framebuffer, RD::DRAW_CLEAR_DEPTH, VectorView<Color>(), 1.0f, 0, region);
	RD::VertexFormatID bound_format = 0;
	CasterPush push;
	push.projection[2] = 0.0f;
	push.projection[3] = 0.0f;
	const float near_plane = map_near_plane(p_lamp.range);
	push.extra[0] = p_lamp.range / (p_lamp.range - near_plane);
	push.extra[1] = -near_plane * p_lamp.range / (p_lamp.range - near_plane);
	push.extra[2] = 0.0f;
	push.extra[3] = 0.0f;
	push.decode_position[3] = 0.0f;
	push.decode_size[3] = 0.0f;
	for (int face = 0; face < p_lamp.map_faces; face++) {
		rd->draw_list_set_viewport(list, Rect2(p_lamp.map_origin.x + face * side, p_lamp.map_origin.y, side, side));
		rd->draw_list_disable_scissor(list);
		Vector3 right, up, forward;
		float extent;
		Vector3 normals[4];
		if (p_lamp.spot) {
			forward = p_lamp.axis;
			const SpotMapAxes axes = spot_map_axes(forward, p_lamp.angle);
			right = axes.right;
			up = axes.up;
			extent = axes.extent;
			// Лучи вне конуса не читаются: кастер отсекается по конусу с небольшим запасом, плитка шире.
			const float cone_extent = 1.0f / Math::tan(Math::deg_to_rad(MIN(p_lamp.angle + 1.0f, SPOT_MAP_MAX_HALF_ANGLE_DEGREES)));
			pyramid_normals(right, up, forward, 1.0f / cone_extent, normals);
		} else {
			right = FACE_RIGHT[face];
			up = FACE_UP[face];
			forward = FACE_FORWARD[face];
			extent = guard_extent;
			pyramid_normals(right, up, forward, guard_extent, normals);
		}
		const Basis rows = rows_of(right, up, forward);
		const Transform3D light_from_world(rows, rows.xform(-p_lamp.position));
		push.projection[0] = extent;
		push.projection[1] = extent;
		for (uint32_t i = 0; i < p_lamp.caster_count; i++) {
			const CasterSurface &caster = caster_pool[lamp_casters[p_lamp.caster_first + i]];
			if (p_settings.caster_culling) {
				const Vector3 half = caster.bounds.size * 0.5f;
				const Vector3 center = caster.bounds.position + half - p_lamp.position;
				if (box_outside_planes(normals, 4, center, half)) {
					frame_metrics.surfaces_culled++;
					continue;
				}
			}
			if (caster.cutout >= 0) {
				_draw_cutout_surface(list, caster, light_from_world, extent, push.extra);
				bound_format = 0;
				frame_metrics.surfaces_drawn++;
				frame_metrics.cutout_surfaces_drawn++;
				frame_metrics.triangles_drawn += caster.triangles;
				continue;
			}
			if (caster.format != bound_format) {
				rd->draw_list_bind_render_pipeline(list, _pipeline_for(caster_pipelines, caster_rd_shader, caster.format));
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
	_ensure_data_table();
	uint32_t in_grid = 0;
	for (uint32_t i = 0; i < lamps.size(); i++) {
		Lamp &lamp = lamps[i];
		if (!lamp.active || !lamp.in_grid) {
			continue;
		}
		in_grid++;
		if (lamp.data_dirty) {
			_write_lamp_data(i);
			lamp.data_dirty = false;
			data_changed = true;
		}
	}
	frame_metrics.lamps_in_grid = in_grid;
	if (grid_changed) {
		rd->texture_update(grid_rd, 0, grid_bytes);
		rd->texture_update(list_rd, 0, list_bytes);
		grid_changed = false;
		frame_metrics.grid_uploads++;
		frame_metrics.uploaded_bytes += grid_bytes.size() + list_bytes.size();
	}
	if (data_changed) {
		rd->texture_update(data_rd, 0, data_bytes);
		data_changed = false;
		frame_metrics.data_uploads++;
		frame_metrics.uploaded_bytes += data_bytes.size();
	}
}

// В блоке ячейки сначала Omni, затем Spot: материал обходит их разными циклами.
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
	const uint32_t needed_rows = MAX(1u, (list_end + LIST_WIDTH - 1) / LIST_WIDTH);
	if (needed_rows > list_rows) {
		list_rows = Math::next_power_of_2(needed_rows);
		_retire_texture(list_rd, list_texture);
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R32_SFLOAT;
		format.width = LIST_WIDTH;
		format.height = list_rows;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		list_rd = rd->texture_create(format, RD::TextureView());
		list_texture = _wrap_texture(list_rd, RSE::TEXTURE_LAYERED_2D_ARRAY);
		list_bytes.resize(LIST_WIDTH * list_rows * sizeof(float));
		globals_published = false;
	}
	float *list = reinterpret_cast<float *>(list_bytes.ptrw());
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < lamps.size(); i++) {
			const Lamp &lamp = lamps[i];
			if (!lamp.active || !lamp.in_grid || lamp.spot != (pass == 1)) {
				continue;
			}
			for (const uint32_t cell : lamp.cells) {
				list[cell_next[cell]++] = float(i);
			}
		}
	}
	grid_changed = true;
}

void LocalLightsWorld::_ensure_data_table() {
	const uint32_t needed_rows = MAX(1u, (lamps.size() + LAMPS_PER_DATA_ROW - 1) / LAMPS_PER_DATA_ROW);
	if (needed_rows <= data_rows) {
		return;
	}
	data_rows = Math::next_power_of_2(needed_rows);
	_retire_texture(data_rd, data_texture);
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	format.width = DATA_WIDTH;
	format.height = data_rows;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	data_rd = rd->texture_create(format, RD::TextureView());
	data_texture = _wrap_texture(data_rd, RSE::TEXTURE_LAYERED_2D_ARRAY);
	data_bytes.resize(DATA_WIDTH * data_rows * BYTES_PER_DATA_TEXEL);
	memset(data_bytes.ptrw(), 0, data_bytes.size());
	for (Lamp &lamp : lamps) {
		lamp.data_dirty = true;
	}
	globals_published = false;
}

void LocalLightsWorld::_write_lamp_data(uint32_t p_index) {
	const Lamp &lamp = lamps[p_index];
	float values[SLOT_FLOATS] = {};
	values[0] = lamp.position.x;
	values[1] = lamp.position.y;
	values[2] = lamp.position.z;
	values[3] = 1.0f / (lamp.range * lamp.range);
	values[8] = lamp.linear_color.r * lamp.energy;
	values[9] = lamp.linear_color.g * lamp.energy;
	values[10] = lamp.linear_color.b * lamp.energy;
	values[11] = lamp.decay;
	values[14] = lamp.cone_attenuation;
	values[15] = lamp.specular_amount;
	if (lamp.spot) {
		const float outer = Math::deg_to_rad(MIN(lamp.angle, 89.0f));
		const float cos_outer = Math::cos(outer);
		values[4] = lamp.axis.x;
		values[5] = lamp.axis.y;
		values[6] = lamp.axis.z;
		values[7] = cos_outer * cos_outer;
		values[12] = cos_outer;
	}
	values[27] = -1.0f;
	if (lamp.casts && lamp.map_valid) {
		values[16] = map_near_plane(lamp.baked_range);
		values[18] = float(lamp.map_origin.y);
		values[19] = lamp.baked_range;
		values[20] = lamp.spot ? lamp.baked_forward.x : 0.0f;
		values[21] = lamp.spot ? lamp.baked_forward.y : 0.0f;
		values[22] = lamp.spot ? lamp.baked_forward.z : 1.0f;
		values[23] = lamp.spot ? lamp.baked_inverse_tangent : 0.0f;
		const Vector3 delta = lamp.position - lamp.baked_origin;
		values[24] = delta.x;
		values[25] = delta.y;
		values[26] = delta.z;
		values[27] = float(lamp.map_origin.x);
		values[28] = lamp.spot ? lamp.baked_up.x : 0.0f;
		values[29] = lamp.spot ? lamp.baked_up.y : 1.0f;
		values[30] = lamp.spot ? lamp.baked_up.z : 0.0f;
		values[31] = float(lamp.map_side);
	}
	const uint64_t row = p_index / LAMPS_PER_DATA_ROW;
	const uint64_t column = p_index % LAMPS_PER_DATA_ROW;
	memcpy(data_bytes.ptrw() + (row * DATA_WIDTH + column * SLOT_TEXELS) * BYTES_PER_DATA_TEXEL, values, sizeof(values));
}

void LocalLightsWorld::_publish_globals() {
	if (globals_published) {
		return;
	}
	globals_published = true;
	RendererRD::MaterialStorage *materials = RendererRD::MaterialStorage::get_singleton();
	materials->global_shader_parameter_set("local_light_origin", origin);
	materials->global_shader_parameter_set("local_light_inverse_cell_size", 1.0f / CELL_SIZE);
	materials->global_shader_parameter_set("local_light_dims", CELL_DIMS);
	materials->global_shader_parameter_set("local_light_grid", grid_texture);
	materials->global_shader_parameter_set("local_light_data", data_texture);
	materials->global_shader_parameter_set("local_light_list", list_texture);
	materials->global_shader_parameter_set("local_light_map_atlas", map_atlas_texture);
	materials->global_shader_parameter_set("local_light_shadow_ratio", blurred_ratio_texture);
	// Новая раскладка сетки уже загружена: материалы этого кадра должны читать её через новые текстуры и начало окна, иначе индексы укажут мимо.
	materials->_update_global_shader_uniforms();
	materials->_update_queued_materials();
}

void LocalLightsWorld::_ensure_screen_textures(const Size2i &p_size) {
	if (p_size.x <= screen_size.x && p_size.y <= screen_size.y && blurred_ratio.is_valid()) {
		return;
	}
	screen_size = Size2i(MAX(p_size.x, screen_size.x), MAX(p_size.y, screen_size.y));
	_retire_texture(blurred_ratio, blurred_ratio_texture);
	RID no_wrapper;
	_retire_texture(ratio_surface, no_wrapper);
	RD::TextureFormat format;
	format.width = screen_size.x;
	format.height = screen_size.y;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	format.format = RD::DATA_FORMAT_R32G32_UINT;
	ratio_surface = rd->texture_create(format, RD::TextureView());
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
	const RID no_atlas = textures->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	const RID depth = buffers->get_depth_texture(0);
	const Vector2i groups((size.x + 7) / 8, (size.y + 7) / 8);
	UniformSetCacheRD *uniform_sets = UniformSetCacheRD::get_singleton();

	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights ratio");
	}
	RID ratio_set = uniform_sets->get_cache(ratio_rd_shader, 0, sampled_uniform(0, sampler, depth), sampled_uniform(1, sampler, grid_rd), sampled_uniform(2, sampler, data_rd),
			sampled_uniform(3, sampler, map_atlas.is_valid() ? map_atlas : no_atlas), sampled_uniform(4, sampler, list_rd), buffer_uniform(5, params_buffer), image_uniform(7, ratio_surface));
	RD::ComputeListID compute = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute, ratio_pipeline);
	rd->compute_list_bind_uniform_set(compute, ratio_set, 0);
	rd->compute_list_dispatch(compute, groups.x, groups.y, 1);
	rd->compute_list_end();

	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights blur");
	}
	RID blur_set = uniform_sets->get_cache(blur_rd_shader, 0, image_uniform(1, ratio_surface), image_uniform(2, blurred_ratio), buffer_uniform(3, params_buffer));
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
	report["atlasBytes"] = uint64_t(ATLAS_WIDTH) * atlas_height * 2;
	report["atlasHeight"] = atlas_height;
	report["atlasUsedHeight"] = atlas_used_height;
	report["screenBytes"] = uint64_t(screen_size.x) * screen_size.y * (8 + 4);
	report["gridBytes"] = uint64_t(grid_bytes.size());
	report["dataBytes"] = uint64_t(data_bytes.size()) + uint64_t(list_bytes.size());
	uint64_t cpu = uint64_t(grid_bytes.size()) + uint64_t(data_bytes.size()) + uint64_t(list_bytes.size());
	cpu += lamps.get_capacity() * sizeof(Lamp);
	for (const Lamp &lamp : lamps) {
		cpu += lamp.cells.get_capacity() * sizeof(uint32_t);
	}
	cpu += (cell_omni.get_capacity() + cell_spot.get_capacity() + cell_next.get_capacity() + filled_cells.get_capacity() + serve_order.get_capacity()) * sizeof(uint32_t);
	cpu += caster_pool.get_capacity() * sizeof(CasterSurface) + cutout_pool.get_capacity() * sizeof(CasterCutout) + instance_casters.get_capacity() * sizeof(InstanceCasters) + lamp_casters.get_capacity() * sizeof(uint32_t) + map_shelves.get_capacity() * sizeof(MapShelf);
	for (const KeyValue<uint32_t, LocalVector<Vector2i>> &entry : free_map_rects) {
		cpu += entry.value.get_capacity() * sizeof(Vector2i);
	}
	report["cpuBytes"] = cpu;
	return report;
}

Dictionary LocalLightsWorld::read_screen_buffers() const {
	Dictionary result;
	result["size"] = screen_size;
	if (blurred_ratio.is_valid()) {
		result["hard"] = rd->texture_get_data(ratio_surface, 0);
		result["blurred"] = rd->texture_get_data(blurred_ratio, 0);
	}
	return result;
}
