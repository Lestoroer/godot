# Lestoroer Godot Fork — краткая карта отличий

Форк основан на официальной ветке Godot 4.7 и близок к upstream. Собственные
изменения точечные и перечислены ниже на уровне доступных возможностей. Если
область здесь не названа, в ней нет намеренных отличий от официального движка.
Точная версия базы, реализация патчей и правила обновления находятся в
`FORK_NOTES.md`.

## Что изменено

### Фоновый GPU-рендеринг без окна

- На Windows флаг `--offscreen` запускает настоящий Vulkan Mobile/Forward renderer
  без создания игрового/видимого окна, display surface и swapchain. GPU-сцена,
  viewport texture, PNG/movie writer и
  shader/RD-проверки работают, но окно не появляется в taskbar или Alt-Tab и не
  может получить системный фокус.
- Режим не обращается к системной мыши: `MOUSE_MODE_CAPTURED` хранится логически,
  но не вызывает `ClipCursor`, `SetCapture` или перемещение физического курсора.
  Audio всегда Dummy, XR отключён, accessibility dummy.
- Windows IME и GPU-драйвер могут создавать собственные невидимые служебные HWND;
  они не являются окном Godot, не видны, не получают foreground и не появляются
  в taskbar/Alt-Tab.
- Режим предназначен только для запуска проекта и доступен только явно из CLI.
  Editor, project manager, XR, native subwindows, другие rendering drivers и
  автоматический fallback не поддерживаются и завершаются понятной ошибкой.
- Без surface главный `RenderingDevice` сохраняет обычные singleton-инварианты и
  PSO cache, но работает с одним frame-in-flight. Поэтому режим подходит для
  correctness-тестов, GPU-картинки и скриншотов, но его frame timing нельзя
  сравнивать с обычным оконным запуском или Quest. Перф проекта по-прежнему
  измеряется на устройстве.

### Renderer-integrated outline в Forward Mobile

- Добавлены `RenderingServer.instance_geometry_set_highlighted(instance, enabled)`
  и read-only `instance_geometry_is_highlighted(instance)`; getter читает
  сохранённое scene-cull состояние без GPU readback.
- При включённой возможности Forward Mobile использует alpha существующего scene
  color как внутреннюю coverage-mask: обычные opaque поверхности сохраняют alpha,
  а подсвеченные записывают её с обычным depth test. Mobile tonemap строит из mask
  внешний screen-space контур.
- Меш не рисуется второй раз: подсветка не добавляет draw calls и треугольники.
  Дополнительного color attachment, MSAA resolve и отдельной mask-текстуры нет.
  Кадры без видимой подсветки используют полностью штатные alpha pipeline states.
- Возможность включается до старта renderer через
  `rendering/renderer/highlight_outline/enabled`; ширина задаётся в пикселях через
  `rendering/renderer/highlight_outline/width`, общий цвет — через
  `rendering/renderer/highlight_outline/color`.
- Реализация предназначена для Forward Mobile. Она depth-tested, не является
  x-ray; несколько мешей и соприкасающиеся подсвеченные объекты образуют общий
  силуэт. Первый контракт использует одну бинарную маску и один общий стиль.
- Outline намеренно не включается для transparent viewport/passthrough, reflection
  probes и background `KEEP`, `CANVAS`, `CAMERA_FEED`, где alpha уже имеет другой
  compositing-контракт. В highlight-кадре alpha `SCREEN_TEXTURE` зарезервирована
  под coverage-mask.

### Инкрементальный Android export из CLI

- Android exporter сохраняет состояние Gradle-кэша между процессами редактора.
  Повторный headless export больше не считается первой сборкой и не вызывает
  `gradle clean` без причины.
- Clean сохраняется для первого export, другого build-каталога, изменившегося
  набора Android export-плагинов и обновлённых legacy Android-плагинов.
- Состояние лежит рядом со сгенерированным Android build template и исчезает
  вместе с ним при его переустановке.
- Для APK готовый результат Gradle копируется напрямую; второй запуск Gradle
  только ради `copyAndRenameBinary` используется лишь как fallback.
- `EditorInterface.export_project()` позволяет editor-плагину запускать export
  из уже прогретого процесса редактора.

### Смена viewport внутри RD-прохода

- Существующий `RenderingDevice.draw_list_set_viewport(draw_list, rect)` доступен
  из GDScript. Он позволяет рисовать независимые тайлы теней в общем framebuffer
  без завершения render pass. Scissor задаётся отдельно.

### Отсечение пустых MSAA2-треугольников для теней

- `Geometry3D.filter_shadow_sample_coverage()` возвращает индексы треугольников,
  потенциально покрывающих хотя бы один стандартный MSAA2-сэмпл в 512px-тайле.
- Это явный вызов проекта; штатный renderer его не использует. Движущаяся
  геометрия требует нового расчёта.

### Внешняя нативная подача теней Voxel Underworld

- При сборке с проектным модулем `game/lighting/native/vu_shadow_submit`
  доступен класс `VuShadowSubmit`. Он переносит исполнение готовых команд из
  GDScript в C++, используя публичный RD без изменения renderer или GPU-проходов.
- Исходники, контракт и сборочная команда находятся в игровом репозитории;
  обычная сборка форка без `custom_modules` этот класс не содержит.

## Что не изменено

В форке нет собственных патчей физики, физических запросов, collision data или
ShapeCast. Для них ожидается поведение официального Godot 4.7, однако критичные
для проекта свойства всё равно следует проверять техническими probe на текущей
сборке форка.

## Экспериментальная ветка Surface Cache GI

Только `codex/gi-surface-cache`: RT descriptors получают правильную stage mask,
Vulkan корректно включает поддерживаемый
`VK_KHR_ray_query` и передаёт RT/query SPIR-V драйверу без неподдерживаемой
обработки re-spirv. Самого Surface Cache GI этот патч ещё не добавляет.

В этой же экспериментальной ветке добавлены недеструктивные charts и GPU-захват
материала фактического экземпляра в Forward+. Контракт описан в `FORK_NOTES.md`.
Это материальная основа Surface Cache; освещение поверх неё проверяется отдельно.

Scenario может передавать Surface Cache адресные изменения объектов и один
callback за кадр мира. Доступ к skin/morph-буферу не требует GPU readback.

Forward+ может выбирать diffuse irradiance из world-owned Surface Cache по
исходной поверхности. Пользовательский IRRADIANCE заменяется до AO и тонемапа.

Экспериментальная RT-интеграция использует Vulkan AS device addresses и раздельные
диапазоны данных нескольких TLAS build в одном кадре.

В эксперименте Surface Cache неактивный normal mapping не зависит от
вырожденного tangent basis; адреса тонких треугольников вычисляются устойчивее.

Surface layout сохраняет идентификаторы charts, выданные xatlas, для изоляции
фильтрации света на несвязанных поверхностях.

Surface Cache: отдельный callback Scenario после depth/normal prepass выполняется
для каждого viewport (включая зеркало), независимо от его Compositor. Binding 41
читает viewport-текстуру `surface_cache/gather`; глубина проверяется до применения
непрямого света к фрагменту. Общий world cache обновляется один раз за кадр.

Surface Cache: read-only `SURFACE_CACHE_IRRADIANCE` передаёт material fragment
линейное непрямое освещение до художественной ramp (alpha=0, если GI отсутствует).
Материал, использующий вход, сам пишет IRRADIANCE; renderer не подменяет его
повторно. Mobile/GLES возвращают нулевую alpha. Очистка адресов удалённого
instance идемпотентна.

Surface Cache: depth/normal prepass сохраняет surface и primitive ID только
при активном GI. MSAA выбирает ID того же sample, что depth/normal. Final gather
и материал проверяют эту идентичность; поиска треугольника по допуску глубины
и зависимости от схемы MSAA samples видеокарты нет.

Surface Cache propagates changes in mesh content, texture pixels and global
shader inputs. Its material coverage is independent of fragment discard.
Shader vertex displacement is an explicit unsupported transport contract.
