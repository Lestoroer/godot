# Lestoroer Godot Fork — обслуживание и обновление движка

Краткая карта пользовательских отличий от upstream находится в `FORK_OVERVIEW.md`.
Перед любым изменением движка агент обязан прочитать этот файл, записать изменение
в инвентарь и связанные инструкции ниже, а при изменении доступных возможностей
или поведения также обновить `FORK_OVERVIEW.md`.

Личный форк Godot Engine. С 03.07.2026 в дереве есть **свои патчи** (см. «Инвентарь патчей»
внизу) — дерево `lestoroer/main` НЕ равно upstream, при апгрейде вместо read-tree-трюка (шаг 3)
нужен **честный merge** с разруливанием конфликтов по каждому патчу. Все патч-строки помечены
маркером `Fork(Lestoroer)` в комментариях — `grep -rn "Fork(Lestoroer)"` показывает весь дифф.
Этот файл — рабочая инструкция: вышел новый релиз → подтянуть его в форк. Писано так, чтобы
агент **без контекста** сделал это быстро и не наступил на грабли.

Инструкция — для **Windows** (основная машина). Где на **🍎 macOS** иначе — врезка `🍎 MACOS:`.
Сами git-шаги на обеих машинах идентичны; различается только сборка (шаги 5–6).

## Машины: пути и тулчейн

|                         | Windows (основная)                          | 🍎 macOS (MBP M1 Max)                      |
|-------------------------|---------------------------------------------|--------------------------------------------|
| Чекаут форка            | `D:\Godot\godot`                            | `~/godot`                                  |
| Вызов scons             | `py -m SCons …` ¹                           | `scons …` ²                                |
| Флаги сборки редактора  | `platform=windows target=editor d3d12=no -j16` | `platform=macos target=editor arch=arm64 vulkan=no -j10` |
| Бинарь редактора        | `bin\godot.windows.editor.x86_64.exe`       | `bin/godot.macos.editor.arm64`             |
| Объектный кеш           | `bin\obj\`                                  | `bin/obj/`                                 |

¹ **Windows:** `scons` НЕ в PATH; `python` в PATH — это заглушка Microsoft Store (падает с
«Python was not found»). Настоящий Python 3.10 — в `C:\Users\Sergey\AppData\Local\Programs\Python\Python310`,
SCons 4.10 в его user-site. Поэтому всегда `py -m SCons …`. MSVC (VS2022) scons находит сам —
Developer-консоль НЕ нужна, годится обычный шелл (PowerShell/Git Bash).

² **🍎 MACOS:** scons ставился `pip3 install --user scons` (Homebrew на маке НЕТ) → бинарь
`~/Library/Python/3.9/bin/scons`. Если `scons` уже в PATH — просто `scons`.

По умолчанию собираем **НЕ-dev** (правило Сергея: повседневные запуски — через не-dev сборку).
dev-сборка (асерты `DEV_ENABLED`, для отладки самого движка) — добавить `dev_build=yes`; тогда в
имени бинаря и объектников появляется `.dev`.

---

## ⚡ Обновить движок на новый релиз — по шагам

> Пример: было `4.7-rc3`, вышел `4.7-stable` — обновляемся. Та же процедура для патча (`4.7.1`)
> и для нового минора (`4.8-*`). Шаги 0–4, 7–8 одинаковы на Windows и маке; различается только
> сборка (5–6).

### 0. Узнать цель и где её исходник
```bash
cd <чекаут форка>          # D:\Godot\godot  (🍎 ~/godot)
git fetch upstream
```
- **Стабл** (`X.Y-stable` или патч `X.Y.Z`): у upstream есть ветка с именем минора. Проверка:
  ```bash
  git ls-remote --heads upstream 4.7      # есть строка → исходник = upstream/4.7
  ```
  → дальше `REF = upstream/4.7`.
- **Пре-релиз** (dev/beta/rc, напр. `4.8-rc1`): ветки минора у upstream ещё НЕТ. Исходник —
  коммит на `master`, ссылка в манифесте `godot-builds`:
  ```bash
  git fetch builds "refs/tags/4.8-rc1:refs/tags/4.8-rc1"
  git show 4.8-rc1:releases/godot-4.8-rc1.json | grep git_reference   # → коммит
  ```
  → дальше `REF = <этот коммит>`.

### 1. Якорь для отката
```bash
git tag -f fork-pre-update lestoroer/main     # откат при беде: git reset --hard fork-pre-update
```

### 2. Навести `<minor>-base` на исходник
```bash
git branch -f 4.7-base <REF>                  # REF = upstream/4.7  (или коммит — для пре-релиза)
```

### 3. Влить в `lestoroer/main`
> ⚠️ С появлением патчей (см. «Инвентарь патчей») старый read-tree-трюк ЗАПРЕЩЁН — он
> молча выкинет наши патчи. Теперь только честный merge:
```bash
git checkout lestoroer/main
git merge --no-ff 4.7-base      # конфликты разруливать РУКАМИ, сверяясь с «Инвентарём патчей»
```
Конфликт в файле из инвентаря → сохранить и upstream-изменение, и наш `Fork(Lestoroer)`-блок.
После merge: `grep -rn "Fork(Lestoroer)" core/ editor/ scene/ servers/ drivers/ platform/ modules/ doc/ | wc -l` — число строк-маркеров
не должно уменьшиться против инвентаря.
> Историческая справка: пока патчей не было, дерево бралось точь-в-точь read-tree-трюком
> (`git merge --no-commit || true` + `git read-tree -u --reset 4.7-base`).

### 4. Проверить, что дерево = upstream
```bash
git diff --stat <REF> HEAD     # ДОЛЖНО быть пусто
cat version.py                 # status = "stable" (или нужный статус) — глянуть глазами
```
Не пусто → стоп, разобраться (обычно `REF` указан не туда).

### 5. ⚠️ ПЕРЕСОБРАТЬ НАЧИСТО — это те самые грабли
**Сначала закрыть редактор** (он лочит .exe, иначе линковка упадёт):
```bash
# Windows (PowerShell):  Get-Process godot* | Stop-Process -Force
# 🍎 MACOS:              pkill -f godot.macos.editor      # или просто выйти из редактора
```
**Снести объектный кеш и пересобрать.** Удаление `bin/obj` форсит перекомпиляцию строки версии
во всех файлах — без этого scons НЕ обновит часть version-зависимых объектников, и в GUI останется
старая версия (подробно — в «Грабли» ниже):
```bash
rm -rf bin/obj

# Windows:
py -m SCons platform=windows target=editor d3d12=no -j16
# 🍎 MACOS:
#   scons platform=macos target=editor arch=arm64 vulkan=no -j10
```
Время: Windows ~10–20 мин (пересобирается и thirdparty), мак (M1 Max) ~3–5 мин.
> Быстрее, если жалко времени: вместо всего `bin/obj` снести только годотовские объектники —
> `rm -rf bin/obj/{core,editor,main,scene,servers,drivers,platform,modules}` (thirdparty не
> трогаем, строка версии живёт только в коде Godot). Foolproof-вариант — всё равно весь `bin/obj`.

### 6. Проверить версию (обязательно — ловит граблю)
```bash
# Windows (🍎 MACOS: bin/godot.macos.editor.arm64):
bin/godot.windows.editor.x86_64.exe --version          # → 4.7.stable.custom_build.<hash>

# Главная проверка: в бинаре НЕ должно остаться старого статуса.
grep -a -o '4\.7\.rc'     bin/godot.windows.editor.x86_64.exe | wc -l   # → 0
grep -a -o '4\.7\.stable' bin/godot.windows.editor.x86_64.exe | wc -l   # → >0
```
(Замени `rc`/`stable` на старый/новый статус.) Старого статуса ≠ 0 → шаг 5 не дочистил: снеси
`bin/obj` целиком и пересобери. Затем запусти редактор — внизу Project Manager должна быть новая
версия, а плашка «Update available» — исчезнуть.

### 7. Запушить
```bash
git push origin lestoroer/main
git push origin 4.7-base
```

### 8. Обновить строку «Текущая база» внизу этого файла и закоммитить FORK_NOTES.

### (если деплоишь на Quest) Android-библиотека движка тоже устаревает
Движок в APK приходит НЕ из редактора, а из `godot-lib.template_release.aar` в gradle-шаблоне
проекта (`<проект>/android/build/libs/release/`) — после ЛЮБОГО форк-патча движка пересобрать,
иначе APK живёт без патчей. Симптомы: старая версия движка на шлеме; SCRIPT ERROR
о ненайденном методе форка на шлеме при живом десктопе (поймали 05.07.2026).
Рецепт (Windows, ~5 мин; NDK 29.0.14206865 через sdkmanager, cmdline-tools лежат как `11.0`):
```bash
cd /d/Godot/godot
ANDROID_HOME="C:/Users/Sergey/AppData/Local/Android/Sdk"   python -m SCons platform=android target=template_release arch=arm64 -j16
cd platform/android/java   # gradle сам дособерёт debug-вариант; scons.exe обязан быть в PATH
PATH="/c/Users/Sergey/AppData/Roaming/Python/Python310/Scripts:$PATH"   ANDROID_HOME="C:/Users/Sergey/AppData/Local/Android/Sdk" ./gradlew generateGodotTemplates
cp /d/Godot/godot/bin/godot-lib.template_release.aar <проект>/android/build/libs/release/
cp /d/Godot/godot/bin/godot-lib.template_debug.aar   <проект>/android/build/libs/debug/
```

---

## 🪤 Грабли: почему обязателен чистый ребилд
Версия зашита в `version.py` (поле `status`). При сборке она попадает в
`core/version_generated.gen.h`, который инклюдят ~45 файлов Godot (footer Project Manager, диалог
About, заголовок окна, `--version` и т.д.). **Баг SCons:** он не считает этот сгенерённый заголовок
зависимостью всех этих файлов, поэтому при инкрементальной сборке после смены `version.py` часть
`.obj` НЕ пересобирается. В итоге в один бинарь слинкованы и старые («rc»), и новые («stable»)
объектники: `--version` (свежий TU) пишет stable, а footer редактора (старый TU) — rc, и **ни
ребут, ни перезапуск не помогают** — версия зашита в .exe. Лечится только удалением объектников
(шаг 5). Поймали 19.06.2026 на 4.7-rc3 → stable, потеряли кучу времени.

## Репозитории / ремоуты
- `origin` → https://github.com/Lestoroer/godot.git — наш форк, пушим сюда.
- `upstream` → https://github.com/godotengine/godot.git — Godot, только fetch.
- `builds` → https://github.com/godotengine/godot-builds.git — только fetch; **репо-метаданные**,
  не исходник. В нём `releases/godot-<ver>.json`, поле `git_reference` = реальный коммит движка,
  из которого собран пре-релиз. Нужен только для пре-релизов (шаг 0).

## Модель веток
```
upstream/<minor>  ──►  origin/<minor>-base  ──►  origin/lestoroer/main  ──►  lestoroer/feat-*, fix-*
 (Godot, не трогаем)    (зеркало, ff-only)        (интеграция, только мержи)   (вся работа тут)
```
- `<minor>-base` (напр. `4.7-base`) — чистое зеркало исходника релиза. Напрямую не коммитить.
- `lestoroer/main` — интеграционная. Напрямую не коммитить — только `git merge --no-ff` из
  `lestoroer/feat-*`/`fix-*`. Не rebase, не force-push.
- Фича: ветка `lestoroer/feat-<имя>` от main → коммиты с префиксом `[Lestoroer]` → push →
  `git merge --no-ff` обратно в main.

## Чего НЕ делать
- Не коммитить напрямую в `<minor>-base` и `lestoroer/main`.
- Не rebase / не force-push `lestoroer/main` и `*-base` (ломает feat-ветки и ff на origin).
- Не коммитить `bin/` (артефакты сборки).
- Не пропускать шаг 5 (чистый ребилд) при смене версии — иначе призрак старой версии в GUI.

## Текущая база
**`4.7.2-stable`** — официальный тег Godot, commit `ed1daf0bf0`. Влит в
`lestoroer/main` 22.08.2026 merge-коммитом `4ae2727fa6`. Сейчас в `lestoroer/main` пять
патчей движка из инвентаря ниже и описание внешнего модуля VU; четыре неиспользуемых патча
удалены 05.10.2026. Якорь состояния непосредственно до обновления — теги
`fork-pre-update` и `fork-pre-4.7.2-stable`; исторические якоря —
`fork-pre-4.7-stable` и `fork-pre-4.7-upgrade`.

## Инвентарь патчей
Все строки патчей помечены `Fork(Lestoroer)` в комментарии — greppable. Формат: ветка | файлы | зачем.

Удалены 05.10.2026 как неиспользуемые: ни Voxel Underworld, ни Interior Star их не вызывали
(тени VU перешли на VSM с обычным `sampler2DArray` и R16G16, данные света — на RGBA16F):
доступ к depth-текстуре вьюпорта (`viewport_get_depth_texture_rd`), `sampler2DArrayShadow` в
языке шейдеров и Shader Globals, identity-swizzle D16, `usampler2D` в Shader Globals с форматом
`R32G32B32A32_UINT`. Откат — revert-коммиты ветки `lestoroer/chore-remove-unused-vu-patches`;
исходные ветки `lestoroer/feat-vu-shadows`, `lestoroer/fix-d16-swizzle`,
`lestoroer/feat-vu-uint-globals` сохранены — понадобятся снова, вливать их обратно merge'ем.

1. **offscreen Vulkan для агентных тестов на Windows** | `lestoroer/feat-offscreen-display` |
   `servers/display/display_server_offscreen.{h,cpp}`, `platform/windows/display_server_windows.cpp`,
   `main/main.cpp`, `servers/rendering/rendering_device.{h,cpp}`,
   `tests/servers/test_display_server_registration.cpp` | Опциональный CLI-режим `--offscreen`:
   настоящий Mobile/Forward Vulkan-рендер и чтение viewport/screenshot без игрового/видимого
   HWND, display surface, swapchain,
   taskbar/Alt-Tab, системного фокуса, захвата физической мыши, звукового устройства и XR.
   Основан на идее draft PR [godotengine/godot#94530](https://github.com/godotengine/godot/pull/94530),
   но перенесён на современную архитектуру 4.7 как компактный наследник
   `DisplayServerHeadless`; код устаревшего PoC не cherry-pick'ался. Windows-only, только Vulkan,
   только запуск проекта; editor/project manager, XR и OS-subwindows намеренно запрещены.
   Режим выбирается только явно через CLI и никогда не участвует в fallback. Конфликтующий
   живой audio driver или `--xr-mode on` приводит к явной ошибке, а не к тихому небезопасному
   запуску. Явный CLI-выбор также имеет приоритет над project feature `dedicated_server`, чтобы
   тот не подменял offscreen на headless после проверки инвариантов. При отсутствии surface
   `RenderingDevice` остаётся главным singleton-устройством (включая PSO cache и запрет local
   `submit/sync`), но использует `frame_count = 1`: режим проверяет корректность GPU-рендера и
   изображения, однако его frame timing нельзя сравнивать с оконным запуском или Quest.
   Windows IME и GPU-драйвер могут создавать собственные невидимые
   служебные HWND; они не являются окном Godot, не видны, не получают foreground и не входят в
   taskbar/Alt-Tab.

2. **renderer-integrated outline в Forward Mobile** | `lestoroer/feat-highlight-outline` |
   API и instance state: `rendering_server.{h,cpp}`, `rendering_server_default.h`,
   `rendering_method.h`, `renderer_scene_cull.{h,cpp}`, `renderer_geometry_instance.{h,cpp}`,
   `doc/classes/RenderingServer.xml`; Forward Mobile:
   `renderer_rd/forward_mobile/render_forward_mobile.{h,cpp}`,
   `scene_shader_forward_mobile.{h,cpp}`; composite:
   `renderer_rd/effects/tone_mapper.{h,cpp}`, `renderer_scene_render_rd.cpp`,
   `storage_rd/render_data_rd.h`, `shaders/effects/tonemap_mobile.glsl`; sky:
   `environment/sky.{h,cpp}` |
   `RenderingServer.instance_geometry_set_highlighted(instance, enabled)` проводит бинарный
   instance-флаг до Forward Mobile; парный read-only
   `instance_geometry_is_highlighted(instance)` возвращает сохранённое состояние
   `RendererSceneCull` без GPU readback. Только когда в видимом render list есть подсвеченная
   геометрия, Forward Mobile временно резервирует alpha уже существующего scene color под coverage:
   обычные opaque поверхности рисуются первыми и сохраняют alpha, highlighted opaque — последними
   и записывают её с обычным depth test. Поэтому закрытый непрозрачной геометрией объект не даёт
   x-ray-контура. Sky использует отдельное pipeline-state только с `write_a = false`.

   Для настоящей прозрачности сохраняется material RGB blend, а highlighted coverage объединяется
   отдельным alpha blend `src + dst * (1 - src)`. Материалы без depth test/write не помечаются:
   их экранное покрытие неоднозначно. Mobile tonemap читает coverage из alpha исходного color,
   четырьмя диагональными bilinear taps строит только внешний контур заданной ширины и применяет
   его после color conversion, до debanding. Отдельного `R8` attachment, resolve, descriptor binding
   и highlight shader family больше нет. Geometry не дублируется, draw calls и triangles не
   добавляются. При нуле видимых highlighted-instance сохраняются штатные alpha writes, subpass и
   pipeline states.

   Capability `rendering/renderer/highlight_outline/enabled` startup-only: `false` не создаёт
   новые pipeline states; `true` прогревает режимы preserve/write. Первый
   контракт — Forward Mobile, одна бинарная маска, общий depth-tested outline без x-ray и без
   разделения соприкасающихся подсвеченных объектов. Ширина —
   `rendering/renderer/highlight_outline/width` в пикселях; общий цвет — startup-настройка
   `rendering/renderer/highlight_outline/color`, передаваемая tonemap как push constant без
   отдельной текстуры.

   Alpha-mask намеренно отключается для transparent viewport/passthrough, reflection probes и
   background `KEEP`, `CANVAS`, `CAMERA_FEED`: там alpha принадлежит compositing-контракту либо
   не может быть надёжно очищена. В highlight-кадре shader, читающий alpha `SCREEN_TEXTURE`, увидит
   coverage mask; это зарезервированный внутренний канал, а не material alpha экрана.

   Проверка на Quest 3, release, clocks 4/4, одинаковый фиксированный кадр: один outline
   `6.206 ms` против `6.198 ms` baseline; 32 outline `5.643 ms` против `5.680 ms` baseline.
   Разница находится в шуме замера; draw calls одинаковы (`44`), primitives отличаются только
   штатным счётчиком стенда (`118840` против `118836`). Предыдущий вариант с отдельным R8 MRT
   стоил около `+1.70 ms GPU` на этом же стенде и полностью удалён.

3. **fast automated Android export** | `lestoroer/feat-fast-headless-android-export` |
   `platform/android/export/export_plugin.cpp`, `editor/editor_interface.{h,cpp}`,
   `doc/classes/EditorInterface.xml` | Android exporter сохраняет рядом с
   установленным build template время последней Gradle-сборки, build-каталог и набор
   export-плагинов. Новый headless editor восстанавливает это состояние и не принимает
   каждый CLI-export за первую сборку с обязательным `clean`. Настоящий первый export,
   смена build-каталога и изменения Android-плагинов по-прежнему требуют clean; удаление
   или переустановка build template удаляет и состояние. Имена export-плагинов
   сортируются перед сравнением: порядок их регистрации между процессами не считается
   изменением состава. Для APK готовый
   подписанный artifact копируется напрямую из `build/outputs/apk`; отдельный
   второй Gradle-процесс для `copyAndRenameBinary` остаётся fallback на случай
   изменения upstream layout. `EditorInterface.export_project()` даёт проектному
   editor-плагину узкий способ запустить export из прогретого процесса без
   автоматизации GUI.

4. **Dynamic viewport в RD из GDScript** | `lestoroer/feat-rd-draw-list-viewport` |
   `servers/rendering/rendering_device.cpp`, `doc/classes/RenderingDevice.xml` |
   Привязан существующий `draw_list_set_viewport(draw_list, rect)` для пакетного
   рисования независимых тайлов теней в одном render pass. Реализация viewport,
   драйверы и синхронизация не менялись. Scissor обновляется отдельно; clear и
   resolve остаются общими для framebuffer. Проверки качества и стоимости
   выполняются в проекте Voxel Underworld.

5. **CPU-отсечение MSAA2-треугольников для теней VU** | `lestoroer/feat-vu-shadow-experiments` |
   `core/core_bind.{h,cpp}`, `doc/classes/Geometry3D.xml` |
   `Geometry3D.filter_shadow_sample_coverage` принимает реальные FP32-позиции,
   индексы и байты MVP. Сохраняет порядок оставшихся индексов; неопределённые
   clip-пересечения не отсекает. Контракт ограничен 512px-тайлами в полосе
   до восьми тайлов и стандартными позициями MSAA2. Погрешность viewport
   учитывает абсолютную координату в полосе. Принят в main 05.10.2026 как явный
   API проекта: renderer его не вызывает, драйвер и GPU-синхронизация не меняются.
   Строгая граница fixed-function точности и выигрыш на устройствах — предмет
   проб проекта, их результаты в `vu-doc/technical/lighting.md`. Проверка при обновлении —
   `tests/light/shadows/auto/vu_test_shadow_sample_coverage.gd` проекта.

6. **Внешний модуль подачи теней Voxel Underworld** | проектный `custom_modules` |
   `voxel-underworld/game/lighting/native/vu_shadow_submit` |
   Класс `VuShadowSubmit` исполняет готовые проектные команды через публичный
   RenderingDevice на render thread. Порядок draw, барьеры, ресурсы и GPU-код
   не меняются; внутри движка патча renderer нет. Исходники и сборочная команда
   `tools/vu_build_shadow_module.py` принадлежат игровому репозиторию.
   Бинарник содержит класс только при сборке с этим внешним модулем; Android
   templates требуют того же `custom_modules`. Проект сохраняет GDScript-путь
   для бинарников без класса. При upstream-обновлении проверить сигнатуры RD
   и прогнать проектный `vu_test_native_shadow_submission.gd`.

### Чеклист апгрейда для RD-зависимостей проекта (вариант D теней)
Проектный RD-пасс (vu_shadow_system.gd) живёт на сыром RD API и порядке кадра — при каждом
мёрже upstream проверить:
1. Сигнатуры `draw_list_begin` (флаговый API, DrawFlags), `render_pipeline_create`,
   `draw_list_set_push_constant`, `texture_create_shared_from_slice` не поехали.
2. Порядок кадра сохранён: `call_on_render_thread` Callable исполняется в FIFO command_queue
   ДО `_draw` того же кадра (rendering_server_default: sync/draw).
3. Барьеры RD всё ещё автоматические (RenderingDeviceGraph; ручные barrier() — no-op).
4. `draw_list_set_viewport` по-прежнему привязан в ClassDB (патч №4 не потерялся в конфликте).

### Чеклист апгрейда offscreen-режима
1. `DisplayServer::register_create_function()` всё ещё оставляет headless последним;
   порядок на Windows после регистрации — native Windows, offscreen, затем headless.
2. Offscreen по-прежнему пропускается по имени в автоматическом fallback, а ошибка создания
   явно запрошенного offscreen не открывает native Windows server.
3. Dummy accessibility, Dummy audio и XR-off остаются обязательными инвариантами режима;
   выбор через project settings запрещён.
4. Базовый `RenderingContextDriverVulkan` остаётся конкретным и пригодным для initialize без
   platform surface (контрольный upstream-путь — `DisplayServer::is_rendering_device_supported`).
5. Offscreen вызывает `RenderingDevice::initialize(context, INVALID_WINDOW_ID, true)`, чтобы
   только его windowless singleton сохранял main-instance инварианты; общий RD-support probe и
   local device при `INVALID_WINDOW_ID` должны оставаться не-main.
6. `screen_prepare_for_drawing()` тихо возвращает ошибку только для главного windowless RD;
   отсутствие swapchain у обычного оконного RD должно оставаться громкой ошибкой. Оба
   compositor caller'а должны продолжать трактовать non-OK как «не презентовать».
7. При новых методах `DisplayServerHeadless` перепроверить virtual window size, screen list,
   `can_any_window_draw`, mouse-mode state и запрет subwindows в offscreen-наследнике.
8. PR #94530 остаётся историческим источником требований, а не кодом для повторного merge.

### Эксперимент Surface Cache GI: Vulkan RT foundation

Только ветка `codex/gi-surface-cache`, отдельный worktree. Основной движок
не получает этот патч. `drivers/vulkan/rendering_device_driver_vulkan.cpp`
и `servers/rendering/rendering_shader_container.cpp`:

- Маска видимости RT-uniform использует `SHADER_STAGE_*_BIT`, а не номера
  enum stages. Номера давали маску graphics stages и закрывали descriptors
  для RT: compute hit работал, но RT pipeline возвращал miss.
- Запрос и включение `VK_KHR_ray_query` по фактическим capabilities устройства.
- RT shader stages и shaders с acceleration-structure descriptor обходят
  re-spirv: он не поддерживает соответствующие инструкции. Vulkan получает
  исходный SPIR-V и сам применяет specialization constants. Без этого smoke
  probe мог успешно создать pipeline, но возвращать miss для известного hit.

Это исправления технической основы, не реализация GI. Проверка принадлежит
игровому эксперименту: `lighting/surface_cache/probes/hardware_rt.gd`. При
обновлении re-spirv проверить поддержку RT/query до удаления обхода.

### Surface Cache: GPU-захват материалов

Только экспериментальная ветка. `Mesh.surface_cache_get_layout` возвращает
недеструктивную xatlas-развёртку и remap исходных вершин/треугольников.
`mesh_surface_set_capture_uv` назначает отдельный неизменяемый поток координат
частному capture-мешу до создания экземпляров. Авторские UV/UV2/CUSTOM сохранены.
`instance_surface_cache_capture` рисует на GPU материал исходного instance,
используя remapped vertex/index streams второго instance. Трансформ, overrides
и instance uniforms принадлежат исходному экземпляру; skin/morph-поза переносится
на подготовленную геометрию. Результаты остаются в framebuffer вызывающего.

Spatial built-in `IN_SURFACE_CACHE_PASS` позволяет отделить raw material от
художественных преобразований. Сторона `FRONT_FACING` в этом проходе семантическая,
а не вычисленная по winding UV. Захват Forward+ выдаёт пять targets:
albedo/alpha, encoded normal, AO/roughness/metallic/specular, emission и мировую
позицию/coverage. Материальная программа одна с raster. Остальные renderer-ы
возвращают false для built-in и явно отвергают GPU capture API.

Проверки актуальных материалов и remap находятся в игровом эксперименте.
Наличие API не означает готовность GI или прохождение материальной приёмки.

Capture читает параметры распаковки с фактически рисуемого chart-потока,
а не со сжатого исходного mesh. Поля view uniform задаются детерминированно;
материальные ветки не наследуют состояние камеры предыдущего прохода.

### Surface Cache: события мира и GPU-деформация

Scenario хранит одного opt-in владельца обновлений. Callback вызывается перед
рендером мира один раз за renderer frame; дополнительные камеры используют
тот же результат. `scenario_surface_cache_poll` отдаёт coalesced изменения
экземпляров по RID (регистрация/удаление, трансформ, материал, pose, свет).
Подписка начинает с одного снимка; покадрового обхода SceneTree нет.
Content notification не запускает лишнюю пересборку raster-материалов.

`instance_get_deformed_surface` предоставляет заимствованный GPU buffer
skin/morph, layout и version; читать только на render thread и не хранить RID
после callback. Чтения вершин с GPU на CPU в этом API нет. Произвольная
vertex-shader деформация этим accessor не поддерживается.

### Surface Cache: выборка в Forward+

`scenario_set_surface_cache_buffers` задаёт четыре RD storage buffer: таблицу
поверхностей, мировые треугольники с chart UV, материальные texel и irradiance/PI.
`instance_set_surface_cache_ids` связывает поверхности экземпляра с таблицей.
Forward+ заменяет ambient после пользовательского IRRADIANCE, до AO/albedo/tonemap.
Источником выбора является реальный raster primitive и мировая позиция; фильтр
не смешивает charts и стороны. Отсутствие покрытия показывается пурпурным.

Для зарегистрированных экземпляров используется исходная геометрия без mesh LOD:
LOD пока не имеет соответствия primitive-to-chart. Это сохраняет детализацию,
но его дополнительную стоимость требуется учитывать. Capture position.w теперь
содержит primitive+1 (0 — отсутствие покрытия), без изменения авторских UV.

Выборка проверяет также surface ID: одинаковые локальные номера chart разных
поверхностей не смешиваются. Повторная установка того же material parameter
не создаёт ложное событие обновления Surface Cache.

Дескрипторы Surface Cache объявлены во всех вариантах Forward+ (включая depth,
shadow, unshaded), чтобы общий render-pass uniform set имел совместимый layout.

### Surface Cache: TLAS для ray queries

`hit_sbt_range = 0` допустим для query-only TLAS: Vulkan ray queries не читают
SBT. RD больше не требует создавать фиктивный RT pipeline только ради query.
Трассировка RT pipeline по-прежнему задаёт свои диапазоны SBT.

Capture хранит нормаль напрямую в RGBA16F: нормализация лучей не зависит от
8-bit best-fit lookup. Тонкие границы chart покрываются штатной стратегией
UV2 bake (смещённый wireframe и затем внутренний проход), без слияния chart ID.

### Surface Cache: глобальная видимость AS для ray query

В экспериментальной ветке Vulkan при включённом `rayQuery` сохраняет
`ACCELERATION_STRUCTURE_READ` в глобальных memory barriers. Барьер только
на буфере TLAS не покрывает читаемые через него BLAS и scratch повторной сборки.
Регрессия: main RD, build → compute-запись вершин → rebuild → query в одном кадре;
проверка `lighting/surface_cache/probes/main_rd_query.gd` в изолированной игре.


Временная диагностика этого дефекта: `SURFACE_CACHE_GRAPH_TRACE` включает
порядок команд и зависимости сборки AS; по умолчанию выключена.

`SURFACE_CACHE_FULL_BARRIERS` — диагностический полный barrier между уровнями,
только для поиска нарушенной синхронизации; обычный renderer его не включает.

### Surface Cache: адреса BLAS и повторная сборка TLAS

TLAS ссылается на адрес, возвращённый `vkGetAccelerationStructureDeviceAddressKHR`,
а не на device address буфера, в котором выделена BLAS. Первая запись нового
instance buffer сразу резервирует свой диапазон; следующая сборка в том же
кадре не перезаписывает вход ещё не исполненной команды.

Диагностика `SURFACE_CACHE_FULL_BARRIERS` принимает `stages` и `access` для
раздельной проверки масок; `1` проверяет обе.

Диагностическое значение `levelN` включает полный barrier только перед указанным
уровнем графа; не используется в обычном GI.

`SURFACE_CACHE_MAIN_TRANSFER` проверяет загрузку на семействе main queue;
`SURFACE_CACHE_GRAPH_TRACE` также печатает выбранные семейства. Это временная
диагностика first-use ray queries, выключенная по умолчанию.

`SURFACE_CACHE_BARRIER_MASKS` диагностически добавляет четыре числовые маски
(src stage, dst stage, src access, dst access) для локализации зависимости.

### Surface Cache: нормали и тонкие треугольники

Нулевая normal-map XY или depth=0 использует нормаль без вычисления TBN: у
геометрии с постоянной UV tangent может быть параллелен normal, и умножение
неопределённой binormal на ноль заражало весь результат NaN. Адрес cache
восстанавливается через проекцию на главную ось, без разности квадратов Gram.

### Surface Cache: видимость и время жизни BLAS

Uniform sets учитывают текущие BLAS, доступные через TLAS, при каждом новом
bind/dispatch. Их нельзя перезаписать, пока предыдущие ray queries ещё читают
старую структуру. Для проверяемого NVIDIA 610.88 AS-read в compute/fragment
расширяется MEMORY_READ и AS_BUILD destination stage только на переходе к
потребителю. Это требуется и для hit→miss→hit с записью вершин в compute,
и для первого запуска транспорта ванной; одной MEMORY_READ недостаточно. Отрицательный контроль
`SURFACE_CACHE_DISABLE_AS_VISIBILITY` выключает эту защиту. Это экспериментальная
совместимость с драйвером, а не изменение профиля качества.

Вырожденная binormal остаётся нулевой вместо NaN; нейтральная normal map
использует исходную нормаль.

### Surface Cache: идентичность charts

Surface layout возвращает chart ID из xatlas. Связность индексов исходного меша
не определяет chart: дублированные вершины UV/normal seams могут принадлежать
одному chart. GI-захват получает два texel padding между charts; штатная
lightmap-развёртка сохраняет прежнюю упаковку.

Треугольник с ненулевой площадью, исключённый xatlas как почти вырожденный,
получает отдельный минимальный chart. Геометрия луча не удаляется и не
сдвигается; материал остаётся материалом того же исходного треугольника.

### Surface Cache: раздельные адреса материала и света

Таблица поверхности содержит независимые material/lighting mapping. World
triangle хранит обе UV-параметризации. Подробность исходного материала больше
не задаёт размер уравнения многократного отражения.

Surface Cache: отдельный callback Scenario после depth/normal prepass выполняется
для каждого viewport (включая зеркало), независимо от его Compositor. Binding 41
читает viewport-текстуру `surface_cache/gather`; глубина проверяется до применения
непрямого света к фрагменту. Общий world cache обновляется один раз за кадр.

Surface Cache: read-only `SURFACE_CACHE_IRRADIANCE` передаёт material fragment
линейное непрямое освещение до художественной ramp (alpha=0, если GI отсутствует).
Материал, использующий вход, сам пишет IRRADIANCE; renderer не подменяет его
повторно. Mobile/GLES возвращают нулевую alpha. Очистка адресов удалённого
instance идемпотентна.

Surface Cache требует normal prepass и без WorldEnvironment. Вход материала
`SURFACE_CACHE_GATHER_USED` позволяет проверять реальное использование viewport
результата. Чтение coarse fallback выполняется только при несовпадении глубины.

Направленные PCSS-тени: поиск блокеров и фильтр сравнивают глубину с плоскостью
принимающей поверхности в каждой точке ядра. Это устраняет самозатенение
наклонных поверхностей при широком источнике без увеличения смещения всей тени.

Surface Cache: depth/normal prepass сохраняет surface и primitive ID только
при активном GI. MSAA выбирает ID того же sample, что depth/normal. Final gather
и материал проверяют эту идентичность; поиска треугольника по допуску глубины
и зависимости от схемы MSAA samples видеокарты нет.

PCSS: bilinear PCF проверяет receiver plane у каждого из четырёх depth texel,
а не только в центре tap. Это сохраняет контакт без завышенного slope bias.

Surface Cache: native mesh revisions invalidate shared geometry on same-RID
mesh edits. Texture contents and global shader inputs notify subscribed material
instances through dependencies. Shader vertex/clip-space displacement is exposed
as unsupported instead of letting raster and ray geometry silently diverge.
Material capture first records geometric coverage, then shades it: fragment
`discard` changes opacity without changing the allocation of transport rows.

Surface Cache: `surface_cache_global_invariant` — явный контракт spatial shader:
глобальные параметры не меняют ни один выход захвата материала или геометрии.
Только такой shader освобождается от global-зависимостей GI; остальные изменения
материала/текстур/instance и обычный raster сохраняют прежнее поведение.
RenderingDevice публикует существующие subgroup limits в GDScript, чтобы
выбирать dispatch без предположения о размере аппаратной subgroup.

### Surface Cache: покрытие физической поверхности

В изолированной ветке `codex/gi-surface-cache` capture получает отдельный домен
каждого исходного примитива: тонкие участки материала не перетираются соседями.
Плотность вдоль длинного ребра задаёт профиль; короткая ось имеет минимум два
texel. Private stream сохраняет UV/UV2/CUSTOM и порядок исходных примитивов.
Расширенная растеризация перечисляет пересечения с texel; material fragment
вычисляется в центре площади пересечения. Пустые границы не создают строк GI.
Capture использует аналитическую интерполяцию встроенных и пользовательских smooth varyings.

Primary visibility теперь RGBA32UI: surface/primitive и barycentrics покрытой
centroid-позиции. MSAA resolve переносит их вместе с выбранными depth/normal.
Это устраняет восстановление primary на ребре из непокрытого центра пикселя;
дополнительная стоимость — 8 байт на resolved pixel и MSAA sample.
