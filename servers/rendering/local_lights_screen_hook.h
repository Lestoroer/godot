// lestoroer#local-lights
#pragma once

#include <cstdint>

class RenderDataRD;

// Вид, который сейчас рисуется в мире с локальным светом: рендер вызывает проходы отношения после предварительной глубины.
class LocalLightsScreenHook {
	static inline LocalLightsScreenHook *active_view = nullptr;

public:
	// Бит в CAMERA_VISIBLE_LAYERS: сетка и данные ламп одни на процесс, а считать лампы материал должен только в виде их мира.
	// Слоёв 20, старшие биты свободны. Контракт с local_lights_inc.gdshaderinc.
	static constexpr uint32_t ACTIVE_VIEW_LAYER_BIT = 1u << 31;

	static LocalLightsScreenHook *get_active_view() { return active_view; }
	static void set_active_view(LocalLightsScreenHook *p_view) { active_view = p_view; }

	virtual void render_screen_passes(RenderDataRD *p_render_data) = 0;
	virtual ~LocalLightsScreenHook() = default;
};
