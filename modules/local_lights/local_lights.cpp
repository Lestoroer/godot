// lestoroer#local-lights
#include "local_lights.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

LocalLights *LocalLights::singleton = nullptr;

namespace {
RendererSceneCull *scene_cull() {
	return static_cast<RendererSceneCull *>(RSG::scene);
}
} // namespace

void LocalLights::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_world_enabled", "world", "enabled"), &LocalLights::set_world_enabled);
	ClassDB::bind_method(D_METHOD("is_world_enabled", "world"), &LocalLights::is_world_enabled);
	ClassDB::bind_method(D_METHOD("set_update_cap", "cap"), &LocalLights::set_update_cap);
	ClassDB::bind_method(D_METHOD("get_update_cap"), &LocalLights::get_update_cap);
	ClassDB::bind_method(D_METHOD("set_softness", "softness_per_meter"), &LocalLights::set_softness);
	ClassDB::bind_method(D_METHOD("set_several_lamps_growth", "growth"), &LocalLights::set_several_lamps_growth);
	ClassDB::bind_method(D_METHOD("set_show_mask", "show"), &LocalLights::set_show_mask);
	ClassDB::bind_method(D_METHOD("set_fixed_noise_frame", "frame"), &LocalLights::set_fixed_noise_frame);
	ClassDB::bind_method(D_METHOD("set_caster_culling", "enabled"), &LocalLights::set_caster_culling);
	ClassDB::bind_method(D_METHOD("set_map_sizes", "omni_face", "spot_tile"), &LocalLights::set_map_sizes);
	ClassDB::bind_method(D_METHOD("set_map_texel_size", "meters"), &LocalLights::set_map_texel_size);
	ClassDB::bind_method(D_METHOD("set_gpu_timestamps", "enabled"), &LocalLights::set_gpu_timestamps);
	ClassDB::bind_method(D_METHOD("get_frame_metrics"), &LocalLights::get_frame_metrics);
	ClassDB::bind_method(D_METHOD("get_memory_report", "world"), &LocalLights::get_memory_report);
	ClassDB::bind_method(D_METHOD("read_screen_buffers", "world"), &LocalLights::read_screen_buffers);
}

RendererSceneCull::LocalLightsBackend *LocalLights::create_backend(RID p_scenario) {
	return memnew(LocalLightsWorld(p_scenario));
}

void LocalLights::publish_frame_metrics(const LocalLightsFrameMetrics &p_metrics) {
	MutexLock lock(metrics_mutex);
	last_frame_metrics = p_metrics;
}

void LocalLights::_set_world_enabled_on_render_thread(RID p_scenario, bool p_enabled) {
	RendererSceneCull::LocalLightsBackend *backend = scene_cull()->scenario_get_local_lights(p_scenario);
	if (backend) {
		static_cast<LocalLightsWorld *>(backend)->set_enabled(p_enabled);
	} else if (p_enabled) {
		scene_cull()->scenario_set_local_lights(p_scenario, create_backend(p_scenario));
	}
}

void LocalLights::set_world_enabled(const Ref<World3D> &p_world, bool p_enabled) {
	ERR_FAIL_COND(p_world.is_null());
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LocalLights::_set_world_enabled_on_render_thread).bind(p_world->get_scenario(), p_enabled));
}

bool LocalLights::is_world_enabled(const Ref<World3D> &p_world) const {
	ERR_FAIL_COND_V(p_world.is_null(), false);
	const RendererSceneCull::LocalLightsBackend *backend = scene_cull()->scenario_get_local_lights(p_world->get_scenario());
	return backend && backend->is_enabled();
}

void LocalLights::set_update_cap(int p_cap) {
	settings.update_cap = MAX(p_cap, 0);
}

int LocalLights::get_update_cap() const {
	return settings.update_cap;
}

void LocalLights::set_softness(float p_softness_per_meter) {
	settings.softness_per_meter = p_softness_per_meter;
}

void LocalLights::set_several_lamps_growth(float p_growth) {
	settings.several_lamps_growth = p_growth;
}

void LocalLights::set_show_mask(bool p_show) {
	settings.show_hard_mask = p_show;
}

void LocalLights::set_fixed_noise_frame(int p_frame) {
	settings.fixed_noise_frame = p_frame;
}

void LocalLights::set_caster_culling(bool p_enabled) {
	settings.caster_culling = p_enabled;
}

void LocalLights::set_map_sizes(int p_omni_face, int p_spot_tile) {
	ERR_FAIL_COND_MSG(p_omni_face % 32 != 0 || p_spot_tile % 32 != 0, "Сторона плитки локального света кратна 32: начало плитки хранится в этих единицах.");
	settings.omni_face_size = p_omni_face;
	settings.spot_tile_size = p_spot_tile;
}

void LocalLights::set_map_texel_size(float p_meters) {
	settings.map_texel_size = p_meters;
}

void LocalLights::set_gpu_timestamps(bool p_enabled) {
	settings.gpu_timestamps = p_enabled;
}

Dictionary LocalLights::get_frame_metrics() const {
	LocalLightsFrameMetrics metrics;
	{
		MutexLock lock(metrics_mutex);
		metrics = last_frame_metrics;
	}
	Dictionary result;
	result["views"] = metrics.views;
	result["lampsInView"] = metrics.lamps_in_view;
	result["lampsInGrid"] = metrics.lamps_in_grid;
	result["lampsServed"] = metrics.lamps_served;
	result["queue"] = metrics.queue;
	result["oldestWaitFrames"] = metrics.oldest_wait_frames;
	result["servedWaitFrames"] = metrics.served_wait_frames;
	result["facesBaked"] = metrics.faces_baked;
	result["surfacesDrawn"] = metrics.surfaces_drawn;
	result["surfacesCulled"] = metrics.surfaces_culled;
	result["trianglesDrawn"] = metrics.triangles_drawn;
	result["repacks"] = metrics.repacks;
	result["gridUploads"] = metrics.grid_uploads;
	result["dataUploads"] = metrics.data_uploads;
	result["uploadedBytes"] = metrics.uploaded_bytes;
	result["trackUsec"] = metrics.track_usec;
	result["gatherUsec"] = metrics.gather_usec;
	result["recordUsec"] = metrics.record_usec;
	result["gridUsec"] = metrics.grid_usec;
	result["screenUsec"] = metrics.screen_usec;
	result["totalUsec"] = metrics.track_usec + metrics.gather_usec + metrics.record_usec + metrics.grid_usec + metrics.screen_usec;
	return result;
}

Dictionary LocalLights::get_memory_report(const Ref<World3D> &p_world) const {
	ERR_FAIL_COND_V(p_world.is_null(), Dictionary());
	const RendererSceneCull::LocalLightsBackend *backend = scene_cull()->scenario_get_local_lights(p_world->get_scenario());
	return backend ? static_cast<const LocalLightsWorld *>(backend)->memory_report() : Dictionary();
}

Dictionary LocalLights::read_screen_buffers(const Ref<World3D> &p_world) const {
	ERR_FAIL_COND_V(p_world.is_null(), Dictionary());
	ERR_FAIL_COND_V_MSG(!RenderingServer::get_singleton()->is_on_render_thread(), Dictionary(), "Буферы читаются на потоке рендера.");
	const RendererSceneCull::LocalLightsBackend *backend = scene_cull()->scenario_get_local_lights(p_world->get_scenario());
	return backend ? static_cast<const LocalLightsWorld *>(backend)->read_screen_buffers() : Dictionary();
}

LocalLights::LocalLights() {
	singleton = this;
}

LocalLights::~LocalLights() {
	singleton = nullptr;
}
