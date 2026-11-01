# Самостоятельные исправления движка

| Исправление | Файлы |
|---|---|
| `--editor --script` не создаёт лишний SceneTree, который теряется при установке MainLoop скрипта | [Main::start](../main/main.cpp) |
| Запрос normal/roughness от Compositor учитывается и без Environment | [RenderForwardClustered::_render_scene](../servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp) |
| Вырожденный tangent basis и неиспользуемая normal map не порождают NaN | [scene_forward_clustered.glsl](../servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl) |
| Качество Hard у направленных теней не наследует билинейную PCF-фильтрацию sampler | [sample_directional_pcf_shadow](../servers/rendering/renderer_rd/shaders/scene_forward_lights_inc.glsl) |
