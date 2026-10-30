// Fork(Lestoroer): Interior Star Surface Cache runtime.
#include "register_types.h"

#include "surface_cache_gather.h"
#include "surface_cache_geometry.h"
#include "surface_cache_world.h"

#include "core/object/class_db.h"
void initialize_interior_surface_cache_module(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(SurfaceCacheGatherRD);
		GDREGISTER_ABSTRACT_CLASS(SurfaceCacheGeometry);
		GDREGISTER_CLASS(SurfaceCacheWorldRD);
	}
}
void uninitialize_interior_surface_cache_module(ModuleInitializationLevel level) {}
