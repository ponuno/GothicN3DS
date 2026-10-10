# Передача работы: контекст и ТЗ

Документ для продолжения работы в новом чате. Состояние на 10.10.2026, версия архива `gothic3ds-base.zip`: позы в кроватях и на скамьях (узлы без дорожки), скольжение в желобах, «устойчивые объекты», восприятие и отношения гильдий.

## 0. Как начать новый чат

Приложить к первому сообщению:

1. `gothic3ds-base.zip` — весь проект (этот файл лежит в нём: `docs/HANDOFF.md`).
2. Последний отчёт запекания (вывод `g3ds-bake`, текстом или файлом). Желательно.

Первое сообщение, например: «Продолжаем порт Gothic 1 на New 3DS. Прочитай `docs/HANDOFF.md` в архиве и начни с задачи 1».

Новый чат не видит ни этого контейнера, ни прошлых сообщений: всё, что ему нужно, записано здесь и в `README.md`, `docs/ROADMAP.md`, `docs/FORMATS.md`.

## 1. Проект

Порт Gothic 1 на New Nintendo 3DS: свой лёгкий движок поверх данных игры.

- **`g3ds-bake`** (ПК, C++20, ZenKit) читает установленную Steam-версию и делает `data.pak`.
- **Рантайм** (`core`, `engine`, `platform/3ds`; C++17 без STL, исключений и RTTI; libctru + citro3d) работает на консоли.
- **`g3ds-host`** — та же логика на ПК с программным рендером 400×240, для отладки без консоли.

Правила проекта:

- Данные игры не распространяются: в архив и репозиторий не попадает ничего из игры (`.pak`, `.vdf`, `.sav`, `.zen`, `.dat`, тексты скриптов). `data.pak` каждый запекает из своей копии.
- Общение на русском. Ответ начинается с итога, затем прямо: что проверено на ПК, что может проверить только консоль, какие строки отчёта и фото прислать.
- Цель пользователя — «как можно больше функций»; направление последних версий — скрипты и живой мир.

## 2. Среда пользователя и порядок работы

- Windows + WSL (пользователь `vasya`), проект в `~/gothic3ds`. Архив распаковывается в `C:\`, затем `cp -r /mnt/c/gothic3ds/. ~/gothic3ds/`. Собирать только в файловой системе Linux (на `/mnt/c` CMake падает).
- Запекание:
  `./build/g3ds-bake world --game "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic" -o data.pak`
- Серверы devkitPro отвечают 403 на адрес пользователя, поэтому `.3dsx` собирает **GitHub Actions** (`.github/workflows/build.yml`: задания `host` и `n3ds`, контейнер `devkitpro/devkitarm`, `make -j2`). Файлы пользователь загружает в репозиторий через браузер.
- Проверка — на настоящей New 3DS (Luma3DS, Homebrew Launcher, файлы в `SD:/3ds/gothic3ds/`) и в Azahar.
- После смены форматов `data.pak` нужно запекать заново — об этом каждый раз писать явно.
- Кнопки на консоли и переключатели при запуске описаны в `README.md` (раздел «Сборка для 3DS»). Нижний экран — отчёт из 24 строк; из него пользователь присылает фото.

## 3. Что работает

Подтверждено на консоли с настоящими данными:

- Мир со стримингом чанков 40 м в фоне, вода, порталы между мирами, небо и время суток, лестницы.
- Герой: анимации, ходьба, плавание, камера по настройкам оригинала (`CAMERA.DAT`).
- Своя ВМ Daedalus: 2075 запусков из 2075 совпали с интерпретатором ZenKit.
- 1276 персонажей расставлены стартовыми скриптами; 12 ближайших (в 30 м) живут по состояниям `ZS_*` и ходят по сети путей.
- Кластеры 10 м внутри чанков и столкновения по этажам: 60 к/с в замке при дальности 200 м.
- Жители пользуются предметами мира (`AI_UseMob`): в строке `life` счётчик `use` 3–7, отказов `x0`.
- Светильники: по отчёту берутся из `.MDL` (`FIREPLACE_HIGH2.ASC x112 … <- FIREPLACE_HIGH2.MDL … node ZM_OCLIGHTER 188 tris`). Глазами пользователь это ещё не подтвердил.

Замеры пользователя на New 3DS (все — 16,7 мс, 60 к/с, `late 0/30`, `err 0/0`, `x0`):

| Сцена | cpu / gpu | update | треугольники: нарисовано / в чанках | вызовов |
|---|---|---|---|---|
| Старый лагерь, полёт, 60 м | 18 % / 30 % | 1,32 мс | 8 031 / 56 068 | 185 |
| Замок внутри, 60 м | 28 % / 88 % | 1,43 мс | 42 648 / 123 498 | 355 |
| Хижина с кроватью, 60 м, 05:00 | 20 % / 33 % | 1,26 мс | 9 681 / 67 998 | 247 |
| Замок, **200 м**, 09:00 | 67 % / 80 % | 1,35 мс | 40 038 / 202 246 | 796 |

До этой версии `update` был 9,47 мс. Разбивка `upd w0.5 n0.8 s0.2 p0.0 c0.0` показывает, что время уходило в столкновения камеры и игрока. В замке при 200 м: 41 чанк, память мешей 13,83 МБ, текстур 4,13 МБ, столкновений 9 255 КБ, свободно 11,2 МБ линейной памяти, анимации 867 КБ.

## 4. Замечания пользователя (два последних раунда)

Раунд 1 (версия с кластерами): «НПС пользуются кроватями, но сразу встают»; «скамейки и стулья работают странно, идут сквозь стены»; «сидят то задом наперёд, то в воздухе»; «герой скользит по склону и застревает, если прыгнуть к возвышению».

Раунд 2 (версия с кроватями), дословно:
1. «Сон и другие действия теперь постоянны, но анимация сна кривая, персонажи спят по вертикали в воздухе».
2. «Персонажи всё ещё сидят задом наперёд, во многих сценах».
3. «Не нравится, как объекты постоянно появляются и исчезают прямо на глазах … было бы славно сделать кнопку при запуске, чтобы проверить улучшенный вариант и влияние на производительность».
4. «Займись восприятием» (пункт 1 списка «дальше»).
5. «Баг с застреванием героя на склонах не исправлен, он теперь как бы скользит в сторону, но не вниз».

Фото раунда 2: спящий `Grim SLEEPBED`, `mob BED I s1 bs 15 off 0`, `ani S_BED_FRONT_S1` — состояние и предмет верны, поза нет; при 200 м в замке и у хижин 60 к/с, `detail` 76–100%.

## 5. Что сделано

**Раунд 1** (проверено на ПК; на консоли подтвердилось: кровати до утра, `bs 15`). Кровать даёт `BS_MOBINTERACT`; второй `AI_UseMob` другого вида пропускается; встроенный `ZS_Sleep` по образцу игры; видимость предмета (`World::lineFree`, `Mobs::candidates`, `Npcs::findMob`); житель встаёт на пол, по которому пришёл; диагностика (отчёт `things to use: what … has`, `places to stand at`, имена состояний `script/states`, строки нижнего экрана). Запись движения корня у переходов без «M» (`model_bake.cpp`) оказалась ни при чём: в игре все анимации предметов с «M».

**Раунд 2.**
- **Поза (замечания 1–2).** По отчёту у `S_BENCH_S1`, `S_THRONE_*`, `S_BED_FRONT_S1`, `S_BEDHIGH_*_S1`, `R_CHAIR_RANDOM_*`, `R_BENCH_RANDOM_*` нет дорожки корня (в отчёте теперь `no root`). `Animator` ставил такие узлы в позу покоя; теперь (`Animator::_base`) узел без дорожки хранит значение, оставленное предыдущей анимацией, как в ZenGin; `play(nullptr)` — поза покоя. Встроенные `S_BENCH_S1`, `S_BEDHIGH_S1` без корня (`makeClip(…, withRoot=false)`); тест `life` на старом коде: 80 «стоит в воздухе» за ночь.
- **Склон (5).** Желоб под уклоном: с граней тело сталкивало поперёк. `Player::_slideVel`: скорость скольжения набирается по нормали (1600 см/с², не больше 700), то, что упёрлось в скалу (`collideWalls`), гасится. Тесты: желоб 25°/65° в `collision` и `hero`.
- **Объекты (3).** Переключатель при запуске — крестовина вниз (`steady`): `detailDistance` 60/100 м вместо 35/70, `pacer.cfg.minDetail = 1` (регулятор не убирает объекты, только переходит на 30 к/с). На ПК не измерялось — цену покажет консоль.
- **Восприятие (4).** `engine/npc_perc.cpp`: `Npc_PercEnable/Disable`, `Npc_PerceiveAll`, `Npc_SendPassivePerc` (OTHER — отправитель, VICTIM — жертва, без эха), активные `PERC_ASSESSPLAYER` и `PERC_ASSESSENEMY` (ближайший враждебный по `Game::attitude`, видимость — `World::lineFree` r = 12 см), раз в `percTime` с разбросом. `Game`: таблица гильдий (`GIL_MAX`, всё враждебно до заполнения), отношения к игроку, предметы в инвентаре, цель, `Perc_SetRange` (+ вызов `InitPerceptions` при загрузке). Взгляд: `NPC_ACT_LOOK`, голова — `Model::turnNode` по `BIP01 HEAD` (до 1,2 рад вбок, 0,5 вверх-вниз, 3 рад/с), учитывается поворот корня. `AI_OutputSVM` — ожидание 1,4 с, фраза в `NpcActor::said`. Драк нет: `startState` не начинает состояния с `ATTACK` в имени (счётчик `fightsRefused`, `fight` на экране). Крестовина влево при запуске выключает восприятие. Нижний экран: строка `perc`.

## 6. ТЗ

### Задача 1. Приёмка на консоли

Запечь заново, прислать: строки отчёта `life: the states call …` (что осталось из функций), `things to use: what humans has …`; фото спящего и сидящего (поза), героя в желобе, нижнего экрана со строкой `perc` у ворот лагеря и рядом со зверями; замеры `gpu`/`tris`/`fps` одного места с крестовиной вниз и без.

Возможные последствия восприятия на настоящих скриптах, за которыми следить: люди, увидев опасного зверя, уходят в `ZS_AssessMonster` (оружия и боя нет); зверь-охотник (`HUNTER`) ходит за героем, пока тот в его радиусе; `B_AssessSC` при сближении меньше 50 см ведёт в `ZS_ObserveIntruder`. Если что-то из этого мешает — крестовина влево и фото.

### Задача 2. Остаток задачи о предметах

1. Последний отрезок до предмета: по сети путей до ближайшей к месту точки, оттуда напрямик; `World::pushSphere` на отрезке.
2. Житель, который не может дойти (`stuck`), отцепляется и считается в `mobsFailed`.

### Дальше — по решению пользователя

1. **Бой** — следующее по отчёту (`AI_EQUIPBESTMELEEWEAPON`, `AI_READYMELEEWEAPON`, `AI_REMOVEWEAPON`, `NPC_ISINFIGHTMODE`, `NPC_HASREADIEDWEAPON` …); без него состояния `ZS_ATTACK`/`ZS_MM_ATTACK` не начинаются.
2. **Диалоги**: `Npc_CheckInfo`, `ZS_Talk`, тексты из `OU.BIN`; голоса `AI_OutputSVM`.
3. **Столкновения жителей** с миром, игроком и друг с другом.
4. **Пламя и частицы**; **отсечение по помещениям**; предел 48 моделей жителей; оверлеи анимаций; анимации скольжения героя (`S_SLIDE`).
5. Мелочи из отчёта: `WASH_SLOT.ASC`, `OW_MONSTER_NAVIGATE02`, 2 текстуры жителей.

## 7. Карта кода

| Где | Что |
|---|---|
| `core/include/g3ds/` | Форматы: `pak.h`, `tex.h`, `world.h` (G3WD/G3CH, версия 4, `ChunkBatch::cluster`), `model.h`, `script.h`, `game.h` (персонажи, сеть путей, `mobs`: `MobsHeader`, `WorldMob`, `MobSlot`), `math.h` |
| `engine/world.*` | Стриминг, отсечение по кластерам, слияние вызовов, столкновения мира |
| `engine/collision.*` | Сетка 2,5 м с уровнями высоты; `rayDown`, `pushSphere`, `boxFree`, счётчики; линия видимости — `World::lineFree` |
| `engine/app.*`, `player.*`, `hero.*` | Кадр, игрок (скольжение и стойка на крутом склоне — `Player::walk`), камера, герой |
| `engine/script.*` | ВМ Daedalus |
| `engine/game.*` | Персонажи, распорядок, привязка внешних функций; отношения гильдий, инвентарь, цель; `lifeFunctions` — 47 имён |
| `engine/npcs.*`, `npc_ai.cpp`, `npc_perc.cpp` | Живые жители (восприятие и взгляд — `npc_perc.cpp`): очередь действий, состояния, ходьба, предметы (`useMob`, `mobClip`, `releaseMob`, `bodyStateOf`, `findMob`), внешние функции `AI_*`, `Wld_*`, `Npc_*`; для отчёта `stateName`, `animationName`, `nearest` |
| `engine/mobs.*` | Таблица предметов мира: поиск (`find`, `candidates`), места, кто чем занят |
| `engine/waynet.*`, `model.*`, `loader.*`, `pacer.*`, `sky.*` | Сеть путей, скелетные модели, фоновая загрузка, регулятор нагрузки, небо |
| `platform/3ds/` | citro3d, шейдеры PICA (`*.v.pica` и готовые `*_shbin.h`), `main.cpp` с отчётом |
| `platform/host/` | Программный рендер и `g3ds-host` (`--who`, `--time`, `--to-time`, `--stats`, `--whole-chunks` …) |
| `tools/bake/` | `import_zen.cpp` (всё из ZenGin), `chunker.cpp`, `populate.cpp` (заселение и нужные анимации), `script_bake.cpp`, `script_check.cpp` (сверка с ZenKit), `synth*.cpp` (встроенная сцена без данных игры), `bake.cpp`, `main.cpp` |
| `tests/` | `test_core.cpp`, `test_script.cpp`, `test_frames.cpp`, `test_zen.cpp`, `e2e.sh`, имитация citro3d в `mock3ds/` |
| `scripts/` | `check_3ds.sh` (код консоли без devkitARM), `shbin2h.py` |

Как устроены предметы мира: вид — первое слово имени модели (`BEDHIGH_PSI.ASC` → `BEDHIGH`); места — узлы `ZS_POS*` скелета предмета (`_FRONT`, `_BACK`, `_DIST`); имена анимаций составляет движок: `T_<ВИД>[_FRONT|_BACK]_STAND_2_S0`, `T_<ВИД>_S0_2_S1`, `S_<ВИД>_S1`, обратно `T_<ВИД>_S1_2_S0`, `T_<ВИД>_S0_2_STAND`. Стадии жителя: `MOB_COMING → MOB_TURNING → MOB_ENTERING → MOB_IN → MOB_LEAVING`. Предмет без мест никому не предлагается, как в оригинале.

## 8. Ловушки

- **Типы на консоли.** У devkitARM `int32_t` — это `long`: `clamp(i32, 0, 5)`, `min(u32, sizeof x)` собираются везде, кроме консоли. Ловит `scripts/check_3ds.sh`; однажды из-за этого упала сборка у пользователя.
- **Файл модели объекта.** Визуал `.ASC` — это собранный `.MDL` с тем же именем; `.MDM` + `.MDH` только если `.MDL` нет; `.MDS` строится из скрипта модели. Обратный порядок превращал светильники в костры.
- **Обрезанный `GOTHIC.DAT`:** ZenKit читает нули за концом файла; `importZenScript` проверяет адреса сам.
- **`pushSphere` и уровни:** диапазон уровней пересчитывается после каждого выталкивания, иначе ответы расходятся с эталоном.
- Тестовые программы без аргумента-каталога пишут в текущий каталог: запускать `build/tests/test_core build/tests <фильтр>` и `build/tests/test_zen build/tests/zen_fixture <ZenKit>/tests/samples`.
- Пока тесты под санитайзерами собираются или идут, не править исходники и `tests/e2e.sh`: получится ложный отказ.
- Встроенная сцена: хижины расставлены случайно, и точки `CAMP_HUT_*` стоят у их стен. С проверкой видимости кровать по ту сторону стены не выбирается — кровати сцены поставлены с видимой стороны; низкая кровать у первой хижины нарочно за стеной (тест видимости). Вторая кровать ближе 10 м к площади, это учтено в ответах `PROBE_MOB`.
- Движение корня: оно записывается и у анимаций без «M», если они не зациклены и корень уходит больше чем на 2 см. Добавляя встроенные анимации, держать корень петли там же, где кончается переход.
- Узел без дорожки в анимации хранит то, что ему оставила предыдущая (`Animator::_base`), а не позу покоя: у петель игры часто нет корня. Отчёт помечает такие анимации `no root`.
- Восприятие вызывает функции скриптов во время шага жителя (вложенный вызов машины) и может рассылать пассивные восприятия; рассылка не повторяется изнутри реакции (без эха).
- Встроенная сцена (`synth*`) — единственное, на чём можно проверить поведение в контейнере. Она должна повторять приёмы настоящих скриптов; ошибка с кроватями прошла мимо тестов именно потому, что встроенный `ZS_SleepBed` был проще настоящего.

## 9. Среда разработки в контейнере

Восстановление (S — временный каталог сеанса):

```sh
cd /home/claude && unzip -q <путь к gothic3ds-base.zip>
mkdir -p deps && cd deps
git clone -q https://github.com/GothicKit/ZenKit.git && (cd ZenKit && git checkout -q ddf27decd5eeb48ec715e6e66e5f5072c51d88ee && git submodule update --init --recursive -q)
git clone -q --depth 1 https://github.com/Try/OpenGothic.git
git clone -q --depth 1 https://github.com/devkitPro/libctru.git
git clone -q --depth 1 https://github.com/devkitPro/citro3d.git
git clone -q --depth 1 https://github.com/devkitPro/picasso.git
git clone -q --depth 1 https://github.com/AmProsius/gothic-1-community-patch.git g1cp   # тексты скриптов G1, только читать
git clone -q --depth 1 --filter=blob:none --sparse https://github.com/devkitPro/newlib.git newlib && (cd newlib && git sparse-checkout set newlib/libc/include)
mkdir -p $S/pic && (cd picasso && g++ -O1 -w -std=gnu++11 -DPACKAGE_STRING='"picasso"' -o $S/pic/picasso source/*.cpp)
P=/home/claude/gothic3ds; Z=-DFETCHCONTENT_SOURCE_DIR_ZENKIT=/home/claude/deps/ZenKit
cmake -S $P -B $P/build -G Ninja $Z
cmake -S $P -B $P/build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug $Z \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake -S $P -B $P/build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DG3DS_WITH_ZENKIT=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
```

Время: ctest в выпуске ≈ 70 с (6 тестов), под ASan+UBSan ≈ 14 мин (6 тестов), под TSan ≈ 12 мин (4 теста, без ZenKit). Длинные запускать в фоне.

Проверки кода консоли:

```sh
scripts/check_3ds.sh /home/claude/deps        # должно напечатать: check_3ds: clean
D=/home/claude/deps
g++ -m32 -fsyntax-only -std=gnu++17 -fno-rtti -fno-exceptions -Wall -Wextra -Wshadow \
  -U__UINT32_TYPE__ '-D__UINT32_TYPE__=long unsigned int' -U__INT32_TYPE__ '-D__INT32_TYPE__=long int' \
  -D__3DS__ -DARM11 -D_3DS -nostdinc -isystem $D/newlib/newlib/libc/include -isystem $D/libctru/libctru/include \
  -isystem $D/citro3d/include -isystem $(g++ -print-file-name=include) \
  -Icore/include -Iengine -Iplatform/3ds -Ithird_party/lz4 core/src/*.cpp engine/*.cpp platform/3ds/*.cpp
$S/pic/picasso -o $S/world.shbin platform/3ds/world.v.pica && python3 scripts/shbin2h.py $S/world.shbin $S/w.h world_shbin
# сравнить $S/w.h с platform/3ds/world_shbin.h без первой строки; то же для world_plain
```

Архив:

```sh
cd /home/claude && rm -f gothic3ds-base.zip && zip -qr gothic3ds-base.zip gothic3ds \
  -x 'gothic3ds/build/*' 'gothic3ds/build-dbg/*' 'gothic3ds/build-tsan/*' 'gothic3ds/build' 'gothic3ds/build-dbg' 'gothic3ds/build-tsan'
```

**Справочники** (только читать; в проект и архив ничего из них не копировать):

- **OpenGothic** — поведение движка: `common/world/objects/interactive.cpp`, `npc.cpp` (`Npc::nextAiAction`, `Npc::tickRoutine`, `bodyState`), `common/game/gamescript.cpp`. Сделан прежде всего под Gothic 2, в Gothic 1 может ошибаться (см. 5.1).
- **Исходные тексты скриптов Gothic 1** — репозиторий `AmProsius/gothic-1-community-patch`, каталог `scriptbase/_work/Data/Scripts/Content/` (кодировка cp1252; `git clone --depth 1`, 21 МБ). Состояния людей — `Story/ZS/*.d`, монстров — `AI/ZS_Monster/`, константы — `AI/AI_Intern/BodyStates.d`, `AI_Constants.d`, проверки — `AI/AI_Intern/C_Functions.d`, объявления внешних функций с пояснениями — `AI/AI_Intern/Externals.d`. Лицензия набора разработчика: только некоммерческое использование. Раньше считалось, что скриптов игры в контейнере нет, — с этим репозиторием настоящие состояния можно читать, а встроенную сцену писать по их образцу.
- **Настоящие данные Gothic 1 в образцах ZenKit:** `tests/samples/G1/Save/WORLD.SAV` (все объекты и сеть путей `WORLD.ZEN`, без мешей) и `tests/samples/hierarchy0.mdh` (скелет каменной мельницы с узлом `ZS_POS0`).

Чего в контейнере нет: моделей, анимаций и собранного `GOTHIC.DAT` игры; консоли и Azahar. Всё это проверяет только пользователь.

## 10. Последний отчёт запекания (настоящая игра)

- `wrote data.pak: 5 worlds, 3692 entries in 27.3 s`.
- `clusters: the chunks are cut into 18893 boxes of 10 m (75 at most in one): 54 triangles and 2.6 batches in a box on average`; 48 753 пакета (43,8 на чанк); самый тяжёлый чанк — 46 779 треугольников, 811 пакетов.
- `things to use: 1201 of 40 kinds, with 1646 places to stand at (48 have none …)`: `CHAIR x203, CHESTBIG x194, FIREPLACE x157, BEDLOW x115, BED x98, BENCH x71, …` — те же числа, что в сохранённой игре из образцов ZenKit.
- В мирах 46 видов предметов, скрипты называют 35; не названы: `BARRELO, CHESTBIG, CHESTSMALL, EXCALIBUR, ORCDRUM, PC, PILLAR, PRIESTGRAVE, SPORTAL, TOUCHPLATE, TURNSWITCH`.
- Анимации: скрипты называют или подразумевают 154, предметы добавляют 70 семейств; 22 скелета, 933 анимации, 10,39 МБ (было 579 и 7,65 МБ); у людей 387.
- `life: the states call 146 engine functions, 50 of which the engine carries out`. Первые недостающие: `NPC_PERCENABLE x153, AI_LOOKATNPC x51, AI_STOPLOOKAT x42, AI_OUTPUTSVM x31, NPC_SETTARGET x25, AI_REMOVEWEAPON x25, NPC_ISDEAD x23, AI_EQUIPBESTMELEEWEAPON x17, NPC_GETTARGET x17, NPC_ISINFIGHTMODE x17, NPC_GETATTITUDE x17, NPC_PERCEIVEALL x16, NPC_SETTEMPATTITUDE x16, NPC_HASITEMS x15`.
- Состояния по числу жителей: `ZS_MM_ALLSCHEDULER x682, ZS_SLEEP x194, ZS_SMALLTALK x97, ZS_GUARD x93, ZS_STANDAROUND x65, ZS_SITAROUND x63, ZS_SITCAMPFIRE x52, ZS_PICKORE x47`.
- Сверка ВМ: 2075 запусков, 2075 совпали.
- Новое предупреждение: `object visual WASH_SLOT.ASC could not be read: no triangles`.

## 11. Приёмка каждой версии

1. Шесть тестов ctest в выпуске.
2. Те же тесты под ASan+UBSan; четыре — под TSan.
3. `scripts/check_3ds.sh` печатает `check_3ds: clean`; проверка GCC `-m32` без предупреждений.
4. Заголовки шейдеров совпадают с выводом picasso (если шейдеры менялись).
5. Чистая сборка и тесты из распакованного архива.
6. В архиве нет данных игры.
7. Архив отдаётся файлом `gothic3ds-base.zip`.
8. В ответе: что изменено; что проверено на ПК; что проверит только консоль; какие строки отчёта запекания и какие фото нижнего экрана прислать; нужно ли запекать `data.pak` заново.

Если когда-нибудь появятся коммиты в git: проект здесь не репозиторий, коммитов не было; пользователь загружает файлы в GitHub сам.
