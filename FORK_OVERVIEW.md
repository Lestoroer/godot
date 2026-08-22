# Lestoroer Godot Fork — краткая карта отличий

Форк основан на официальной ветке Godot 4.7 и близок к upstream. Собственные
изменения точечные и перечислены ниже на уровне доступных возможностей. Если
область здесь не названа, в ней нет намеренных отличий от официального движка.
Точная версия базы, реализация патчей и правила обновления находятся в
`FORK_NOTES.md`.

## Что изменено

### Доступ к depth-текстуре viewport

- Добавлен `RenderingServer.viewport_get_depth_texture_rd(viewport)` для
  получения RD RID depth-текстуры 3D-вьюпорта.
- Depth-текстуры создаются с возможностью копирования.
- RD-обёртки поддерживают используемые проектом packed depth(+stencil) форматы.

### Depth compare в gdshader

- В язык шейдеров и Shader Globals добавлен `sampler2DArrayShadow`.
- Он использует штатный shadow sampler сцены и предназначен для spatial-шейдеров.
- Compatibility renderer (GLES3), canvas, sky и particles этот путь не поддерживают.

### D16 depth-текстуры

- Для `DATA_FORMAT_D16_UNORM` используется identity swizzle, необходимый Vulkan
  для compare-семплинга.

### Беззнаковые текстуры в Shader Globals

- В Shader Globals добавлен `usampler2D`.
- RD-обёртки поддерживают `DATA_FORMAT_R32G32B32A32_UINT` для этого пути.

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

- Добавлены `RenderingServer.instance_geometry_set_highlight_style(instance, style)`
  со значениями `0…3` и read-only `instance_geometry_get_highlight_style(instance)`; getter читает
  сохранённое scene-cull состояние без GPU readback.
- При включённой возможности Forward Mobile использует alpha существующего scene
  color как внутренний A2 style ID: обычные opaque поверхности сохраняют alpha,
  а подсвеченные записывают стиль с обычным depth test. Mobile tonemap строит из ID
  внешний screen-space контур одного из трёх глобальных цветов.
- На Vulkan с dual-source blending прозрачный material продолжает смешивать RGB своей
  настоящей opacity из второго fragment output. Alpha-to-coverage не получает outline;
  без dual-source все ненулевые стили автоматически используют первый цвет как прежнюю
  бинарную coverage-mask.
- Для устойчивого цвета после MSAA resolve tonemap определяет coverage четырьмя обычными
  taps, а ID читает одним дополнительным tap глубже внутрь только на найденном контуре.
- Меш не рисуется второй раз: подсветка не добавляет draw calls и треугольники.
  Дополнительного color attachment, MSAA resolve и отдельной mask-текстуры нет.
  Кадры без видимой подсветки используют полностью штатные alpha pipeline states.
- Возможность включается до старта renderer через
  `rendering/renderer/highlight_outline/enabled`; ширина задаётся в пикселях через
  `rendering/renderer/highlight_outline/width`, три цвета — через
  `rendering/renderer/highlight_outline/color_1…3`.
- Реализация предназначена для Forward Mobile. Она depth-tested, не является
  x-ray; несколько мешей и соприкасающиеся подсвеченные объекты одного стиля образуют общий
  силуэт. Между соприкасающимися объектами разных стилей возможна граница палитры.
- Outline намеренно не включается для transparent viewport/passthrough, reflection
  probes и background `KEEP`, `CANVAS`, `CAMERA_FEED`, где alpha уже имеет другой
  compositing-контракт. В highlight-кадре alpha `SCREEN_TEXTURE` зарезервирована
  под coverage-mask.

## Что не изменено

В форке нет собственных патчей физики, физических запросов, collision data или
ShapeCast. Для них ожидается поведение официального Godot 4.7, однако критичные
для проекта свойства всё равно следует проверять техническими probe на текущей
сборке форка.
