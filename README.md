# AnimeLogon

[![Build](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml/badge.svg?branch=develop)](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

AnimeLogon replaces the Windows 10/11 lock screen with an overlay it draws itself, so you
can place videos, widgets and other content on it.

[中文说明](README.zh-CN.md)

## Planned features

- [ ] Full-screen video on the lock screen and on the sign-in screen at startup
- [ ] Custom clock styles
- [ ] Widgets

## Requirements

- Windows 10 or Windows 11, x64 or ARM64
- Administrator rights for installation

Windows N and KN editions are not supported.

## Installation

> No release has been published yet. To try AnimeLogon now, [build it from source](#building-from-source).

1. Download the latest version from [Releases](https://github.com/helloyork/AnimeLogon/releases).
2. Run `install.exe` and follow the prompts.
3. Open the AnimeLogon settings and import a video.

## Usage

Lock the computer (<kbd>Win</kbd>+<kbd>L</kbd>) or restart it. The video plays until
you press a key or click, and the Windows sign-in screen appears.

To change the video, import a different one in the settings.

## Changes to your system

- **The Windows lock screen is turned off.** This is required for the video to be
  visible. Windows Spotlight and lock screen widgets are unavailable while AnimeLogon is
  installed.
- **The sign-in screen background is set by AnimeLogon.** The lock screen picture can no
  longer be changed in Windows Settings.
- **Imported videos are stored on drive C:.** The location cannot be changed.

Uninstalling AnimeLogon restores these settings.

## Uninstallation

Open **Settings > Apps > Installed apps**, find AnimeLogon and select **Uninstall**.

## Building from source

Prerequisites:

- Visual Studio 2022 or later with the **Desktop development with C++** workload
  (add **MSVC ARM64 build tools** for ARM64)
- CMake 3.25 or later

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

- Custom clock drawn on the lock screen
- Custom lock screen elements (widgets), defined in a declarative format
- Rendered animated scenes

## Contributing

- Open pull requests against `develop`. `master` is updated only through pull requests
  from `develop`.
- CI builds x64 and ARM64 for every pull request.
- Do not commit video or image files, or code under a licence incompatible with MIT.

## License

MIT © 2026 Nomen (helloyork). See [LICENSE](LICENSE).

Includes [Micula](https://github.com/helloyork/micula) (MIT) in `vendor/micula`.
