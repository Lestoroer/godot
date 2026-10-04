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

## Что не изменено

В форке нет собственных патчей физики, физических запросов, collision data или
ShapeCast. Для них ожидается поведение официального Godot 4.7, однако критичные
для проекта свойства всё равно следует проверять техническими probe на текущей
сборке форка.
