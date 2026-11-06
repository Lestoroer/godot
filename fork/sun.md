# Солнце по кастерам

Код карты лежит в [renderer_scene_cull_sun.cpp](../servers/rendering/renderer_scene_cull_sun.cpp). В
[renderer_scene_cull.cpp](../servers/rendering/renderer_scene_cull.cpp) остаются зацепки с метками: выбор режима
у направленного света, учёт изменений и удаления кастеров, шаги карты в кадре.

- **Режим теней `SHADOW_CASTER_BOUNDS` у DirectionalLight3D и в RenderingServer, отладочные настройки и состояние карты; в инспекторе режим скрывает дальность, затухание и pancake** —
  [rendering_server_enums.h](../servers/rendering/rendering_server_enums.h),
  [rendering_server.cpp](../servers/rendering/rendering_server.cpp),
  [rendering_server.h](../servers/rendering/rendering_server.h),
  [rendering_server_default.h](../servers/rendering/rendering_server_default.h),
  [rendering_method.h](../servers/rendering/rendering_method.h),
  [light_3d.h](../scene/3d/light_3d.h),
  [light_3d.cpp](../scene/3d/light_3d.cpp),
  [DirectionalLight3D.xml](../doc/classes/DirectionalLight3D.xml),
  [RenderingServer.xml](../doc/classes/RenderingServer.xml).

- **Область карты по кастерам: границы как состояние света, рост с запасом и сжатие в покое** —
  [renderer_scene_cull_sun.cpp](../servers/rendering/renderer_scene_cull_sun.cpp),
  [renderer_scene_cull.cpp](../servers/rendering/renderer_scene_cull.cpp),
  [renderer_scene_cull.h](../servers/rendering/renderer_scene_cull.h).

- **Окна карты для разнесённых построек: группы кастеров по зазорам в плоскости солнца, раскладка по атласу с общим текселем, окно-попутчик для вынесенного предмета, выбор окна в шейдере и поднятие точки ниже дальней плоскости** —
  [renderer_scene_cull_sun.cpp](../servers/rendering/renderer_scene_cull_sun.cpp),
  [renderer_scene_cull.h](../servers/rendering/renderer_scene_cull.h),
  [light_data_inc.glsl](../servers/rendering/renderer_rd/shaders/light_data_inc.glsl),
  [scene_forward_clustered.glsl](../servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl).

- **Постоянная карта: единая точка учёта изменений кастеров, подвижные и неподвижные кастеры, устаревшие плитки, ограничение кадром камеры, шаги обновления** —
  [renderer_scene_cull_sun.cpp](../servers/rendering/renderer_scene_cull_sun.cpp),
  [renderer_scene_cull.cpp](../servers/rendering/renderer_scene_cull.cpp),
  [renderer_scene_cull.h](../servers/rendering/renderer_scene_cull.h),
  [sun_tile_mask.h](../servers/rendering/sun_tile_mask.h),
  [renderer_scene_render.h](../servers/rendering/renderer_scene_render.h).

- **Изменения, о которых кулл раньше не знал: параметр материала, от которого зависит глубина, трансформы мультимеша, вырез от времени** —
  [utilities.h](../servers/rendering/storage/utilities.h),
  [material_storage.h](../servers/rendering/renderer_rd/storage_rd/material_storage.h),
  [material_storage.cpp](../servers/rendering/renderer_rd/storage_rd/material_storage.cpp),
  [scene_shader_forward_clustered.h](../servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h),
  [scene_shader_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp),
  [mesh_storage.h](../servers/rendering/renderer_rd/storage_rd/mesh_storage.h),
  [mesh_storage.cpp](../servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp).

- **Статическая карта и эталон для проверки, владелец атласа, проход теней по шагам с копией и ножницами** —
  [light_storage.h (общий)](../servers/rendering/storage/light_storage.h),
  [light_storage.cpp](../servers/rendering/renderer_rd/storage_rd/light_storage.cpp),
  [light_storage.h](../servers/rendering/renderer_rd/storage_rd/light_storage.h),
  [render_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp),
  [render_forward_clustered.h](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h).

- **Метки времени вокруг проходов теней направленного света для сводки кадра игры** —
  [renderer_scene_render.h](../servers/rendering/renderer_scene_render.h),
  [render_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp),
  [render_forward_clustered.h](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h).

- **Режим не отсекает кастеры по кадру камеры в штатном кулле** —
  [rendering_light_culler.cpp](../servers/rendering/rendering_light_culler.cpp).

- **Проход теней режима рисует LOD0** —
  [render_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp),
  [render_forward_mobile.cpp](../servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp).

- **Режим обрабатывается как один разделитель в данных света; Forward Mobile и GLES3 перерисовывают карту каждый кадр** —
  [light_storage.cpp](../servers/rendering/renderer_rd/storage_rd/light_storage.cpp),
  [light_storage.cpp (GLES3)](../drivers/gles3/storage/light_storage.cpp),
  [rasterizer_scene_gles3.cpp](../drivers/gles3/rasterizer_scene_gles3.cpp).
