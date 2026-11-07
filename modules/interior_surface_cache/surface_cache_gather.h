// lestoroer#gi
#pragma once
#include "surface_cache_runtime.h"

class SurfaceCacheGatherRD : public RefCounted {
	GDCLASS(SurfaceCacheGatherRD, RefCounted);

public:
	struct Motion {
		Transform3D from, to;
		AABB bounds;
	};
	// Изменения мира для транспорта мировых зондов: номер набора (растёт с каждым обновлением мира), рамки изменившейся
	// геометрии этого набора и поколение источников-прямоугольников.
	struct Transport {
		uint64_t serial;
		const LocalVector<AABB> &changes;
		uint32_t emitter_generation;
	};
	// Рамок изменений, которые вид получает за кадр (остальные объединяются), и событий журнала сбора.
	static constexpr int TRANSPORT_CHANGES = 16, TRANSPORT_LOG = 256;
	// Лимит ограничивает проверки на пиксель; объединение сохраняет кадр самого свежего изменения.
	enum { VIEW_CHANGES = 4,
		VIEW_MOVED = 8 };
	// Сколько секунд вид держит область изменения: столько отражение пути через неё укорочено.
	static constexpr double CHANGE_SECONDS = 0.13;
	// Биты поля профиля disabled_blocks в порядке его флагов (surface_cache_profile.gd). Выключенный блок не
	// считается, следующие проходы получают его нейтральный результат; биты, которые читают шейдеры, — в
	// GatherView.temporal.x (cache_types). Бит 2 когда-то выключал контактную поправку экранных зондов; у мировых её
	// нет, бит не занят и ничего не значит.
	enum Block { BLOCK_REFLECTIONS = 1,
		BLOCK_PROBE_FILTER = 4,
		BLOCK_CACHE_UPDATE = 8,
		BLOCK_DIFFUSE_HISTORY = 16,
		BLOCK_REFLECTION_HISTORY = 32,
		BLOCK_PROBES = 64 };
	// Мировые зонды поверхностей (world_probe шейдеров): таблица ключей мировых клеток → номер зонда в пуле; у зонда —
	// опора, записи лучей (транспорт) и свет кадра. Заголовок работы: счётчики кадра, число зондов пула, аргументы
	// запусков и список свободных (world_probe: WORLD_WORK_*).
	static constexpr uint32_t WORLD_PROBE_BYTES = 112, WORLD_RAY_BYTES = 12, WORLD_EMITTER_BYTES = 12, WORLD_RAYS = 64, WORLD_HEADER_WORDS = 24;
	static constexpr uint32_t WORLD_LIGHT_BYTES = 144, WORLD_PIXEL_LIGHT_BYTES = 64, WORLD_INTERPOLATION_BYTES = 16, WORLD_TABLE_SLOT_BYTES = 16;
	// Слова заголовка, которые пишет движок: кадр вида (+1) и кадр, раньше которого вытесняются неиспользованные зонды.
	static constexpr uint32_t WORLD_WORD_POOL = 2, WORLD_WORD_FREE = 16, WORLD_WORD_FREE_TAKEN = 17, WORLD_WORD_FRAME = 19, WORLD_WORD_EVICT_BEFORE = 20;
	// Шаг пикселей запроса зондов по каждой оси (world_probe: WORLD_REQUEST_STRIDE): меньше шага зондов на экране (не
	// меньше 0,7 · шага профиля); столбец, который он пропустит на скользящем взгляде, добирает ореол фильтра.
	static constexpr int WORLD_REQUEST_STRIDE = 8;
	// Зондов пула на клетку шага зондов в пикселях вида (с запасом на соседние уровни, стороны, слои, пустые клетки
	// у кромок и ореол) и слотов таблицы на зонд пула.
	static constexpr double WORLD_POOL_PER_CELL = 6.0;
	static constexpr uint32_t WORLD_TABLE_PER_PROBE = 8, WORLD_POOL_MINIMUM = 4096;

private:
	enum Pass { REFLECTION,
		REFLECTION_TEMPORAL,
		WORLD_REQUEST,
		WORLD_HALO,
		WORLD_CHECK,
		WORLD_TRACE,
		WORLD_SHADE,
		WORLD_FILTER,
		WORLD_INTEGRATE,
		WORLD_EVICT,
		PASS_COUNT };
	struct Sets {
		RID passes[PASS_COUNT];
		Dictionary hash;
		RID resources[8];
		bool diagnostics = false;
		bool ready = false;
	};
	struct View {
		RID camera, emission_change, stats;
		// Мировые зонды: ключи таблицы, опоры для пикселей, свет для пикселей; зонды пула, записи лучей по полусфере и к
		// источникам, свет зонда кадра; работа, аргументы косвенных запусков и изменения мира кадра.
		RID world_tags, world_slots, world_keys, world_probes, world_rays, world_emitters, world_light, world_work, world_args, world_transport;
		// Зондов пула и слотов таблицы вида: по размеру вида и шагу зондов (world_spacing), с которыми созданы.
		uint32_t world_pool = 0, world_table = 0;
		float world_spacing = 0.0f;
		// Кеш мировых зондов начинается заново: таблица, пул и свет.
		bool world_reset = true;
		// Свободных зондов почти не осталось (по чтению заголовка): кадр вытесняет зонды, не использованные world_evict_age
		// кадров; world_evict_frame — кадр вида (+1) последнего вытеснения. Возраст уменьшается, если вытеснение нужно
		// снова раньше, чем через два возраста, и растёт, когда оно редко.
		bool world_evict = false;
		uint32_t world_evict_age = 256, world_evict_frame = 0;
		// Номер кадра вида (+1) последнего изменения мира: зонд, проверенный раньше него, пропустил изменения.
		uint32_t world_last_change = 0;
		// Последнее событие журнала, которое вид уже передал своим зондам.
		uint64_t transport_consumed = 0, transport_last_serial = 0;
		Vector2i size;
		int frame = 0;
		int reflection_divisor = 1;
		bool reflections = false;
		// Свет пикселя, для которого набраны выход и история: 1 — зонды, 0 — мировой кеш, −1 — не набраны.
		int light_scheme = -1;
		Projection previous;
		Transform3D previous_camera;
		Sets sets[2];
	};
	struct Change {
		AABB bounds;
		double time = 0.0;
	};
	struct Parameters {
		float matrices[48];
		int32_t dimensions[4];
		float sky[4];
		int32_t history[4];
		int32_t reflection[4];
		int32_t emitters[2];
		float roughness, reflection_history;
		int32_t change_count[4];
		float changes[VIEW_CHANGES][8];
		float inverse_matrices[32];
		int32_t move_count[4];
		float moves[VIEW_MOVED][20];
		float limits[4];
		float time[4];
	};
	static_assert(sizeof(Parameters) == 288 + VIEW_CHANGES * 32 + 128 + 16 + VIEW_MOVED * 80 + 32);
	// Кадр мировых зондов вида (world_probe: WorldTransport): число рамок, кадр вида (+1) последнего изменения,
	// поколение источников, потеря событий; номер набора изменений и признак ровно одного нового набора; копия слов 0–3
	// заголовка работы после запроса пикселями (пишет GPU); зондов пула, слотов таблицы, шаг зондов в пикселях; рамки.
	struct TransportParameters {
		uint32_t counts[4];
		uint32_t serial[4];
		uint32_t requested[4];
		uint32_t sizes[4];
		float changes[TRANSPORT_CHANGES][8];
	};
	struct TransportEvent {
		uint64_t number;
		AABB bounds;
	};
	// Журнал изменений геометрии для транспорта зондов всех видов: вид забирает события новее своего последнего.
	LocalVector<TransportEvent> transport_log;
	uint64_t transport_events = 0, transport_serial = 0;
	void write_transport(View &r_view, TransportParameters &r_parameters, const Transport &p_transport);
	HashMap<ObjectID, View *> views;
	LocalVector<Change> changes;
	int64_t changes_frame = -1;
	// Часы вида и время последней смены солнца или неба: она меняет свет всего мира.
	double clock = 0.0;
	double world_light_time = -1e30;
	RD *device = nullptr;
	Ref<RefCounted> world_hash;
	SurfaceCache::Kernel kernels[PASS_COUNT];
	// Суммы по лучам зонда подгруппами (WORLD_SUBGROUP_ARITHMETIC шейдеров), иначе — общей памятью.
	bool subgroup_sums = false;
	static uint32_t world_pool_for(Vector2i p_size, float p_spacing);
	static uint64_t world_buffer_bytes(uint32_t p_pool, uint32_t p_table);
	// p_budget — байт, которые буферы зондов этого вида могут занять под лимитом памяти (может быть меньше нуля).
	bool create_world_resources(View &r_view, float p_spacing, int64_t p_budget);
	void free_world_resources(View &r_view);
	void receive_world_header(const PackedByteArray &p_data, ObjectID p_view);
	RID sampler, pass_stats[3];
	String error;
	double resource_ms = 0;
	void free_view(View *p_view);
	void create_resources(View &r_view, const Ref<RenderSceneBuffersRD> &p_buffers, Vector2i p_size);
	void build_sets(Sets &r_sets, View &r_view, const Ref<RenderSceneBuffersRD> &p_buffers, int p_current, Span<RID> p_resources, RID p_tlas, RID p_changes, bool p_diagnostics);
	bool sets_valid(const Sets &p_sets) const;
	void take_changes(double p_now);
	void write_changes(Parameters &r_parameters);
	void write_moves(Parameters &r_parameters, const LocalVector<Motion> &p_moves);
	void mark(const char *p_name, ObjectID p_view, bool p_enabled);
	static void _bind_methods();

public:
	static constexpr int VIEW_BYTES = sizeof(Parameters);
	bool initialize(RenderingDevice *p_device, const Ref<RefCounted> &p_world_hash);
	// p_world_bytes — буферы мира под тем же лимитом памяти профиля (memory_limit_mib): буферы зондов вида с ними
	// в него помещаются, иначе сбор останавливается с ошибкой.
	String render(RenderData *p_data, Span<RID> p_resources, RID p_tlas, const Ref<Resource> &p_settings, int p_light_count, int p_emitter_count, RID p_material_changes, const LocalVector<Motion> &p_moves, const Transport &p_transport, uint64_t p_world_bytes);
	void dispose();
	String get_error() const { return error; }
	// Номер последнего набора изменений мира, который сбор принял в журнал транспорта.
	uint64_t get_transport_serial() const { return transport_serial; }
	// Буферы видов для повторяемых проверок: текстуры сбора, заголовок работы и записи мировых зондов.
	Array get_probe_views() const;
	double get_resource_ms() const { return resource_ms; }
	// Буферы мировых зондов всех видов, байт.
	uint64_t get_probe_buffer_bytes() const;
};
