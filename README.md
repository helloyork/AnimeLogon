<img src="res/readme/banner.png" alt="AnimeLogon">

# AnimeLogon

[![Build](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml/badge.svg?branch=develop)](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](LICENSE)

AnimeLogon replaces the Windows 10/11 lock screen with an overlay it draws itself, so you
can place videos, widgets and other content on it.

[中文说明](README.zh-CN.md)

## Planned features

- [x] Full-screen video on the lock screen and on the sign-in screen at startup
- [x] Custom clock styles
- [x] Widgets
- [ ] Scene wallpapers (HLSL and Lottie)

## Requirements

- x64 PCs: Windows 10 or Windows 11
- ARM64 PCs: Windows 11
- Administrator rights for installation
- The Microsoft Edge WebView2 runtime for the AnimeLogon window. Windows 11 includes it;
  on Windows 10 the installer says so if it is missing.

Windows N and KN editions are not supported.

## Installation

> The programs are not code-signed.

1. Download the setup program, `AnimeLogon-<version>-setup.exe`, from [Releases](https://github.com/helloyork/AnimeLogon/releases). The same setup program is used on x64 and ARM64 PCs; it installs the version for your PC's processor.
2. Run it and follow the wizard. Its first page lets you choose the language. If Windows says "Windows protected your PC", select **More info > Run anyway**.
3. Open AnimeLogon from the Start menu and select **Import…** to add a video or picture.

For silent installation and uninstallation, see [Command line](#command-line).

## Usage

Lock the computer (<kbd>Win</kbd>+<kbd>L</kbd>) or restart it. The lock screen shows the theme in use
until you press a key or click, and the Windows sign-in screen appears.

To change it, click a theme in the theme library. Videos, pictures and theme packages
(.altheme) are added with **Import…**, and an imported theme is put in use at once. Select
**Customize** on a theme to change its wallpaper and widgets. Themes can be exported as .altheme
files to share.

Choose the display language in **Settings > General > Display language**: 简体中文, 繁體中文, English or
日本語. It follows Windows unless you choose one.

AnimeLogon shows an icon in the system tray. Click it to open the settings; right-click it to
switch between recently used themes. Under **System tray** in the settings, the icon can be hidden
and the number of themes in its menu changed.

## Changes to your system

- **The Windows lock screen is turned off.** This is required for the video to be
  visible. Windows Spotlight and lock screen widgets are unavailable while AnimeLogon is
  installed.
- **The sign-in screen background is set by AnimeLogon.** The lock screen picture can no
  longer be changed in Windows Settings.
- **Imported videos, pictures and themes are stored on drive C:.** The location cannot be changed.
- **The system tray icon runs at sign-in.** The installer adds a startup entry for every account
  and uninstalling removes it. It can also be disabled under **Startup apps** in Task Manager.
- **A Windows service named AnimeLogon is installed.** It runs as the local system account, starts
  with Windows and shows the lock screen. Turning off **Enable AnimeLogon** stops it and sets it to
  start manually; uninstalling removes it.

The installer describes the first two changes and asks before it makes them. Turning off
**Enable AnimeLogon** in the settings, or uninstalling AnimeLogon, restores these settings.

## Uninstallation

Open **Settings > Apps > Installed apps**, find AnimeLogon and select **Uninstall**, or select **Uninstall** in AnimeLogon's Settings. You can choose whether to delete your themes, wallpapers and settings as well.

## Interfaces

These file formats, command lines and file locations are for theme authors, scripts and other
programs.

### File formats

| Format | Description |
|---|---|
| Component (`.xml`, root element `<component>`) | What a theme draws over its wallpaper: clocks, dates, battery, now playing, countdowns and audio spectra. See [Component format](skills/animelogon-components/reference/component-format.md) and [Component attributes](skills/animelogon-components/reference/attributes.md). |
| Theme (`theme.xml`) and theme package (`.altheme`) | A theme is a wallpaper and up to 8 components. A theme package is an uncompressed zip file that carries a theme with its wallpaper and component files. See [Themes and theme packages](skills/animelogon-components/reference/theme-package.md). |
| Scene wallpaper (`.alscene`) | Not imported by the current version. See [tools/SCENES.md](tools/SCENES.md) (Chinese). |

Each format is at version `format="1"`. AnimeLogon refuses elements and attributes it does not
recognize, so a file that uses something added in a later version cannot be imported by an
earlier one.

### Writing components

[`skills/animelogon-components`](skills/animelogon-components) is an Agent Skill for AI coding
assistants such as Claude Code. Copy the folder into the assistant's skills folder (for Claude
Code, `%USERPROFILE%\.claude\skills\`) and the assistant can write, check and package components
in the format. The folder is also a reference for writing components by hand.

A build from source includes two developer tools: `component_check` checks components and theme
packages with AnimeLogon's own reader and packs `.altheme` files, and `skin_preview` draws a
component over a picture.

To try components without changing what is installed, use `animelogon.exe --sandbox <folder>`
(below). Imports there need no administrator approval.

### Command line

**Setup program** (`AnimeLogon-<version>-setup.exe`; `install.exe` in a build):

| Argument | Description |
|---|---|
| `--quiet` | No wizard; the result is written to the console. In a script, wait for it with `start /wait`. |
| `--lang <language>` | The language: `zh-Hans`, `zh-Hant`, `en` or `ja`. |
| `--dir <path>` | Installs to `<path>`. Without it, a new installation goes to `C:\Program Files\AnimeLogon` and an update to where AnimeLogon is installed. |
| `--setup-in-app` | With `--quiet`: installs without turning AnimeLogon on. A wizard turns it on the first time AnimeLogon is opened. |

Exit codes: `0` installed, `1` failed, `1223` cancelled.

**Uninstaller** (`uninstall.exe` in the installation folder):

| Argument | Description |
|---|---|
| `--quiet` | No wizard; removes AnimeLogon and keeps themes, wallpapers and settings. |
| `--quiet --purge` | Also deletes themes, wallpapers and settings. |

Exit codes: `0` removed, `1` a change could not be undone, `1223` cancelled.

**AnimeLogon** (`animelogon.exe` in the installation folder):

| Argument | Description |
|---|---|
| `--tray` | Shows the system tray icon instead of the window. The startup entry runs it this way at sign-in. |
| `--show switch`, `--show lock` | Opens the window, or brings it forward, at the **Enable AnimeLogon** switch or at the lock screen preview of the theme in use. |
| `--sandbox <folder>` | Opens a window that works on separate data (themes, components, settings) in `<folder>`, for testing. Imports there need no administrator approval, and the lock screen does not use them. |
| `--switch-on`, `--switch-off` | Turns AnimeLogon on or off, as the switch in the settings does. Requires administrator rights. Exit codes: `0` done, `1` failed, `4` not run as administrator. |

With arguments it does not recognize, `animelogon.exe` says so and exits with code `2`. The
`--commit-*` and `--remove-*` arguments, run with administrator rights, install or remove what
the window has prepared for import; see [config/src/commit.h](config/src/commit.h). The helper of
the Steam edition (planned), `animelogon-steam.exe`, speaks the protocol in
[steam/PROTOCOL.md](steam/PROTOCOL.md).

### File locations

| Location | Contents |
|---|---|
| `C:\Program Files\AnimeLogon\` | The programs, `uninstall.exe` and the third-party licences (`licenses\`). |
| `%ProgramData%\AnimeLogon\settings.ini` | Machine-wide settings: the theme in use, multiple displays, the clock, sound, and the changes made to each theme. One `key = value` per line, holding only ids, choices and typed values, never a path or free text. The keys are described in [settings.h](common/include/animelogon/settings.h). |
| `%ProgramData%\AnimeLogon\themes\`, `components\`, `wallpapers\` | Imported themes, components and wallpapers, and the default theme's wallpaper, which the installer writes. Only administrators can write there, and AnimeLogon writes them in normalized form. |
| `%ProgramData%\AnimeLogon\logs\` | Logs. |

## Building from source

Prerequisites:

- Visual Studio 2022 or later with the **Desktop development with C++** workload
  (add **MSVC ARM64 build tools** for ARM64)
- CMake 3.25 or later
- Node.js 20 or later, with npm, to bundle the settings app's page

```powershell
git clone https://github.com/helloyork/AnimeLogon.git
cd AnimeLogon
cmake --preset x64
cmake --build --preset x64-release
```

Output is written to `build\<preset>\bin\<configuration>\`. The setup program is `install.exe` there.

| Configure preset | Build presets |
|---|---|
| `x64` | `x64-debug`, `x64-release` |
| `arm64` | `arm64-debug`, `arm64-release` |

Options:

| Option | Description |
|---|---|
| `-DANIMELOGON_BUILD=<n>` | Build number written to the version resource. Default `0`. |
| `-DANIMELOGON_VERSION_SUFFIX=<suffix>` | Pre-release suffix added to the version, for example `-beta1`. Default empty. |
| `-DANIMELOGON_ARM64_PAYLOAD_DIR=<path>` | x64 builds only: the `bin\Release` directory of an ARM64 Release build. `install.exe` then includes the ARM64 version as well and installs on both kinds of PC. |
| `-DBUILD_TESTING=OFF` | Leaves out the tests and developer tools. Default `ON`. |
| `-DCMAKE_GENERATOR_INSTANCE=<path>` | Visual Studio installation to use when more than one is installed. |

To run the tests (x64):

```powershell
ctest --preset x64-release --output-on-failure
```

To make the setup program a release has, with both architectures, build ARM64 first and point the
x64 build at its output:

```powershell
cmake --preset arm64
cmake --build --preset arm64-release
cmake --preset x64 -DANIMELOGON_ARM64_PAYLOAD_DIR="$PWD\build\arm64\bin\Release"
cmake --build --preset x64-release
```

## Roadmap

Planned:

- Steam edition: browse, download and share themes through the Steam Workshop

## Contributing

- Open pull requests against `develop`. `master` is updated only through pull requests
  from `develop`.
- CI builds x64 and ARM64 for every pull request.
- Do not commit video or image files, or code under a licence incompatible with the MPL 2.0.
- Contributions are licensed under the MPL 2.0, as the rest of the code is.

## License

Mozilla Public License 2.0 © 2026 Nomen (helloyork). See [LICENSE](LICENSE). Releases up to
0.1.0-alpha were published under the MIT License.

The following notice applies to every file in this repository outside `vendor/`:

> This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a
> copy of the MPL was not distributed with this file, You can obtain one at
> https://mozilla.org/MPL/2.0/.

Includes the headers and loader of the Microsoft Edge WebView2 SDK (BSD 3-Clause) in
`vendor/webview2`. The loader, `WebView2Loader.dll`, is installed with AnimeLogon.

`vendor/thorvg` contains ThorVG 1.1.2 (MIT) and the licence for its embedded RapidJSON.
Only the sandboxed scene renderer links ThorVG. See [SOURCE.md](vendor/thorvg/SOURCE.md).

The settings app's page bundles npm packages, among them React and Fluent UI (MIT). The build
writes the name, version and licence text of each.

These third-party licences are installed in the `licenses` folder of the install directory, and
**Third-party notices** in AnimeLogon's Settings opens it.
