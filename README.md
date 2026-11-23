<div align="center">

<img src="assets/icon.png" width="128" alt="SteelTemple Sanctum icon">

# SteelTemple Sanctum

**One home for every SteelTemple PC port.**<br>
A console-style launcher for Windows that you can drive entirely with a controller: browse your ports, download them,
keep them up to date, change their settings, and start any game straight from the couch.

[![Release](https://img.shields.io/github/v/release/STS78721/SteelTempleSanctum?include_prereleases&label=release&color=e8243a)](https://github.com/STS78721/SteelTempleSanctum/releases)
![Windows 10/11](https://img.shields.io/badge/Windows-10%20%2F%2011-0078d4)
![Controller first](https://img.shields.io/badge/controller-first-2bb3c0)
![No game files](https://img.shields.io/badge/game%20files-never%20included-3a3a3a)

[**Download**](https://github.com/STS78721/SteelTempleSanctum/releases) ·
[Features](#features) · [Supported ports](#supported-ports) · [Controls](#controls) · [How it works](docs/TECHNICAL.md)

<img src="docs/screenshots/home.png" width="900" alt="Sanctum's Home screen: a row of port tiles and round buttons below">

</div>

---

## Features

### 🏠 A home screen that feels like a console

Your ports sit in one row — sorted by **Last played**, **Most played**, or **A to Z**, with **Pin to front** and a
**Quick menu (`X`)** for one-press actions — each in its own colours. The round buttons below open **Releases & news**,
**Get ports** (with an available-updates badge), **Downloads**, **Album**, **Mods**, **Controllers**, **System
Settings** and **Power**. Sanctum opens fullscreen on your chosen monitor or TV with smooth transitions, hides the mouse
pointer while you use a controller, and shows live **handheld/laptop and wireless controller battery indicators** in the
status bar. Dark and Light themes, **12h/24h clock**, and **OLED Idle Dim** protection are built in.

<div align="center">
<img src="docs/screenshots/home-light.png" width="800" alt="Home in the Light theme">
</div>

### 🎮 Press Play and you're in the game

**Play** starts the game itself and skips the port's own launcher. Sanctum steps aside while you play and comes back
when you quit, on the game's page with a note on how the run went. If something went wrong, the log and the issue
report are one button away — and if a game ever freezes, hold **Back + Start + L + R** for 2 seconds to force-close it
safely from the couch. Each port page shows a **TIME PLAYED** card tracked per profile, and you can also add **custom PC
games** or set per-game **custom launch arguments and environment overrides**.

<div align="center">
<img src="docs/screenshots/game.png" width="800" alt="A port's page: Play, version, TIME PLAYED card, and the copy that will start">
</div>

### 🔎 Finds the ports you already have

At start, Sanctum looks through the Setup Wizards' entries and every drive for ports already on this PC, so nothing
is installed twice. It reads each copy's real version from its game program, companion renderer DLLs, and `VERSION`
files (picking the newest component so neither stale `VERSION` files nor launcher/renderer hotfixes misreport updates).
If you keep a port in an unusual location — or want to launch any other PC `.exe` — add it in **System Settings > Ports
on this PC**. If you have more than one copy, you pick which one Play starts.

### ⬇️ Get ports, update them, read what's new

**Get ports** lists every released port: tick the ones you want and press Download, or use **Update all** to queue every
available port update in one click. Downloads run in the background while you keep browsing. Each one is checked against
the SHA-256 that GitHub publishes, and an update keeps a way back if it fails. Every port's release notes can be read on
its own page — and you can install or roll back to any older release straight from the release notes pane — while
**Releases & news** puts all of them in one list.

<table>
<tr>
<td><img src="docs/screenshots/getports.png" alt="Get ports: every released port, ready to tick and download"></td>
<td><img src="docs/screenshots/news.png" alt="Releases and news across every port"></td>
</tr>
<tr>
<td colspan="2"><img src="docs/screenshots/notes.png" alt="A port's release notes, read with a controller"></td>
</tr>
</table>

### ⚙️ Each game's own settings, from the couch

A port's **Settings** tab shows the same pages, options and values as the game's own in-game menu, in the port's
colours — across **all 7 ports** (MKW-THWOMP, MasterSword, TotK SteelTempleBuild, SlowSeparation, ShadowRacket,
dkb-BANANAS, and WonderBloom). Changes go straight into the game's settings file, keeping its comments and layout
intact. **Y** puts any option back to the game's default.

<table>
<tr>
<td><img src="docs/screenshots/game-settings.png" alt="A port's Settings tab"></td>
<td><img src="docs/screenshots/port-settings.png" alt="A port's own settings pages inside Sanctum"></td>
</tr>
</table>

### 💾 Storage overview, save backups & mod manager

- **System Settings > Storage** shows every mounted drive's **total capacity, free space, used space, and SteelTemple
  library footprint**, plus one-click download cache cleanup and direct jumps into each title's storage page.
- Each port's **Files & storage** tab lets you create timestamped **ZIP save-file backups** (pure C++, zero external
  tools) and safely restore them with a 2-press guard and automatic pre-restore snapshot, verify required files, open
  logs, or uninstall to the Windows Recycle Bin.
- Open **Mods** from the Home dock or a port's **Mods** tab — or drag and drop any `.zip` mod straight onto Sanctum — to
  install and toggle mod folders safely without deleting files.

<table>
<tr>
<td colspan="2"><img src="docs/screenshots/storage.png" alt="System Settings, Storage: drive capacities, free space, and per-title library sizes"></td>
</tr>
<tr>
<td><img src="docs/screenshots/mods.png" alt="Mods manager: toggle mod folders per port"></td>
<td><img src="docs/screenshots/files.png" alt="Files and storage tab: disk size, verify, save backup, and safe uninstall"></td>
</tr>
</table>

### 🕹️ Any controller, your button layout

Xbox, PlayStation, Switch Pro, Joy-Con and generic pads all work, and the on-screen hints show your pad's own
buttons. **System Settings > Controllers** lets you choose the **Button layout** (Automatic, Nintendo with A on the
right, or Xbox with A at the bottom), **Button icons**, **Controller vibration** and strength (**Low**, **Medium**,
**High**), **Menu stick deadzone**, the **Force-close chord**, and a live dual-stick, analog trigger, button & battery
**Controller Tester**. Keyboard and mouse work everywhere too.

<div align="center">
<img src="docs/screenshots/controllers.png" width="800" alt="System Settings, Controllers: layout, icons, vibration, deadzone, force-close chord, and tester">
</div>

### 🔊 Console menu sounds & ambient audio

Clean, tactile Switch 2–style UI sounds play as you navigate, toggle settings, type on the on-screen keyboard, or start
a game, and pause automatically while a game runs. **System Settings > Sound** offers 3 built-in **Sound profiles**
(**Switch 2 Crisp**, **Warm Retro**, **Soft Minimal**), **Volume** control, and optional **Home ambient audio** (a soft
console hum or your own custom `.wav` track).

<div align="center">
<img src="docs/screenshots/sound.png" width="800" alt="System Settings, Sound: sound profiles, volume, and Home ambient audio">
</div>

### 👤 Profiles, Album & your cover art

- **Profiles:** Create up to four local profiles, each with its own name, background colour, per-profile play time, and
  icon from 60 original designs in six sets (SteelTemple, Creatures, Food, Nature, Space and Play).
- **Album:** Browse screenshots from your ports and Windows Screenshots folder in a Switch-style grid with a full-screen
  lightbox and safe Recycle Bin delete.
- **Cover art:** Sanctum ships no game art. Point **System Settings > Cover art** at your own folder, pick a custom
  cover per title, or drag and drop an image straight onto a game tile.

<table>
<tr>
<td><img src="docs/screenshots/profile.png" alt="Profile: choosing an icon from the Creatures set"></td>
<td><img src="docs/screenshots/profile-name.png" alt="Naming the profile with the on-screen keyboard"></td>
</tr>
</table>

### 🛠️ System diagnostics & optional Discord Rich Presence

**System Settings > About** displays a quick hardware/OS diagnostics summary with one-click **Copy support info** for
bug reports, manages automatic background self-updates (with a one-time **"What's New"** card after each update), and
includes optional, off-by-default **Discord Rich Presence**.

<div align="center">
<img src="docs/screenshots/about.png" width="800" alt="System Settings, About Sanctum: version, updates, Discord RPC, shortcuts, and diagnostics">
</div>

---

## Supported ports

| Game | Port | Repository |
|---|---|---|
| Mario Kart World | **MKW-THWOMP** | [STS78721/MKW-THWOMP](https://github.com/STS78721/MKW-THWOMP) |
| The Legend of Zelda: Ocarina of Time | **MasterSword** | [STS78721/OOT-MASTERSWORD](https://github.com/STS78721/OOT-MASTERSWORD) |
| The Legend of Zelda: Tears of the Kingdom | **TotK SteelTempleBuild** | [STS78721/totk-pc-recomp-SteelTempleBuild](https://github.com/STS78721/totk-pc-recomp-SteelTempleBuild) |
| Fast Fusion | **SlowSeparation** | [STS78721/SlowSeparation](https://github.com/STS78721/SlowSeparation) |
| Mario Tennis Fever | **ShadowRacket** | [STS78721/MTF-ShadowRacket](https://github.com/STS78721/MTF-ShadowRacket) |
| Donkey Kong Bananza | **dkb-BANANAS** | [STS78721/dkb-BANANAS](https://github.com/STS78721/dkb-BANANAS) |
| Super Mario Bros. Wonder | **WonderBloom** | [STS78721/SMBWONDERBLOOM](https://github.com/STS78721/SMBWONDERBLOOM) |

New releases of these ports show up in Sanctum by themselves. Press **+** (Menu / Options) to check right away.

## Getting started

1. Download the latest Sanctum from [**Releases**](https://github.com/STS78721/SteelTempleSanctum/releases) and
   unzip it anywhere.
2. Start `SteelTemple-Sanctum.exe`. Ports you already have appear on Home within a few seconds.
   From then on Sanctum updates itself: a new version downloads in the background and goes in when you close it.
3. Open **Get ports** to download more. A port that builds its game from your files asks for them on its first
   start, through **Set up** on its page.
4. Optional: in **System Settings > About**, add a Desktop or Start Menu shortcut, or turn on **Start with Windows**.

> [!IMPORTANT]
> Sanctum and the ports never include or download game files. Every port runs from **your own** legally dumped copy
> of the game.

## Controls

| | Controller | Keyboard | Mouse |
|---|---|---|---|
| Move | D-pad / left stick | Arrows / WASD | Hover |
| Confirm | A | Enter / Space | Left click |
| Back | B | Esc / Backspace | Right click |
| Options / GitHub | X / Y | X / Tab | |
| Switch tabs or pages | L / R | Q / E | Click a tab |
| Check for ports | + (Menu / Options) | F5 | |
| Back to Home | Home (Guide) | Home | |
| Fullscreen | System Settings > Display | F11 or Alt+Enter | |

## Your privacy

- No account, sign-in or token. Sanctum only reads the public release lists from GitHub, and caches them so it
  rarely needs to ask.
- Nothing about you or your PC is sent anywhere.
- Your settings, play time and cover-art choice stay in your own Windows profile. Nothing is written into the ports'
  downloads or releases.

## Building from source

Sanctum is C++20 with SDL3 and Dear ImGui. It builds to a single exe of about 6 MB with no DLLs to install.

```sh
python tools/build.py --llvm-mingw <llvm-mingw folder> --sdl <SDL3 static prefix>
```

[docs/TECHNICAL.md](docs/TECHNICAL.md) covers how ports are found, downloaded, started and configured, where Sanctum
keeps its files, the test hooks, and a map of the source.

## Credits

Built by **SteelTemple**. It uses [SDL3](https://libsdl.org) (zlib),
[Dear ImGui](https://github.com/ocornut/imgui) (MIT), [stb_image](https://github.com/nothings/stb) (public domain /
MIT) and [Roboto](https://fonts.google.com/specimen/Roboto) (Apache 2.0).

<sub>SteelTemple Sanctum is an unofficial fan project. It is not affiliated with or endorsed by Nintendo. All game
titles are trademarks of their respective owners.</sub>
