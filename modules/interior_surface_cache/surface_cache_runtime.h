// Fork(Lestoroer): native runtime support for Interior Star Surface Cache.
#pragma once

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/storage/render_data.h"

namespace SurfaceCache {
inline Ref<RefCounted> script_object(const String &p_name) {
	Ref<Script> script = ResourceLoader::load("res://features/lighting/surface_cache/" + p_name + ".gd");
	ERR_FAIL_COND_V(script.is_null(), Ref<RefCounted>());
	Ref<RefCounted> object = Object::cast_to<RefCounted>(ClassDB::instantiate(script->get_instance_base_type()));
	ERR_FAIL_COND_V(object.is_null(), Ref<RefCounted>());
	object->set_script(script);
	return object;
}

inline double milliseconds(uint64_t p_start) {
	return double(OS::get_singleton()->get_ticks_usec() - p_start) / 1000.0;
}

struct Kernel {
	Ref<RefCounted> compiler;
	RID shader;
	RID pipeline;
	String error;
	bool initialize(RenderingDevice *p_device, const String &p_name, const PackedStringArray &p_defines = {}) {
		compiler = script_object("gpu_kernel");
		if (compiler.is_null() || !bool(compiler->call("initialize", p_device, "res://features/lighting/surface_cache/" + p_name + ".glsl.template", p_defines))) {
			error = compiler.is_valid() ? String(compiler->get("error")) : "Cannot load Surface Cache shader compiler";
			return false;
		}
		shader = compiler->get("shader");
		pipeline = compiler->get("pipeline");
		return true;
	}
	void dispose() {
		if (compiler.is_valid()) {
			compiler->call("dispose");
			compiler.unref();
		}
		shader = RID();
		pipeline = RID();
	}
	void dispatch(RID p_set, const void *p_push, uint32_t p_bytes, const Vector3i &p_groups) const {
		RD *rd = RD::get_singleton();
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, p_set, 0);
		if (p_bytes) {
			rd->compute_list_set_push_constant(list, p_push, p_bytes);
		}
		rd->compute_list_dispatch(list, p_groups.x, p_groups.y, p_groups.z);
		rd->compute_list_end();
	}
	void pixels(RID p_set, const Vector2i &p_size) const {
		dispatch(p_set, nullptr, 0, Vector3i((p_size.x + 7) / 8, (p_size.y + 7) / 8, 1));
	}
	void elements(RID p_set, const void *p_push, uint32_t p_bytes, int p_count) const {
		dispatch(p_set, p_push, p_bytes, Vector3i(MAX((p_count + 63) / 64, 1), 1, 1));
	}
};

inline RD::Uniform uniform(uint32_t p_binding, RD::UniformType p_type, RID p_resource) {
	RD::Uniform result;
	result.binding = p_binding;
	result.uniform_type = p_type;
	result.append_id(p_resource);
	return result;
}
inline RD::Uniform sampled(uint32_t p_binding, RID p_sampler, RID p_texture) {
	RD::Uniform result = uniform(p_binding, RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, p_sampler);
	result.append_id(p_texture);
	return result;
}
inline void replace(LocalVector<RD::Uniform> &r_uniforms, const RD::Uniform &p_uniform) {
	for (RD::Uniform &entry : r_uniforms) {
		if (entry.binding == p_uniform.binding) {
			entry = p_uniform;
			return;
		}
	}
	r_uniforms.push_back(p_uniform);
}
inline RID cached(const Kernel &p_kernel, const LocalVector<RD::Uniform> &p_uniforms) {
	return UniformSetCacheRD::get_singleton()->get_cache_vec(p_kernel.shader, 0, p_uniforms);
}
inline RID cached_buffers(const Kernel &p_kernel, std::initializer_list<RID> p_buffers) {
	LocalVector<RD::Uniform> uniforms;
	for (RID buffer : p_buffers) {
		uniforms.push_back(uniform(uniforms.size(), RD::UNIFORM_TYPE_STORAGE_BUFFER, buffer));
	}
	return cached(p_kernel, uniforms);
}
inline TypedArray<RDUniform> script_uniforms(const LocalVector<RD::Uniform> &p_uniforms) {
	TypedArray<RDUniform> result;
	for (const RD::Uniform &entry : p_uniforms) {
		Ref<RDUniform> item;
		item.instantiate();
		item->set_binding(entry.binding);
		item->set_uniform_type(entry.uniform_type);
		for (uint32_t i = 0; i < entry.get_id_count(); ++i) {
			item->add_id(entry.get_id(i));
		}
		result.push_back(item);
	}
	return result;
}
inline void matrix(float *r_target, const Projection &p_matrix) {
	for (int col = 0; col < 4; ++col) {
		for (int row = 0; row < 4; ++row) {
			r_target[col * 4 + row] = p_matrix[col][row];
		}
	}
}

struct Ranges {
	int extent = 0;
	LocalVector<Vector2i> free;
	int allocate(int p_size) {
		ERR_FAIL_COND_V(p_size <= 0, 0);
		for (uint32_t i = 0; i < free.size(); ++i) {
			Vector2i block = free[i];
			if (block.y < p_size) {
				continue;
			}
			if (block.y == p_size) {
				free.remove_at(i);
			} else {
				free[i] = Vector2i(block.x + p_size, block.y - p_size);
			}
			return block.x;
		}
		int start = extent;
		extent += p_size;
		return start;
	}
	void release(int p_start, int p_size) {
		uint32_t at = 0;
		while (at < free.size() && free[at].x < p_start) {
			++at;
		}
		free.insert(at, Vector2i(p_start, p_size));
		// The list is sorted and coalesced: only the inserted range's neighbors can join it.
		if (at > 0) {
			ERR_FAIL_COND(free[at - 1].x + free[at - 1].y > free[at].x);
			if (free[at - 1].x + free[at - 1].y == free[at].x) {
				free[at - 1].y += free[at].y;
				free.remove_at(at--);
			}
		}
		if (at + 1 < free.size()) {
			ERR_FAIL_COND(free[at].x + free[at].y > free[at + 1].x);
			if (free[at].x + free[at].y == free[at + 1].x) {
				free[at].y += free[at + 1].y;
				free.remove_at(at + 1);
			}
		}
		if (!free.is_empty() && free[free.size() - 1].x + free[free.size() - 1].y == extent) {
			extent = free[free.size() - 1].x;
			free.remove_at(free.size() - 1);
		}
	}
};
} //namespace SurfaceCache
