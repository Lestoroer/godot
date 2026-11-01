# Тени Voxel Underworld

- **Отсечение треугольников без покрытия MSAA2: `Geometry3D.filter_shadow_sample_coverage`** —
  [core_bind.cpp](../core/core_bind.cpp),
  [core_bind.h](../core/core_bind.h),
  [Geometry3D.xml](../doc/classes/Geometry3D.xml).

- **Смена viewport из GDScript внутри прохода теневого атласа: `draw_list_set_viewport`** —
  [rendering_device.cpp](../servers/rendering/rendering_device.cpp),
  [RenderingDevice.xml](../doc/classes/RenderingDevice.xml).
