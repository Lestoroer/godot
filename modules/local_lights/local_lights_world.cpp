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

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace {

// Сетка покрывает охват ламп по каждой оси отдельно: интерьер плоский, и куб тратил бы ячейки на высоту. Чем мельче ячейка,
// тем короче её список и тем меньше ламп вне радиуса перебирают материал и проход отношения. Число ячеек ограничено:
// сетка заново раскладывается и загружается на каждом кадре движения ламп.
const float CELL_SIZES[] = { 0.5f, 0.63f, 0.8f, 1.0f, 1.26f, 1.6f, 2.0f, 2.5f, 3.2f };
// Мельче ячейки нужно больше: на стенде нагрузки ячейка 0,8 м вместо 1,26 м сокращает перебор ещё на четверть, но втрое дороже на CPU при движении ламп.
const uint64_t GRID_CELL_BUDGET = 12288;
// Сетка лежит в текстуре шириной по x и высотой y × z.
const int GRID_TEXTURE_MAX_SIDE = 16384;
// Окно шире охвата: лампы двигаются без пересчёта. Ячейка меняется ступенями, а вернуться к меньшей можно только с запасом побольше.
const float WINDOW_SLACK = 1.1f;
const float CELL_RETURN_SLACK = 1.4f;
const float RECENTER_CELLS = 4.0f;
// Таблица ламп: восемь текселей на лампу, в строке 256 ламп. Список ламп ячеек: индекс лампы в текселе. Контракт с local_light_ratio.glsl и local_lights_inc.gdshaderinc.
const uint32_t DATA_WIDTH = 2048;
const uint32_t SLOT_TEXELS = 8;
const uint32_t SLOT_FLOATS = SLOT_TEXELS * 4;
const uint32_t LAMPS_PER_DATA_ROW = DATA_WIDTH / SLOT_TEXELS;
const uint32_t BYTES_PER_DATA_TEXEL = 16;
const uint32_t LIST_WIDTH = 2048;
const int ATLAS_WIDTH = 8192;
// Атлас растёт по наименьшей плитке, а за кадр его высота меняется не больше раза.
const int ATLAS_HEIGHT_STEP = 64;
const int ATLAS_MAX_HEIGHT = 16384;
const int MAP_SIDES[] = { 64, 96, 128, 192, 256, 384, 512 };
// Клетка карты занятости атласа: все стороны плиток ей кратны, и место любой карты — целое число клеток.
const int ATLAS_CELL = 32;
const int ATLAS_COLUMNS = ATLAS_WIDTH / ATLAS_CELL;
const int ATLAS_ROW_WORDS = ATLAS_COLUMNS / 64;
// План сторон карт. Оценка площади берёт часть бюджета: остальное уходит на потери размещения. Раскладка по убыванию при загрузке теряет
// около процента, раздробленное место только откладывает повышение. Повышение — только если оно поместилось бы и в чуть меньшей ёмкости:
// движение камеры не гоняет карты туда-обратно. Времена в секундах: поведение не зависит от частоты кадров.
const float PLAN_FILL = 0.95f;
const float PLAN_FILL_FAILURE_STEP = 0.03f;
const float PLAN_FILL_MIN = 0.6f;
const float PLAN_UPGRADE_CAPACITY = 0.92f;
const double PLAN_PERIOD_SECONDS = 0.5;
const double PLAN_SIDE_DWELL_SECONDS = 1.0;
// Экранный размер лампы считается по полуоктавам: мелкое движение камеры не переставляет лампы в порядке важности.
const float VIEW_SIZE_STEPS_PER_OCTAVE = 2.0f;
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

RD::Uniform storage_uniform(int p_binding, RID p_buffer) {
	RD::Uniform uniform;
	uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	uniform.binding = p_binding;
	uniform.append_id(p_buffer);
	return uniform;
}

RD::Uniform buffer_uniform(int p_binding, RID p_buffer) {
	RD::Uniform uniform;
	uniform.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	uniform.binding = p_binding;
	uniform.append_id(p_buffer);
	return uniform;
}

// Плоскости снимка: точка внутри, если скалярное произведение с нормалью неотрицательно. Исход у разных коробок непредсказуем, поэтому без раннего выхода.
bool box_inside_pyramid(const Vector3 *p_normals, const Vector3 &p_center, const Vector3 &p_half) {
	bool inside = true;
	for (int i = 0; i < 4; i++) {
		const Vector3 &n = p_normals[i];
		inside &= n.dot(p_center) + Math::abs(n.x) * p_half.x + Math::abs(n.y) * p_half.y + Math::abs(n.z) * p_half.z >= 0.0f;
	}
	return inside;
}

void pyramid_normals(const Vector3 &p_right, const Vector3 &p_up, const Vector3 &p_forward, float p_extent, Vector3 *r_normals) {
	r_normals[0] = (p_right + p_forward * p_extent).normalized();
	r_normals[1] = (-p_right + p_forward * p_extent).normalized();
	r_normals[2] = (p_up + p_forward * p_extent).normalized();
	r_normals[3] = (-p_up + p_forward * p_extent).normalized();
}

// Грани куба, в снимок которых попадает коробка, по биту на грань в порядке FACE_FORWARD; углы коробки заданы относительно лампы.
// Грань видит точку, когда обе боковые координаты по модулю не больше глубины, умноженной на запас грани.
uint32_t box_cube_faces(const Vector3 &p_low, const Vector3 &p_high, float p_guard) {
	const Vector3 inside(MIN(p_high.x, -p_low.x), MIN(p_high.y, -p_low.y), MIN(p_high.z, -p_low.z));
	const float across_x = MIN(inside.y, inside.z);
	const float across_y = MIN(inside.x, inside.z);
	const float across_z = MIN(inside.x, inside.y);
	return uint32_t(p_guard * p_high.x + across_x >= 0.0f) |
			uint32_t(across_x - p_guard * p_low.x >= 0.0f) << 1 |
			uint32_t(across_y - p_guard * p_low.y >= 0.0f) << 2 |
			uint32_t(p_guard * p_high.z + across_z >= 0.0f) << 3 |
			uint32_t(across_z - p_guard * p_low.z >= 0.0f) << 4 |
			uint32_t(p_guard * p_high.y + across_y >= 0.0f) << 5;
}

int lowest_set_bit(uint64_t p_value) {
#ifdef _MSC_VER
	unsigned long index;
	_BitScanForward64(&index, p_value);
	return int(index);
#else
	return __builtin_ctzll(p_value);
#endif
}

// Начало первого отрезка из p_width свободных клеток строки, минус один — такого нет.
int first_free_run(const uint64_t *p_free, int p_width) {
	int run_start = 0;
	int run = 0;
	int x = 0;
	while (x < ATLAS_COLUMNS) {
		const uint64_t bits = p_free[x >> 6] >> (x & 63);
		if (bits & 1) {
			// Сдвиг заполняет старшие биты нулями: отрезок единиц не выходит за слово.
			const uint64_t zeros = ~bits;
			const int ones = zeros == 0 ? 64 : lowest_set_bit(zeros);
			run_start = run == 0 ? x : run_start;
			run += ones;
			x += ones;
			if (run >= p_width) {
				return run_start;
			}
		} else {
			run = 0;
			x += bits == 0 ? 64 - (x & 63) : lowest_set_bit(bits);
		}
	}
	return -1;
}

int map_area(int p_side, bool p_spot) {
	return p_side * p_side * (p_spot ? 1 : 6);
}

int budget_rows_for(float p_mib) {
	const int rows = int(double(p_mib) * 1048576.0 / (double(ATLAS_WIDTH) * 2.0)) / ATLAS_HEIGHT_STEP * ATLAS_HEIGHT_STEP;
	return CLAMP(rows, ATLAS_HEIGHT_STEP, ATLAS_MAX_HEIGHT);
}

bool grid_fits(const Vector3i &p_dims) {
	return uint64_t(p_dims.x) * p_dims.y * p_dims.z <= GRID_CELL_BUDGET && p_dims.x <= GRID_TEXTURE_MAX_SIDE && p_dims.y * p_dims.z <= GRID_TEXTURE_MAX_SIDE;
}

struct GridLayout {
	float cell = 0.0f;
	Vector3i dims;
};

// Наименьшая ячейка, при которой окно с запасом p_slack покрывает охват. Две лишние ячейки по оси — привязка начала окна к шагу ячейки.
// Охват шире наибольшего окна: длинные оси укорачиваются, и окно следует за центром ламп.
GridLayout grid_layout_for(const Vector3 &p_extent, float p_slack) {
	GridLayout layout;
	for (const float cell : CELL_SIZES) {
		layout.cell = cell;
		layout.dims = Vector3i((p_extent * p_slack / cell).ceil()) + Vector3i(2, 2, 2);
		if (grid_fits(layout.dims)) {
			return layout;
		}
	}
	while (!grid_fits(layout.dims)) {
		const int axis = layout.dims.max_axis_index();
		layout.dims[axis] = MAX(1, layout.dims[axis] * 7 / 8);
	}
	return layout;
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
	// До первой лампы окна нет: пустая сетка в одну ячейку.
	_resize_grid(Vector3i(1, 1, 1));
}

void LocalLightsWorld::_resize_grid(const Vector3i &p_dims) {
	cell_dims = p_dims;
	const uint32_t cell_total = p_dims.x * p_dims.y * p_dims.z;
	cell_omni.resize(cell_total);
	cell_spot.resize(cell_total);
	cell_next.resize(cell_total);
	memset(cell_omni.ptr(), 0, cell_total * sizeof(uint32_t));
	memset(cell_spot.ptr(), 0, cell_total * sizeof(uint32_t));
	filled_cells.clear();
	grid_bytes.resize(cell_total * 4 * sizeof(float));
	memset(grid_bytes.ptrw(), 0, grid_bytes.size());
	_retire_texture(grid_rd, grid_texture);
	RD::TextureFormat grid_format;
	grid_format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	grid_format.width = p_dims.x;
	grid_format.height = p_dims.y * p_dims.z;
	grid_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	grid_rd = rd->texture_create(grid_format, RD::TextureView());
	grid_texture = _wrap_texture(grid_rd, RSE::TEXTURE_LAYERED_2D_ARRAY);
	globals_published = false;
	repack_needed = true;
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
	for (const RID &rid : { data_buffer, list_buffer, map_atlas, grid_rd, data_rd, list_rd, ratio_surface, blurred_ratio, params_buffer, ratio_pipeline, blur_pipeline }) {
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

void LocalLightsWorld::prepare_view(const LocalVector<Instance *> &p_lights, const Vector3 &p_camera_position) {
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame != current_frame) {
		_begin_frame(frame);
	}
	frame_metrics.views++;
	camera_position = p_camera_position;
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
		if (is_new) {
			plan_dirty = true;
		}
		if (is_new || refresh_all || light->is_shadow_dirty()) {
			const bool reach_changed = _refresh_lamp(lamp, instance, p_frame);
			light->clear_shadow_dirty();
			form_changed = form_changed || is_new || refresh_all || reach_changed;
		}
		// Несколько видов за кадр: лампа важна по ближнему из них.
		const float size = lamp.range / MAX(lamp.position.distance_to(camera_position) - lamp.range, 0.1f * lamp.range);
		lamp.view_size = frame_metrics.views > 1 ? MAX(lamp.view_size, size) : size;
	}
	refresh_all = false;
	if (form_changed) {
		_update_window();
	}
}

// Возвращает, сдвинулся ли охват лампы: окну сетки важны её позиция и радиус.
bool LocalLightsWorld::_refresh_lamp(Lamp &r_lamp, const Instance *p_instance, uint64_t p_frame) {
	const RID base = p_instance->base;
	auto *storage = RSG::light_storage;
	const Transform3D &transform = p_instance->transform;
	const float range = storage->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
	const bool spot = storage->light_get_type(base) == RSE::LIGHT_SPOT;
	const float angle = spot ? storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE) : 0.0f;
	const Vector3 axis = spot ? -transform.basis.get_column(2).normalized() : Vector3();
	const bool reach_changed = r_lamp.position != transform.origin || r_lamp.range != range;
	const bool casts = storage->light_has_shadow(base);
	if (r_lamp.range != range || r_lamp.angle != angle || r_lamp.spot != spot || r_lamp.casts != casts) {
		plan_dirty = true;
	}
	const bool form_changed = reach_changed || r_lamp.angle != angle || r_lamp.axis != axis || r_lamp.cells.is_empty();
	r_lamp.spot = spot;
	r_lamp.position = transform.origin;
	r_lamp.axis = axis;
	r_lamp.range = range;
	r_lamp.angle = angle;
	r_lamp.linear_color = storage->light_get_color(base).srgb_to_linear();
	r_lamp.energy = storage->light_get_param(base, RSE::LIGHT_PARAM_ENERGY);
	r_lamp.decay = storage->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION);
	r_lamp.cone_attenuation = spot ? 1.0f / storage->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION) : 1.0f;
	r_lamp.specular_amount = storage->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f;
	r_lamp.caster_mask = storage->light_get_shadow_caster_mask(base);
	r_lamp.casts = casts;
	r_lamp.data_dirty = true;
	if (form_changed && origin_valid) {
		_compute_cells(r_lamp, fresh_cells);
		if (fresh_cells.size() != r_lamp.cells.size() || (fresh_cells.size() > 0 && memcmp(fresh_cells.ptr(), r_lamp.cells.ptr(), fresh_cells.size() * sizeof(uint32_t)) != 0)) {
			SWAP(r_lamp.cells, fresh_cells);
			if (r_lamp.in_grid) {
				repack_needed = true;
			}
		}
	}
	if (r_lamp.casts) {
		// Лампа ждала только смены разрешения: очередь и ожидание обновления считаются с этого изменения.
		if (r_lamp.queued_order == 0 || !r_lamp.needs_update) {
			r_lamp.queued_order = next_queue_order++;
			r_lamp.queued_frame = p_frame;
		}
		r_lamp.needs_update = true;
	} else {
		r_lamp.queued_order = 0;
		_release_map(r_lamp);
	}
	return reach_changed;
}

// Окно следует за лампами: центр их охвата, снап к ячейке. Сдвиг окна заново раскладывает все лампы.
void LocalLightsWorld::_update_window() {
	AABB bounds;
	bool first = true;
	for (const Lamp &lamp : lamps) {
		if (!lamp.active) {
			continue;
		}
		const AABB reach(lamp.position - Vector3(lamp.range, lamp.range, lamp.range), Vector3(lamp.range, lamp.range, lamp.range) * 2.0f);
		if (first) {
			bounds = reach;
			first = false;
		} else {
			bounds.merge_with(reach);
		}
	}
	if (first) {
		return;
	}
	const Vector3 center = bounds.get_center();
	// Текущее окно остаётся, пока держит охват и меньшая ячейка не помещается с запасом побольше.
	if (origin_valid && grid_layout_for(bounds.size, CELL_RETURN_SLACK).cell >= cell_size && _window_holds(bounds)) {
		// Лампа могла появиться до первого окна: её ячейки посчитаны в _refresh_lamp.
		for (Lamp &lamp : lamps) {
			if (lamp.active && lamp.cells.is_empty()) {
				_compute_cells(lamp, lamp.cells);
			}
		}
		return;
	}
	const GridLayout layout = grid_layout_for(bounds.size, WINDOW_SLACK);
	cell_size = layout.cell;
	window_narrow_axes = 0;
	for (int axis = 0; axis < 3; axis++) {
		window_narrow_axes |= float(layout.dims[axis] - 2) * cell_size < bounds.size[axis] ? 1 << axis : 0;
	}
	if (layout.dims != cell_dims) {
		_resize_grid(layout.dims);
	}
	const Vector3 corner = center - Vector3(cell_dims) * 0.5f * cell_size;
	origin = (corner / cell_size).floor() * cell_size;
	window_center = origin + Vector3(cell_dims) * 0.5f * cell_size;
	origin_valid = true;
	globals_published = false;
	for (Lamp &lamp : lamps) {
		if (lamp.active) {
			_compute_cells(lamp, lamp.cells);
		}
	}
	repack_needed = true;
}

// По оси, которую окно вмещает, охват лежит внутри окна. По оси, на которую окна не хватило, оно следует за центром ламп с запасом в несколько ячеек.
bool LocalLightsWorld::_window_holds(const AABB &p_bounds) const {
	const Vector3 window_end = origin + Vector3(cell_dims) * cell_size;
	const Vector3 bounds_end = p_bounds.get_end();
	const Vector3 center = p_bounds.get_center();
	for (int axis = 0; axis < 3; axis++) {
		const bool holds = (window_narrow_axes & (1 << axis)) ? Math::abs(center[axis] - window_center[axis]) <= RECENTER_CELLS * cell_size : p_bounds.position[axis] >= origin[axis] && bounds_end[axis] <= window_end[axis];
		if (!holds) {
			return false;
		}
	}
	return true;
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
	const Vector3 window = Vector3(cell_dims) * cell_size;
	const Vector3 lo = box.position - origin;
	const Vector3 hi = box.get_end() - origin;
	if (hi.x < 0.0f || hi.y < 0.0f || hi.z < 0.0f || lo.x > window.x || lo.y > window.y || lo.z > window.z) {
		return;
	}
	const auto clamp_cell = [this](float p_value, int p_dim) { return CLAMP(int(Math::floor(p_value / cell_size)), 0, p_dim - 1); };
	const Vector3i cell_min(clamp_cell(lo.x, cell_dims.x), clamp_cell(lo.y, cell_dims.y), clamp_cell(lo.z, cell_dims.z));
	const Vector3i cell_max(clamp_cell(hi.x, cell_dims.x), clamp_cell(hi.y, cell_dims.y), clamp_cell(hi.z, cell_dims.z));
	const float sin_outer = Math::sqrt(MAX(1.0f - cos_outer * cos_outer, 0.0f));
	const float half_cell = cell_size * 0.5f;
	const float cell_bound = half_cell * Math::sqrt(3.0f);
	const float reach = reach_range + cell_bound;
	const float range_sq = reach_range * reach_range;
	const Vector3 first_center = origin + Vector3(0.5f, 0.5f, 0.5f) * cell_size;
	const float vx_base = first_center.x - r_lamp.position.x;
	const int dx = cell_dims.x;
	const int dxy = cell_dims.x * cell_dims.y;
	for (int cz = cell_min.z; cz <= cell_max.z; cz++) {
		const float vz = first_center.z + float(cz) * cell_size - r_lamp.position.z;
		const float gap_z = MAX(Math::abs(vz) - half_cell, 0.0f);
		if (gap_z * gap_z > range_sq) {
			continue;
		}
		for (int cy = cell_min.y; cy <= cell_max.y; cy++) {
			const float vy = first_center.y + float(cy) * cell_size - r_lamp.position.y;
			const float gap_y = MAX(Math::abs(vy) - half_cell, 0.0f);
			const float span_sq = range_sq - gap_z * gap_z - gap_y * gap_y;
			if (span_sq < 0.0f) {
				continue;
			}
			// Ячейка задета, пока её ближняя грань по x внутри хорды сферы.
			const float span = Math::sqrt(span_sq) + half_cell;
			const int cx_lo = MAX(int(Math::ceil((-span - vx_base) / cell_size)), cell_min.x);
			const int cx_hi = MIN(int(Math::floor((span - vx_base) / cell_size)), cell_max.x);
			const int row = cz * dxy + cy * dx;
			if (!r_lamp.spot) {
				for (int cx = cx_lo; cx <= cx_hi; cx++) {
					r_cells.push_back(row + cx);
				}
				continue;
			}
			const float dot_yz = vy * r_lamp.axis.y + vz * r_lamp.axis.z;
			for (int cx = cx_lo; cx <= cx_hi; cx++) {
				const float vx = vx_base + float(cx) * cell_size;
				const float axial = vx * r_lamp.axis.x + dot_yz;
				if (axial > reach || axial < -cell_bound) {
					continue;
				}
				const float d2 = vx * vx + vy * vy + vz * vz;
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
	if (r_lamp.map_faces > 0) {
		_mark_map_rect(r_lamp.map_origin, r_lamp.map_side * r_lamp.map_faces, r_lamp.map_side, true);
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
	plan_dirty = true;
}

void LocalLightsWorld::_apply_map_sizes(const LocalLightsSettings &p_settings) {
	const int budget_rows = budget_rows_for(p_settings.map_budget_mib);
	const bool same_maps = p_settings.omni_face_size == omni_size && p_settings.spot_tile_size == spot_size && p_settings.map_texel_size == map_texel_size;
	if (same_maps && budget_rows == atlas_budget_rows) {
		return;
	}
	// Больший бюджет снятые карты не трогает: к карте занятости добавляются свободные строки.
	if (same_maps && budget_rows > atlas_budget_rows) {
		const uint32_t used_words = atlas_free_cells.size();
		atlas_free_cells.resize(budget_rows / ATLAS_CELL * ATLAS_ROW_WORDS);
		for (uint32_t word = used_words; word < atlas_free_cells.size(); word++) {
			atlas_free_cells[word] = ~uint64_t(0);
		}
		atlas_budget_rows = budget_rows;
		plan_dirty = true;
		return;
	}
	// Другое разрешение или меньший бюджет: снятые карты не переносятся.
	for (Lamp &lamp : lamps) {
		if (lamp.active && lamp.map_faces > 0) {
			_release_map(lamp);
			lamp.in_grid = false;
			repack_needed = true;
			if (lamp.casts) {
				lamp.needs_update = true;
				if (lamp.queued_order == 0) {
					lamp.queued_order = next_queue_order++;
					lamp.queued_frame = current_frame;
				}
			}
		}
		lamp.planned_side = 0;
	}
	map_framebuffer = RID();
	_retire_texture(map_atlas, map_atlas_texture);
	// Списанный атлас освободится в начале следующего кадра: если в этом кадре нового не будет, материалы не должны его читать.
	globals_published = false;
	atlas_height = 0;
	atlas_used_height = 0;
	atlas_budget_rows = budget_rows;
	atlas_free_cells.resize(budget_rows / ATLAS_CELL * ATLAS_ROW_WORDS);
	for (uint64_t &word : atlas_free_cells) {
		word = ~uint64_t(0);
	}
	atlas_used_area = 0;
	plan_fill = PLAN_FILL;
	plan_dirty = true;
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

// Меньшая карта ложится в начало своего же места: ей не нужно свободное место, а старая карта светит, пока новую не нарисуют в этом же кадре.
// Большей нужно новое место; старое освобождается только после его выдачи: при нехватке места лампа остаётся со старой картой.
bool LocalLightsWorld::_place_map(Lamp &r_lamp, int p_side) {
	const int faces = r_lamp.spot ? 1 : 6;
	const int width = p_side * faces;
	const int previous_width = r_lamp.map_side * r_lamp.map_faces;
	const bool fits_in_place = r_lamp.map_faces > 0 && width <= previous_width && p_side <= r_lamp.map_side;
	// Прямоугольник, вмещающий уже не найденный в этом кадре, тоже не найдётся: поиск по всему атласу не повторяется.
	bool known_to_fail = false;
	for (const Vector2i &failed : failed_place_sizes) {
		known_to_fail = known_to_fail || (width >= failed.x && p_side >= failed.y);
	}
	Vector2i place;
	const bool allocated = !known_to_fail && _allocate_map_rect(width, p_side, place);
	if (!allocated && !known_to_fail) {
		failed_place_sizes.push_back(Vector2i(width, p_side));
	}
	// Уменьшенная карта переезжает, только если новое место ниже: старое освобождается целиком и остаётся годным крупной карте.
	if (fits_in_place && (!allocated || place.y > r_lamp.map_origin.y || (place.y == r_lamp.map_origin.y && place.x > r_lamp.map_origin.x))) {
		if (allocated) {
			_mark_map_rect(place, width, p_side, true);
		}
		_mark_map_rect(r_lamp.map_origin, previous_width, r_lamp.map_side, true);
		_mark_map_rect(r_lamp.map_origin, width, p_side, false);
	} else if (allocated) {
		if (r_lamp.map_faces > 0) {
			_mark_map_rect(r_lamp.map_origin, previous_width, r_lamp.map_side, true);
		}
		r_lamp.map_origin = place;
	} else {
		// Лампа остаётся с прежней картой до следующей смены стороны. Площади не хватило — план ошибся с запасом на потери размещения,
		// и следующий берёт меньшую долю бюджета; площади хватало — место раздроблено, повышение просто откладывается.
		frame_metrics.map_place_failures++;
		if (uint64_t(atlas_budget_rows) * ATLAS_WIDTH - atlas_used_area >= uint64_t(width) * p_side) {
			frame_metrics.map_fragmented_failures++;
		} else {
			plan_fill = MAX(PLAN_FILL_MIN, plan_fill - PLAN_FILL_FAILURE_STEP);
		}
		plan_dirty = true;
		if (r_lamp.map_faces > 0) {
			r_lamp.planned_side = r_lamp.map_side;
			r_lamp.planned_time = frame_time;
		} else {
			WARN_PRINT_ONCE("В бюджете атласа локального света не нашлось места для карты: лампа ждёт, пока план уменьшит другие карты.");
		}
		return false;
	}
	if (r_lamp.map_faces > 0) {
		frame_metrics.map_resizes++;
	}
	r_lamp.map_side = p_side;
	r_lamp.map_faces = faces;
	r_lamp.map_changed_time = frame_time;
	return true;
}

// Сторона карты меняется не чаще выдержки: план мог сменить её раньше, чем лампа дошла до очереди.
bool LocalLightsWorld::_side_change_allowed(const Lamp &p_lamp) const {
	return p_lamp.map_faces == 0 || frame_time - p_lamp.map_changed_time >= PLAN_SIDE_DWELL_SECONDS;
}

// Первое подходящее место снизу слева в пределах бюджета. Освобождённое место годится карте любого размера.
bool LocalLightsWorld::_allocate_map_rect(int p_width, int p_height, Vector2i &r_origin) {
	const int columns = p_width / ATLAS_CELL;
	const int rows = p_height / ATLAS_CELL;
	const int row_limit = atlas_budget_rows / ATLAS_CELL;
	for (int y = 0; y + rows <= row_limit; y++) {
		uint64_t free[ATLAS_ROW_WORDS];
		for (int word = 0; word < ATLAS_ROW_WORDS; word++) {
			free[word] = ~uint64_t(0);
		}
		for (int row = y; row < y + rows; row++) {
			for (int word = 0; word < ATLAS_ROW_WORDS; word++) {
				free[word] &= atlas_free_cells[row * ATLAS_ROW_WORDS + word];
			}
		}
		const int x = first_free_run(free, columns);
		if (x >= 0) {
			r_origin = Vector2i(x * ATLAS_CELL, y * ATLAS_CELL);
			_mark_map_rect(r_origin, p_width, p_height, false);
			return true;
		}
	}
	return false;
}

void LocalLightsWorld::_mark_map_rect(const Vector2i &p_origin, int p_width, int p_height, bool p_free) {
	const int column_end = (p_origin.x + p_width) / ATLAS_CELL;
	const int row_end = (p_origin.y + p_height) / ATLAS_CELL;
	for (int row = p_origin.y / ATLAS_CELL; row < row_end; row++) {
		uint64_t *words = atlas_free_cells.ptr() + row * ATLAS_ROW_WORDS;
		for (int column = p_origin.x / ATLAS_CELL; column < column_end; column++) {
			const uint64_t bit = uint64_t(1) << (column & 63);
			words[column >> 6] = p_free ? words[column >> 6] | bit : words[column >> 6] & ~bit;
		}
	}
	const uint64_t area = uint64_t(p_width) * p_height;
	atlas_used_area = p_free ? atlas_used_area - area : atlas_used_area + area;
	if (!p_free) {
		atlas_used_height = MAX(atlas_used_height, p_origin.y + p_height);
	}
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

void LocalLightsWorld::_gather_casters(Lamp &r_lamp, const Instance *p_instance, bool p_cull) {
	r_lamp.caster_first = lamp_casters.size();
	const InstanceLightData *light = static_cast<const InstanceLightData *>(p_instance->base_data);
	const Vector3 position = r_lamp.position;
	const float range_sq = r_lamp.range * r_lamp.range;
	const float cube_guard = 1.0f + CUBE_GUARD_TEXELS / float(r_lamp.map_side);
	const uint32_t all_faces = (1u << r_lamp.map_faces) - 1;
	Vector3 cone_normals[4];
	if (r_lamp.spot) {
		// Лучи вне конуса не читаются: кастер отсекается по конусу с небольшим запасом, плитка шире.
		pyramid_normals(r_lamp.baked_right, r_lamp.baked_up, r_lamp.baked_forward, Math::tan(Math::deg_to_rad(MIN(r_lamp.angle + 1.0f, SPOT_MAP_MAX_HALF_ANGLE_DEGREES))), cone_normals);
	}
	for (const Instance *geometry : light->geometries) {
		if (geometry->base_type != RSE::INSTANCE_MESH || geometry->cast_shadows == RSE::SHADOW_CASTING_SETTING_OFF || !(geometry->layer_mask & r_lamp.caster_mask)) {
			continue;
		}
		for (const CasterSurface &surface : _instance_surfaces(geometry).surfaces) {
			LampCaster entry;
			entry.surface = &surface;
			entry.face_mask = all_faces;
			if (p_cull) {
				const AABB &bounds = surface.bounds;
				const Vector3 low = bounds.position - position;
				const Vector3 high = bounds.get_end() - position;
				// Лампа вне коробки по оси: ненулевая только одна из двух величин.
				const Vector3 nearest(MAX(low.x, 0.0f) + MIN(high.x, 0.0f), MAX(low.y, 0.0f) + MIN(high.y, 0.0f), MAX(low.z, 0.0f) + MIN(high.z, 0.0f));
				const bool in_range = nearest.length_squared() <= range_sq;
				if (r_lamp.spot) {
					const Vector3 half = bounds.size * 0.5f;
					entry.face_mask = box_inside_pyramid(cone_normals, bounds.position + half - position, half) ? 1 : 0;
				} else {
					entry.face_mask = box_cube_faces(low, high, cube_guard);
				}
				if (!in_range || entry.face_mask == 0) {
					frame_metrics.surfaces_culled += in_range ? r_lamp.map_faces : 1;
					continue;
				}
			}
			if (surface.cutout >= 0) {
				lamp_cutout_casters.push_back(entry);
			} else {
				lamp_casters.push_back(entry);
			}
		}
	}
	// Вырез рисуется последним: его отбрасывание выключает ранний тест, а готовая глубина отсекает закрытое до чтения карты.
	for (const LampCaster &entry : lamp_cutout_casters) {
		lamp_casters.push_back(entry);
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
	if (!entry.collect_every_pass && entry.instance == p_geometry->self && entry.version == p_geometry->version) {
		return entry;
	}
	entry.instance = p_geometry->self;
	entry.version = p_geometry->version;
	entry.collect_every_pass = p_geometry->mesh_instance.is_valid();
	entry.surfaces.clear();
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
		if (shader_data && shader_data->uses_alpha_clip) {
			entry.collect_every_pass = true;
			if (_read_cutout(material, shader_data, surface, cutout)) {
				caster.cutout = cutout_pool.size();
				cutout_pool.push_back(cutout);
			}
		}
		const uint64_t attributes = caster.cutout >= 0 ? cutout_mask : position_mask;
		RD::VertexFormatID format = 0;
		if (p_geometry->mesh_instance.is_valid()) {
			mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(p_geometry->mesh_instance, index, attributes, false, false, caster.vertex_array, format);
		} else {
			mesh_storage->mesh_surface_get_vertex_arrays_and_format(surface, attributes, false, false, caster.vertex_array, format);
		}
		caster.pipeline = caster.cutout >= 0 ? _pipeline_for(cutout_pipelines, cutout_rd_shader, format) : _pipeline_for(caster_pipelines, caster_rd_shader, format);
		caster.index_array = mesh_storage->mesh_surface_get_index_array(surface, 0);
		const bool compressed = mesh_storage->mesh_surface_get_format(surface) & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
		caster.decode_position = compressed ? local_bounds.position : Vector3();
		caster.decode_size = compressed ? local_bounds.size : Vector3(1, 1, 1);
		caster.triangles = mesh_storage->mesh_surface_get_vertices_drawn_count(surface) / 3;
		entry.surfaces.push_back(caster);
	}
	return entry;
}

// Стороны карт в бюджете атласа. Нужная по радиусу сторона уменьшается сначала у ламп вне кадра — они не видны, затем у мелких на экране,
// последними — у задетых предметом в руках и у лампы в руках. Другая сторона ставит лампу в очередь как обычное изменение.
void LocalLightsWorld::_plan_map_sides(double p_now) {
	if (!plan_dirty && p_now < next_plan_time) {
		return;
	}
	plan_dirty = false;
	next_plan_time = p_now + PLAN_PERIOD_SECONDS;
	_assign_queue_tiers();
	plan_order.clear();
	for (uint32_t i = 0; i < lamps.size(); i++) {
		if (lamps[i].active && lamps[i].casts) {
			plan_order.push_back(i);
		}
	}
	// Наименее важные первыми: вне кадра — дольше не виденные, в кадре — меньшие на экране. Номер лампы делает порядок однозначным.
	struct ByImportance {
		const LocalVector<Lamp> *lamps;
		int64_t key(uint32_t p_index) const {
			const Lamp &lamp = (*lamps)[p_index];
			if (lamp.queue_tier == QUEUE_TIER_HIDDEN) {
				return int64_t(lamp.last_visible_frame);
			}
			const int size_step = CLAMP(int(Math::floor(Math::log2(MAX(lamp.view_size, 1e-6f)) * VIEW_SIZE_STEPS_PER_OCTAVE)) + 512, 0, 1023);
			return (int64_t(1 + lamp.held_rank) << 52) + (int64_t(size_step) << 40);
		}
		bool operator()(uint32_t a, uint32_t b) const {
			const int64_t first = key(a);
			const int64_t second = key(b);
			return first != second ? first < second : a < b;
		}
	};
	SortArray<uint32_t, ByImportance> sorter;
	sorter.compare.lamps = &lamps;
	sorter.sort(plan_order.ptr(), plan_order.size());
	const uint64_t capacity = uint64_t(double(atlas_budget_rows) * ATLAS_WIDTH * plan_fill);
	_fit_sides(capacity);
	// Сторона лампы меняется не чаще выдержки в обе стороны: разворот камеры не гоняет карту туда-обратно. Уменьшение ставится сразу
	// после выдержки, а повышение — только в площадь, которая свободна уже сейчас: уменьшения ламп вне кадра идут в очереди последними,
	// и повышение раньше них не нашло бы места.
	uint64_t committed = 0;
	for (const uint32_t index : plan_order) {
		Lamp &lamp = lamps[index];
		const bool settled = p_now - lamp.planned_time >= PLAN_SIDE_DWELL_SECONDS;
		if (lamp.planned_side == 0 || (settled && lamp.fit_side < lamp.planned_side)) {
			lamp.planned_side = lamp.fit_side;
			lamp.planned_time = p_now;
		}
		committed += map_area(MAX(lamp.planned_side, lamp.map_side), lamp.spot);
	}
	const uint64_t upgrade_capacity = uint64_t(double(capacity) * PLAN_UPGRADE_CAPACITY);
	for (int64_t i = int64_t(plan_order.size()) - 1; i >= 0; i--) {
		Lamp &lamp = lamps[plan_order[i]];
		if (lamp.fit_side <= lamp.planned_side || p_now - lamp.planned_time < PLAN_SIDE_DWELL_SECONDS) {
			continue;
		}
		const uint64_t growth = map_area(lamp.fit_side, lamp.spot) - map_area(MAX(lamp.planned_side, lamp.map_side), lamp.spot);
		if (committed + growth <= upgrade_capacity) {
			committed += growth;
			lamp.planned_side = lamp.fit_side;
			lamp.planned_time = p_now;
		}
	}
	uint32_t reduced = 0;
	for (const uint32_t index : plan_order) {
		Lamp &lamp = lamps[index];
		if (lamp.planned_side != lamp.map_side && lamp.queued_order == 0) {
			lamp.queued_order = next_queue_order++;
			lamp.queued_frame = current_frame;
		}
		reduced += lamp.planned_side < _map_side_for(lamp) ? 1 : 0;
	}
	lamps_reduced = reduced;
}

// Стороны plan_order в пределах p_capacity текселей: лампы вне кадра уменьшаются сразу до наименьшей, лампы кадра — по ступени за проход,
// начиная с наименее важной.
void LocalLightsWorld::_fit_sides(uint64_t p_capacity) {
	uint64_t total = 0;
	for (const uint32_t index : plan_order) {
		Lamp &lamp = lamps[index];
		int &side = lamp.fit_side;
		side = _map_side_for(lamp);
		total += map_area(side, lamp.spot);
	}
	for (const uint32_t index : plan_order) {
		Lamp &lamp = lamps[index];
		if (total <= p_capacity || lamp.queue_tier != QUEUE_TIER_HIDDEN) {
			break;
		}
		int &side = lamp.fit_side;
		total -= map_area(side, lamp.spot) - map_area(MAP_SIDES[0], lamp.spot);
		side = MAP_SIDES[0];
	}
	bool reducing = true;
	while (total > p_capacity && reducing) {
		reducing = false;
		for (const uint32_t index : plan_order) {
			Lamp &lamp = lamps[index];
			int &side = lamp.fit_side;
			if (side <= MAP_SIDES[0]) {
				continue;
			}
			int smaller = MAP_SIDES[0];
			for (const int candidate : MAP_SIDES) {
				if (candidate < side) {
					smaller = candidate;
				}
			}
			total -= map_area(side, lamp.spot) - map_area(smaller, lamp.spot);
			side = smaller;
			reducing = true;
			if (total <= p_capacity) {
				return;
			}
		}
	}
}

void LocalLightsWorld::_serve_queue(uint64_t p_frame) {
	const LocalLightsSettings &settings = LocalLights::get_singleton()->get_settings();
	frame_metrics.queue = 0;
	_apply_map_sizes(settings);
	frame_time = RSG::rasterizer->get_total_time();
	_plan_map_sides(frame_time);
	const uint32_t budget = _order_queue(settings, p_frame);
	if (budget == 0) {
		return;
	}
	uint64_t started = OS::get_singleton()->get_ticks_usec();
	_gather_served_lamps(budget, settings, p_frame);
	frame_metrics.gather_usec += OS::get_singleton()->get_ticks_usec() - started;

	started = OS::get_singleton()->get_ticks_usec();
	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights maps");
	}
	for (uint32_t i = 0; i < budget; i++) {
		const Lamp &lamp = lamps[serve_order[i]];
		if (lamp.map_faces > 0) {
			_record_lamp(lamp);
		}
	}
	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights maps end");
	}
	frame_metrics.record_usec += OS::get_singleton()->get_ticks_usec() - started;

	_finish_served_lamps(budget, p_frame);
}

// Ждущие лампы вида и уменьшение карт ламп вне кадра в порядке обслуживания. Возвращает, сколько первых из них обслужить в этом кадре.
uint32_t LocalLightsWorld::_order_queue(const LocalLightsSettings &p_settings, uint64_t p_frame) {
	serve_order.clear();
	for (uint32_t i = 0; i < lamps.size(); i++) {
		const Lamp &lamp = lamps[i];
		if (!lamp.active || lamp.queued_order == 0) {
			continue;
		}
		const bool visible = lamp.last_visible_frame == p_frame;
		// Смена разрешения без изменения лампы ждёт выдержки: перерисовка ради стороны, которую нельзя менять, ничего не даёт.
		if ((visible && (lamp.needs_update || _side_change_allowed(lamp))) || (!visible && lamp.planned_side < lamp.map_side && _side_change_allowed(lamp))) {
			serve_order.push_back(i);
		}
	}
	if (serve_order.is_empty()) {
		return 0;
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
	const uint32_t budget = p_settings.update_cap > 0 && !bake_all_lamps ? MIN(uint32_t(p_settings.update_cap), serve_order.size()) : serve_order.size();
	_choose_lamps_to_serve(budget, p_frame);
	return budget;
}

void LocalLightsWorld::_gather_served_lamps(uint32_t p_budget, const LocalLightsSettings &p_settings, uint64_t p_frame) {
	cutout_pool.clear();
	lamp_casters.clear();
	gather_pass++;
	// Сначала уменьшения: они освобождают место, затем места крупным картам раньше мелких — так атлас плотнее.
	place_order.clear();
	for (uint32_t i = 0; i < p_budget; i++) {
		const Lamp &lamp = lamps[serve_order[i]];
		if (lamp.map_side != lamp.planned_side) {
			place_order.push_back(serve_order[i]);
		}
	}
	struct ByPlacement {
		const LocalVector<Lamp> *lamps;
		bool operator()(uint32_t a, uint32_t b) const {
			const Lamp &first = (*lamps)[a];
			const Lamp &second = (*lamps)[b];
			const bool first_shrinks = first.map_faces > 0 && first.planned_side < first.map_side;
			const bool second_shrinks = second.map_faces > 0 && second.planned_side < second.map_side;
			if (first_shrinks != second_shrinks) {
				return first_shrinks;
			}
			return map_area(first.planned_side, first.spot) > map_area(second.planned_side, second.spot);
		}
	};
	SortArray<uint32_t, ByPlacement> sorter;
	sorter.compare.lamps = &lamps;
	sorter.sort(place_order.ptr(), place_order.size());
	failed_place_sizes.clear();
	for (const uint32_t index : place_order) {
		Lamp &lamp = lamps[index];
		if (_side_change_allowed(lamp)) {
			_place_map(lamp, lamp.planned_side);
		}
	}
	_ensure_atlas_height();
	for (uint32_t i = 0; i < p_budget; i++) {
		Lamp &lamp = lamps[serve_order[i]];
		if (lamp.map_faces == 0) {
			continue;
		}
		const Instance *instance = scene_cull()->instance_owner.get_or_null(lamp.instance);
		ERR_CONTINUE(!instance);
		if (lamp.spot) {
			// Снимаемая поза считается один раз: она нужна отбору, записи и данным лампы.
			const SpotMapAxes axes = spot_map_axes(lamp.axis, lamp.angle);
			lamp.baked_right = axes.right;
			lamp.baked_up = axes.up;
			lamp.baked_forward = lamp.axis;
			lamp.baked_extent = axes.extent;
		}
		_gather_casters(lamp, instance, p_settings.caster_culling);
		if (lamp.needs_update) {
			frame_metrics.served_wait_frames = MAX(frame_metrics.served_wait_frames, uint32_t(p_frame - lamp.queued_frame));
		}
	}
	RendererRD::MeshStorage::get_singleton()->update_mesh_instances();
}

void LocalLightsWorld::_finish_served_lamps(uint32_t p_budget, uint64_t p_frame) {
	frame_metrics.lamps_served += p_budget;
	frame_metrics.queue = serve_order.size() - p_budget;
	for (uint32_t i = 0; i < p_budget; i++) {
		Lamp &lamp = lamps[serve_order[i]];
		frame_metrics.lamps_served_held += lamp.queue_tier == QUEUE_TIER_HELD ? 1 : 0;
		frame_metrics.lamps_served_influenced += lamp.queue_tier == QUEUE_TIER_INFLUENCED ? 1 : 0;
		if (lamp.map_faces == 0) {
			frame_metrics.queue++;
			continue;
		}
		lamp.queued_order = 0;
		lamp.needs_update = false;
		lamp.map_valid = true;
		lamp.baked_origin = lamp.position;
		lamp.baked_range = lamp.range;
		lamp.data_dirty = true;
		if (!lamp.in_grid && lamp.last_visible_frame == p_frame) {
			repack_needed = true;
		}
	}
	uint32_t oldest = 0;
	for (uint32_t i = p_budget; i < serve_order.size(); i++) {
		const Lamp &lamp = lamps[serve_order[i]];
		if (lamp.needs_update && lamp.last_visible_frame == p_frame) {
			oldest = MAX(oldest, uint32_t(p_frame - lamp.queued_frame));
		}
	}
	frame_metrics.oldest_wait_frames = MAX(frame_metrics.oldest_wait_frames, oldest);
}

void LocalLightsWorld::set_held_instances(const Vector<RID> &p_instances) {
	held_instances.clear();
	for (const RID &instance : p_instances) {
		held_instances.push_back(instance);
	}
}

// Близость к рукам игрока ставит ступень важности любой лампе, а очередь поднимает её только ради настоящего обновления:
// смена разрешения карты идёт после всех изменившихся ламп кадра.
void LocalLightsWorld::_assign_queue_tiers() {
	for (Lamp &lamp : lamps) {
		lamp.held_rank = 0;
	}
	for (const RID &held : held_instances) {
		const Instance *instance = scene_cull()->instance_owner.get_or_null(held);
		if (!instance) {
			continue;
		}
		if (instance->base_type == RSE::INSTANCE_LIGHT) {
			const uint32_t *index = lamp_of_instance.getptr(held);
			if (index) {
				lamps[*index].held_rank = 2;
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
			if (lamp.held_rank == 0 && nearest.distance_squared_to(lamp.position) <= lamp.range * lamp.range) {
				lamp.held_rank = 1;
			}
		}
	}
	for (Lamp &lamp : lamps) {
		if (lamp.last_visible_frame != current_frame) {
			lamp.queue_tier = QUEUE_TIER_HIDDEN;
		} else if (!lamp.needs_update) {
			lamp.queue_tier = QUEUE_TIER_RESIZE;
		} else {
			lamp.queue_tier = lamp.held_rank == 2 ? QUEUE_TIER_HELD : (lamp.held_rank == 1 ? QUEUE_TIER_INFLUENCED : QUEUE_TIER_OTHER);
		}
	}
}

// Движущийся предмет ставит свои лампы в очередь каждый кадр и мог бы занять весь cap: остальным всегда остаётся четверть.
// При cap 1 они идут через кадр, а при cap от 2 лампа в руках обновляется каждый кадр.
void LocalLightsWorld::_choose_lamps_to_serve(uint32_t p_budget, uint64_t p_frame) {
	uint32_t others_first = 0;
	while (others_first < serve_order.size() && lamps[serve_order[others_first]].queue_tier < QUEUE_TIER_OTHER) {
		others_first++;
	}
	uint32_t others_end = others_first;
	while (others_end < serve_order.size() && lamps[serve_order[others_end]].queue_tier == QUEUE_TIER_OTHER) {
		others_end++;
	}
	if (others_first == 0 || others_first == others_end || others_first <= p_budget / 2) {
		// Либо приоритетных нет, либо остальных в кадре нет, либо приоритетные и так оставляют им больше четверти.
		return;
	}
	uint32_t reserved = p_budget >= 2 ? MAX(1u, p_budget / 4) : (p_frame % 2);
	reserved = MIN(reserved, others_end - others_first);
	const uint32_t priority_served = MIN(others_first, p_budget - reserved);
	for (uint32_t i = 0; i < reserved && priority_served + i < others_first; i++) {
		SWAP(serve_order[priority_served + i], serve_order[others_first + i]);
	}
}

void LocalLightsWorld::_record_lamp(const Lamp &p_lamp) {
	const int side = p_lamp.map_side;
	const Rect2 region(p_lamp.map_origin, Vector2i(side * p_lamp.map_faces, side));
	RD::DrawListID list = rd->draw_list_begin(map_framebuffer, RD::DRAW_CLEAR_DEPTH, VectorView<Color>(), 1.0f, 0, region);
	RID bound_pipeline;
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
	const float extent = p_lamp.spot ? p_lamp.baked_extent : 1.0f + CUBE_GUARD_TEXELS / float(side);
	push.projection[0] = extent;
	push.projection[1] = extent;
	const LampCaster *casters = lamp_casters.ptr() + p_lamp.caster_first;
	for (int face = 0; face < p_lamp.map_faces; face++) {
		Basis rows;
		if (p_lamp.spot) {
			rows = rows_of(p_lamp.baked_right, p_lamp.baked_up, p_lamp.baked_forward);
		} else {
			// Ножницы остаются на всю карту лампы: за плитку не выходит и так, примитивы отсекаются окном вывода.
			rd->draw_list_set_viewport(list, Rect2(p_lamp.map_origin.x + face * side, p_lamp.map_origin.y, side, side));
			rows = rows_of(FACE_RIGHT[face], FACE_UP[face], FACE_FORWARD[face]);
		}
		const Transform3D light_from_world(rows, rows.xform(-p_lamp.position));
		const uint32_t face_bit = 1u << face;
		// Поверхности экземпляра идут подряд и делят одно преобразование.
		const Instance *transformed_instance = nullptr;
		for (uint32_t i = 0; i < p_lamp.caster_count; i++) {
			if (!(casters[i].face_mask & face_bit)) {
				frame_metrics.surfaces_culled++;
				continue;
			}
			const CasterSurface &caster = *casters[i].surface;
			frame_metrics.surfaces_drawn++;
			frame_metrics.triangles_drawn += caster.triangles;
			if (caster.pipeline != bound_pipeline) {
				rd->draw_list_bind_render_pipeline(list, caster.pipeline);
				bound_pipeline = caster.pipeline;
			}
			if (caster.cutout >= 0) {
				_draw_cutout_surface(list, caster, light_from_world, extent, push.extra);
				frame_metrics.cutout_surfaces_drawn++;
				continue;
			}
			rd->draw_list_bind_vertex_array(list, caster.vertex_array);
			if (caster.index_array.is_valid()) {
				rd->draw_list_bind_index_array(list, caster.index_array);
			}
			if (caster.instance != transformed_instance) {
				RendererRD::MaterialStorage::store_transform(light_from_world * caster.instance->transform, push.to_light);
				transformed_instance = caster.instance;
			}
			for (int axis = 0; axis < 3; axis++) {
				push.decode_position[axis] = caster.decode_position[axis];
				push.decode_size[axis] = caster.decode_size[axis];
			}
			rd->draw_list_set_push_constant(list, &push, sizeof(CasterPush));
			rd->draw_list_draw(list, caster.index_array.is_valid(), 1);
		}
		frame_metrics.faces_baked++;
	}
	rd->draw_list_end();
}

void LocalLightsWorld::_update_grid() {
	uint32_t dark_in_view = 0;
	for (Lamp &lamp : lamps) {
		if (!lamp.active) {
			continue;
		}
		const bool wanted = lamp.last_visible_frame + 1 >= current_frame && (!lamp.casts || lamp.map_valid) && !lamp.cells.is_empty();
		if (wanted != lamp.in_grid) {
			lamp.in_grid = wanted;
			repack_needed = true;
		}
		dark_in_view += lamp.last_visible_frame == current_frame && !wanted ? 1 : 0;
	}
	frame_metrics.lamps_dark_in_view = dark_in_view;
	frame_metrics.lamps_reduced = lamps_reduced;
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
		// Проход отношения читает буферы только до конца списка и до последней лампы: хвост не загружается.
		rd->buffer_update(list_buffer, 0, list_used * sizeof(float), list_bytes.ptr());
		grid_changed = false;
		frame_metrics.grid_uploads++;
		frame_metrics.uploaded_bytes += grid_bytes.size() + list_bytes.size() + list_used * sizeof(float);
	}
	if (data_changed) {
		rd->texture_update(data_rd, 0, data_bytes);
		const uint32_t data_used = MIN(uint32_t(data_bytes.size()), lamps.size() * SLOT_TEXELS * BYTES_PER_DATA_TEXEL);
		rd->buffer_update(data_buffer, 0, data_used, data_bytes.ptr());
		data_changed = false;
		frame_metrics.data_uploads++;
		frame_metrics.uploaded_bytes += data_bytes.size() + data_used;
	}
}

// В блоке ячейки сначала Omni, затем Spot: материал обходит их разными циклами.
void LocalLightsWorld::_repack() {
	repack_needed = false;
	frame_metrics.repacks++;
	float *grid = reinterpret_cast<float *>(grid_bytes.ptrw());
	// Остальные ячейки уже пусты: стирается только заполненное прошлой раскладкой.
	for (uint32_t cell : filled_cells) {
		cell_omni[cell] = 0;
		cell_spot[cell] = 0;
		grid[cell * 4] = 0.0f;
		grid[cell * 4 + 1] = 0.0f;
		grid[cell * 4 + 2] = 0.0f;
	}
	filled_cells.clear();
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
	list_used = MAX(1u, list_end);
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
		if (list_buffer.is_valid()) {
			rd->free_rid(list_buffer);
		}
		list_buffer = rd->storage_buffer_create(list_bytes.size());
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
	if (data_buffer.is_valid()) {
		rd->free_rid(data_buffer);
	}
	data_buffer = rd->storage_buffer_create(data_bytes.size());
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
		values[23] = lamp.spot ? 1.0f / lamp.baked_extent : 0.0f;
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
	materials->global_shader_parameter_set("local_light_inverse_cell_size", 1.0f / cell_size);
	materials->global_shader_parameter_set("local_light_dims", cell_dims);
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
	params.origin_and_step[3] = cell_size;
	params.dims[0] = cell_dims.x;
	params.dims[1] = cell_dims.y;
	params.dims[2] = cell_dims.z;
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
	RID ratio_set = uniform_sets->get_cache(ratio_rd_shader, 0, sampled_uniform(0, sampler, depth), sampled_uniform(1, sampler, grid_rd), storage_uniform(2, data_buffer),
			sampled_uniform(3, sampler, map_atlas.is_valid() ? map_atlas : no_atlas), storage_uniform(4, list_buffer), buffer_uniform(5, params_buffer), image_uniform(7, ratio_surface));
	RD::ComputeListID compute = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute, ratio_pipeline);
	rd->compute_list_bind_uniform_set(compute, ratio_set, 0);
	rd->compute_list_dispatch(compute, groups.x, groups.y, 1);
	rd->compute_list_end();

	if (settings.gpu_timestamps) {
		rd->capture_timestamp("LocalLights blur");
	}
	RID blur_set = uniform_sets->get_cache(blur_rd_shader, 0, image_uniform(1, ratio_surface), image_uniform(2, blurred_ratio), buffer_uniform(5, params_buffer));
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
	report["mapBudgetBytes"] = uint64_t(ATLAS_WIDTH) * atlas_budget_rows * 2;
	report["lampsReduced"] = lamps_reduced;
	report["atlasOccupiedBytes"] = atlas_used_area * 2;
	report["screenBytes"] = uint64_t(screen_size.x) * screen_size.y * (8 + 4);
	report["gridBytes"] = uint64_t(grid_bytes.size());
	report["dataBytes"] = uint64_t(data_bytes.size()) + uint64_t(list_bytes.size());
	report["lampBufferBytes"] = uint64_t(data_bytes.size()) + uint64_t(list_bytes.size());
	uint64_t cpu = uint64_t(grid_bytes.size()) + uint64_t(data_bytes.size()) + uint64_t(list_bytes.size());
	cpu += lamps.get_capacity() * sizeof(Lamp);
	for (const Lamp &lamp : lamps) {
		cpu += lamp.cells.get_capacity() * sizeof(uint32_t);
	}
	cpu += (cell_omni.get_capacity() + cell_spot.get_capacity() + cell_next.get_capacity() + filled_cells.get_capacity() + serve_order.get_capacity()) * sizeof(uint32_t);
	cpu += cutout_pool.get_capacity() * sizeof(CasterCutout) + instance_casters.get_capacity() * sizeof(InstanceCasters) + lamp_casters.get_capacity() * sizeof(LampCaster) + atlas_free_cells.get_capacity() * sizeof(uint64_t);
	cpu += (plan_order.get_capacity() + place_order.get_capacity()) * sizeof(uint32_t);
	for (const InstanceCasters &entry : instance_casters) {
		cpu += entry.surfaces.get_capacity() * sizeof(CasterSurface);
	}
	report["cpuBytes"] = cpu;
	return report;
}

// Карта каждой лампы для проверки бюджета атласа: сторона, нужная по радиусу сторона, начало места, число граней, сторона по плану
// и попадание в кадр последнего вида.
Dictionary LocalLightsWorld::read_map_sides() const {
	Dictionary result;
	for (const Lamp &lamp : lamps) {
		if (lamp.active && lamp.casts) {
			result[lamp.instance] = PackedInt32Array({ lamp.map_side, _map_side_for(lamp), lamp.map_origin.x, lamp.map_origin.y, lamp.map_faces, lamp.planned_side, lamp.last_visible_frame == current_frame ? 1 : 0 });
		}
	}
	return result;
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
