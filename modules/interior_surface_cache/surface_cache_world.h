// Fork(Lestoroer): Surface Cache scene updates and resource ownership.
#pragma once
#include "surface_cache_gather.h"

#include "scene/resources/mesh.h"

class SurfaceCacheWorldRD : public RefCounted {
	GDCLASS(SurfaceCacheWorldRD, RefCounted);
	enum Buffer { SURFACES,
		TRIANGLES,
		MATERIALS,
		MATERIAL_RANKS,
		MATERIAL_FLAGS,
		LIGHTS,
		MATERIAL_CHANGES,
		SURFACE_FLAGS,
		BUFFER_COUNT };
	enum Pass { GEOMETRY,
		MATERIAL,
		MOVE,
		POSE,
		CLEAR_FLAGS,
		RANKS,
		PASS_COUNT };
	enum ChangeKind { REMOVE,
		LIGHT,
		ADD,
		REPLACE,
		MOVE_INSTANCE,
		POSE_INSTANCE,
		RECAPTURE };
	struct Geometry {
		String key;
		int references = 0;
		Ref<ArrayMesh> mesh;
		Vector2i size;
		int sample_count = 0;
		struct Surface {
			Vector2i offset, size;
			int triangles = 0;
		};
		LocalVector<Surface> surfaces;
		TypedArray<PackedByteArray> triangles, vertex_data;
		PackedInt32Array coverage, coverage_ranks;
		LocalVector<RID> vertices, local;
		RID blas, coverage_buffer, ranks_buffer;
	};
	struct Instance {
		uint64_t generation = 0;
		RID source, base, chart;
		int64_t mesh_revision = 0;
		Geometry *shared = nullptr;
		AABB bounds;
		Transform3D transform, move_from;
		PackedInt32Array sides;
		bool deformed = false;
		bool triangles_uploaded = false, geometry_dirty = true, pose_dirty = true, capture_dirty = true;
		bool has_move = false, ranks_written = false;
		uint64_t capture_serial = 0, opacity_serial = 0;
		PackedByteArray opaque;
		int node_offset = 0, node_count = 0, first_surface = 0, first_triangle = 0, triangle_count = 0;
		int whole_first = 0, whole_count = 0, storage_rank = 0, storage_count = 0, storage_offset = 0;
		LocalVector<int> chunks;
		LocalVector<RID> vertices, geometry_sets;
		RID blas, pose_cells;
		int tlas_index = -1;
	};
	struct RankChunk {
		int offset = 0;
		int references = 0;
		bool written = false;
	};
	struct Counters {
		int64_t captures = 0, moves = 0, poses = 0, geometry_updates = 0, blas_builds = 0, tlas_builds = 0, layouts = 0;
		double frame_ms = 0, layouts_ms = 0, captures_ms = 0, tlas_ms = 0, gather_ms = 0;
	} counters;
	RID scenario;
	Ref<Resource> profile;
	RD *device = nullptr;
	HashMap<RID, Instance *> instances;
	HashMap<String, Geometry *> geometries;
	HashMap<RID, PackedByteArray> lights;
	SurfaceCache::Ranges surface_ranges, triangle_ranges, node_ranges, storage_ranges;
	HashMap<int, RankChunk> rank_chunks;
	RID buffers[BUFFER_COUNT];
	uint32_t capacities[BUFFER_COUNT] = {};
	uint64_t buffer_memory_bytes = 0;
	bool capacity_dirty = true;
	int material_cells = 0, material_storage = 0;
	Ref<SurfaceCacheGatherRD> gather;
	Ref<RefCounted> capture, world_hash;
	SurfaceCache::Kernel kernels[PASS_COUNT];
	Vector2i capture_dimensions;
	int64_t last_capture_frame = 0;
	Array rooms;
	bool rooms_dirty = false, lights_dirty = true, sun_changed = false, sky_changed = false;
	TypedArray<PackedByteArray> light_changes;
	TypedArray<AABB> dirty_bounds;
	RID tlas, dummy, move_set;
	int tlas_capacity = 0;
	LocalVector<RD::AccelerationStructureInstance> tlas_instances;
	LocalVector<Instance *> tlas_owners;
	LocalVector<RID> tlas_added;
	HashMap<RID, bool> tlas_updates, moved;
	HashMap<RID, Dictionary> deferred_changes;
	LocalVector<SurfaceCacheGatherRD::Motion> frame_moves;
	Array opacity_states;
	bool initialized = false, disposed = false, opacity_pending = false, opacity_requested = false;
	uint64_t capture_serial = 0, next_generation = 0;
	int64_t frame = 0;
	double cpu_submit_ms = 0;
	String error;
	static bool measure_gpu_time;
	static double gpu_time_ms;
	static void _bind_methods();
	void render_frame();
	void update_world();
	String render_view(RenderData *p_data);
	Array take_changes(int p_capture_limit);
	ChangeKind change_kind(const Dictionary &p_change) const;
	bool initialize_gpu();
	bool add_instance(const Dictionary &p_change);
	void remove_instance(RID p_source);
	void release_geometry(Geometry *p_geometry);
	LocalVector<RID> vertex_buffers(const Geometry &p_geometry);
	RID create_blas(const LocalVector<RID> &p_vertices, const Geometry &p_geometry, bool p_build = true);
	bool ensure_capacity();
	void allocate_storage(Instance &r_state);
	void release_storage(Instance &r_state);
	void write_ranks(Instance &r_state);
	static int coverage_rank(const Geometry &p_geometry, int p_local);
	void clear_flags(int p_first, int p_count);
	void update_geometry(Instance &r_state);
	void capture_material(Instance &r_state);
	void move_material(Instance &r_state);
	void pose_material(Instance &r_state);
	void rebuild_tlas();
	void write_tlas_instance(Instance &r_state);
	void remove_tlas_instance(Instance &r_state);
	PackedByteArray encode_light(const Dictionary &p_light);
	void note_light_change(const PackedByteArray &p_previous, const PackedByteArray &p_current);
	void note_dirty(const AABB &p_bounds);
	void upload_lights();
	void request_opacity();
	void receive_opacity(const PackedByteArray &p_data, uint64_t p_serial, const Array &p_states);
	bool fail(const String &p_message);
	void mark(const String &p_name, bool p_boundary = false);
	void read_gpu_time();

public:
	static constexpr uint32_t MATERIAL_RANK_CHUNK = 0x80000000u;
	static constexpr int CAPTURE_TEXEL_BYTES = 52;
	void attach(RID p_world, const Ref<Resource> &p_settings);
	void update_profile(const Ref<Resource> &p_settings);
	void set_rooms(const Array &p_rooms);
	void dispose();
	String get_error() const { return error; }
	Dictionary get_metrics() const;
	Dictionary get_operations() const;
	Dictionary get_operation_ms() const;
	Dictionary buffer_report() const;
	Dictionary get_probe_resources() const;
	static void set_measure_gpu_time(bool p_enabled) { measure_gpu_time = p_enabled; }
	static double get_gpu_time_ms() { return gpu_time_ms; }
};
