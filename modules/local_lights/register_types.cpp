// lestoroer#local-lights
#include "register_types.h"

#include "local_lights.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"

static LocalLights *local_lights = nullptr;

void initialize_local_lights_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GLOBAL_DEF("rendering/lights_and_shadows/local_lights/enabled", false);
	GDREGISTER_ABSTRACT_CLASS(LocalLights);
	local_lights = memnew(LocalLights);
	Engine::get_singleton()->add_singleton(Engine::Singleton("LocalLights", LocalLights::get_singleton()));
	RendererSceneCull::local_lights_backend_factory = &LocalLights::create_backend;
}

void uninitialize_local_lights_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	RendererSceneCull::local_lights_backend_factory = nullptr;
	Engine::get_singleton()->remove_singleton("LocalLights");
	memdelete(local_lights);
	local_lights = nullptr;
}
