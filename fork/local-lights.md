# Собственный локальный свет

- **Модуль: реестр ламп, сетка и данные для материалов, очередь и карты теней, проходы отношения и размытия** —
  [local_lights_world.cpp](../modules/local_lights/local_lights_world.cpp),
  [local_lights_world.h](../modules/local_lights/local_lights_world.h),
  [local_light_caster.glsl](../modules/local_lights/shaders/local_light_caster.glsl),
  [local_light_ratio.glsl](../modules/local_lights/shaders/local_light_ratio.glsl),
  [local_light_ratio_surface_inc.glsl](../modules/local_lights/shaders/local_light_ratio_surface_inc.glsl),
  [local_light_blur.glsl](../modules/local_lights/shaders/local_light_blur.glsl).

- **Настройка проекта, включение на мир, настройки и показатели для скриптов** —
  [local_lights.cpp](../modules/local_lights/local_lights.cpp),
  [local_lights.h](../modules/local_lights/local_lights.h),
  [register_types.cpp](../modules/local_lights/register_types.cpp),
  [register_types.h](../modules/local_lights/register_types.h),
  [config.py](../modules/local_lights/config.py),
  [SCsub](../modules/local_lights/SCsub).

- **Лампы мира уходят из штатного света в кулле камеры, GI их не учитывает, доступ модуля к флагу обновления тени, предмет за радиусом лампы её не обновляет** —
  [renderer_scene_cull.cpp](../servers/rendering/renderer_scene_cull.cpp),
  [renderer_scene_cull.h](../servers/rendering/renderer_scene_cull.h).

- **Предварительная глубина, запуск экранных проходов в кадре и признак вида для материалов** —
  [render_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp),
  [renderer_scene_render_rd.cpp](../servers/rendering/renderer_rd/renderer_scene_render_rd.cpp),
  [local_lights_screen_hook.h](../servers/rendering/local_lights_screen_hook.h).

- **Формат A2B10G10R10 у текстуры, обёрнутой для материалов** —
  [texture_storage.cpp](../servers/rendering/renderer_rd/storage_rd/texture_storage.cpp).
