// lestoroer#local-lights
#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"

#include "shaders/local_light_blur.glsl.gen.h"
#include "shaders/local_light_caster.glsl.gen.h"
#include "shaders/local_light_ratio.glsl.gen.h"

struct LocalLightsSettings {
	// Целых ламп, чьи карты обновляются за кадр; 0 — все.
	int update_cap = 16;
	float softness_per_meter = 0.04f;
	float several_lamps_growth = 3.0f;
	bool show_hard_mask = false;
	int fixed_noise_frame = -1;
	bool caster_culling = true;
	// Наибольшие стороны плитки; меньшие выбираются по радиусу лампы так, чтобы тексель на границе радиуса был не крупнее map_texel_size.
	int omni_face_size = 256;
	int spot_tile_size = 512;
	float map_texel_size = 0.015f;
	bool gpu_timestamps = false;
};

// Значения одного кадра, времена в микросекундах.
struct LocalLightsFrameMetrics {
	uint32_t views = 0;
	uint32_t lamps_in_view = 0;
	uint32_t lamps_in_grid = 0;
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
	uint64_t screen_usec = 0;
};

// Omni и Spot одного мира: карты теней, сетка и данные для материалов, проходы отношения и размытия. Вся работа идёт на потоке
// рендера внутри кадра: лампы вида приходят из кулла камеры, изменения — из флага тени лампы, который ставит сцена.
class LocalLightsWorld final : public RendererSceneCull::LocalLightsBackend {
	typedef RendererSceneCull::Instance Instance;
	typedef RendererSceneCull::InstanceLightData InstanceLightData;

	// Порядок очереди: лампа в руках, лампы, на которые влияет предмет в руках, остальные.
	enum QueueTier : uint8_t {
		QUEUE_TIER_HELD,
		QUEUE_TIER_INFLUENCED,
		QUEUE_TIER_OTHER,
	};

	struct Lamp {
		RID instance;
		bool active = false;
		bool spot = false;
		bool casts = false;
		Vector3 position;
		Vector3 axis;
		Transform3D transform;
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
		// Очередь: кадр постановки и порядок прихода, ноль — не ждёт.
		uint64_t queued_order = 0;
		uint64_t queued_frame = 0;
		QueueTier queue_tier = QUEUE_TIER_OTHER;
		// Карта: место в атласе и снятая поза; map_faces == 0 — места нет.
		Vector2i map_origin;
		int map_side = 0;
		int map_faces = 0;
		bool map_valid = false;
		Vector3 baked_origin;
		float baked_range = 0.0f;
		Vector3 baked_up;
		Vector3 baked_forward;
		float baked_inverse_tangent = 0.0f;
		// Кастеры текущего снимка в общем пуле.
		uint32_t caster_first = 0;
		uint32_t caster_count = 0;
	};

	struct CasterSurface {
		const Instance *instance = nullptr;
		AABB bounds;
		RID vertex_array;
		RID index_array;
		RD::VertexFormatID format = 0;
		Vector3 decode_position;
		Vector3 decode_size;
		uint32_t triangles = 0;
		// Индекс в cutout_pool, минус один — материал без выреза.
		int32_t cutout = -1;
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

	// Отрезок caster_pool с поверхностями экземпляра: лампы видят одни и те же предметы, поверхности собираются один раз за обход очереди.
	struct InstanceCasters {
		uint64_t gather_pass = 0;
		uint32_t first = 0;
		uint32_t count = 0;
	};

	// Полка атласа: места одной высоты слева направо.
	struct MapShelf {
		int y = 0;
		int height = 0;
		int next_x = 0;
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

	struct ScreenParams {
		float inverse_projection[16];
		float projection[16];
		float view_to_world[16];
		float origin_and_step[4];
		int32_t dims[4];
		float blur[4];
		int32_t view_size[4];
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
	LocalLightRatioShaderRD ratio_shader;
	RID ratio_shader_version;
	RID ratio_rd_shader;
	RID ratio_pipeline;
	LocalLightBlurShaderRD blur_shader;
	RID blur_shader_version;
	RID blur_rd_shader;
	RID blur_pipeline;
	RID params_buffer;

	// Общий атлас карт. Ширина постоянна, высота растёт небольшими шагами с копией снятых карт; у Omni на лампу шесть плиток подряд.
	RID map_atlas;
	RID map_atlas_texture;
	RID map_framebuffer;
	int atlas_height = 0;
	int atlas_used_height = 0;
	int omni_size = 0;
	int spot_size = 0;
	float map_texel_size = -1.0f;
	LocalVector<MapShelf> map_shelves;
	HashMap<uint32_t, LocalVector<Vector2i>> free_map_rects;
	LocalVector<CasterSurface> caster_pool;
	LocalVector<CasterCutout> cutout_pool;
	// По локальному индексу RID экземпляра.
	LocalVector<InstanceCasters> instance_casters;
	uint64_t gather_pass = 0;
	// Индексы caster_pool по лампам: отрезок лампы — caster_first и caster_count.
	LocalVector<uint32_t> lamp_casters;
	LocalVector<uint32_t> lamp_cutout_casters;
	// Экземпляры предмета в руках: лампы и геометрия.
	LocalVector<RID> held_instances;
	// Следующий кадр снимает тени всех ламп мира: этап загрузки.
	bool bake_all_lamps = false;
	LocalVector<Instance *> scenario_lamps;
	LocalVector<uint32_t> serve_order;

	// Сетка: окно по миру и список ламп ячеек; таблица ламп: одна строка данных на лампу, по её индексу в lamps.
	Vector3 origin;
	bool origin_valid = false;
	Vector3 window_center;
	bool repack_needed = false;
	bool grid_changed = false;
	bool data_changed = false;
	uint32_t data_rows = 0;
	uint32_t list_rows = 0;
	Vector<uint8_t> grid_bytes;
	Vector<uint8_t> data_bytes;
	Vector<uint8_t> list_bytes;
	LocalVector<uint32_t> cell_omni;
	LocalVector<uint32_t> cell_spot;
	LocalVector<uint32_t> cell_next;
	LocalVector<uint32_t> filled_cells;
	RID grid_rd;
	RID data_rd;
	RID list_rd;
	RID grid_texture;
	RID data_texture;
	RID list_texture;

	// Экранные буферы: один набор по наибольшему вьюпорту, каждый вид перезаписывает его перед своим проходом.
	Size2i screen_size;
	RID ratio_surface;
	RID blurred_ratio;
	RID blurred_ratio_texture;
	bool globals_published = false;
	// Материалы кадра уже связаны со старой текстурой и получат новую только в начале следующего кадра.
	LocalVector<RID> retired_rd_textures;
	LocalVector<RID> retired_wrapped_textures;

	LocalLightsFrameMetrics frame_metrics;

	void _begin_frame(uint64_t p_frame);
	void _take_view_lamps(const LocalVector<Instance *> &p_lights, uint64_t p_frame);
	void _refresh_lamp(Lamp &r_lamp, const Instance *p_instance, uint64_t p_frame);
	void _compute_cells(Lamp &r_lamp, LocalVector<uint32_t> &r_cells) const;
	void _update_window();
	void _release_map(Lamp &r_lamp);
	void _release_lamp(uint32_t p_index);
	void _serve_queue(uint64_t p_frame);
	void _apply_map_sizes(const LocalLightsSettings &p_settings);
	int _map_side_for(const Lamp &p_lamp) const;
	bool _place_map(Lamp &r_lamp, int p_side);
	void _free_map_place(const Lamp &p_lamp);
	void _ensure_atlas_height();
	RID _pipeline_for(HashMap<RD::VertexFormatID, RID> &r_pipelines, RID p_shader, RD::VertexFormatID p_format);
	bool _read_cutout(RID p_material, const void *p_shader_data, void *p_surface, CasterCutout &r_cutout) const;
	void _draw_cutout_surface(RD::DrawListID p_list, const CasterSurface &p_caster, const Transform3D &p_light_from_world, float p_extent, const float *p_depth);
	void _collect_scenario_lamps();
	void _assign_queue_tiers();
	void _choose_lamps_to_serve(uint32_t p_budget, uint64_t p_frame);
	void _gather_casters(Lamp &r_lamp, const Instance *p_instance);
	const InstanceCasters &_instance_surfaces(const Instance *p_geometry);
	void _record_lamp(const Lamp &p_lamp, const LocalLightsSettings &p_settings);
	void _update_grid();
	void _repack();
	void _ensure_data_table();
	void _write_lamp_data(uint32_t p_index);
	void _publish_globals();
	void _ensure_screen_textures(const Size2i &p_size);
	RID _wrap_texture(RID p_rd_texture, RSE::TextureLayeredType p_type) const;
	void _free_wrapped(RID &r_texture) const;
	void _retire_texture(RID &r_rd_texture, RID &r_wrapped);
	void _free_retired_textures();

public:
	explicit LocalLightsWorld(RID p_scenario);
	~LocalLightsWorld() override;

	virtual bool is_enabled() const override { return enabled; }
	virtual void prepare_view(const LocalVector<Instance *> &p_lights) override;
	virtual void render_screen_passes(RenderDataRD *p_render_data) override;

	void set_held_instances(const Vector<RID> &p_instances);
	void request_bake_all_lamps() { bake_all_lamps = true; }
	void set_enabled(bool p_enabled);
	Dictionary memory_report() const;
	Dictionary read_screen_buffers() const;
};
