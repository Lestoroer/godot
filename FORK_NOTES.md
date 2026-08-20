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
После merge: `grep -rn "Fork(Lestoroer)" servers/ drivers/ doc/ | wc -l` — число строк-маркеров
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
`viewport_get_depth_texture_rd() not found` на шлеме при живом десктопе (поймали 05.07.2026).
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
**`4.7.2-rc`** — текущая голова поддерживаемой upstream-ветки `4.7`, commit `36a04fe528`
(очередь исправлений будущей 4.7.2). Влита в `lestoroer/main` 03.08.2026 merge-коммитом
`ddb208a235`; четыре группы патчей форка из инвентаря ниже сохранены. Якорь состояния до
обновления — тег `fork-pre-update`; исторические якоря — `fork-pre-4.7-stable` и
`fork-pre-4.7-upgrade`.

## Инвентарь патчей
Все строки патчей помечены `Fork(Lestoroer)` в комментарии — greppable. Формат: ветка | файлы | зачем.

1. **viewport depth-доступ** | `lestoroer/feat-vu-shadows` | `rendering_server.{h,cpp}`,
   `rendering_server_default.h`, `renderer_viewport.{h,cpp}`, `storage/render_scene_buffers.h`,
   `storage_rd/render_scene_buffers_rd.{h,cpp}`, `storage_rd/texture_storage.cpp`,
   `doc/classes/RenderingServer.xml` | `RenderingServer.viewport_get_depth_texture_rd(viewport)`
   — сырой RD-RID depth-текстуры 3D-рендера вьюпорта (кастомные тени Voxel Underworld: копия
   depth-тайла в свой атлас). Плюс: depth получает `CAN_COPY_FROM` usage (attachment-ветка
   `get_depth_usage_bits`), packed depth(+stencil) форматы поддержаны в `TextureXDRD`-обёртках
   (`_texture_format_from_rd`, identity-swizzle — обязателен для Dref).

2. **sampler2DArrayShadow в gdshader** | `lestoroer/feat-vu-shadows` | `shader_language.{h,cpp}`,
   `shader_compiler.cpp`, `storage_rd/material_storage.cpp`, `gles3/storage/material_storage.cpp`;
   global-тип: `rendering_server_enums.h`, `rendering_server.cpp`, три таблицы
   `global_var_type_names` (storage_rd/dummy/gles3 material_storage), `shader_globals_editor.cpp`,
   `shader_globals_override.cpp`
   | Новый сэмплер-тип шейдерного языка + одноимённый тип Shader Globals
   (`GLOBAL_VAR_TYPE_SAMPLER2DARRAYSHADOW`, добавлен В КОНЕЦ enum'а — ничего не сдвигает;
   сэмплер-глобалы в хранении идут диапазоном `>= GLOBAL_VAR_TYPE_SAMPLER2D`, поэтому значение
   работает без доп. правок): аппаратный depth-compare семпл (2x2 PCF бесплатно на
   Adreno/Apple). Использует ГОТОВЫЙ immutable `shadow_sampler` сцены (set0/binding2, GREATER,
   linear) — работает только в spatial-шейдерах (в canvas/sky/particles GLSL-ошибка «undeclared
   shadow_sampler»; ок для нашего использования). В Compatibility (GLES3) тип не поддержан
   (ERR_PRINT_ONCE, как samplerCubeArray). ВАЖНО про enum: тип вставлен между `TYPE_SAMPLEREXT`
   и `TYPE_STRUCT` СИНХРОННО в TokenType/DataType/token_names (get_token_datatype — арифметика,
   is_sampler_type — диапазон); DataType-индексированные таблицы (`scalar_types`,
   `cardinality_table`, gles3 `target_from_type`) дополнены — при апгрейде свежедобавленные
   upstream'ом таблицы ловятся их же static_assert'ами.

3. **identity-swizzle для D16** | `lestoroer/fix-d16-swizzle` | `storage_rd/texture_storage.cpp`
   (`_texture_format_from_rd`, кейс `DATA_FORMAT_D16_UNORM`) | Upstream задавал swizzle
   R,ZERO,ZERO,ONE — Vulkan запрещает non-identity swizzle при Dref (compare) семпле; D16-атлас
   теней Voxel Underworld (вариант D: RD depth-only пасс) семплится через Texture2DArrayRD +
   sampler2DArrayShadow и требует identity (как packed depth-форматы патча №1).

4. **usampler2D в Shader Globals + uint-формат RD-обёрток** | `lestoroer/feat-vu-uint-globals` |
   global-тип: `rendering_server_enums.h`, `rendering_server.cpp`, три таблицы
   `global_var_type_names` (storage_rd/dummy/gles3 material_storage), `shader_globals_editor.cpp`,
   `shader_globals_override.cpp`; формат: `storage_rd/texture_storage.cpp`
   (`_texture_format_from_rd`, кейс `R32G32B32A32_UINT`)
   | Тип `usampler2D` для Shader Globals (`GLOBAL_VAR_TYPE_USAMPLER2D`, добавлен В КОНЕЦ enum'а;
   сам язык шейдеров usampler2D знает апстримно — патч лишь проводит тип через глобалы) +
   маппинг `DATA_FORMAT_R32G32B32A32_UINT` в `_texture_format_from_rd` (метаданные RGBAF,
   те же 16 Б/тексель; CPU get_data не поддержан). Без формата RS-обёртка
   `texture_rd_create` над uint-текстурой МОЛЧА остаётся неинициализированной (ERR в
   ОТЛОЖЕННОЙ инициализации не всплывает к вызывающему), и глобал-семплер вечно
   резолвится в движковый 4x4-дефолт — ловили сутки 06.07.2026. Потребитель:
   `vu_cull_data` (упакованные half-слоты света). NB для потребителей: RS-обёртку для
   глобала создавать синхронным `RenderingServer.texture_rd_create(rd_rid)`, НЕ через
   `Texture2DRD.get_rid()` — тот до исполнения отложенного колбэка отдаёт
   placeholder-RID, который никогда не станет настоящей текстурой.

5. **offscreen Vulkan для агентных тестов на Windows** | `lestoroer/feat-offscreen-display` |
   `servers/display/display_server_offscreen.{h,cpp}`, `platform/windows/display_server_windows.cpp`,
   `main/main.cpp`, `servers/rendering/rendering_device.cpp`,
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
   запуску. При отсутствии surface `RenderingDevice` использует `frame_count = 1`: режим
   проверяет корректность GPU-рендера и изображения, но его frame timing нельзя сравнивать с
   оконным запуском или Quest. Windows IME и GPU-драйвер могут создавать собственные невидимые
   служебные HWND; они не являются окном Godot, не видны, не получают foreground и не входят в
   taskbar/Alt-Tab.

### Чеклист апгрейда для RD-зависимостей проекта (вариант D теней)
Проектный RD-пасс (vu_shadow_system.gd) живёт на сыром RD API и порядке кадра — при каждом
мёрже upstream проверить:
1. Сигнатуры `draw_list_begin` (флаговый API, DrawFlags), `render_pipeline_create`,
   `draw_list_set_push_constant`, `texture_create_shared_from_slice` не поехали.
2. Порядок кадра сохранён: `call_on_render_thread` Callable исполняется в FIFO command_queue
   ДО `_draw` того же кадра (rendering_server_default: sync/draw).
3. Барьеры RD всё ещё автоматические (RenderingDeviceGraph; ручные barrier() — no-op).
4. Кейс D16 в `_texture_format_from_rd` остался identity (патч №3 не потерялся в конфликте).

### Чеклист апгрейда offscreen-режима
1. `DisplayServer::register_create_function()` всё ещё оставляет headless последним;
   порядок на Windows после регистрации — native Windows, offscreen, затем headless.
2. Offscreen по-прежнему пропускается по имени в автоматическом fallback, а ошибка создания
   явно запрошенного offscreen не открывает native Windows server.
3. Dummy accessibility, Dummy audio и XR-off остаются обязательными инвариантами режима;
   выбор через project settings запрещён.
4. Базовый `RenderingContextDriverVulkan` остаётся конкретным и пригодным для initialize без
   platform surface (контрольный upstream-путь — `DisplayServer::is_rendering_device_supported`).
5. `RenderingDevice::initialize(context, INVALID_WINDOW_ID)` остаётся поддержанным путём.
6. `screen_prepare_for_drawing()` по-прежнему тихо возвращает ошибку при отсутствии swapchain;
   оба compositor caller'а должны продолжать трактовать её как «не презентовать».
7. При новых методах `DisplayServerHeadless` перепроверить virtual window size, screen list,
   `can_any_window_draw`, mouse-mode state и запрет subwindows в offscreen-наследнике.
8. PR #94530 остаётся историческим источником требований, а не кодом для повторного merge.
