// lestoroer#local-lights
#pragma once

#include "local_lights_world.h"

#include "core/object/object.h"
#include "core/os/mutex.h"
#include "scene/resources/3d/world_3d.h"

// Настройки и показатели локального света для скриптов стендов. Сама работа — в LocalLightsWorld на потоке рендера.
class LocalLights : public Object {
	GDCLASS(LocalLights, Object);

	static LocalLights *singleton;

	LocalLightsSettings settings;
	mutable Mutex metrics_mutex;
	LocalLightsFrameMetrics last_frame_metrics;

	void _set_world_enabled_on_render_thread(RID p_scenario, bool p_enabled);

protected:
	static void _bind_methods();

public:
	static LocalLights *get_singleton() { return singleton; }
	static RendererSceneCull::LocalLightsBackend *create_backend(RID p_scenario);

	const LocalLightsSettings &get_settings() const { return settings; }
	void publish_frame_metrics(const LocalLightsFrameMetrics &p_metrics);

	void set_world_enabled(const Ref<World3D> &p_world, bool p_enabled);
	bool is_world_enabled(const Ref<World3D> &p_world) const;
	void set_update_cap(int p_cap);
	int get_update_cap() const;
	void set_softness(float p_softness_per_meter);
	void set_several_lamps_growth(float p_growth);
	void set_show_mask(bool p_show);
	void set_fixed_noise_frame(int p_frame);
	void set_caster_culling(bool p_enabled);
	void set_map_sizes(int p_omni_face, int p_spot_tile);
	void set_map_texel_size(float p_meters);
	void set_gpu_timestamps(bool p_enabled);
	Dictionary get_frame_metrics() const;
	Dictionary get_memory_report(const Ref<World3D> &p_world) const;
	Dictionary read_screen_buffers(const Ref<World3D> &p_world) const;

	LocalLights();
	~LocalLights();
};
