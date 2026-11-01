# Отличия форка

Правила изменения, ветки и обновление upstream — в [AGENTS.md](AGENTS.md).
Пути ниже отсчитываются от корня движка. Маркер поиска — `Fork(Lestoroer)`.

| Возможность | Где искать |
|---|---|
| Vulkan без окна на Windows, `--offscreen` | [display_server_offscreen.cpp](servers/display/display_server_offscreen.cpp), `main/main.cpp`, `platform/windows/display_server_windows.cpp`, `servers/rendering/rendering_device.*` |
| Контур выделенных объектов в Forward Mobile | [forward_mobile](servers/rendering/renderer_rd/forward_mobile/), [tone_mapper.cpp](servers/rendering/renderer_rd/effects/tone_mapper.cpp), [sky.cpp](servers/rendering/renderer_rd/environment/sky.cpp); API — `RenderingServer.instance_geometry_set_highlighted` |
| Повторный Android-экспорт без лишнего Gradle clean; экспорт из редактора через API | [export_plugin.cpp](platform/android/export/export_plugin.cpp), `editor/editor_interface.*` |
| `draw_list_set_viewport` из GDScript | [rendering_device.cpp](servers/rendering/rendering_device.cpp) |
| Отсечение треугольников без покрытия MSAA2 для теней VU | [core_bind.cpp](core/core_bind.cpp), `Geometry3D.filter_shadow_sample_coverage` |
| Аппаратные ray query, position fetch и управление памятью BLAS | [Vulkan](drivers/vulkan/rendering_device_driver_vulkan.cpp), [RenderingDevice](servers/rendering/rendering_device.cpp), [rendering_shader_container.cpp](servers/rendering/rendering_shader_container.cpp) |
| Захват материалов Surface Cache тем же шейдером | [material_storage.cpp](servers/rendering/renderer_rd/storage_rd/material_storage.cpp), [render_forward_clustered.cpp](servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp), [surface_cache_capture_inc.glsl](servers/rendering/renderer_rd/shaders/surface_cache_capture_inc.glsl) |
| События изменения сцены и доступ к деформированным мешам для GI | [renderer_scene_cull.cpp](servers/rendering/renderer_scene_cull.cpp), [rendering_server.cpp](servers/rendering/rendering_server.cpp), [mesh_storage.cpp](servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp) |
| Непрямой свет и отражения Surface Cache в Forward+ | [forward_clustered](servers/rendering/renderer_rd/forward_clustered/), [surface_cache_inc.glsl](servers/rendering/renderer_rd/shaders/surface_cache_inc.glsl), [render_scene_data_rd.cpp](servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.cpp) |
| Режимы и встроенные переменные Surface Cache в языке шейдеров | [shader_types.cpp](servers/rendering/shader_types.cpp), [shader_compiler.cpp](servers/rendering/shader_compiler.cpp), [scene_shader_forward_clustered.cpp](servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp); заглушки — Forward Mobile и GLES3 |
| Нативная подготовка геометрии, мир GI и история видов | [interior_surface_cache](modules/interior_surface_cache/); [шаблоны и скрипты игры](../is-game/game/features/lighting/surface_cache/) |
| Создание кешированных вариантов шейдеров на GPU по запросу | `servers/rendering/renderer_rd/shader_rd.*` |
| Callback `POST_TEMPORAL`; normal/roughness для Compositor без WorldEnvironment | `scene/resources/compositor.*`, `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp` |
| Нефильтрованные направленные тени при качестве Hard | [scene_forward_lights_inc.glsl](servers/rendering/renderer_rd/shaders/scene_forward_lights_inc.glsl) |
| Запуск `--editor --script` без лишнего SceneTree | [main.cpp](main/main.cpp) |
| Уведомления при обновлении областей меша; пропуск повторных одинаковых параметров материалов и света | [mesh_storage.cpp](servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp), [material_storage.cpp](servers/rendering/renderer_rd/storage_rd/material_storage.cpp), [light_storage.cpp](servers/rendering/renderer_rd/storage_rd/light_storage.cpp) |

Пути в столбце могут перечислять основные точки входа. Полный набор изменений —
сравнение этой ветки с upstream, а не перечень строк таблицы.
