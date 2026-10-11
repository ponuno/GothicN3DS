# Building the project from scratch on Windows with WSL (beginner's guide)

*(Русская версия — ниже, после этого раздела. / The Russian version follows below.)*

**Legal note.** The game's data (textures, models, sounds, music, scripts) is not distributed with this project and must not be distributed by you either. You need your own legal copy of Gothic 1. The files you produce (`data.pak`, `music.pak`, `speech.pak`, rendered WAVs) contain the game's content: keep them for yourself and never publish them. Only the source code, the tools, the compiled `gothic3ds.3dsx` (it contains no game data) and these instructions may be shared.

What you get at the end (step 7 is optional if you download a ready `.3dsx`): an SD card with `gothic3ds.3dsx`, `data.pak` (worlds and sounds), `music.pak` (music) and `speech.pak` (voices), plus the DSP firmware file `dspfirm.cdc` that every sound needs (see the box below).

> **⚠ NO SOUND WITHOUT `dspfirm.cdc`.** The 3DS plays sound through its DSP chip, and the chip needs a firmware file that Nintendo's system keeps inside the console. Homebrew cannot use it until you dump it **once** into the SD card as `sdmc:/3ds/dspfirm.cdc` (the `3ds` folder itself, **not** `3ds/gothic3ds`). On the console: Homebrew Launcher → **DSP1** (also in the Homebrew App Store) → run it → it says the firmware was dumped. Check on the PC that `3ds/dspfirm.cdc` exists on the SD card. Without this file the game runs but is completely silent: no effects, no music, no voices, however correct your `.pak` files are. After you quit the game, `3ds/gothic3ds/sound.txt` shows it in its first line: `dsp: open` is good; `dsp: NOT open (ndspInit 0xD880A7FA ...)` means the file is missing.

All commands below go into an **Ubuntu (WSL)** terminal unless stated otherwise. You can copy a whole command and paste it with the right mouse button. Lines starting with `#` are comments; do not type them.

**The main WSL rule.** Windows drives live under `/mnt/` in WSL: drive `C:` is `/mnt/c`, and `C:\Program Files (x86)\Steam` is `"/mnt/c/Program Files (x86)/Steam"`. Backslashes `\` and drive letters (`C:`) do not work in WSL commands. Always put paths with spaces in quotes.

---

## Step 0. What you need

- Windows 10 (2004+) or Windows 11.
- Gothic 1 installed (Steam version); you need the path to its folder.
- A **New Nintendo 3DS** with Homebrew Launcher, and the DSP firmware dumped with **DSP1** to `sdmc:/3ds/dspfirm.cdc` (see the warning above; once per console).
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

Check all four; each must print a version and not "command not found":
```
cmake --version
ninja --version
g++ --version
unzip -v | head -1
```
`cmake` must be 3.16 or newer, `g++` GCC 11 or newer (C++20 is required). **If `ninja` or `unzip` is missing, step 4 and step 3 will fail**: run the `apt install` line above again.

## Step 3. The project

Keep the project **inside WSL** (your home folder `~`), not on the `C:` drive: everything is several times slower under `/mnt/c`.

1. Download the **newest** `gothic3ds-base.zip` and keep exactly one copy of it in Windows, e.g. in `C:\Users\YOUR_USER\Downloads` (delete old copies: browsers add `(1)`, `(2)` ... to the names and an old one is easy to unpack by mistake).
2. Find it and check that it is the right one (replace `YOUR_USER` with your Windows user name, the folder name under `C:\Users`):
   ```
   cd ~
   ZIP=$(ls -t /mnt/c/Users/YOUR_USER/Downloads/gothic3ds-base*.zip | head -1)
   echo "$ZIP"
   unzip -p "$ZIP" gothic3ds/tools/bake/import_zen.cpp | grep -c importZenSfxScript
   ```
   The last command must print `1` or more. If it prints `0`, this is an old archive: delete it and download the newest one.
3. Unpack it into the WSL home folder, **into an empty place** (move the old project away first, so old and new files do not mix):
   ```
   cd ~
   [ -d gothic3ds ] && mv gothic3ds gothic3ds_old_$(date +%s)
   unzip -q "$ZIP"
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
strings build/g3ds-bake | grep -c "script/sfx"
```

The last line must print a number above 0: it proves that the converter you built is the new one. (If `ninja` is not installed, cmake stops at once with "unable to find a build program corresponding to Ninja": install it with `sudo apt install -y ninja-build`, delete the folder `build` and run the commands again.)

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

## Step 6. Bake the game's data (`data.pak`, `music.pak`, `speech.pak`)

The converter reads the game's files and makes an archive for the 3DS. The game itself is not touched.

```
cd ~/gothic3ds
mkdir -p ~/out
./build/g3ds-bake world --game "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic" -o ~/out/data.pak --music-dir ~/music_wavs
```

Good to know:
- The converter runs for several minutes and prints a report. It should contain lines like `script/sfx: SFX.DAT`, `sounds: N C_SFX instances ... M baked` (M must be well above zero), `ambient sounds: ...`, `script/music: MUSIC.DAT` and `music: N C_MUSICTHEME instances ... K baked` (K above zero). If a number is 0: the game's sounds and themes live in `SFX.DAT` and `MUSIC.DAT` in `_work\Data\Scripts\_compiled`; the report says `sounds: SFX.DAT not found` / `music: MUSIC.DAT not found` if they are missing. If there is no `music:` line at all, check that `--music-dir` points to the folder with the WAVs from step 5. Check quickly: `grep -E "script/(sfx|music)|sounds:|music:" ~/out/bake.log`.
- `music.pak` and `speech.pak` appear next to `data.pak`: all three are needed (`speech.pak` holds the spoken lines; without it the game shows subtitles only).
- The voices come from the game's own `Speech.vdf`/`Sounds.vdf` (or loose files in `_work\Data\Sound\Speech`). The report has a line `voices: N lines of dialogue, M recordings baked (... MB ...)`; M should be in the thousands. `speech.pak` is large (hundreds of MB; it is IMA ADPCM at 11.025 kHz). Smaller and lower quality: `--voice-rate 8000`; skip the voices altogether: `--no-voices`. If M is 0 the game was installed without the speech archive (some localized versions keep it elsewhere) — send the `voices:` line to the developer.
- Print the report again with `./build/g3ds-bake info ~/out/data.pak`.
- The quality of the sound effects is set with `--sound-rate 22050`, of the music with `--music-rate 32000`, of the voices with `--voice-rate 11025`.
- **Never put the game's data into the project archive** and never publish `data.pak`, `music.pak` or `speech.pak`: they are the game's content.
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
cp ~/out/music.pak ~/out/speech.pak /mnt/c/gothic3ds_sd/3ds/gothic3ds/
```

Then, in Windows, copy the folder `C:\gothic3ds_sd\3ds\gothic3ds` to `3ds\gothic3ds` on the SD card. The card then holds:

```
/3ds/gothic3ds/gothic3ds.3dsx
/3ds/gothic3ds/gothic3ds.smdh
/3ds/gothic3ds/data.pak
/3ds/gothic3ds/music.pak
/3ds/gothic3ds/speech.pak
/3ds/dspfirm.cdc          <- NOT inside gothic3ds: directly in the 3ds folder
```

**Do not forget `/3ds/dspfirm.cdc`** (the box at the top): the DSP firmware dumped with DSP1 on your console. Without it there is no sound at all. Sound menu in the game: **Volume** (effects), **Music**, **Voices**.

Start: Homebrew Launcher → Gothic 3DS.

If something is wrong with the sound, after you quit the game the report `/3ds/gothic3ds/sound.txt` appears: it lists the sounds and themes the game looked for and did not find. Send it to the developer.

---

## Troubleshooting

| What you see | Cause and fix |
|---|---|
| `unable to find a build program corresponding to "Ninja"` / `CMAKE_CXX_COMPILER not set` | `ninja` (or the compiler) is not installed: `sudo apt install -y ninja-build build-essential cmake`, then `rm -rf build` and repeat step 4 |
| `unzip: command not found` | `sudo apt install -y unzip` |
| `strings build/g3ds-bake \| grep -c "script/sfx"` prints 0, or the report says `0 C_SFX instances` | An old converter/archive: the project folder holds old files. Repeat step 3 with the newest archive (check it with the `unzip -p ... \| grep -c importZenSfxScript` command), then `rm -rf build` and step 4 |
| `cannot find or open .../YOUR_USER/Downloads/...zip` | `YOUR_USER` must be replaced by your real Windows user name; list them with `ls /mnt/c/Users` |
| `cmake: command not found` | Step 2 was not done: `sudo apt install -y build-essential cmake ninja-build git` |
| `The source directory ".../tools/dmrender" does not exist` | You are not in the project folder (`cd ~/gothic3ds`) or it was unpacked elsewhere |
| `Parse error in command line argument: DMUSIC_DIR` | The command was cut off: a path must follow `-DDMUSIC_DIR=`. Step 5 does not need this option at all |
| `tools/dmrender` contains many extra files (`src`, `include`, `vendor`) | The dmusic library got copied into that folder. It must hold only `CMakeLists.txt` and `dmrender.c`; unpack the project again |
| `build-dmrender/dmrender: No such file or directory` | The build did not finish: run `cmake --build build-dmrender` and read the last lines with `error` |
| `usage: dmrender MUSIC_DIR OUT_DIR ...` | Both paths are needed: where the music is and where to put the WAVs |
| `No such file or directory` for the game path | Wrong path: check with `ls "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic"`; drive letters like `C:` and `\` do not work |
| `Please set DEVKITARM in your environment` | Item 3 of step 7 was not done; run `source ~/.bashrc` |
| The converter cannot open / find `WORLD.ZEN` | That is not the game folder. It must contain `Data` and `_work` |
| The game is silent on the 3DS (no effects, no music, no voices) | `/3ds/dspfirm.cdc` is missing on the SD card: run DSP1 on the console. `sound.txt` then starts with `dsp: NOT open (ndspInit 0xD880A7FA ...)` |
| Sound and music work, nobody speaks | `speech.pak` is not next to `data.pak`, the **Voices** slider is 0, or the bake had no recordings (`voices:` line of the report, `voices:` line of `sound.txt`) |
| Sound works, no music | `music.pak` is not next to `data.pak`, or the theme names were not found in `sound.txt`; send the report |
| `wsl: ... localhost proxy ...` warning | Harmless for the build |
| Very slow | The project is on `/mnt/c`. Move it to `~` (step 3) |

## Updating the project

When a new `gothic3ds-base.zip` arrives: repeat step 3 (find and check the newest archive, move the old project away, unpack), then steps 4, 6, 7 and 8. The project is rebuilt from scratch (a few minutes). The WAV files in `~/music_wavs` and the results in `~/out` are outside the project and stay, so step 5 is needed only if you want to render the music again.

---

---

# Сборка проекта с нуля через WSL (инструкция для новичка) — русская версия

**Юридическое замечание.** Данные игры (текстуры, модели, звуки, музыка, скрипты) в проект не входят, и вы тоже не должны их распространять. Нужна ваша собственная легальная копия Gothic 1. Файлы, которые вы получаете (`data.pak`, `music.pak`, `speech.pak`, WAV с музыкой), содержат материалы игры: оставьте их себе и нигде не публикуйте. Делиться можно только исходным кодом, инструментами, собранным `gothic3ds.3dsx` (в нём нет данных игры) и этими инструкциями.

Что получится в конце: на SD-карте 3DS лежат `gothic3ds.3dsx`, `data.pak` (мир и звуки), `music.pak` (музыка) и `speech.pak` (голоса), а также файл прошивки DSP `dspfirm.cdc`, без которого не будет никакого звука (см. рамку ниже).

> **⚠ БЕЗ `dspfirm.cdc` ЗВУКА НЕ БУДЕТ.** 3DS проигрывает звук через чип DSP, а ему нужна прошивка, которую система Nintendo прячет внутри консоли. Хоумбрю может ею пользоваться, только если вы **один раз** выгрузите её на SD-карту как `sdmc:/3ds/dspfirm.cdc` (прямо в папку `3ds`, **не** в `3ds/gothic3ds`). На консоли: Homebrew Launcher → **DSP1** (есть и в Homebrew App Store) → запустить → появится сообщение, что прошивка выгружена. Проверьте на ПК, что на SD-карте есть `3ds/dspfirm.cdc`. Без этого файла игра запускается, но полностью немая: ни эффектов, ни музыки, ни голосов, как бы правильно ни были собраны `.pak`. После выхода из игры файл `3ds/gothic3ds/sound.txt` показывает это в первой строке: `dsp: open` — хорошо; `dsp: NOT open (ndspInit 0xD880A7FA ...)` — файла нет.

Все команды ниже вводятся в терминале **Ubuntu (WSL)**, если не сказано иное. Команду можно скопировать целиком и вставить правой кнопкой мыши. Строки, которые начинаются с `#`, — пояснения, их вводить не нужно.

**Главное правило WSL.** Диски Windows в WSL лежат в `/mnt/`: диск `C:` — это `/mnt/c`, а `C:\Program Files (x86)\Steam` — это `"/mnt/c/Program Files (x86)/Steam"`. Обратные слэши `\` и буквы дисков (`C:`) в командах WSL не работают. Пути с пробелами всегда берите в кавычки.

---

## Шаг 0. Что понадобится

- Windows 10 (2004+) или Windows 11.
- Установленная **Gothic 1** (Steam-версия) — для неё нужен путь к папке игры.
- Консоль **New Nintendo 3DS** с Homebrew Launcher и выгруженная утилитой **DSP1** прошивка DSP в `sdmc:/3ds/dspfirm.cdc` (см. предупреждение выше; один раз на консоль).
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

Проверьте все четыре команды: каждая должна напечатать версию, а не «command not found»:
```
cmake --version
ninja --version
g++ --version
unzip -v | head -1
```
`cmake` — 3.16 или выше, `g++` — GCC 11 или новее (нужна поддержка C++20). **Если нет `ninja` или `unzip`, шаги 3 и 4 не пройдут**: выполните строку `apt install` выше ещё раз.

## Шаг 3. Проект

Проект надо держать **внутри WSL** (в домашней папке `~`), а не на диске `C:`: на `/mnt/c` всё работает в разы медленнее.

1. Скачайте **самый свежий** `gothic3ds-base.zip` и оставьте в Windows ровно одну его копию, например в `C:\Users\ВАШ_ПОЛЬЗОВАТЕЛЬ\Downloads` (старые копии удалите: браузер добавляет к именам `(1)`, `(2)` ..., и старый архив легко распаковать по ошибке).
2. Найдите его и проверьте, что он правильный (`ВАШ_ПОЛЬЗОВАТЕЛЬ` — имя вашего пользователя Windows, папка в `C:\Users`):
   ```
   cd ~
   ZIP=$(ls -t /mnt/c/Users/ВАШ_ПОЛЬЗОВАТЕЛЬ/Downloads/gothic3ds-base*.zip | head -1)
   echo "$ZIP"
   unzip -p "$ZIP" gothic3ds/tools/bake/import_zen.cpp | grep -c importZenSfxScript
   ```
   Последняя команда должна напечатать `1` или больше. Если `0`, это старый архив: удалите его и скачайте самый свежий.
3. Распакуйте в домашнюю папку WSL **в пустое место** (сначала уберите старый проект, чтобы старые и новые файлы не смешались):
   ```
   cd ~
   [ -d gothic3ds ] && mv gothic3ds gothic3ds_old_$(date +%s)
   unzip -q "$ZIP"
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
strings build/g3ds-bake | grep -c "script/sfx"
```

Последняя строка должна напечатать число больше 0: это доказывает, что собран именно новый конвертер. (Если `ninja` не установлен, cmake сразу остановится с «unable to find a build program corresponding to Ninja»: поставьте его `sudo apt install -y ninja-build`, удалите папку `build` и повторите команды.)

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

## Шаг 6. Запекание данных игры (`data.pak`, `music.pak`, `speech.pak`)

Конвертер читает файлы игры и делает архив для 3DS. Сама игра остаётся нетронутой.

```
cd ~/gothic3ds
mkdir -p ~/out
./build/g3ds-bake world --game "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic" -o ~/out/data.pak --music-dir ~/music_wavs
```

Полезно знать:
- Конвертер работает несколько минут и печатает отчёт. В нём должны быть строки `script/sfx: SFX.DAT`, `sounds: N C_SFX instances ... M baked` (M заметно больше нуля), `ambient sounds: ...`, `script/music: MUSIC.DAT` и `music: N C_MUSICTHEME instances ... K baked` (K больше нуля). Если где-то стоит 0: звуки и темы игры лежат в `SFX.DAT` и `MUSIC.DAT` в `_work\Data\Scripts\_compiled`; если их нет, в отчёте будет `sounds: SFX.DAT not found` / `music: MUSIC.DAT not found`. Если строки `music:` нет совсем, проверьте, что `--music-dir` указывает на папку с WAV из шага 5. Быстрая проверка: `grep -E "script/(sfx|music)|sounds:|music:" ~/out/bake.log`.
- Рядом с `data.pak` появятся `music.pak` и `speech.pak`: нужны все три файла (в `speech.pak` озвучка реплик; без него игра покажет только субтитры).
- Голоса берутся из `Speech.vdf`/`Sounds.vdf` игры (или из отдельных файлов в `_work\Data\Sound\Speech`). В отчёте будет строка `voices: N lines of dialogue, M recordings baked (... MB ...)`; M должно быть в тысячах. `speech.pak` большой (сотни МБ; это IMA ADPCM, 11,025 кГц). Меньше и хуже качеством: `--voice-rate 8000`; совсем без голосов: `--no-voices`. Если M равно 0 — в вашей копии игры нет архива озвучки (в некоторых локализациях он лежит в другом месте); пришлите строку `voices:` разработчику.
- Папка `~/out` создаётся командой `mkdir -p ~/out`; без неё конвертер напишет ошибку.
- Отчёт можно напечатать ещё раз: `./build/g3ds-bake info ~/out/data.pak`.
- Качество звуков задаётся флагом `--sound-rate 22050`, качество музыки — `--music-rate 32000`, качество голосов — `--voice-rate 11025`.
- **Данные игры в архив проекта не кладите** и никуда не выкладывайте `data.pak`, `music.pak` и `speech.pak`: это материалы игры.
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
cp ~/out/music.pak ~/out/speech.pak /mnt/c/gothic3ds_sd/3ds/gothic3ds/
```

Затем в Windows скопируйте папку `C:\gothic3ds_sd\3ds\gothic3ds` на SD-карту в `3ds\gothic3ds`. Итог на карте:

```
/3ds/gothic3ds/gothic3ds.3dsx
/3ds/gothic3ds/gothic3ds.smdh
/3ds/gothic3ds/data.pak
/3ds/gothic3ds/music.pak
/3ds/gothic3ds/speech.pak
/3ds/dspfirm.cdc          <- НЕ в gothic3ds, а прямо в папке 3ds
```

**Не забудьте `/3ds/dspfirm.cdc`** (рамка в начале): прошивка DSP, выгруженная утилитой DSP1 с вашей консоли. Без неё звука нет совсем. Меню звука в игре: **Громкость** (эффекты), **Музыка**, **Голоса**.

Запуск: Homebrew Launcher → Gothic 3DS.

Если что-то не так со звуком, после выхода из игры появится отчёт `/3ds/gothic3ds/sound.txt`: там перечислено, какие звуки и темы игра искала и не нашла. Пришлите его разработчику.

---

## Если что-то не получается

| Что видите | Причина и решение |
|---|---|
| `unable to find a build program corresponding to "Ninja"` / `CMAKE_CXX_COMPILER not set` | Не установлен `ninja` (или компилятор): `sudo apt install -y ninja-build build-essential cmake`, затем `rm -rf build` и повторите шаг 4 |
| `unzip: command not found` | `sudo apt install -y unzip` |
| `strings build/g3ds-bake \| grep -c "script/sfx"` печатает 0 или в отчёте `0 C_SFX instances` | Старый конвертер или архив: в папке проекта старые файлы. Повторите шаг 3 с самым свежим архивом (проверьте его командой `unzip -p ... \| grep -c importZenSfxScript`), затем `rm -rf build` и шаг 4 |
| `cannot find or open .../ВАШ_ПОЛЬЗОВАТЕЛЬ/Downloads/...zip` | `ВАШ_ПОЛЬЗОВАТЕЛЬ` нужно заменить на настоящее имя пользователя Windows; посмотреть: `ls /mnt/c/Users` |
| `cmake: command not found` | Не выполнен шаг 2: `sudo apt install -y build-essential cmake ninja-build git` |
| `The source directory ".../tools/dmrender" does not exist` | Вы не в папке проекта (`cd ~/gothic3ds`) или проект распакован не туда |
| `Parse error in command line argument: DMUSIC_DIR` | Команда оборвана: после `-DDMUSIC_DIR=` должен стоять путь. В шаге 5 этот параметр не нужен вовсе |
| В `tools/dmrender` лежит много лишних файлов (`src`, `include`, `vendor`) | В эту папку попала библиотека dmusic. Должны быть только `CMakeLists.txt` и `dmrender.c`; распакуйте проект заново |
| `build-dmrender/dmrender: No such file or directory` | Сборка не дошла до конца: выполните `cmake --build build-dmrender` и прочитайте последние строки с `error` |
| `usage: dmrender MUSIC_DIR OUT_DIR ...` | Не указаны оба пути: откуда брать музыку и куда класть WAV |
| `No such file or directory` для пути к игре | Путь неверен: проверьте `ls "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic"`; буквы дисков `C:` и `\` не работают |
| `Please set DEVKITARM in your environment` | Не выполнен пункт 3 шага 7; выполните `source ~/.bashrc` |
| Конвертер пишет `cannot open` / не находит `WORLD.ZEN` | Указана не папка игры. В ней должны лежать `Data` и `_work` |
| Игра на 3DS молчит (ни эффектов, ни музыки, ни голосов) | Нет `/3ds/dspfirm.cdc` на SD-карте: запустите DSP1 на консоли. `sound.txt` тогда начинается с `dsp: NOT open (ndspInit 0xD880A7FA ...)` |
| Звуки и музыка есть, никто не говорит | `speech.pak` не лежит рядом с `data.pak`, ползунок **Голоса** на 0, или при запекании не нашлось записей (строка `voices:` отчёта и строка `voices:` в `sound.txt`) |
| Звук есть, музыки нет | Нет `music.pak` рядом с `data.pak`, или имена тем в `sound.txt` не нашлись; пришлите отчёт |
| `wsl: Конфигурация прокси-сервера localhost обнаружена...` | Предупреждение, на сборку не влияет |
| Очень медленно | Проект лежит на `/mnt/c`. Перенесите в `~` (шаг 3) |

## Обновление проекта

Когда пришёл новый `gothic3ds-base.zip`: повторите шаг 3 (найдите и проверьте самый свежий архив, уберите старый проект, распакуйте), затем шаги 4, 6, 7 и 8. Проект пересобирается с нуля (несколько минут). WAV в `~/music_wavs` и результаты в `~/out` лежат вне проекта и остаются, поэтому шаг 5 нужен, только если хотите заново отрендерить музыку.
