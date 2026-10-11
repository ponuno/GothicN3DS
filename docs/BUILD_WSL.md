# Building the project from scratch on Windows with WSL (beginner's guide)

*(Русская версия — ниже, после этого раздела. / The Russian version follows below.)*

**Legal note.** The game's data (textures, models, sounds, music, scripts) is not distributed with this project and must not be distributed by you either. You need your own legal copy of Gothic 1. The files you produce (`data.pak`, `music.pak`, rendered WAVs) contain the game's content: keep them for yourself and never publish them. Only the source code, the tools, the compiled `gothic3ds.3dsx` (it contains no game data) and these instructions may be shared.

What you get at the end (step 7 is optional if you download a ready `.3dsx`): an SD card with `gothic3ds.3dsx`, `data.pak` (worlds and sounds) and `music.pak` (music).

All commands below go into an **Ubuntu (WSL)** terminal unless stated otherwise. You can copy a whole command and paste it with the right mouse button. Lines starting with `#` are comments; do not type them.

**The main WSL rule.** Windows drives live under `/mnt/` in WSL: drive `C:` is `/mnt/c`, and `C:\Program Files (x86)\Steam` is `"/mnt/c/Program Files (x86)/Steam"`. Backslashes `\` and drive letters (`C:`) do not work in WSL commands. Always put paths with spaces in quotes.

---

## Step 0. What you need

- Windows 10 (2004+) or Windows 11.
- Gothic 1 installed (Steam version); you need the path to its folder.
- A **New Nintendo 3DS** with Homebrew Launcher.
- About 15 GB of free disk space and an internet connection.

## Step 1. Install WSL and Ubuntu (once)

1. Open **PowerShell as administrator** (Start → PowerShell → right click → "Run as administrator").
2. Type:
   ```
   wsl --install -d Ubuntu
   ```
3. Reboot if asked. After the reboot an Ubuntu window opens: choose a user name and a password (the password is not shown while you type; that is normal).
4. From now on open the terminal via Start → **Ubuntu** (or type `wsl` in PowerShell).

## Step 2. Build tools (once)

```
sudo apt update
sudo apt install -y build-essential cmake ninja-build git zip unzip python3 wget gpg
```

Check: `cmake --version` must show 3.16 or newer, `g++ --version` GCC 11 or newer (C++20 is required).

## Step 3. The project

Keep the project **inside WSL** (your home folder `~`), not on the `C:` drive: everything is several times slower under `/mnt/c`.

1. Put `gothic3ds-base.zip` somewhere in Windows, e.g. `C:\Users\YOUR_USER\Downloads`.
2. Unpack it into the WSL home folder:
   ```
   cd ~
   unzip -o /mnt/c/Users/YOUR_USER/Downloads/gothic3ds-base.zip
   cd ~/gothic3ds
   ls
   ```
   `ls` must show `CMakeLists.txt  Makefile  README.md  core  docs  engine  platform  scripts  tests  third_party  tools`.

Run all further commands from `~/gothic3ds`. If you closed the window, return with `cd ~/gothic3ds`.

> Files named `*:Zone.Identifier` are harmless Windows marks. To delete them: `find ~/gothic3ds -name '*:Zone.Identifier' -delete`.

## Step 4. Build the PC tools

```
cd ~/gothic3ds
cmake -S . -B build -G Ninja
cmake --build build -j
```

The first run downloads the ZenKit library from GitHub and takes a few minutes. You get `build/g3ds-bake` (the converter) and `build/g3ds-host` (the PC runner).

Quick check without the game (optional):

```
./build/g3ds-bake synth -o /tmp/test.pak
ls -la /tmp/test.pak
```

Tests (optional, slow): `ctest --test-dir build --output-on-failure`.

## Step 5. The game's music as WAV

The original Gothic music is in the DirectMusic format, which the 3DS cannot play. So it is rendered once to WAV by a separate program, `dmrender`.

1. Build it (the dmusic library is downloaded automatically):
   ```
   cd ~/gothic3ds
   cmake -S tools/dmrender -B build-dmrender
   cmake --build build-dmrender
   ls build-dmrender/dmrender
   ```
   The last command must simply print the file's path.
2. Render all the music (the folder is the game's `_work\DATA\Music`; the program searches the subfolders `orchestra`, `dungeon`, `menu_men` ... by itself):
   ```
   build-dmrender/dmrender "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic/_work/DATA/Music" ~/music_wavs
   ```
   To try one theme, add its name at the end: `... ~/music_wavs OC_DAY_STD.sgt`. The result is `~/music_wavs/*.wav`.
3. Listen to a file or two in Windows: `\\wsl$\Ubuntu\home\YOUR_NAME\music_wavs`.

If the program prints `cannot load` for a theme, its `.sty` or `.dls` was not found; other themes are not affected.

## Step 6. Bake the game's data (`data.pak`, `music.pak`)

The converter reads the game's files and makes an archive for the 3DS. The game itself is not touched.

```
cd ~/gothic3ds
mkdir -p ~/out
./build/g3ds-bake world --game "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic" -o ~/out/data.pak --music-dir ~/music_wavs
```

Good to know:
- The converter runs for several minutes and prints a report. It should contain lines like `script/sfx: SFX.DAT`, `sounds: N C_SFX instances ... M baked` (M must be well above zero), `ambient sounds: ...`, `script/music: MUSIC.DAT` and `music: N C_MUSICTHEME instances ... K baked` (K above zero). If a number is 0: the game's sounds and themes live in `SFX.DAT` and `MUSIC.DAT` in `_work\Data\Scripts\_compiled`; the report says `sounds: SFX.DAT not found` / `music: MUSIC.DAT not found` if they are missing. If there is no `music:` line at all, check that `--music-dir` points to the folder with the WAVs from step 5. Check quickly: `grep -E "script/(sfx|music)|sounds:|music:" ~/out/bake.log`.
- `music.pak` appears next to `data.pak`: both files are needed.
- Print the report again with `./build/g3ds-bake info ~/out/data.pak`.
- The quality of the sound effects is set with `--sound-rate 22050`, of the music with `--music-rate 32000`.
- **Never put the game's data into the project archive** and never publish `data.pak` or `music.pak`: they are the game's content.
- After every project update, bake `data.pak` again (`docs/HANDOFF.md` says in the section of the round whether that is needed).

See the result on the PC without a console (optional):
```
./build/g3ds-host --pak ~/out/data.pak --still --shots 1 --out ~/out/world
```

## Step 7. The 3DS program (`gothic3ds.3dsx`) — OPTIONAL

**You can skip this step.** The project's author publishes a ready-made `gothic3ds.3dsx`; it contains no game data (the data is in the separate `data.pak` and `music.pak` that you bake yourself in steps 4–6), so it may be shared freely. Download it and go straight to step 8. Build it yourself only if you want to change the code or do not trust a prebuilt file.

To build it you need the devkitPro toolkit.

1. Install its package manager (once):
   ```
   wget https://apt.devkitpro.org/install-devkitpro-pacman
   chmod +x install-devkitpro-pacman
   sudo ./install-devkitpro-pacman
   ```
2. Install the compiler and libraries for the 3DS (about 1 GB):
   ```
   sudo dkp-pacman -S 3ds-dev
   ```
   Press Enter at the questions (the default is "all").
3. Environment variables (add them to `~/.bashrc` so you need not type them every time):
   ```
   echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
   echo 'export DEVKITARM=/opt/devkitpro/devkitARM' >> ~/.bashrc
   source ~/.bashrc
   ```
4. Build:
   ```
   cd ~/gothic3ds
   make
   ```
   `gothic3ds.3dsx` and `gothic3ds.smdh` appear in the project root. After changing sources, repeat `make`; a clean rebuild is `make clean && make`.

## Step 8. Copy to the SD card

Put the SD card into a card reader (it shows up in Windows as a drive, e.g. `E:`). First copy the result from WSL into Windows:

```
mkdir -p /mnt/c/gothic3ds_sd/3ds/gothic3ds
cp ~/gothic3ds/gothic3ds.3dsx ~/gothic3ds/gothic3ds.smdh ~/out/data.pak /mnt/c/gothic3ds_sd/3ds/gothic3ds/
cp ~/out/music.pak /mnt/c/gothic3ds_sd/3ds/gothic3ds/
```

Then, in Windows, copy the folder `C:\gothic3ds_sd\3ds\gothic3ds` to `3ds\gothic3ds` on the SD card. The card then holds:

```
/3ds/gothic3ds/gothic3ds.3dsx
/3ds/gothic3ds/gothic3ds.smdh
/3ds/gothic3ds/data.pak
/3ds/gothic3ds/music.pak
```

**Sound also needs the DSP firmware:** the file `/3ds/dspfirm.cdc` on the SD card. You dump it from your own console with the DSP1 tool (Homebrew App Store → "DSP1"). Without it the game runs but is silent (a message is shown at start).

Start: Homebrew Launcher → Gothic 3DS.

If something is wrong with the sound, after you quit the game the report `/3ds/gothic3ds/sound.txt` appears: it lists the sounds and themes the game looked for and did not find. Send it to the developer.

---

## Troubleshooting

| What you see | Cause and fix |
|---|---|
| `cmake: command not found` | Step 2 was not done: `sudo apt install -y build-essential cmake ninja-build git` |
| `The source directory ".../tools/dmrender" does not exist` | You are not in the project folder (`cd ~/gothic3ds`) or it was unpacked elsewhere |
| `Parse error in command line argument: DMUSIC_DIR` | The command was cut off: a path must follow `-DDMUSIC_DIR=`. Step 5 does not need this option at all |
| `tools/dmrender` contains many extra files (`src`, `include`, `vendor`) | The dmusic library got copied into that folder. It must hold only `CMakeLists.txt` and `dmrender.c`; unpack the project again |
| `build-dmrender/dmrender: No such file or directory` | The build did not finish: run `cmake --build build-dmrender` and read the last lines with `error` |
| `usage: dmrender MUSIC_DIR OUT_DIR ...` | Both paths are needed: where the music is and where to put the WAVs |
| `No such file or directory` for the game path | Wrong path: check with `ls "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic"`; drive letters like `C:` and `\` do not work |
| `Please set DEVKITARM in your environment` | Item 3 of step 7 was not done; run `source ~/.bashrc` |
| The converter cannot open / find `WORLD.ZEN` | That is not the game folder. It must contain `Data` and `_work` |
| The game is silent on the 3DS | `/3ds/dspfirm.cdc` is missing on the SD card (step 8) |
| Sound works, no music | `music.pak` is not next to `data.pak`, or the theme names were not found in `sound.txt`; send the report |
| `wsl: ... localhost proxy ...` warning | Harmless for the build |
| Very slow | The project is on `/mnt/c`. Move it to `~` (step 3) |

## Updating the project

When a new `gothic3ds-base.zip` arrives: unpack it over the old one (`cd ~ && unzip -o ...zip`), then repeat steps 4, 6, 7 and 8 (step 5 only if you want to render the music again). The folders `build`, `build-dmrender` and `~/out` can stay: they rebuild by themselves.

---

---

# Сборка проекта с нуля через WSL (инструкция для новичка) — русская версия

**Юридическое замечание.** Данные игры (текстуры, модели, звуки, музыка, скрипты) в проект не входят, и вы тоже не должны их распространять. Нужна ваша собственная легальная копия Gothic 1. Файлы, которые вы получаете (`data.pak`, `music.pak`, WAV с музыкой), содержат материалы игры: оставьте их себе и нигде не публикуйте. Делиться можно только исходным кодом, инструментами, собранным `gothic3ds.3dsx` (в нём нет данных игры) и этими инструкциями.

Что получится в конце: на SD-карте 3DS лежат `gothic3ds.3dsx`, `data.pak` (мир и звуки) и `music.pak` (музыка).

Все команды ниже вводятся в терминале **Ubuntu (WSL)**, если не сказано иное. Команду можно скопировать целиком и вставить правой кнопкой мыши. Строки, которые начинаются с `#`, — пояснения, их вводить не нужно.

**Главное правило WSL.** Диски Windows в WSL лежат в `/mnt/`: диск `C:` — это `/mnt/c`, а `C:\Program Files (x86)\Steam` — это `"/mnt/c/Program Files (x86)/Steam"`. Обратные слэши `\` и буквы дисков (`C:`) в командах WSL не работают. Пути с пробелами всегда берите в кавычки.

---

## Шаг 0. Что понадобится

- Windows 10 (2004+) или Windows 11.
- Установленная **Gothic 1** (Steam-версия) — для неё нужен путь к папке игры.
- Консоль **New Nintendo 3DS** с Homebrew Launcher.
- Около 15 ГБ свободного места и интернет.

## Шаг 1. Установка WSL и Ubuntu (один раз)

1. Откройте **PowerShell от имени администратора** (Пуск → PowerShell → правая кнопка → «Запуск от имени администратора»).
2. Введите:
   ```
   wsl --install -d Ubuntu
   ```
3. Перезагрузите компьютер, если попросит. После перезагрузки откроется окно Ubuntu: придумайте имя пользователя и пароль (при вводе пароля символы не видны — это нормально).
4. Дальше терминал Ubuntu открывается так: Пуск → **Ubuntu** (или в PowerShell командой `wsl`).

## Шаг 2. Программы для сборки (один раз)

```
sudo apt update
sudo apt install -y build-essential cmake ninja-build git zip unzip python3 wget gpg
```

Проверка: `cmake --version` должен показать версию 3.16 или выше, `g++ --version` — GCC 11 или новее (нужна поддержка C++20).

## Шаг 3. Проект

Проект надо держать **внутри WSL** (в домашней папке `~`), а не на диске `C:`: на `/mnt/c` всё работает в разы медленнее.

1. Положите `gothic3ds-base.zip` куда удобно в Windows, например в `C:\Users\ВАШ_ПОЛЬЗОВАТЕЛЬ\Downloads`.
2. Распакуйте в домашнюю папку WSL:
   ```
   cd ~
   unzip -o /mnt/c/Users/ВАШ_ПОЛЬЗОВАТЕЛЬ/Downloads/gothic3ds-base.zip
   cd ~/gothic3ds
   ls
   ```
   `ls` должен показать `CMakeLists.txt  Makefile  README.md  core  docs  engine  platform  scripts  tests  third_party  tools`.

Дальше все команды выполняются из папки `~/gothic3ds`. Если окно закрыли, вернитесь командой `cd ~/gothic3ds`.

> Файлы вида `*:Zone.Identifier` — служебные метки Windows, они безвредны. Удалить: `find ~/gothic3ds -name '*:Zone.Identifier' -delete`.

## Шаг 4. Сборка инструментов для ПК

```
cd ~/gothic3ds
cmake -S . -B build -G Ninja
cmake --build build -j
```

Первый запуск скачивает библиотеку ZenKit с GitHub, это займёт несколько минут. Получатся программы `build/g3ds-bake` (конвертер) и `build/g3ds-host` (запуск на ПК).

Быстрая проверка без игры (необязательно):

```
./build/g3ds-bake synth -o /tmp/test.pak
ls -la /tmp/test.pak
```

Тесты (необязательно, долго): `ctest --test-dir build --output-on-failure`.

## Шаг 5. Музыка игры в WAV

Оригинальная музыка Gothic — формат DirectMusic, 3DS её читать не умеет. Поэтому её один раз превращают в WAV отдельной программой `dmrender`.

1. Соберите программу (библиотека dmusic скачается сама):
   ```
   cd ~/gothic3ds
   cmake -S tools/dmrender -B build-dmrender
   cmake --build build-dmrender
   ls build-dmrender/dmrender
   ```
   Последняя команда должна просто напечатать путь к файлу.
2. Отрендерите всю музыку (путь — папка игры `_work\DATA\Music`; программа сама просматривает подпапки `orchestra`, `dungeon`, `menu_men` …):
   ```
   build-dmrender/dmrender "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic/_work/DATA/Music" ~/music_wavs
   ```
   Для пробы одной темы добавьте в конец её имя: `... ~/music_wavs OC_DAY_STD.sgt`. Результат — файлы `~/music_wavs/*.wav`.
3. Послушайте один-два файла в Windows: папка `\\wsl$\Ubuntu\home\ВАШЕ_ИМЯ\music_wavs`.

Если для какой-то темы программа пишет `cannot load`, значит, не нашёлся её `.sty` или `.dls`; остальные темы это не останавливает.

## Шаг 6. Запекание данных игры (`data.pak`, `music.pak`)

Конвертер читает файлы игры и делает архив для 3DS. Сама игра остаётся нетронутой.

```
cd ~/gothic3ds
mkdir -p ~/out
./build/g3ds-bake world --game "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic" -o ~/out/data.pak --music-dir ~/music_wavs
```

Полезно знать:
- Конвертер работает несколько минут и печатает отчёт. В нём должны быть строки `script/sfx: SFX.DAT`, `sounds: N C_SFX instances ... M baked` (M заметно больше нуля), `ambient sounds: ...`, `script/music: MUSIC.DAT` и `music: N C_MUSICTHEME instances ... K baked` (K больше нуля). Если где-то стоит 0: звуки и темы игры лежат в `SFX.DAT` и `MUSIC.DAT` в `_work\Data\Scripts\_compiled`; если их нет, в отчёте будет `sounds: SFX.DAT not found` / `music: MUSIC.DAT not found`. Если строки `music:` нет совсем, проверьте, что `--music-dir` указывает на папку с WAV из шага 5. Быстрая проверка: `grep -E "script/(sfx|music)|sounds:|music:" ~/out/bake.log`.
- Рядом с `data.pak` появится `music.pak`: нужны оба файла.
- Папка `~/out` создаётся командой `mkdir -p ~/out`; без неё конвертер напишет ошибку.
- Отчёт можно напечатать ещё раз: `./build/g3ds-bake info ~/out/data.pak`.
- Качество звуков задаётся флагом `--sound-rate 22050`, качество музыки — `--music-rate 32000`.
- **Данные игры в архив проекта не кладите** и никуда не выкладывайте `data.pak` и `music.pak`: это материалы игры.
- После каждого обновления проекта `data.pak` нужно запекать заново (в `docs/HANDOFF.md` в разделе нужного раунда указано, нужно ли это).

Посмотреть результат на ПК без консоли (необязательно):
```
./build/g3ds-host --pak ~/out/data.pak --still --shots 1 --out ~/out/world
```

## Шаг 7. Программа для 3DS (`gothic3ds.3dsx`) — НЕ ОБЯЗАТЕЛЬНО

**Этот шаг можно пропустить.** Автор проекта выкладывает готовый `gothic3ds.3dsx`; в нём нет данных игры (они в отдельных `data.pak` и `music.pak`, которые вы запекаете сами в шагах 4–6), поэтому его можно свободно распространять. Скачайте его и переходите сразу к шагу 8. Собирайте сами, только если хотите менять код или не доверяете готовому файлу.

Для сборки нужен набор разработчика devkitPro.

1. Установка менеджера пакетов (один раз):
   ```
   wget https://apt.devkitpro.org/install-devkitpro-pacman
   chmod +x install-devkitpro-pacman
   sudo ./install-devkitpro-pacman
   ```
2. Установка компилятора и библиотек для 3DS (скачивает ~1 ГБ):
   ```
   sudo dkp-pacman -S 3ds-dev
   ```
   На вопросы отвечайте Enter (по умолчанию «все»).
3. Переменные окружения (чтобы не вводить каждый раз, добавьте в `~/.bashrc`):
   ```
   echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
   echo 'export DEVKITARM=/opt/devkitpro/devkitARM' >> ~/.bashrc
   source ~/.bashrc
   ```
4. Сборка:
   ```
   cd ~/gothic3ds
   make
   ```
   Получатся `gothic3ds.3dsx` и `gothic3ds.smdh` в корне проекта. После изменения исходников повторяйте `make`; чистая пересборка: `make clean && make`.

## Шаг 8. Копирование на SD-карту

SD-карту вставьте в картридер ПК (она появится в Windows как диск, например `E:`). Сначала скопируйте результат из WSL в Windows (например, на рабочий стол):

```
mkdir -p /mnt/c/gothic3ds_sd/3ds/gothic3ds
cp ~/gothic3ds/gothic3ds.3dsx ~/gothic3ds/gothic3ds.smdh ~/out/data.pak /mnt/c/gothic3ds_sd/3ds/gothic3ds/
cp ~/out/music.pak /mnt/c/gothic3ds_sd/3ds/gothic3ds/
```

Затем в Windows скопируйте папку `C:\gothic3ds_sd\3ds\gothic3ds` на SD-карту в `3ds\gothic3ds`. Итог на карте:

```
/3ds/gothic3ds/gothic3ds.3dsx
/3ds/gothic3ds/gothic3ds.smdh
/3ds/gothic3ds/data.pak
/3ds/gothic3ds/music.pak
```

**Для звука нужна ещё прошивка DSP:** файл `/3ds/dspfirm.cdc` на SD-карте. Его получают с самой консоли утилитой DSP1 (Homebrew App Store → «DSP1»). Без него игра работает, но молча (об этом будет сообщение при запуске).

Запуск: Homebrew Launcher → Gothic 3DS.

Если что-то не так со звуком, после выхода из игры появится отчёт `/3ds/gothic3ds/sound.txt`: там перечислено, какие звуки и темы игра искала и не нашла. Пришлите его разработчику.

---

## Если что-то не получается

| Что видите | Причина и решение |
|---|---|
| `cmake: command not found` | Не выполнен шаг 2: `sudo apt install -y build-essential cmake ninja-build git` |
| `The source directory ".../tools/dmrender" does not exist` | Вы не в папке проекта (`cd ~/gothic3ds`) или проект распакован не туда |
| `Parse error in command line argument: DMUSIC_DIR` | Команда оборвана: после `-DDMUSIC_DIR=` должен стоять путь. В шаге 5 этот параметр не нужен вовсе |
| В `tools/dmrender` лежит много лишних файлов (`src`, `include`, `vendor`) | В эту папку попала библиотека dmusic. Должны быть только `CMakeLists.txt` и `dmrender.c`; распакуйте проект заново |
| `build-dmrender/dmrender: No such file or directory` | Сборка не дошла до конца: выполните `cmake --build build-dmrender` и прочитайте последние строки с `error` |
| `usage: dmrender MUSIC_DIR OUT_DIR ...` | Не указаны оба пути: откуда брать музыку и куда класть WAV |
| `No such file or directory` для пути к игре | Путь неверен: проверьте `ls "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic"`; буквы дисков `C:` и `\` не работают |
| `Please set DEVKITARM in your environment` | Не выполнен пункт 3 шага 7; выполните `source ~/.bashrc` |
| Конвертер пишет `cannot open` / не находит `WORLD.ZEN` | Указана не папка игры. В ней должны лежать `Data` и `_work` |
| Игра на 3DS молчит | Нет `/3ds/dspfirm.cdc` на SD-карте (шаг 8) |
| Звук есть, музыки нет | Нет `music.pak` рядом с `data.pak`, или имена тем в `sound.txt` не нашлись; пришлите отчёт |
| `wsl: Конфигурация прокси-сервера localhost обнаружена...` | Предупреждение, на сборку не влияет |
| Очень медленно | Проект лежит на `/mnt/c`. Перенесите в `~` (шаг 3) |

## Обновление проекта

Когда пришёл новый `gothic3ds-base.zip`: распакуйте его поверх (`cd ~ && unzip -o ...zip`), затем повторите шаги 4, 6, 7 и 8 (шаг 5 — только если хотите заново отрендерить музыку). Папки `build`, `build-dmrender` и `~/out` можно оставить: они пересоберутся сами.
