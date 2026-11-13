<img src="res/animelogon.svg" alt="" width="72">

# AnimeLogon

[![Build](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml/badge.svg?branch=develop)](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

AnimeLogon replaces the Windows 10/11 lock screen with an overlay it draws itself, so you
can place videos, widgets and other content on it.

[中文说明](README.zh-CN.md)

## Planned features

- [x] Full-screen video on the lock screen and on the sign-in screen at startup
- [x] Custom clock styles
- [x] Layered HLSL and Lottie scene wallpapers, with editable parameters and layer placement
- [x] Widgets

## Requirements

- Windows 10 or Windows 11, x64 or ARM64
- Administrator rights for installation
- The Microsoft Edge WebView2 runtime for the AnimeLogon window. Windows 11 includes it;
  on Windows 10 the installer says so if it is missing.

Windows N and KN editions are not supported.

## Installation

> The current version is a preview (alpha) and may have problems. The programs are not code-signed.

1. Download the setup program for your computer's architecture (`AnimeLogon-<version>-x64-setup.exe` or `AnimeLogon-<version>-arm64-setup.exe`) from [Releases](https://github.com/helloyork/AnimeLogon/releases).
2. Run it and follow the wizard. Its first page lets you choose the language. If Windows says "Windows protected your PC", select **More info > Run anyway**.
3. Open AnimeLogon from the Start menu and select **Import…** to add a video or picture.

Run with `--quiet`, the setup program shows no wizard and writes its result to the console; in a
script, wait for it with `start /wait`.

## Usage

Lock the computer (<kbd>Win</kbd>+<kbd>L</kbd>) or restart it. The lock screen shows the theme in use
until you press a key or click, and the Windows sign-in screen appears.

To change it, click a theme in the theme library. Videos, pictures and theme packages
(.altheme), along with scenes (.alscene), are added with **Import…**, and an imported theme is put in use at once. Select
**Customize** on a theme to change its wallpaper and widgets. Themes can be exported as .altheme
files to share.

Choose the display language in **Settings > General > Display language**: 简体中文, 繁體中文, English or
日本語. It follows Windows unless you choose one.

Scene controls appear under **Wallpaper** on the theme page and do not need elevation.
Hover a scene card to play it. See the [scene author guide](tools/SCENES.md) for the format
and the `scene_preview` image and frame-sequence tool.

## Changes to your system

- **The Windows lock screen is turned off.** This is required for the video to be
  visible. Windows Spotlight and lock screen widgets are unavailable while AnimeLogon is
  installed.
- **The sign-in screen background is set by AnimeLogon.** The lock screen picture can no
  longer be changed in Windows Settings.
- **Imported videos, pictures and themes are stored on drive C:.** The location cannot be changed.

The installer describes the first two changes and asks before it makes them. Turning off
**Enable AnimeLogon** in the settings, or uninstalling AnimeLogon, restores these settings.

## Uninstallation

Open **Settings > Apps > Installed apps**, find AnimeLogon and select **Uninstall**, or select **Uninstall** in AnimeLogon's Settings. You can choose whether to delete your themes, wallpapers and settings as well.

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

Output is written to `build\<preset>\bin\<configuration>\`.

| Configure preset | Build presets |
|---|---|
| `x64` | `x64-debug`, `x64-release` |
| `arm64` | `arm64-debug`, `arm64-release` |

Options:

| Option | Description |
|---|---|
| `-DANIMELOGON_BUILD=<n>` | Build number written to the version resource. Default `0`. |
| `-DCMAKE_GENERATOR_INSTANCE=<path>` | Visual Studio installation to use when more than one is installed. |

## Roadmap

Planned:

- A theme community: browse and download themes made by others from the settings

## Contributing

- Open pull requests against `develop`. `master` is updated only through pull requests
  from `develop`.
- CI builds x64 and ARM64 for every pull request.
- Do not commit video or image files, or code under a licence incompatible with MIT.

## License

MIT © 2026 Nomen (helloyork). See [LICENSE](LICENSE).

Includes the headers and loader of the Microsoft Edge WebView2 SDK (BSD 3-Clause) in
`vendor/webview2`. The loader, `WebView2Loader.dll`, is installed with AnimeLogon.

`vendor/thorvg` contains ThorVG 1.1.2 (MIT) and the licence for its embedded RapidJSON.
Only the sandboxed scene renderer links ThorVG. See [SOURCE.md](vendor/thorvg/SOURCE.md).
