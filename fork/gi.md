# Surface Cache GI

- **Мир GI и его ресурсы** —
  [surface_cache_world.cpp](../modules/interior_surface_cache/surface_cache_world.cpp),
  [surface_cache_world.h](../modules/interior_surface_cache/surface_cache_world.h).

- **События изменения сцены** —
  [renderer_scene_cull.cpp](../servers/rendering/renderer_scene_cull.cpp),
  [renderer_scene_cull.h](../servers/rendering/renderer_scene_cull.h).

- **Подготовка геометрии и деформации** —
  [surface_cache_geometry.cpp](../modules/interior_surface_cache/surface_cache_geometry.cpp),
  [surface_cache_geometry.h](../modules/interior_surface_cache/surface_cache_geometry.h),
  [mesh_storage.cpp](../servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp),
  [mesh_storage.h](../servers/rendering/renderer_rd/storage_rd/mesh_storage.h),
  [интерфейс MeshStorage](../servers/rendering/storage/mesh_storage.h).

- **Захват материалов** —
  [render_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp),
  [render_forward_clustered.h](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h),
  [surface_cache_capture_inc.glsl](../servers/rendering/renderer_rd/shaders/surface_cache_capture_inc.glsl).

- **Зависимости материалов, текстур и света** —
  [material_storage.cpp](../servers/rendering/renderer_rd/storage_rd/material_storage.cpp),
  [material_storage.h](../servers/rendering/renderer_rd/storage_rd/material_storage.h),
  [интерфейс MaterialStorage](../servers/rendering/storage/material_storage.h),
  [texture_storage.cpp](../servers/rendering/renderer_rd/storage_rd/texture_storage.cpp),
  [texture_storage.h](../servers/rendering/renderer_rd/storage_rd/texture_storage.h),
  [light_storage.cpp](../servers/rendering/renderer_rd/storage_rd/light_storage.cpp),
  [utilities.h](../servers/rendering/storage/utilities.h).

- **Экранный GI, отражения и история видов** —
  [surface_cache_gather.cpp](../modules/interior_surface_cache/surface_cache_gather.cpp),
  [surface_cache_gather.h](../modules/interior_surface_cache/surface_cache_gather.h),
  [surface_cache_inc.glsl](../servers/rendering/renderer_rd/shaders/surface_cache_inc.glsl),
  [scene_forward_clustered.glsl](../servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl).

- **Состояние рендера** —
  [render_scene_data_rd.cpp](../servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.cpp),
  [render_scene_data_rd.h](../servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h),
  [scene_data_inc.glsl](../servers/rendering/renderer_rd/shaders/scene_data_inc.glsl),
  [renderer_scene_render.h](../servers/rendering/renderer_scene_render.h),
  [renderer_geometry_instance.h](../servers/rendering/renderer_geometry_instance.h).

- **Режимы и переменные GI в языке шейдеров** —
  [shader_types.cpp](../servers/rendering/shader_types.cpp),
  [shader_compiler.cpp](../servers/rendering/shader_compiler.cpp),
  [shader_compiler.h](../servers/rendering/shader_compiler.h),
  [scene_shader_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp),
  [scene_shader_forward_clustered.h](../servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h).

- **Заглушки GI для Forward Mobile и GLES3** —
  [scene_shader_forward_mobile.cpp](../servers/rendering/renderer_rd/forward_mobile/scene_shader_forward_mobile.cpp),
  [scene_forward_mobile.glsl](../servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl),
  [material_storage.cpp](../drivers/gles3/storage/material_storage.cpp),
  [scene.glsl](../drivers/gles3/shaders/scene.glsl).

- **API сцены** —
  [rendering_server.cpp](../servers/rendering/rendering_server.cpp),
  [rendering_server.h](../servers/rendering/rendering_server.h),
  [rendering_server_default.h](../servers/rendering/rendering_server_default.h),
  [rendering_method.h](../servers/rendering/rendering_method.h).

- **Подключение нативного модуля** —
  [register_types.cpp](../modules/interior_surface_cache/register_types.cpp),
  [register_types.h](../modules/interior_surface_cache/register_types.h),
  [surface_cache_runtime.h](../modules/interior_surface_cache/surface_cache_runtime.h),
  [SCsub](../modules/interior_surface_cache/SCsub),
  [config.py](../modules/interior_surface_cache/config.py).

Шаблоны GPU-проходов и скрипты игры — [освещение](../../is-game/game/features/lighting/README.md#gi).
Расширения Vulkan/RenderingDevice — [ray query](ray-query.md).
