# Ray query и расширения BLAS

- **Поддержка Vulkan ray query и чтения позиций треугольников из BLAS** —
  [rendering_device_driver_vulkan.cpp](../drivers/vulkan/rendering_device_driver_vulkan.cpp),
  [rendering_device_driver_vulkan.h](../drivers/vulkan/rendering_device_driver_vulkan.h),
  [rendering_device_commons.h](../servers/rendering/rendering_device_commons.h).

- **TLAS для ray query без shader binding table** —
  [rendering_device.cpp](../servers/rendering/rendering_device.cpp),
  [RDAccelerationStructureInstance.xml](../doc/classes/RDAccelerationStructureInstance.xml).

- **Зависимости и барьеры BLAS/TLAS для ray query, включая обход ошибки NVIDIA 610.88** —
  [rendering_device.cpp](../servers/rendering/rendering_device.cpp),
  [rendering_device.h](../servers/rendering/rendering_device.h),
  [rendering_device_graph.cpp](../servers/rendering/rendering_device_graph.cpp),
  [rendering_device_commons.h](../servers/rendering/rendering_device_commons.h),
  [rendering_device_driver_vulkan.cpp](../drivers/vulkan/rendering_device_driver_vulkan.cpp).

- **Освобождение памяти сборки BLAS через `keep_build_memory`** —
  [rendering_device.cpp](../servers/rendering/rendering_device.cpp),
  [rendering_device.h](../servers/rendering/rendering_device.h),
  [RenderingDevice.xml](../doc/classes/RenderingDevice.xml).

Общие исправления существующего RT — [fixes](fixes.md).
