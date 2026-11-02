# Физика ткани на Jolt

Маркер в коде — `lestoroer@physics`.

- **Ресурс ткани `ClothMesh`: частицы, позы, закрепления, сборка из glTF** —
  [cloth_mesh.cpp](../modules/jolt_physics/cloth/cloth_mesh.cpp),
  [cloth_mesh.h](../modules/jolt_physics/cloth/cloth_mesh.h).

- **Узел `ClothBody3D`: поза, сон, отрисовка через скелет и формы** —
  [cloth_body_3d.cpp](../modules/jolt_physics/cloth/cloth_body_3d.cpp),
  [cloth_body_3d.h](../modules/jolt_physics/cloth/cloth_body_3d.h).

- **Мягкое тело Jolt `JoltCloth3D`: следование позе, толчки курсора, зажатые частицы** —
  [jolt_cloth_3d.cpp](../modules/jolt_physics/objects/jolt_cloth_3d.cpp),
  [jolt_cloth_3d.h](../modules/jolt_physics/objects/jolt_cloth_3d.h).

- **Новый тип объекта Jolt и его взаимодействия** —
  [jolt_object_3d.cpp](../modules/jolt_physics/objects/jolt_object_3d.cpp),
  [jolt_object_3d.h](../modules/jolt_physics/objects/jolt_object_3d.h).

- **Контакты частиц из слушателя Jolt** —
  [jolt_contact_listener_3d.cpp](../modules/jolt_physics/spaces/jolt_contact_listener_3d.cpp).

- **Регистрация классов и сборка** —
  [register_types.cpp](../modules/jolt_physics/register_types.cpp),
  [SCsub](../modules/jolt_physics/SCsub).
