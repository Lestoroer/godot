# Callback POST_TEMPORAL

- **Тип callback в CompositorEffect и RenderingServer** —
  [compositor.cpp](../scene/resources/compositor.cpp),
  [compositor.h](../scene/resources/compositor.h),
  [rendering_server.cpp](../servers/rendering/rendering_server.cpp),
  [rendering_server_enums.h](../servers/rendering/rendering_server_enums.h).

- **Вызов с HDR после TAA или temporal upscaling, до tonemap** —
  [render_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp).
