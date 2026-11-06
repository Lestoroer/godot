// lestoroer#local-lights
#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"

#include "shaders/local_light_caster.glsl.gen.h"

struct LocalLightsSettings {
	// Целых ламп, чьи карты обновляются за кадр; 0 — все.
	int update_cap = 16;
	bool caster_culling = true;
	// Наибольшие стороны плитки; меньшие выбираются по радиусу лампы так, чтобы тексель на границе радиуса был не крупнее map_texel_size.
	int omni_face_size = 256;
	int spot_tile_size = 512;
	float map_texel_size = 0.015f;
	// Атлас карт не больше бюджета: при нехватке места снижается разрешение карт наименее важных ламп.
	float map_budget_mib = 128.0f;
	bool gpu_timestamps = false;
};

// Значения одного кадра, времена в микросекундах.
struct LocalLightsFrameMetrics {
	uint32_t views = 0;
	uint32_t lamps_in_view = 0;
	uint32_t lamps_in_grid = 0;
	// Лампы кадра без света: нет готовой карты.
	uint32_t lamps_dark_in_view = 0;
	// Лампы с картой меньше нужной по радиусу: бюджет атласа.
	uint32_t lamps_reduced = 0;
	uint32_t map_resizes = 0;
	uint32_t map_place_failures = 0;
	// Неудачи, когда свободной площади в бюджете хватало: место раздроблено.
	uint32_t map_fragmented_failures = 0;
	uint32_t lamps_served = 0;
	uint32_t lamps_served_held = 0;
	uint32_t lamps_served_influenced = 0;
	uint32_t queue = 0;
	uint32_t oldest_wait_frames = 0;
	uint32_t served_wait_frames = 0;
	uint32_t faces_baked = 0;
	uint32_t surfaces_drawn = 0;
	uint32_t cutout_surfaces_drawn = 0;
	uint32_t surfaces_culled = 0;
	uint64_t triangles_drawn = 0;
	uint32_t repacks = 0;
	uint32_t grid_uploads = 0;
	uint32_t data_uploads = 0;
	uint64_t uploaded_bytes = 0;
	uint64_t track_usec = 0;
	uint64_t gather_usec = 0;
	uint64_t record_usec = 0;
	uint64_t grid_usec = 0;
};

// Omni и Spot одного мира: карты теней, сетка и данные для материалов. Вся работа идёт на потоке
// рендера внутри кадра: лампы вида приходят из кулла камеры, изменения — из флага тени лампы, который ставит сцена.
class LocalLightsWorld final : public RendererSceneCull::LocalLightsBackend {
	typedef RendererSceneCull::Instance Instance;
	typedef RendererSceneCull::InstanceLightData InstanceLightData;

	// Порядок очереди: лампа в руках, лампы, на которые влияет предмет в руках, остальные изменившиеся в кадре; затем смена разрешения карт
	// ламп кадра под бюджет, последним — уменьшение карт ламп вне кадра.
	enum QueueTier : uint8_t {
		QUEUE_TIER_HELD,
		QUEUE_TIER_INFLUENCED,
		QUEUE_TIER_OTHER,
		QUEUE_TIER_RESIZE,
		QUEUE_TIER_HIDDEN,
	};

	struct Lamp {
		RID instance;
		bool active = false;
		bool spot = false;
		bool casts = false;
		Vector3 position;
		Vector3 axis;
		float range = 0.0f;
		float angle = 0.0f;
		Color linear_color;
		float energy = 0.0f;
		float decay = 0.0f;
		float cone_attenuation = 0.0f;
		float specular_amount = 0.0f;
		uint32_t caster_mask = 0;
		LocalVector<uint32_t> cells;
		bool in_grid = false;
		bool data_dirty = true;
		uint64_t last_visible_frame = 0;
		// Размер на экране: радиус к расстоянию от камеры за вычетом радиуса.
		float view_size = 0.0f;
		// Очередь: кадр постановки и порядок прихода, ноль — не ждёт.
		uint64_t queued_order = 0;
		uint64_t queued_frame = 0;
		QueueTier queue_tier = QUEUE_TIER_OTHER;
		// В очереди ради изменения лампы или её геометрии, а не только ради разрешения карты.
		bool needs_update = false;
		// Лампа в руках — 2, задетая предметом в руках — 1.
		uint8_t held_rank = 0;
		// Сторона карты по плану бюджета, не больше нужной по радиусу, и время её последней смены; ноль — плана ещё не было.
		int planned_side = 0;
		double planned_time = 0.0;
		// Сторона, которую план даёт в пределах ёмкости бюджета.
		int fit_side = 0;
		// Карта: место в атласе, время последней смены её стороны и снятая поза; map_faces == 0 — места нет.
		Vector2i map_origin;
		int map_side = 0;
		double map_changed_time = 0.0;
		// Лампа без карты, которой не нашлось места даже для наименьшей, ждёт до этого времени.
		double place_retry_time = 0.0;
		// Кадр, в котором карта получила новое место или сторону.
		uint64_t resized_frame = UINT64_MAX;
		int map_faces = 0;
		bool map_valid = false;
		Vector3 baked_origin;
		float baked_range = 0.0f;
		Vector3 baked_right;
		Vector3 baked_up;
		Vector3 baked_forward;
		// Тангенс половины угла снимка Spot.
		float baked_extent = 0.0f;
		// Кастеры текущего снимка в общем пуле.
		uint32_t caster_first = 0;
		uint32_t caster_count = 0;
	};

	struct CasterSurface {
		const Instance *instance = nullptr;
		AABB bounds;
		RID vertex_array;
		RID index_array;
		RID pipeline;
		Vector3 decode_position;
		Vector3 decode_size;
		uint32_t triangles = 0;
		// Индекс в cutout_pool, минус один — материал без выреза.
		int32_t cutout = -1;
	};

	// Поверхность в снимке лампы и грани, куда она попадает, по биту на грань.
	struct LampCaster {
		const CasterSurface *surface = nullptr;
		uint32_t face_mask = 0;
	};

	// Что вырезу нужно из материала и поверхности.
	struct CasterCutout {
		RID uniform_set;
		float threshold = 0.0f;
		float alpha = 1.0f;
		bool vertex_alpha = false;
		Vector2 mesh_uv_decode;
		Vector2 uv_scale;
		Vector2 uv_offset;
	};

	// Поверхности экземпляра. Лампы видят одни и те же предметы, поэтому собираются они один раз и живут, пока сцена не поднимет версию экземпляра.
	struct InstanceCasters {
		RID instance;
		uint64_t version = 0;
		uint64_t gather_pass = 0;
		// Кадр деформации меняет буфер вершин, а обычный параметр материала — вырез, и оба не поднимают версию экземпляра.
		bool collect_every_pass = false;
		LocalVector<CasterSurface> surfaces;
	};

	struct CasterPush {
		float to_light[16];
		float projection[4];
		float extra[4];
		float decode_position[4];
		float decode_size[4];
	};

	// Раскладка варианта MAP_CUTOUT в local_light_caster.glsl.
	struct CutoutPush {
		float to_light[12];
		float projection_and_cut[4];
		float extra[4];
		float decode_position[4];
		float decode_size[4];
		float uv_transform[4];
	};

	RID scenario;
	bool enabled = true;
	bool refresh_all = true;
	RD *rd = nullptr;

	LocalVector<Lamp> lamps;
	LocalVector<uint32_t> free_lamps;
	HashMap<RID, uint32_t> lamp_of_instance;
	uint64_t current_frame = UINT64_MAX;
	uint64_t next_queue_order = 1;

	LocalLightCasterShaderRD caster_shader;
	RID caster_shader_version;
	RID caster_rd_shader;
	HashMap<RD::VertexFormatID, RID> caster_pipelines;
	RID cutout_rd_shader;
	HashMap<RD::VertexFormatID, RID> cutout_pipelines;

	// Общий атлас карт. Ширина постоянна, высота растёт небольшими шагами с копией снятых карт; у Omni на лампу шесть плиток подряд.
	RID map_atlas;
	RID map_atlas_texture;
	RID map_framebuffer;
	int atlas_height = 0;
	int atlas_used_height = 0;
	// Высота атласа, которую даёт бюджет.
	int atlas_budget_rows = 0;
	int omni_size = 0;
	int spot_size = 0;
	float map_texel_size = -1.0f;
	// Карта занятости атласа: по биту на клетку, единица — свободно; строка клеток — ATLAS_ROW_WORDS слов.
	LocalVector<uint64_t> atlas_free_cells;
	uint64_t atlas_used_area = 0;
	// План сторон карт: пересчёт при смене набора ламп или их радиусов, при неудачном размещении и по времени, пока движется камера.
	bool plan_dirty = true;
	double next_plan_time = 0.0;
	// Доля бюджета под оценку площади: меньше после плана, за которым размещению не хватило площади.
	float plan_fill = 0.0f;
	bool place_shortage = false;
	// Площадь карт, которую последний план оставил за лампами, тексели.
	uint64_t plan_committed_area = 0;
	uint64_t atlas_peak_bytes = 0;
	LocalVector<uint32_t> plan_order;
	uint32_t lamps_reduced = 0;
	LocalVector<uint32_t> place_order;
	// Стороны мест, которых в этом кадре не нашлось.
	LocalVector<Vector2i> failed_place_sizes;
	Vector3 camera_position;
	// Время кадра по монотонным часам, секунды: выдержки плана не зависят от частоты кадров и масштаба времени игры.
	double frame_time = 0.0;
	// Вырезы текущего обхода очереди.
	LocalVector<CasterCutout> cutout_pool;
	// По локальному индексу RID экземпляра.
	LocalVector<InstanceCasters> instance_casters;
	uint64_t gather_pass = 0;
	// Поверхности в снимках ламп: отрезок лампы — caster_first и caster_count.
	LocalVector<LampCaster> lamp_casters;
	LocalVector<LampCaster> lamp_cutout_casters;
	// Экземпляры предмета в руках: лампы и геометрия.
	LocalVector<RID> held_instances;
	// Следующий кадр снимает тени всех ламп мира: этап загрузки.
	bool bake_all_lamps = false;
	LocalVector<Instance *> scenario_lamps;
	LocalVector<uint32_t> serve_order;

	// Сетка: окно по миру и список ламп ячеек; таблица ламп: восемь текселей на лампу, по её индексу в lamps.
	Vector3 origin;
	float cell_size = 1.6f;
	Vector3i cell_dims;
	bool origin_valid = false;
	Vector3 window_center;
	// Оси, на которые окна не хватило, по биту на ось.
	uint8_t window_narrow_axes = 0;
	bool repack_needed = true;
	bool grid_changed = false;
	bool data_changed = false;
	uint32_t data_rows = 0;
	uint32_t list_rows = 0;
	// Записей в списке ламп ячеек после последней раскладки.
	Vector<uint8_t> grid_bytes;
	Vector<uint8_t> data_bytes;
	Vector<uint8_t> list_bytes;
	LocalVector<uint32_t> cell_omni;
	LocalVector<uint32_t> cell_spot;
	LocalVector<uint32_t> cell_next;
	LocalVector<uint32_t> filled_cells;
	// Ячейки обновляемой лампы до сравнения с её текущими: лампа в движении считает их каждый кадр.
	LocalVector<uint32_t> fresh_cells;
	RID grid_rd;
	RID data_rd;
	RID list_rd;
	RID grid_texture;
	RID data_texture;
	RID list_texture;

	bool globals_published = false;
	// Материалы кадра уже связаны со старой текстурой и получат новую только в начале следующего кадра.
	LocalVector<RID> retired_rd_textures;
	LocalVector<RID> retired_wrapped_textures;

	LocalLightsFrameMetrics frame_metrics;

	void _begin_frame(uint64_t p_frame);
	void _take_view_lamps(const LocalVector<Instance *> &p_lights, uint64_t p_frame);
	void _plan_map_sides(double p_now);
	void _fit_sides(uint64_t p_capacity);
	bool _refresh_lamp(Lamp &r_lamp, const Instance *p_instance, uint64_t p_frame);
	void _compute_cells(Lamp &r_lamp, LocalVector<uint32_t> &r_cells) const;
	void _update_window();
	bool _window_holds(const AABB &p_bounds) const;
	void _resize_grid(const Vector3i &p_dims);
	void _release_map(Lamp &r_lamp);
	void _release_lamp(uint32_t p_index);
	void _serve_queue(uint64_t p_frame);
	uint32_t _order_queue(const LocalLightsSettings &p_settings, uint64_t p_frame);
	// Возвращает, сколько обслуживаемых ламп осталось после отказа от пустых перерисовок.
	uint32_t _gather_served_lamps(uint32_t p_budget, const LocalLightsSettings &p_settings, uint64_t p_frame);
	void _finish_served_lamps(uint32_t p_budget, uint64_t p_frame);
	void _apply_map_sizes(const LocalLightsSettings &p_settings);
	int _map_side_for(const Lamp &p_lamp) const;
	bool _place_map(Lamp &r_lamp, int p_side);
	bool _side_change_allowed(const Lamp &p_lamp) const;
	bool _map_differs_from_plan(const Lamp &p_lamp) const;
	bool _changed_out_of_view(const Lamp &p_lamp) const;
	bool _allocate_map_rect(int p_width, int p_height, int p_highest_row, Vector2i &r_origin);
	void _mark_map_rect(const Vector2i &p_origin, int p_width, int p_height, bool p_free);
	void _ensure_atlas_height();
	RID _pipeline_for(HashMap<RD::VertexFormatID, RID> &r_pipelines, RID p_shader, RD::VertexFormatID p_format);
	bool _read_cutout(RID p_material, const void *p_shader_data, void *p_surface, CasterCutout &r_cutout) const;
	void _draw_cutout_surface(RD::DrawListID p_list, const CasterSurface &p_caster, const Transform3D &p_light_from_world, float p_extent, const float *p_depth);
	void _collect_scenario_lamps();
	void _assign_queue_tiers();
	void _choose_lamps_to_serve(uint32_t p_budget, uint64_t p_frame);
	void _gather_casters(Lamp &r_lamp, const Instance *p_instance, bool p_cull);
	const InstanceCasters &_instance_surfaces(const Instance *p_geometry);
	void _record_lamp(const Lamp &p_lamp);
	void _update_grid();
	void _repack();
	void _ensure_data_table();
	void _write_lamp_data(uint32_t p_index);
	void _publish_globals();
	RID _wrap_texture(RID p_rd_texture, RSE::TextureLayeredType p_type) const;
	void _free_wrapped(RID &r_texture) const;
	void _retire_texture(RID &r_rd_texture, RID &r_wrapped);
	void _free_retired_textures();

public:
	explicit LocalLightsWorld(RID p_scenario);
	~LocalLightsWorld() override;

	virtual bool is_enabled() const override { return enabled; }
	virtual void prepare_view(const LocalVector<Instance *> &p_lights, const Vector3 &p_camera_position) override;

	void set_held_instances(const Vector<RID> &p_instances);
	void request_bake_all_lamps() { bake_all_lamps = true; }
	void set_enabled(bool p_enabled);
	Dictionary memory_report() const;
	Dictionary read_map_sides() const;
};
