// lestoroer#local-lights
#pragma once
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "scene/resources/texture_rd.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"

class VuLightRuntime : public RefCounted {
	GDCLASS(VuLightRuntime, RefCounted);
	struct Light {
		RID instance, base;
		Transform3D transform, baked_transform;
		Color color;
		float range = 1, energy = 1, decay = 1, angle = 45, cone_decay = 1, specular = 1;
		float baked_range = 1, baked_angle = 45;
		uint32_t caster_mask = UINT32_MAX;
		bool spot = false, shadows = true, enabled = true, valid = false, dirty = true, atlas_denied = false;
		int layer = -1;
		uint64_t queued = 0, updated = 0;
		LocalVector<int> cells;
		Vector3 cell_position;
		float cell_range = -1;
	};
	struct Geometry {
		RID base, mesh_instance, material_override;
		Vector<RID> materials;
		Transform3D transform;
		AABB bounds;
		uint32_t layers = 1;
	};
	struct DrawPush {
		float mvp[16], decode_position[4], decode_size[4], options[4], uv_transform[4];
	};
	struct MaterialData {
		RID uniform;
		Vector4 uv = Vector4(1, 1, 0, 0);
		float alpha = 1, threshold = -1;
		bool casts = true;
	};
	RenderingDevice *rd = nullptr;
	RID scenario;
	RID filter_shader, filter_pipeline, filter_scratch, filter_horizontal, filter_vertical;
	HashMap<RID, int> light_slots;
	Vector<Light> lights;
	HashMap<RID, Geometry> geometry;
	LocalVector<RendererSceneCull::LocalLightChange> changes;
	LocalVector<int> pending_filters[2];
	Vector<uint32_t> grid;
	Vector<float> records;
	Vector<int> layer_owner;
	Vector<RID> slices, framebuffers;
	HashMap<uint64_t, RID> pipelines;
	HashMap<RID, MaterialData> materials;
	RID grid_rd, data_rd, atlas_rd, depth_rd, caster_shader, sampler, white_texture;
	Ref<Texture2DRD> grid_texture, data_texture;
	Ref<Texture2DArrayRD> atlas_texture;
	AABB world;
	Vector3i dimensions;
	float cell_size = 1.0f, softness = 0.18f;
	int tile_size = 256, layers = 384, cap = 16, filter_mode = 0, max_sources = 256;
	uint64_t memory_limit = 128ull << 20, frame = 0;
	RID interactive;
	Vector3 camera_position;
	bool disposed = false, ready = false, data_dirty = true, grid_dirty = true, shadows_enabled = true;
	bool gpu_timestamps = false;
	String error;
	uint64_t memory_bytes = 0, uploaded_bytes = 0, updates = 0, faces = 0, triangles = 0, draws = 0;
	uint64_t total_uploads = 0, total_updates = 0, total_faces = 0, total_triangles = 0, total_draws = 0, total_grid_uploads = 0, total_data_uploads = 0;
	uint64_t served_age = 0;
	double total_shadow_ms = 0;
	double cpu_ms = 0, shadow_ms = 0;
	static void _bind_methods();
	bool initialize_gpu();
	void consume_changes();
	void render_frame();
	void dirty(Light &p_light);
	void update_membership(int p_slot);
	void clear_membership(int p_slot);
	void publish(int p_slot);
	bool render_shadow(int p_slot);
	void filter_moments(const LocalVector<int> &p_sources);
	MaterialData material_data(RID p_material);
	RID pipeline(RenderingDevice::VertexFormatID p_format);
	void upload();
	void fail(const String &p_error);
	Vector4 shader_settings() const;

public:
	void attach(RID p_scenario, const TypedArray<RID> &p_sources, const AABB &p_world, int p_tile_size = 256, int p_cap = 16, int p_memory_mib = 128, int p_filter_mode = 0);
	void update(const Vector3 &p_camera) { camera_position = p_camera; }
	void set_cap(int p_cap);
	void set_gpu_timestamps(bool p_enabled) { gpu_timestamps = p_enabled; }
	void set_softness(float p_softness);
	void set_shadows_enabled(bool p_enabled);
	void set_light_enabled(RID p_instance, bool p_enabled);
	void set_interactive(RID p_instance) { interactive = p_instance; }
	bool add_light(RID p_instance);
	void dispose();
	String get_error() const { return error; }
	Ref<Texture2DRD> get_grid_texture() const { return grid_texture; }
	bool owns_light(RID p_instance) const;
	Dictionary diagnostics() const;
	~VuLightRuntime();
};
