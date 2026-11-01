# Рендер без окна на Windows

- **Vulkan-рендер без окна операционной системы** —
  [display_server_offscreen.cpp](../servers/display/display_server_offscreen.cpp),
  [display_server_offscreen.h](../servers/display/display_server_offscreen.h).

- **Параметр `--offscreen` и регистрация режима на Windows** —
  [main.cpp](../main/main.cpp),
  [display_server_windows.cpp](../platform/windows/display_server_windows.cpp).

- **Главный RenderingDevice без окна и swapchain** —
  [rendering_device.cpp](../servers/rendering/rendering_device.cpp),
  [rendering_device.h](../servers/rendering/rendering_device.h).

- **Проверка порядка регистрации display-серверов** —
  [test_display_server_registration.cpp](../tests/servers/test_display_server_registration.cpp).
