# Самостоятельные исправления движка

- **`--editor --script` не создаёт лишний SceneTree, теряемый при установке MainLoop скрипта** —
  [main.cpp](../main/main.cpp).

- **Запрос normal/roughness от Compositor учитывается и без Environment** —
  [render_forward_clustered.cpp](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp).

- **Вырожденный tangent basis и неиспользуемая normal map не порождают NaN** —
  [scene_forward_clustered.glsl](../servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl).

- **Качество Hard у направленных теней не наследует билинейную PCF-фильтрацию sampler** —
  [scene_forward_lights_inc.glsl](../servers/rendering/renderer_rd/shaders/scene_forward_lights_inc.glsl).

- **Маска видимости RT-дескрипторов использует биты стадий** —
  [rendering_shader_container.cpp](../servers/rendering/rendering_shader_container.cpp).

- **TLAS использует адрес acceleration structure; битовые поля экземпляра формируются в CPU-памяти перед записью в upload-буфер** —
  [rendering_device_driver_vulkan.cpp](../drivers/vulkan/rendering_device_driver_vulkan.cpp),
  [rendering_device_driver_vulkan.h](../drivers/vulkan/rendering_device_driver_vulkan.h).

- **Первый диапазон записи в буфер экземпляров TLAS резервируется до следующей сборки** —
  [rendering_device.cpp](../servers/rendering/rendering_device.cpp).

- **RT/query SPIR-V обходит re-spirv, который не поддерживает эти инструкции** —
  [rendering_device_driver_vulkan.cpp](../drivers/vulkan/rendering_device_driver_vulkan.cpp).

- **Release-сборка не отклоняет любой индексный буфер при создании BLAS: `max_index` известен только с DEBUG_ENABLED** —
  [rendering_device.cpp](../servers/rendering/rendering_device.cpp).
