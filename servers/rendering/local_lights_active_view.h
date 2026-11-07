// lestoroer#local-lights
#pragma once

#include <cstdint>

// Сейчас рисуется вид мира с локальным светом: материалы считают лампы модуля только в нём.
class LocalLightsActiveView {
	static inline bool active = false;

public:
	// Бит в CAMERA_VISIBLE_LAYERS: сетка и данные ламп одни на процесс, а считать лампы материал должен только в виде их мира.
	// Слоёв 20, старшие биты свободны. Контракт с local_lights_inc.gdshaderinc.
	static constexpr uint32_t ACTIVE_VIEW_LAYER_BIT = 1u << 31;

	static bool is_active() { return active; }
	static void set_active(bool p_active) { active = p_active; }
};
