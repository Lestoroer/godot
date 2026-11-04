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
	uint32_t queue = 0;
	uint32_t oldest_wait_frames = 0;
	uint32_t served_wait_frames = 0;
	uint32_t faces_baked = 0;
	uint32_t surfaces_drawn = 0;
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
		float shadow_bias = 0.0f;
		float shadow_normal_bias = 0.0f;
		uint32_t caster_mask = 0;
		LocalVector<uint32_t> cells;
		LocalVector<uint32_t> cell_slots;
		bool in_grid = false;
		bool data_dirty = true;
		uint64_t last_visible_frame = 0;
		// Очередь: кадр постановки и порядок прихода, ноль — не ждёт.
		uint64_t queued_order = 0;
		uint64_t queued_frame = 0;
		// Карта: место в атласе и снятая поза; map_faces == 0 — места нет.
		Vector2i map_origin;
		int map_side = 0;
		int map_faces = 0;
		bool map_valid = false;
		Vector3 baked_origin;
		float baked_inverse_range = 0.0f;
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
	LocalVector<uint32_t> serve_order;

	// Сетка: окно по миру и блоки ламп по ячейкам.
	Vector3 origin;
	Vector3i origin_cell;
	bool origin_valid = false;
	Vector3 window_center;
	bool repack_needed = false;
	bool grid_changed = false;
	bool data_changed = false;
	uint32_t data_rows = 0;
	Vector<uint8_t> grid_bytes;
	Vector<uint8_t> data_bytes;
	LocalVector<uint32_t> cell_omni;
	LocalVector<uint32_t> cell_spot;
	LocalVector<uint32_t> cell_next;
	LocalVector<uint32_t> filled_cells;
	RID grid_rd;
	RID data_rd;
	RID grid_texture;
	RID data_texture;

	// Экранные буферы: один набор по наибольшему вьюпорту, каждый вид перезаписывает его перед своим проходом.
	Size2i screen_size;
	RID blur_radius;
	RID ratio_and_normal;
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
	RID _pipeline_for(RD::VertexFormatID p_format);
	void _gather_casters(Lamp &r_lamp, const Instance *p_instance);
	void _record_lamp(const Lamp &p_lamp, const LocalLightsSettings &p_settings);
	void _update_grid();
	void _repack();
	void _write_lamp_data(const Lamp &p_lamp);
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

	void set_enabled(bool p_enabled);
	Dictionary memory_report() const;
	Dictionary read_screen_buffers() const;
};
