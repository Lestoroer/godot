# Контур выделения в Forward Mobile

- **API `instance_geometry_set_highlighted` и настройки контура** —
  [rendering_server.cpp](../servers/rendering/rendering_server.cpp),
  [rendering_server.h](../servers/rendering/rendering_server.h),
  [rendering_server_default.h](../servers/rendering/rendering_server_default.h),
  [rendering_method.h](../servers/rendering/rendering_method.h),
  [RenderingServer.xml](../doc/classes/RenderingServer.xml).

- **Состояние выделения экземпляров** —
  [renderer_geometry_instance.cpp](../servers/rendering/renderer_geometry_instance.cpp),
  [renderer_geometry_instance.h](../servers/rendering/renderer_geometry_instance.h),
  [renderer_scene_cull.cpp](../servers/rendering/renderer_scene_cull.cpp),
  [renderer_scene_cull.h](../servers/rendering/renderer_scene_cull.h).

- **Маска выделения и проходы Forward Mobile** —
  [render_forward_mobile.cpp](../servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp),
  [render_forward_mobile.h](../servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.h),
  [scene_shader_forward_mobile.cpp](../servers/rendering/renderer_rd/forward_mobile/scene_shader_forward_mobile.cpp),
  [scene_shader_forward_mobile.h](../servers/rendering/renderer_rd/forward_mobile/scene_shader_forward_mobile.h).

- **Передача маски в постобработку** —
  [renderer_scene_render_rd.cpp](../servers/rendering/renderer_rd/renderer_scene_render_rd.cpp),
  [render_data_rd.h](../servers/rendering/renderer_rd/storage_rd/render_data_rd.h).

- **Экранный контур в tonemap** —
  [tone_mapper.cpp](../servers/rendering/renderer_rd/effects/tone_mapper.cpp),
  [tone_mapper.h](../servers/rendering/renderer_rd/effects/tone_mapper.h),
  [tonemap_mobile.glsl](../servers/rendering/renderer_rd/shaders/effects/tonemap_mobile.glsl).

- **Сохранение маски при отрисовке неба** —
  [sky.cpp](../servers/rendering/renderer_rd/environment/sky.cpp),
  [sky.h](../servers/rendering/renderer_rd/environment/sky.h).
