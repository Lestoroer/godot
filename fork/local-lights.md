# Локальное освещение Interior Star

- Нативный реестр ламп, транспорт и карты видимости — `modules/interior_local_lights/`.
- Независимые уведомления об изменениях геометрии и ламп, участие скрытых стандартных источников в GI — `servers/rendering/renderer_scene_cull.h`, `servers/rendering/renderer_scene_cull.cpp`.
- Переключаемый вклад локальных ламп в GI — `modules/interior_surface_cache/surface_cache_world.cpp`.
