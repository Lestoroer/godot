// lestoroer#gi
#pragma once
#include "surface_cache_gather.h"

#include "scene/resources/mesh.h"

class SurfaceCacheWorldRD : public RefCounted {
	GDCLASS(SurfaceCacheWorldRD, RefCounted);
	enum Buffer { SURFACES,
		ORIENTATIONS,
		MATERIALS,
		LIGHTS,
		MATERIAL_CHANGES,
		SURFACE_FLAGS,
		BUFFER_COUNT };
	enum Pass { POSE,
		MATERIAL,
		PASS_COUNT };
	enum ChangeKind { REMOVE,
		LIGHT,
		ADD,
		REPLACE,
		MOVE_OR_POSE,
		RECAPTURE };
	struct Instance;
	struct Geometry {
		String key;
		int references = 0;
		Ref<ArrayMesh> mesh;
		Vector2i size;
		int square = 0, columns = 0;
		struct Surface {
			int triangles = 0;
			int vertices = 0;
			// +1/-1 задают общую ориентацию; при нуле она хранится для каждого треугольника битом от orientation_bit.
			int orientation = 0;
			int orientation_bit = 0;
		};
		LocalVector<Surface> surfaces;
		TypedArray<PackedByteArray> orientation_bits;
		int orientation_offset = 0, orientation_words = 0;
		LocalVector<RID> vertices, indices;
		RID blas;
	};
	struct Instance {
		uint64_t generation = 0;
		RID source, base, chart;
		int64_t mesh_revision = 0;
		Geometry *shared = nullptr;
		AABB bounds;
		Transform3D transform;
		PackedInt32Array sides;
		bool deformed = false;
		bool surfaces_written = false, geometry_dirty = true, pose_dirty = true, capture_dirty = true;
		uint64_t capture_serial = 0, opacity_serial = 0;
		PackedByteArray opaque;
		int first_surface = 0, triangle_count = 0;
		LocalVector<RID> vertices;
		RID blas;
		int tlas_index = -1;
	};
	// Светящийся прямоугольник, который зарегистрировал владелец сцены: rect — в осях экземпляра, начало — центр,
	// столбцы базиса — половины сторон и нормаль излучающей стороны.
	struct Emitter {
		RID instance;
		int surface = 0;
		Transform3D rect;
	};
	struct Counters {
		int64_t captures = 0, geometry_updates = 0, blas_builds = 0, tlas_builds = 0, layouts = 0;
		double frame_ms = 0, layouts_ms = 0, captures_ms = 0, tlas_ms = 0, gather_ms = 0;
		double initialize_ms = 0, mesh_prepare_ms = 0, blas_ms = 0, capacity_ms = 0, geometry_ms = 0;
		double mesh_read_ms = 0, mesh_layout_ms = 0, mesh_pack_ms = 0, capture_mesh_ms = 0, capture_allocate_ms = 0, capture_draw_ms = 0;
	} counters;
	RID scenario;
	Ref<Resource> profile;
	RD *device = nullptr;
	HashMap<RID, Instance *> instances;
	HashMap<String, Geometry *> geometries;
	HashMap<RID, PackedByteArray> lights;
	LocalVector<Emitter> emitters;
	PackedByteArray emitter_records;
	int active_emitters = 0;
	SurfaceCache::Ranges surface_ranges, orientation_ranges;
	int64_t triangle_total = 0;
	RID buffers[BUFFER_COUNT];
	uint32_t capacities[BUFFER_COUNT] = {};
	uint64_t buffer_memory_bytes = 0;
	bool capacity_dirty = true;
	Ref<SurfaceCacheGatherRD> gather;
	Ref<RefCounted> capture, world_hash;
	SurfaceCache::Kernel kernels[PASS_COUNT];
	Vector2i capture_dimensions;
	int64_t last_capture_frame = 0;
	Array rooms;
	bool rooms_dirty = false, lights_dirty = true, sun_changed = false, sky_changed = false;
	TypedArray<PackedByteArray> light_changes;
	TypedArray<AABB> dirty_bounds;
	RID tlas, dummy;
	int tlas_capacity = 0;
	LocalVector<RD::AccelerationStructureInstance> tlas_instances;
	LocalVector<Instance *> tlas_owners;
	LocalVector<RID> tlas_added;
	HashMap<RID, bool> tlas_updates;
	HashMap<RID, Dictionary> deferred_changes;
	LocalVector<SurfaceCacheGatherRD::Motion> frame_moves;
	Array opacity_states;
	bool initialized = false, disposed = false, opacity_pending = false, opacity_requested = false;
	uint64_t capture_serial = 0, next_generation = 0;
	int64_t frame = 0;
	Vector3 view_origin;
	bool has_view = false;
	int64_t prepared_triangles = 0;
	double prepare_ms_per_triangle = 0.0003;
	double cpu_submit_ms = 0;
	String error;
	static bool measure_gpu_time;
	static double gpu_time_ms;
	static void _bind_methods();
	void render_frame();
	void update_world();
	String render_view(RenderData *p_data);
	Array take_changes();
	int triangle_count(RID p_base) const;
	ChangeKind change_kind(const Dictionary &p_change) const;
	bool initialize_gpu();
	bool add_instance(const Dictionary &p_change, const HashMap<RID, Vector<RenderingServerTypes::SurfaceData>> &p_sources);
	void remove_instance(RID p_source);
	void release_geometry(Geometry *p_geometry);
	LocalVector<RID> vertex_buffers(const TypedArray<PackedByteArray> &p_vertex_data);
	LocalVector<RID> index_buffers(const TypedArray<PackedByteArray> &p_index_data);
	LocalVector<RID> copy_vertex_buffers(const Geometry &p_geometry);
	RID create_blas(const LocalVector<RID> &p_vertices, const Geometry &p_geometry, bool p_build = true);
	bool ensure_capacity();
	void required_sizes(uint32_t *r_sizes) const;
	void fit_capacity();
	void update_geometry(Instance &r_state);
	void write_surfaces(Instance &r_state);
	void capture_material(Instance &r_state);
	void rebuild_tlas();
	void write_tlas_instance(Instance &r_state);
	void remove_tlas_instance(Instance &r_state);
	PackedByteArray encode_light(const Dictionary &p_light);
	PackedByteArray encode_emitters(int &r_count) const;
	void note_light_change(const PackedByteArray &p_previous, const PackedByteArray &p_current);
	void note_dirty(const AABB &p_bounds);
	void upload_lights();
	void request_opacity();
	void receive_opacity(const PackedByteArray &p_data, uint64_t p_serial, const Array &p_states);
	bool fail(const String &p_message);
	void mark(const String &p_name, bool p_boundary = false);
	void read_gpu_time();

public:
	static constexpr int CAPTURE_TEXEL_BYTES = 52;
	// Ограничивает перебор прямоугольников в каждом зонде: веса выбора лежат в общей памяти группы.
	static constexpr int MAX_EMITTERS = 64;
	void attach(RID p_world, const Ref<Resource> &p_settings);
	void update_profile(const Ref<Resource> &p_settings);
	void set_rooms(const Array &p_rooms);
	void set_emitters(const Array &p_emitters);
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
