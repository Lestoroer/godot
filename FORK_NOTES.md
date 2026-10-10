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
upstream/<minor>  ──►  origin/<minor>-base  ──►  origin/lestoroer/main  ──►  origin/interior-star/main
 (Godot, не трогаем)    (зеркало, ff-only)        (общие патчи, только мержи)  (Interior Star, только мержи)
                                                        ▲                          ▲
                                               lestoroer/feat-*, fix-*    interior-star/feat-*, fix-*
```
Каждая игра собирает движок из своей ветки:

| Игра | Ветка движка | Содержимое |
|---|---|---|
| Voxel Underworld | `lestoroer/main` | общие патчи из инвентаря ниже |
| Interior Star | `interior-star/main` | всё из `lestoroer/main` и патчи Interior Star, прежде всего Surface Cache GI; их инвентарь — в `FORK_NOTES.md` этой ветки |

Interior Star на `lestoroer/main` не работает: её шейдеры требуют патчей своей ветки.

- `<minor>-base` (напр. `4.7-base`) — чистое зеркало исходника релиза. Напрямую не коммитить.
- `lestoroer/main` — интеграционная. Напрямую не коммитить — только `git merge --no-ff` из
  `lestoroer/feat-*`/`fix-*`. Не rebase, не force-push.
- Фича: ветка `lestoroer/feat-<имя>` от main → коммиты с префиксом `[Lestoroer]` → push →
  `git merge --no-ff` обратно в main.
- `interior-star/main` принимает `git merge --no-ff` из `lestoroer/main` и из
  `interior-star/feat-*`/`fix-*` (коммиты тоже с префиксом `[Lestoroer]`). Обратно в
  `lestoroer/main` она не вливается. Патч, нужный обеим играм, делают отдельной
  `lestoroer/feat-*` от `lestoroer/main` и проверяют на Voxel Underworld, включая Quest.
- Обновление upstream: шаги выше для `lestoroer/main`, затем `git merge --no-ff lestoroer/main`
  в `interior-star/main`, пересборка и проверки Surface Cache из игры (`lighting/surface_cache/`).

## Чего НЕ делать
- Не коммитить напрямую в `<minor>-base`, `lestoroer/main` и `interior-star/main`.
- Не rebase / не force-push `lestoroer/main`, `interior-star/main` и `*-base` (ломает feat-ветки и ff на origin).
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

## Патчи Interior Star

Патчи 7–13 есть только в `interior-star/main` (модель веток — выше). Они обслуживают
Surface Cache GI игры Interior Star: динамический непрямой свет и трассируемые отражения.
Игровая часть, проверки и закреплённый коммит движка лежат в репозитории игры —
`is-game/lighting/surface_cache/` (`README.md`, `engine.json`, `build_engine.py`,
`run_checks.py`); имена проверок ниже — файлы его `probes/`. Мастер-материалы игры
используют `surface_cache_global_invariant`, поэтому без этих патчей её шейдеры не
компилируются.

GI работает только на Vulkan с аппаратным ray query в Forward+. Forward Mobile и OpenGL
принимают синтаксис материалов (built-ins возвращают нули), но GI не исполняют.

Формат прежний: что | файлы | контракт. Пути без префикса — от `servers/rendering/`.

7. **Vulkan ray query и синхронизация acceleration structures** |
   `drivers/vulkan/rendering_device_driver_vulkan.{h,cpp}`, `rendering_device.{h,cpp}`,
   `rendering_device_graph.cpp`, `rendering_device_commons.h`,
   `rendering_shader_container.cpp`, `doc/classes/RDAccelerationStructureInstance.xml`,
   `doc/classes/RenderingDevice.xml` |
   - `VK_KHR_ray_query` запрашивается и включается по возможностям устройства.
   - Маска видимости RT-uniform строится из `SHADER_STAGE_*_BIT`, а не из номеров stages:
     upstream закрывал descriptors для RT pipeline, и известный hit возвращал miss.
   - Шейдеры RT stages и шейдеры с AS-descriptor обходят re-spirv, который не поддерживает
     эти инструкции. Vulkan получает исходный SPIR-V и сам применяет specialization constants.
   - TLAS ссылается на адрес из `vkGetAccelerationStructureDeviceAddressKHR`, а не на адрес
     буфера BLAS. Первая запись нового instance buffer сразу резервирует свой диапазон:
     несколько сборок TLAS в одном кадре не перетирают вход неисполненной команды.
   - `hit_sbt_range = 0` допустим для TLAS, который читают только ray queries.
   - При включённом `rayQuery` глобальные барьеры сохраняют `ACCELERATION_STRUCTURE_READ`.
     Uniform set при каждом bind/dispatch заново учитывает все BLAS, достижимые через его
     TLAS: он может пережить пересборку TLAS.
   - Обход драйвера NVIDIA 610.88 (`ray_query_needs_memory_read_barrier`): на переходе к
     compute/fragment-потребителю AS добавляются `MEMORY_READ` и stage
     `ACCELERATION_STRUCTURE_BUILD`. Условие привязано к этой версии драйвера.
   - В GDScript доступны существующие лимиты subgroup (`LIMIT_SUBGROUP_*`).

   Проверки: `hardware_rt.gd`, `main_rd_query.gd`, `krylov_gpu.gd`.

8. **Charts и GPU-захват материала** | `scene/resources/mesh.{h,cpp}`,
   `renderer_rd/storage_rd/mesh_storage.{h,cpp}`, `storage/mesh_storage.h`,
   `renderer_rd/shaders/surface_cache_capture_inc.glsl`,
   `renderer_rd/storage_rd/material_storage.{h,cpp}`,
   `renderer_rd/storage_rd/texture_storage.{h,cpp}`, `renderer_rd/storage_rd/light_storage.cpp`,
   `storage/material_storage.h`, `storage/utilities.h` |
   - `Mesh.surface_cache_get_layout(surface, texel_size)` строит собственную развёртку, не
     xatlas: каждый треугольник — отдельный прямоугольник в texel с отступом 2 texel,
     плотность по длинному ребру, по короткой оси минимум один texel. Вырожденный
     треугольник получает chart 0 и один texel. Прямоугольники пакуются
     `Geometry2D::make_atlas`. Результат: `capture_pixels`, `uv`, `source_vertices`,
     `charts`, `indices`, `size`, `tiles`, `sample_count`. Авторские UV/UV2/CUSTOM,
     порядок треугольников, skin и morph не меняются.
   - `RenderingServer.mesh_surface_set_capture_uv` задаёт частному capture-мешу отдельный
     неизменяемый поток координат (в texel) до создания экземпляров.
   - `RenderingServer.instance_surface_cache_capture(instance, chart_instance, framebuffer,
     region, back_side)` исполняет на GPU настоящий spatial material исходного экземпляра
     на геометрии chart-экземпляра. Transform, overrides, instance uniforms и поза
     skin/morph берутся у исходного. Результат остаётся в framebuffer вызывающего: пять
     targets — albedo/alpha, нормаль RGBA16F (alpha — доля пропускания), AO/roughness/
     metallic/specular, emission, канонические barycentrics и primitive ID.
   - Каждый исходный примитив получает свой домен. Перекрытие с texel определяется строгим
     SAT до float-клиппинга; контакты нулевой площади отбрасываются. Фрагмент вычисляется
     в центре площади пересечения, smooth varyings интерполируются аналитически. Сначала
     пишется геометрическое покрытие, затем материал: `discard` меняет только opacity.
   - Изменения пикселей текстур, global shader uniforms, цвета и энергии света рассылают
     подписанным материалам `DEPENDENCY_CHANGED_SURFACE_CONTENT`. Меш при изменении
     содержимого увеличивает `geometry_revision`.
   - Отличия от штатного поведения, действующие и без Surface Cache:
     `mesh_surface_update_{vertex,attribute,skin,index}_region` рассылают
     `DEPENDENCY_CHANGED_MESH` (upstream молчал; для мешей, обновляемых каждый кадр, это
     лишний пересчёт экземпляров). Повторная установка того же значения в
     `material_set_param`, `global_shader_parameter_set`,
     `global_shader_parameter_set_override` и `light_set_color` ничего не делает (upstream
     заново ставил материал в очередь и перезагружал буферы).
     `material_has_shader_displacement` сообщает о vertex displacement — такой материал GI
     явно отклоняет, чтобы raster и геометрия лучей не разошлись.
   - Остальные renderer-ы GPU capture API явно отвергают.

   Проверки: `material_capture.gd`, `capture_coverage.gd`, `sample_coverage.gd`,
   `row_ownership.gd`, `material_updates.gd`.

9. **Реестр изменений мира и доступ к деформации** | `renderer_scene_cull.{h,cpp}`,
   `rendering_server.{h,cpp}`, `rendering_server_default.h`, `rendering_method.h`,
   `renderer_scene_render.h`, `renderer_geometry_instance.h` |
   - `scenario_set_surface_cache_callback` назначает scenario одного владельца обновлений.
     Callback вызывается один раз за кадр мира до рендера; остальные камеры используют
     его результат.
   - `scenario_surface_cache_poll` отдаёт объединённые изменения экземпляров по RID:
     регистрацию и удаление, transform, материал, позу, свет. Подписка начинается с одного
     снимка мира. Вырожденный transform и Mesh без поверхностей считаются неактивными,
     как в штатном raster.
   - `instance_get_deformed_surface` отдаёт заимствованный GPU-буфер skin/morph с layout и
     версией. Читать только на render thread; RID не хранить после callback.
   - `instance_set_surface_cache_ids` связывает поверхности экземпляра с таблицей кеша.

   Проверки: `world_changes.gd`, `runtime_lifecycle.gd`, `profile_updates.gd`.

10. **Surface Cache в кадре Forward+** |
    `renderer_rd/forward_clustered/render_forward_clustered.{h,cpp}`,
    `renderer_rd/forward_clustered/scene_shader_forward_clustered.{h,cpp}`,
    `renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl`,
    `renderer_rd/shaders/surface_cache_inc.glsl`, `renderer_rd/shaders/scene_data_inc.glsl`,
    `renderer_rd/storage_rd/render_scene_data_rd.{h,cpp}`, `renderer_rd/effects/resolve.{h,cpp}`,
    `renderer_rd/shaders/effects/resolve.glsl`, `scene/resources/compositor.{h,cpp}`,
    `rendering_server_enums.h`; заглушки других renderer-ов —
    `renderer_rd/forward_mobile/scene_shader_forward_mobile.cpp`,
    `renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl`,
    `drivers/gles3/shaders/scene.glsl`, `drivers/gles3/storage/material_storage.cpp` |
    - `scenario_set_surface_cache_buffers` задаёт четыре storage buffer: таблицу поверхностей
      с раздельными адресами материала и света, мировые треугольники с обеими
      параметризациями, материальные texel, irradiance. Descriptors объявлены во всех
      вариантах Forward+, включая depth, shadow и unshaded, ради общего layout uniform set.
    - При активном GI depth/normal prepass пишет primary visibility RGBA32UI: surface,
      primitive и barycentrics покрытой точки (+8 байт на пиксель и на MSAA sample).
      MSAA resolve берёт её из того же sample, что depth/normal.
    - `scenario_set_surface_cache_view_callback` вызывается после prepass для каждого
      viewport, включая зеркала, независимо от его Compositor. Viewport-текстуры
      `surface_cache/gather`, `surface_cache/reflection`, `glass`, `glass_primary`
      применяются к фрагменту только при совпадении surface/primitive с primary visibility.
    - Irradiance заменяет ambient после пользовательского `IRRADIANCE`, до AO, albedo и
      тонемапа. Трассированное отражение заменяет IBL без повторного DFG и SSAO;
      пользовательский `RADIANCE` сохраняет приоритет. Затенение IBL при Surface Cache
      считается по AO, roughness и углу взгляда (аппроксимация Lagarde), без него —
      по-прежнему. Тонкое стекло получает RGB transmittance (binding 46) и копию opaque HDR.
      Отсутствие покрытия рисуется пурпурным.
    - Зарегистрированные в GI экземпляры рисуются без mesh LOD: у LOD нет соответствия
      primitive → chart.
    - `CompositorEffect` получает callback `POST_TEMPORAL`: после TAA, до тонемапа. При
      temporal upscaling потребитель читает соответствующую upscaled-текстуру.
    - `CompositorEffect` с флагом `NEEDS_ROUGHNESS` получает normal/roughness и без
      WorldEnvironment; upstream в этом случае флаг игнорировал.

    Проверки: `gather_reference.gd`, `reflection_contract.gd`, `specular_visibility.gd`,
    `temporal_reprojection.gd`, `temporal_motion.gd`, `surface_origin.gd`,
    `enclosure_reference.gd`, `bathroom_capture.gd`.

11. **Язык шейдеров** | `shader_types.cpp`, `shader_compiler.{h,cpp}`,
    `renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp`,
    `renderer_rd/forward_mobile/scene_shader_forward_mobile.cpp`,
    `drivers/gles3/storage/material_storage.cpp` |
    - Render mode `surface_cache_global_invariant`: автор шейдера объявляет, что global
      uniforms не меняют ни один выход захвата. Тогда их изменения не инвалидируют
      GI-материал; без режима инвалидация консервативная. Корректность объявления —
      ответственность автора.
    - Render mode `surface_cache_presentation` помечает проход (обычно `next_pass`, например
      контур) как визуальный: его не захватывает Surface Cache, его vertex displacement не
      меняет геометрию GI, его globals не инвалидируют материал. Действует на один проход.
    - Built-ins: `IN_SURFACE_CACHE_PASS` (global, bool), `SURFACE_CACHE_IRRADIANCE`
      (fragment, read-only vec4, alpha 0 без GI), `SURFACE_CACHE_TRANSMISSION` (fragment,
      float, доля тонкого пропускания отдельно от alpha), `SURFACE_CACHE_GATHER_USED`
      (fragment, read-only bool). В проходе захвата `FRONT_FACING` задаётся стороной
      захвата, а не winding развёртки.

12. **Направленные тени — меняет штатное поведение** |
    `renderer_rd/shaders/scene_forward_lights_inc.glsl`, вызовы в
    `scene_forward_clustered.glsl` и `scene_forward_mobile.glsl` |
    Действует во всех сценах, независимо от Surface Cache; include общий для Forward+ и
    Forward Mobile.
    - PCSS классифицирует каждую из четырёх глубин texel отдельно, с поправкой на плоскость
      приёмника в каждой точке ядра. Радиус полутени — по разнице глубин блокера и
      приёмника, умноженной на диапазон карты и угловой размер солнца, без перспективного
      множителя. Радиус выборки меняется внутри равновеликих страт. Пустой широкий поиск
      перед признанием точки освещённой проверяет центральный footprint. Смещение
      приёмника — только к свету.
    - Bilinear PCF проверяет плоскость приёмника в каждом из четырёх depth texel.
    - Качество Hard — один `texelFetch` и одно сравнение reverse-Z, без bilinear PCF
      comparison sampler.

    Проверки: `pcf_bias.gd`, `pcf_edge.gd`, `pcf_isolation.gd`, `shadow_plane.gd`,
    `shadow_isolation.gd`.

13. **Редактор со скриптом** | `main/main.cpp` | `--editor --script` не создаёт отдельный
    SceneTree до дерева скрипта. Так проверка открывает настоящий редактор без утечки
    первого дерева (`editor_lifecycle.gd`).

### Чеклист обновления `interior-star/main`

1. `git merge --no-ff lestoroer/main` в `interior-star/main`. Число строк
   `Fork(Lestoroer)` (команда из шага 3 обновления) не должно уменьшиться против
   `lestoroer/main` + MARKER_COUNT строк патчей 7–13.
2. Больше всего конфликтов ждать в `scene_forward_clustered.glsl`,
   `render_forward_clustered.cpp`, `scene_forward_lights_inc.glsl`, `rendering_device.cpp`,
   `rendering_device_graph.cpp`, `rendering_device_driver_vulkan.cpp`, `material_storage.cpp`.
3. Если re-spirv научился RT и ray query, убрать обход в `rendering_shader_container.cpp`.
4. Обход NVIDIA привязан к драйверу 610.88. На другой версии драйвера прогнать
   `main_rd_query.gd`; при регрессии расширить условие в `_check_driver_workarounds`.
5. Собрать движок программой игры `build_engine.py`, записать новый коммит в
   `engine.json`, прогнать `run_checks.py`.
