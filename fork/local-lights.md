# Собственный локальный свет

- **Модуль: реестр ламп, сетка и данные для материалов, очередь, карты теней и их моменты для мягкой тени** —
  [local_lights_world.cpp](../modules/local_lights/local_lights_world.cpp),
  [local_lights_world.h](../modules/local_lights/local_lights_world.h),
  [local_light_caster.glsl](../modules/local_lights/shaders/local_light_caster.glsl),
  [local_light_moments.glsl](../modules/local_lights/shaders/local_light_moments.glsl).

- **Настройка проекта, включение на мир, настройки и показатели для скриптов** —
  [local_lights.cpp](../modules/local_lights/local_lights.cpp),
  [local_lights.h](../modules/local_lights/local_lights.h),
  [register_types.cpp](../modules/local_lights/register_types.cpp),
  [register_types.h](../modules/local_lights/register_types.h),
  [config.py](../modules/local_lights/config.py),
  [SCsub](../modules/local_lights/SCsub).

- **Лампы мира уходят из штатного света в кулле камеры, GI их не учитывает и не получает их перемещения, доступ модуля к флагу обновления тени, предмет за радиусом лампы её не обновляет, позиция камеры вида для важности ламп в бюджете карт, пары лампы с геометрией по коробке с запасом, признак вида с локальным светом** —
  [renderer_scene_cull.cpp](../servers/rendering/renderer_scene_cull.cpp),
  [renderer_scene_cull.h](../servers/rendering/renderer_scene_cull.h).

- **Признак вида с локальным светом для материалов** —
  [renderer_scene_render_rd.cpp](../servers/rendering/renderer_rd/renderer_scene_render_rd.cpp),
  [local_lights_active_view.h](../servers/rendering/local_lights_active_view.h).
