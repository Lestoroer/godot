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
	// Лимит ограничивает проверки на пиксель; объединение сохраняет кадр самого свежего изменения.
	enum { VIEW_CHANGES = 4,
		VIEW_MOVED = 8,
		CHANGE_FRAMES = 120 };

private:
	enum Pass { PLACE,
		TRACE,
		FILTER,
		INTEGRATE,
		CONTACT,
		CONTACT_TEMPORAL,
		TEMPORAL,
		REFLECTION,
		REFLECTION_TEMPORAL,
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
		RID meta[2], light, filtered;
		Vector2i size, grid;
		int frame = 0;
		int reflection_divisor = 1;
		Projection previous;
		Transform3D previous_camera;
		Sets sets[2];
	};
	struct Change {
		AABB bounds;
		int64_t frame = 0;
		float history = 1e30f;
	};
	struct Parameters {
		float matrices[48];
		int32_t dimensions[4];
		float sky[4];
		int32_t history[4];
		int32_t reflection[4];
		int32_t probe_settings[2];
		float roughness, reflection_history;
		int32_t probes[4];
		int32_t change_count[4];
		float changes[VIEW_CHANGES][8];
		float inverse_matrices[32];
		int32_t move_count[4];
		float moves[VIEW_MOVED][20];
		float limits[4];
	};
	static_assert(sizeof(Parameters) == 304 + VIEW_CHANGES * 32 + 128 + 16 + VIEW_MOVED * 80 + 16);
	HashMap<ObjectID, View *> views;
	LocalVector<Change> changes;
	int64_t changes_frame = -1;
	RD *device = nullptr;
	Ref<RefCounted> world_hash;
	SurfaceCache::Kernel kernels[PASS_COUNT];
	RID sampler, pass_stats[3];
	String error;
	double resource_ms = 0;
	void free_view(View *p_view);
	void create_resources(View &r_view, const Ref<RenderSceneBuffersRD> &p_buffers, Vector2i p_size);
	void build_sets(Sets &r_sets, View &r_view, const Ref<RenderSceneBuffersRD> &p_buffers, int p_current, Span<RID> p_resources, RID p_tlas, RID p_changes, bool p_diagnostics);
	bool sets_valid(const Sets &p_sets) const;
	void take_changes(int64_t p_frame);
	void write_changes(Parameters &r_parameters);
	void write_moves(Parameters &r_parameters, const LocalVector<Motion> &p_moves);
	void mark(const char *p_name, ObjectID p_view, bool p_enabled);
	static void _bind_methods();

public:
	static constexpr int VIEW_BYTES = sizeof(Parameters);
	bool initialize(RenderingDevice *p_device, const Ref<RefCounted> &p_world_hash);
	String render(RenderData *p_data, Span<RID> p_resources, RID p_tlas, const Ref<Resource> &p_settings, int p_light_count, RID p_material_changes, const LocalVector<Motion> &p_moves);
	void dispose();
	String get_error() const { return error; }
	Array get_probe_views() const;
	double get_resource_ms() const { return resource_ms; }
};
