# Lestoroer Godot Fork — обслуживание и обновление движка

Личный форк Godot Engine. **Ноль патчей движка** — дерево ветки `lestoroer/main` побайтово
совпадает с upstream-исходником релиза, на котором мы стоим. Этот файл — рабочая инструкция:
вышел новый релиз → подтянуть его в форк. Писано так, чтобы агент **без контекста** сделал это
быстро и не наступил на грабли.

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

### 3. Влить в `lestoroer/main` (форк без патчей → берём дерево как есть)
```bash
git checkout lestoroer/main
git merge --no-ff --no-commit 4.7-base || true   # конфликты ожидаемы и неважны
git read-tree -u --reset 4.7-base                # дерево := 4.7-base точь-в-точь
git commit --no-edit -m "Merge 4.7-base into lestoroer/main: 4.7-rc3 -> 4.7-stable (<REF>)"
```
Так origin потом fast-forward-ится (без force-push), а дерево гарантированно равно upstream.
> Если когда-нибудь появятся реальные патчи форка — этот трюк перестанет работать: тогда честный
> `git merge 4.7-base` и руками разрулить конфликты по каждому патчу (см. «Инвентарь патчей»).

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

### (если деплоишь на Quest) Экспорт-темплейты тоже устаревают
После бампа версии Android-темплейты для Quest всё ещё старой версии → собранный APK будет
рапортовать старую версию (те же грабли, но на шлеме). Пересобрать их через флоу `/deploy-quest`
(только Windows — на маке нет Android-тулчейна). Симптом: приложение на шлеме показывает старую
версию движка.

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
**`4.7-stable`** — engine commit `b45286406`, upstream-бамп `5b4e0cb0fd` («Bump version to
4.7-stable», Thaddeus Crews, 17.06.2026). Перешли с `4.7-rc3` 19.06.2026. Ноль патчей форка.
Якоря отката: теги `fork-pre-4.7-stable`, `fork-pre-4.7-upgrade`.

## Инвентарь патчей
Пока нет — дерево идентично upstream. Заводя `[Lestoroer]`-патч, записать сюда строкой:
ветка | файлы | зачем. Появление патчей ломает read-tree-трюк из шага 3 (тогда честный merge).
