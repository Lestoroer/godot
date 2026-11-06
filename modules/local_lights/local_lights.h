// lestoroer#local-lights
#pragma once

#include "local_lights_world.h"

#include "core/object/object.h"
#include "core/os/mutex.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/3d/world_3d.h"

// Настройки и показатели локального света для скриптов стендов. Сама работа — в LocalLightsWorld на потоке рендера.
class LocalLights : public Object {
	GDCLASS(LocalLights, Object);

	static LocalLights *singleton;

	LocalLightsSettings settings;
	mutable Mutex metrics_mutex;
	LocalLightsFrameMetrics last_frame_metrics;

	void _set_world_enabled_on_render_thread(RID p_scenario, bool p_enabled);
	void _set_held_instances_on_render_thread(RID p_scenario, const Vector<RID> &p_instances);
	void _bake_all_lamps_on_render_thread(RID p_scenario);

protected:
	static void _bind_methods();

public:
	static LocalLights *get_singleton() { return singleton; }
	static RendererSceneCull::LocalLightsBackend *create_backend(RID p_scenario);

	const LocalLightsSettings &get_settings() const { return settings; }
	void publish_frame_metrics(const LocalLightsFrameMetrics &p_metrics);

	void set_world_enabled(const Ref<World3D> &p_world, bool p_enabled);
	bool is_world_enabled(const Ref<World3D> &p_world) const;
	// Что игрок держит в руках: лампы и геометрия этого узла и его потомков идут в очереди обновлений первыми. Пустой узел — руки пусты.
	void set_held_item(const Ref<World3D> &p_world, Node3D *p_item);
	// Этап загрузки: следующий кадр снимает тени всех ламп мира, вне зависимости от cap и попадания в кадр.
	void bake_all_lamps(const Ref<World3D> &p_world);
	void set_update_cap(int p_cap);
	int get_update_cap() const;
	void set_caster_culling(bool p_enabled);
	void set_map_sizes(int p_omni_face, int p_spot_tile);
	void set_map_texel_size(float p_meters);
	// Бюджет атласа карт, МиБ: при нехватке места снижается разрешение карт наименее важных ламп.
	void set_map_budget(float p_mib);
	float get_map_budget() const;
	void set_gpu_timestamps(bool p_enabled);
	Dictionary get_frame_metrics() const;
	// Только на потоке рендера; при обычной модели потоков это основной поток.
	Dictionary get_memory_report(const Ref<World3D> &p_world) const;
	// Для проверок: RID лампы → сторона её карты, сторона, нужная по радиусу, начало места в атласе, число граней, сторона по плану, лампа в кадре.
	Dictionary get_map_sides(const Ref<World3D> &p_world) const;

	LocalLights();
	~LocalLights();
};
