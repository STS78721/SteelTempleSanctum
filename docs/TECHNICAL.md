# SteelTemple Sanctum: how it works

The details behind the [front page](../README.md): controls, downloads, how each port is started, game settings,
the search of this PC, where Sanctum keeps its files, and building it.

## Using it

| | Pad | Keyboard | Mouse |
|---|---|---|---|
| Move | D-pad / left stick (held: repeats) | arrows / WASD | hover |
| Confirm | A (Cross) | Enter / Space | left click |
| Back | B (Circle) | Esc / Backspace | right click |
| Options / GitHub | X / Y | X / Tab | |
| Tabs | L / R | Q / E | click a tab |
| Check for ports (this PC and GitHub) | + (Menu, Options) | F5 | |
| Home from anywhere | Home (Guide) | Home | |
| Fullscreen (on by default) | System Settings > Display | F11, Alt+Enter | |

As in the ports' F1 menus, **System Settings > Controllers** sets the **Button layout**: Automatic (A confirms where
each pad prints it: right on Nintendo pads, at the bottom on others), Nintendo (A right, Switch style, on any pad) or
Xbox (A bottom); and the **Button icons** the hints show: Automatic (the pad's own), Nintendo, Xbox or PlayStation.
The hints always show the button that does it, where it sits, in the chosen icons.

## Cover art

Sanctum ships no game art. Pick your own folder of covers in **System Settings > Cover art** (the picker works with a
controller); files are matched to ports by name (`Mario Kart World.png`, `The Legend of Zelda꞉ Ocarina of Time.png`,
...). A port without a cover shows its colours and monogram.

## Getting and updating ports

**Get ports** lists every released port. Tick the ones you want (A), choose Standalone or Setup Wizard where a release
has both (X), and Download (+). **Downloads** shows each job's steps while you keep browsing:

1. *Download* from the port's GitHub release into `%LOCALAPPDATA%\SteelTemple\Sanctum\downloads` (resumes if stopped)
2. *Verify* against the SHA-256 GitHub publishes for the file
3. *Install*: a Standalone zip is unpacked with Windows' own `tar` and moved into the install folder
   (`<home>\Games\SteelTemple\<port>` unless System Settings > Ports on this PC says otherwise); an **Update** zip is
   laid over the copy in use: each replaced file becomes `*.old` first and is put back if anything fails; a Setup
   Wizard (an `.exe`, or a zip holding one, like WonderBloom's) runs as the port's own setup, then this PC is searched for it
4. *Ready*: the copy joins the port's page. Ports that build their game on your PC (ShadowRacket, SlowSeparation,
   ...) ship only their launcher: the page then says **Set up**, which opens the launcher for the first start with
   your own game files; after that, **Play** starts the game directly.

Release lists are cached with their ETag, so starting Sanctum costs nothing against GitHub's hourly limit when
nothing changed. No account or token is used, and nothing about the player is sent.

## Playing

**Play** starts the game itself, the way the port's own launcher would, and never its start screen
(`src/launch.cpp`, `kProfiles`):

| Port | Sanctum runs | Ready when |
|---|---|---|
| THWOMP | `mkw.exe --play` (`MKW_NO_LAUNCHER=1`) | `mkwgame.dll` is built |
| MasterSword | `mastersword.exe <dir>\image.bin --romfs <dir>\romfs --user <dir>\user` | `image.bin` and `romfs\` are beside it |
| TotK | `totk.exe` | `exefs\` and `romfs\` are beside it |
| SlowSeparation / ShadowRacket | `ff.exe` / `mtf.exe` (`NS2_SHOW_LAUNCHER=0`) | the game is built |
| WonderBloom | `smbw.exe` (`NS2_SHOW_LAUNCHER=0`) | the game is built |
| DK BANANAS | `dkb.exe` (`NS2_SHOW_LAUNCHER=0`, `DKB_GPU_NAME`) | `exefs\main` and `romfs\romfs.bin` are there |

Until a port is ready, the button says **Set up** and opens the port's own launcher for the first setup. While a
game runs, Sanctum minimises and sleeps; its tile says *Playing*, and its page offers **Close game** (press twice).
When it ends Sanctum comes back on its page with how the run went: THWOMP's own checker (`lastrun.py`, its known
errors) when present, else the exit code and the last error in the game's log, with **Open the log** and
**Report it** when it went wrong. Time played is kept per port (`playtime.ini`), and Home lists the ports played
last first, like the console.

## Game settings

A port's page has a **Settings** tab: its own in-game settings, the same pages, rows, words and values as the game's
menu, in the port's colours. A or left/right changes a value, Y puts it back to the game's default, L/R change page.
Sanctum writes the port's own ini beside its game (`mkw.ini`, `totk.ini`, `ff.ini`, `mtf.ini`, `dkb.ini`) the way the
game does: only the changed line, keeping comments, order and line endings, a new key under its section; the first
change of a visit saves `<ini>.sanctum-bak`. A setting the environment fixes is shown greyed; while the game runs the
page is read-only. MasterSword's settings stay in its own launcher (its menu is built differently).

The lists are `assets/settings/<port>.json`, read from each port's source by `tools/extract_settings.py`
(`cfg.c` `metas[]` and `sections[]`, the menu's `tab_*()` rows, the English text); each names the commit it came
from. The source paths are your own, in `tools/settings_sources.local.json` (never committed):

```sh
python tools/extract_settings.py            # every port
python tools/extract_settings.py totk       # one
```

## Ports on this PC

At start the list from the last search shows at once; a new search runs in the background: the Setup Wizards'
entries in Windows' apps list, then every fixed drive, six folders deep, for each port's program (`mkw.exe` with
`mkwgame.id`, `mastersword.exe`, `totk.exe`, `ff.exe`, `mtf.exe`, `dkb.exe`, `smbw.exe`, or the port's own launcher alone), plus Sanctum's
own install folder. A copy found before and still there is never dropped. Each copy's version is read from its
game program, which carries its release tag (`0.2.2c`, `Alpha 0.1.3b`, `Beta 6.3`): truer than a Setup Wizard's
registry entry or a VERSION file, which go stale when a copy is updated another way. Read once per program on a
worker, the copies in use first, and kept in `versions.ini`. Every copy is kept; the best (Setup
Wizard, then one with its launcher, then the newest version) is the one Play starts, and the game page lets you
pick another, which is then remembered ("Use this copy"). Copies inside build, staging or backup folders are shown
too, ranked last; **System Settings > Ports on this PC** can leave them out.

## Sanctum's own updates

Sanctum checks its own releases (`STS78721/SteelTempleSanctum`) at start, at most every 15 minutes, with the saved
ETag. With **System Settings > About > Update Sanctum automatically** on (the default), a newer release's
`SteelTemple-Sanctum-<version>-Portable.zip` downloads in the background, is checked against the SHA-256 GitHub
publishes, and is unpacked to `%LOCALAPPDATA%\SteelTemple\Sanctum\sanctum-update`. Home says when it is ready. It
is put in place when Sanctum closes, or straight away with **Updates > restart and update**. Windows lets a running
program be renamed, so every replaced file is first renamed `*.old` and put back if anything fails. The old program's
`.old` is removed at the next start. Only Sanctum's own files are replaced. With automatic updates off, About shows
the new version and A downloads it. Drafts and pre-releases are never offered.

Releasing: set `SANCTUM_VERSION` in `src/selfupdate.h` and the version in `assets/sanctum.rc` and `.manifest`, build,
then `python tools/package.py` and attach the zip to a release tagged with that version. `SANCTUM_SELF_VERSION=1.0.0`
makes a build think it is older, to test the update; `SANCTUM_NO_UPDATE=1` turns the check off.

## Where things are

| | |
|---|---|
| `%APPDATA%\SteelTemple\Sanctum\settings.ini` | the player's settings (theme, cover folder, profile icon, ...) |
| `%APPDATA%\SteelTemple\Sanctum\library.ini` | the ports found by the last search |
| `%APPDATA%\SteelTemple\Sanctum\playtime.ini` | time played from Sanctum, per port |
| `%LOCALAPPDATA%\SteelTemple\Sanctum\releases\` | each port's GitHub release list and its ETag |
| `%LOCALAPPDATA%\SteelTemple\Sanctum\downloads\` | packages while downloading (removed once installed) |

Nothing personal is ever written next to the program or packaged.

## Building

Needs llvm-mingw and a static SDL3 3.4 (`-DSDL_STATIC=ON`, a prefix with `include/SDL3` and `lib/libSDL3.a`).

```sh
python tools/build.py --llvm-mingw <llvm-mingw folder> --sdl <SDL3 static prefix>
```

`build/SteelTemple-Sanctum.exe` (about 6 MB, no DLLs) and `build/assets/`.

Test hooks: `SANCTUM_NAV="R R A"` (presses from frame 30, one per 8 frames), `SANCTUM_SHOT=<file.bmp>` with
`SANCTUM_SHOT_FRAME=<n>` (saves that frame and quits; with `SANCTUM_SHOT_IDLE=1` the first frame from then on with
the downloads finished), `SANCTUM_THEME=light`, `SANCTUM_WINDOWED=1`, `SANCTUM_LIBRARY=<dir>` (install there this run), `SANCTUM_FOCUS=<port id>` (start on that port: tests never
count tiles, since the row's order follows what was played last), `SANCTUM_LOG=<file>` (a debug log: the version
reads, ...), `SANCTUM_DEMO=1` (screenshot mode: stand-in folders, no cover art, no screen name, nothing saved,
no game started; the README screenshots are taken this way). `tools/build.py --name <name>` builds a copy under another name while Sanctum is running.

| File | What it is |
|---|---|
| `src/main.cpp` | window, frame loop, screens, test hooks |
| `src/app.h` | the shared state, screen list, canvas helpers |
| `src/input.cpp` | pads, keyboard and mouse into one Nav per frame, with console-style repeat |
| `src/draw.cpp` | text, shapes, button glyphs per pad, icons, tiles, focus ring, status and hint bars |
| `src/home.cpp` | Home: profile, the row of ports, the round buttons, Power |
| `src/game.cpp` | a port's page: Play / Download, its launcher, GitHub, tabs, the copies on this PC |
| `src/settings.cpp` | System Settings, the folder picker, Controllers, pages to come |
| `src/ports.cpp` | Get ports, Downloads, Releases & news |
| `src/install.cpp` | the downloads queue: download, verify, unpack, install, update, Setup Wizards |
| `src/github.cpp` | each port's releases from GitHub, cached with ETags |
| `src/net.cpp` | HTTPS (WinHTTP), resumable downloads, SHA-256 (BCrypt) |
| `src/json.cpp` | a small JSON reader |
| `src/profile.cpp` | Profile: the icon picker (by set) and the background colour |
| `src/avatars.cpp` | the profile icons: the atlas, the 16 colours, drawing an icon on its colour |
| `src/selfupdate.cpp` | Sanctum's own updates: check its releases, download and verify, put in place on close or restart |
| `src/catalog.cpp` | the ports, the search of this PC, library.ini |
| `src/covers.cpp` | cover art from the player's folder, decoded on a worker thread |
| `src/launch.cpp` | starting a port and coming back when it ends |
| `src/port_settings.cpp` | a port's own settings page, and the ini editor that keeps the file as it is |
| `tools/package.py` | the release zip, `dist/SteelTemple-Sanctum-<version>-Portable.zip` (version from `src/selfupdate.h`) |
| `tools/extract_settings.py` | each port's settings list from its source tree, into `assets/settings/` |
| `src/theme.h` | Dark and Light (the ports' universal themes) |
| `third_party/` | Dear ImGui 1.91.9 (MIT), stb_image (public domain / MIT) |
| `assets/fonts/` | Roboto Medium (Apache 2.0) |
| `assets/avatars.png`, `avatars.txt` | the 60 profile icons, drawn by `tools/make_avatars.py` from shapes (original art; `--sheet <png>` draws them all on sample colours) |
| `assets/sanctum.ico`, `icon.png` | Sanctum's icon, drawn by `tools/make_icon.py` (no game art) |
| `assets/sanctum.rc`, `.manifest` | the exe's icon, version and manifest (per-monitor DPI, UTF-8) |
