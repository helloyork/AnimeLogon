<img src="res/readme/banner.png" alt="AnimeLogon">

# AnimeLogon

[![Build](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml/badge.svg?branch=develop)](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](LICENSE)

AnimeLogon 使用自己绘制的叠层替换了 Windows 10/11 的锁屏界面，这使得你可以在上面放置视频和组件等内容。

[English](README.md)

## 规划功能

- [x] 在锁屏和开机登录界面全屏播放视频
- [x] 自定义时钟样式
- [x] 小组件
- [ ] 场景壁纸（HLSL 和 Lottie）

## 系统要求

- x64 电脑：Windows 10 或 Windows 11
- ARM64 电脑：Windows 11
- 安装需要管理员权限
- AnimeLogon 窗口需要 Microsoft Edge WebView2 运行时。Windows 11 已自带；Windows 10 上如果没有，安装程序会提示。

不支持 Windows N 和 KN 版本。

## 安装

> 程序没有代码签名。

1. 从 [Releases](https://github.com/helloyork/AnimeLogon/releases) 下载安装程序 `AnimeLogon-<版本>-setup.exe`。x64 和 ARM64 电脑使用同一个安装程序，它会安装与电脑处理器对应的版本。访问 GitHub 慢的话，也可以从 [GitCode 镜像](https://gitcode.com/nominaawa/AnimeLogon/releases)下载同一个文件。
2. 运行安装程序，按向导完成安装。向导第一页可以选择语言。如果 Windows 提示「Windows 已保护你的电脑」，选择 **更多信息 > 仍要运行**。
3. 从开始菜单打开 AnimeLogon，选择「导入…」导入视频或图片。

静默安装与卸载的参数见[命令行](#命令行)。

## 使用

锁定电脑（<kbd>Win</kbd>+<kbd>L</kbd>）或重新启动后，锁屏显示当前使用的主题。按任意键或点击鼠标，即显示 Windows 登录界面。

如需更换，在「主题库」中单击一个主题即可套用。视频、图片和主题包（.altheme）用「导入…」添加，导入的主题会直接设为当前主题。双击 .altheme 文件也会以同样方式导入。选择主题的「自定义」可以调整壁纸和组件。主题可以导出为 .altheme 文件分享给他人。

界面语言可在「设置 > 常规 > 显示语言」中选择：简体中文、繁體中文、English 或日本語，默认跟随 Windows。

AnimeLogon 在系统托盘中显示一个图标。单击图标打开设置；右键单击可以切换最近使用的主题。在设置的「系统托盘」中可以隐藏图标，或更改菜单列出的主题个数。

## 对系统的更改

- **关闭 Windows 锁屏。** 这是显示视频的必要条件。安装期间，Windows 聚焦和锁屏小组件不可用。
- **登录界面背景由 AnimeLogon 设置。** 安装期间无法在 Windows 设置中更改锁屏图片。
- **导入的视频、图片和主题保存在 C 盘，** 位置不可更改。
- **登录时运行系统托盘图标。** 安装程序添加一个对所有账户生效的启动项，卸载时删除。也可以在任务管理器的「启动应用」中禁用。
- **用 AnimeLogon 打开 .altheme 文件。** 安装程序为所有账户注册这种文件类型，卸载时删除。
- **安装名为 AnimeLogon 的 Windows 服务。** 该服务以本地系统账户运行，随 Windows 启动，负责显示锁屏。关闭「启用 AnimeLogon」后服务停止并改为手动启动，卸载时删除。

安装程序会先说明前两项更改，确认后才开始安装。在设置中关闭「启用 AnimeLogon」或卸载 AnimeLogon 后，以上设置将恢复。

## 卸载

打开 **设置 > 应用 > 已安装的应用**，找到 AnimeLogon，选择 **卸载**；也可以在 AnimeLogon 的「设置」中选择「卸载」。卸载时可以选择是否同时删除主题、壁纸和设置。

## 公开接口

以下文件格式、命令行和文件位置可供主题作者、脚本和其他程序使用。

### 文件格式

| 格式 | 说明 |
|---|---|
| 组件（`.xml`，根元素 `<component>`） | 主题中绘制在壁纸之上的内容，如时钟、日期、电量、正在播放、倒计时和音频频谱。见[组件格式](skills/animelogon-components/reference/component-format.md)和[组件属性](skills/animelogon-components/reference/attributes.md)（英文）。 |
| 主题（`theme.xml`）与主题包（`.altheme`） | 主题由一个壁纸和最多 8 个组件组成。主题包是不压缩的 zip 文件，包含主题及其壁纸和组件文件。见[主题与主题包](skills/animelogon-components/reference/theme-package.md)（英文）。 |
| 场景壁纸（`.alscene`） | 当前版本不导入。 |

以上格式的版本均为 `format="1"`。AnimeLogon 拒绝无法识别的元素和属性，因此使用了较新版本所增加内容的文件，无法在较早的版本中导入。

### 编写组件

[`skills/animelogon-components`](skills/animelogon-components) 是供 Claude Code 等 AI 编程助手使用的技能（Agent Skill）。将该文件夹复制到助手的技能目录（Claude Code 为 `%USERPROFILE%\.claude\skills\`）后，助手即可按格式编写、检查和打包组件。该文件夹同时也是手工编写组件的参考。

从源码构建时会生成两个开发工具：`component_check` 使用 AnimeLogon 自身的读取器检查组件和主题包，并可打包 `.altheme`；`skin_preview` 将组件绘制在一张图片上以便查看效果。

如需在不影响已安装内容的情况下试用组件，可使用 `animelogon.exe --sandbox <文件夹>`（见下文），在其中导入不需要管理员批准。

### 命令行

**安装程序**（`AnimeLogon-<版本>-setup.exe`，从源码构建时为 `install.exe`）：

| 参数 | 说明 |
|---|---|
| `--quiet` | 不显示向导，结果输出到命令行。脚本中请用 `start /wait` 等待它结束。 |
| `--lang <语言>` | 界面语言：`zh-Hans`、`zh-Hant`、`en` 或 `ja`。 |
| `--dir <路径>` | 安装到指定路径：`C:\Program Files` 中的文件夹，或其他本地磁盘上的 `<盘符>:\AnimeLogon`。未指定时，新安装位于 `C:\Program Files\AnimeLogon`，更新安装在已安装的位置。 |
| `--drive <盘符>` | 安装到指定磁盘：Windows 所在磁盘装到 `C:\Program Files\AnimeLogon`，其他磁盘装到 `<盘符>:\AnimeLogon`。可移动磁盘、网络驱动器、FAT32 和 exFAT 磁盘，以及开机时 BitLocker 不会自动解锁的磁盘不能使用。更新安装不会换磁盘。 |
| `--setup-in-app` | 与 `--quiet` 一起使用：只安装，不启用。首次打开 AnimeLogon 时由向导启用。 |

退出代码：`0` 安装成功，`1` 失败，`1223` 已取消。

**卸载程序**（安装目录中的 `uninstall.exe`）：

| 参数 | 说明 |
|---|---|
| `--quiet` | 不显示向导，卸载 AnimeLogon，保留主题、壁纸和设置。 |
| `--quiet --purge` | 同时删除主题、壁纸和设置。 |

退出代码：`0` 已卸载，`1` 有更改未能恢复，`1223` 已取消。

**AnimeLogon**（安装目录中的 `animelogon.exe`）：

| 参数 | 说明 |
|---|---|
| `--tray` | 显示系统托盘图标而不打开窗口。登录时由启动项以此参数运行。 |
| `--show switch`、`--show lock` | 打开或切换到窗口，并定位到「启用 AnimeLogon」开关，或当前主题的锁屏预览。 |
| `--sandbox <文件夹>` | 打开一个使用 `<文件夹>` 中独立数据（主题、组件、设置）的窗口，用于测试。其中的导入不需要管理员批准，锁屏也不会使用这些数据。 |
| `<文件>.altheme` | 导入该主题包，与「导入…」相同；已有打开的窗口时，由该窗口导入。双击 .altheme 文件即以此方式运行 AnimeLogon。可与 `--sandbox` 同时使用。 |
| `--switch-on`、`--switch-off` | 启用或停用 AnimeLogon，与设置中的开关相同。需要管理员权限。退出代码：`0` 完成，`1` 失败，`4` 未以管理员身份运行。 |

参数无法识别时，`animelogon.exe` 显示提示并以代码 `2` 退出。提权运行时的 `--commit-*` 与 `--remove-*` 参数用于安装或删除窗口已准备好的导入内容，格式见 [config/src/commit.h](config/src/commit.h)。Steam 版（规划中）的辅助程序 `animelogon-steam.exe` 使用的协议见 [steam/PROTOCOL.md](steam/PROTOCOL.md)。

### 文件位置

| 位置 | 内容 |
|---|---|
| `C:\Program Files\AnimeLogon\`，装在其他磁盘时为 `<盘符>:\AnimeLogon\` | 程序、`uninstall.exe` 和第三方许可证（`licenses\`）。装在其他磁盘时，这个文件夹由安装程序建在磁盘根目录下，只有管理员可以修改，主题也放在里面（`Library\`）。 |
| `%ProgramData%\AnimeLogon\settings.ini` | 全机设置：当前主题、多显示器、时钟、声音，以及对各主题的修改。每行一个 `键 = 值`，只含 id、枚举和带类型的值，不含路径或自由文本。键的说明见 [settings.h](common/include/animelogon/settings.h)。 |
| `%ProgramData%\AnimeLogon\themes\`、`components\`、`wallpapers\`（装在其他磁盘时为 `<盘符>:\AnimeLogon\Library\…`） | 已导入的主题、组件和壁纸，以及安装程序写入的默认主题壁纸。只有管理员可以写入，内容由 AnimeLogon 规范化后写入。 |
| `%ProgramData%\AnimeLogon\logs\` | 日志。 |

## 从源码构建

环境要求：

- Visual Studio 2022 或更高版本，安装「使用 C++ 的桌面开发」工作负载（构建 ARM64 还需安装「MSVC ARM64 生成工具」）
- CMake 3.25 或更高版本
- Node.js 20 或更高版本（含 npm），用于打包设置程序的界面

```powershell
git clone https://github.com/helloyork/AnimeLogon.git
cd AnimeLogon
cmake --preset x64
cmake --build --preset x64-release
```

输出目录为 `build\<预设>\bin\<配置>\`。安装程序为其中的 `install.exe`。

| 配置预设 | 构建预设 |
|---|---|
| `x64` | `x64-debug`、`x64-release` |
| `arm64` | `arm64-debug`、`arm64-release` |

可选参数：

| 参数 | 说明 |
|---|---|
| `-DANIMELOGON_BUILD=<n>` | 写入版本信息的构建号，默认为 `0`。 |
| `-DANIMELOGON_VERSION_SUFFIX=<后缀>` | 写入版本号的预发布后缀，例如 `-beta1`，默认为空。 |
| `-DANIMELOGON_ARM64_PAYLOAD_DIR=<路径>` | 仅用于 x64 构建：ARM64 Release 构建的 `bin\Release` 目录。指定后，`install.exe` 同时包含 ARM64 版本，可在两种电脑上安装。 |
| `-DBUILD_TESTING=OFF` | 不构建测试和开发工具，默认为 `ON`。 |
| `-DCMAKE_GENERATOR_INSTANCE=<路径>` | 安装了多个 Visual Studio 时，指定使用哪一个。 |

运行测试（x64）：

```powershell
ctest --preset x64-release --output-on-failure
```

生成与发布版相同、同时包含两种架构的安装程序：先构建 ARM64，再在 x64 构建中指定 ARM64 的输出目录。

```powershell
cmake --preset arm64
cmake --build --preset arm64-release
cmake --preset x64 -DANIMELOGON_ARM64_PAYLOAD_DIR="$PWD\build\arm64\bin\Release"
cmake --build --preset x64-release
```

## 路线图

规划中：

- Steam 版：通过 Steam 创意工坊浏览、下载和分享主题

## 参与贡献

- 拉取请求请提交到 `develop` 分支。`master` 只通过来自 `develop` 的拉取请求更新。
- CI 会为每个拉取请求构建 x64 和 ARM64 版本。
- 请勿提交视频或图片文件，也不要引入许可证与 MPL 2.0 不兼容的代码。
- 提交的贡献与其余代码一样，以 MPL 2.0 授权。

## 许可证

Mozilla Public License 2.0 © 2026 Nomen (helloyork)。详见 [LICENSE](LICENSE)。0.1.0-alpha 及更早的版本以 MIT 许可证发布。

仓库中 `vendor/` 和 `skills/animelogon-components/` 以外的每个文件都适用以下声明：

> This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a
> copy of the MPL was not distributed with this file, You can obtain one at
> https://mozilla.org/MPL/2.0/.

组件 skill（`skills/animelogon-components/`，包括其中的示例）以 MIT 许可证授权，见其 [LICENSE](skills/animelogon-components/LICENSE)。内置组件的 XML（`common/src/components.cpp`，以及 `common/src/skin.cpp` 中的时钟）也可以按该许可证使用。由它们改出的组件不必采用 MPL。

`vendor/webview2` 中包含 Microsoft Edge WebView2 SDK 的头文件和加载器（BSD 3-Clause），加载器 `WebView2Loader.dll` 随 AnimeLogon 一起安装。

`vendor/thorvg` 包含 ThorVG 1.1.2（MIT）和其内嵌 RapidJSON 的许可证，仅场景沙箱链接 ThorVG。来源与构建范围见 [SOURCE.md](vendor/thorvg/SOURCE.md)。

设置程序的界面打包了 React、Fluent UI 等 npm 包（MIT）。构建时会生成这些包的名称、版本和许可证文本。

以上第三方许可证随程序安装在安装目录的 `licenses` 文件夹中，也可以在 AnimeLogon 的「设置」中打开「第三方声明」查看。
