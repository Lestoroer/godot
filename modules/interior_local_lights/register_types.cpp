// lestoroer#local-lights
#include "register_types.h"

#include "vu_light_runtime.h"

#include "core/object/class_db.h"
void initialize_interior_local_lights_module(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(VuLightRuntime);
	}
}
void uninitialize_interior_local_lights_module(ModuleInitializationLevel level) {}
