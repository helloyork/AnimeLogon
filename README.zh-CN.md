<img src="res/animelogon.svg" alt="" width="72">

# AnimeLogon

[![Build](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml/badge.svg?branch=develop)](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

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

1. 从 [Releases](https://github.com/helloyork/AnimeLogon/releases) 下载安装程序 `AnimeLogon-<版本>-setup.exe`。x64 和 ARM64 电脑使用同一个安装程序，它会安装与电脑处理器对应的版本。
2. 运行安装程序，按向导完成安装。向导第一页可以选择语言。如果 Windows 提示「Windows 已保护你的电脑」，选择 **更多信息 > 仍要运行**。
3. 从开始菜单打开 AnimeLogon，选择「导入…」导入视频或图片。

以 `--quiet` 运行安装程序时不显示向导，结果输出到命令行；脚本中请用 `start /wait` 等待它结束。退出代码 `0` 表示安装成功，`1` 表示失败，`1223` 表示已取消。

## 使用

锁定电脑（<kbd>Win</kbd>+<kbd>L</kbd>）或重新启动后，锁屏显示当前使用的主题。按任意键或点击鼠标，即显示 Windows 登录界面。

如需更换，在「主题库」中单击一个主题即可套用。视频、图片和主题包（.altheme）用「导入…」添加，导入的主题会直接设为当前主题。选择主题的「自定义」可以调整壁纸和组件。主题可以导出为 .altheme 文件分享给他人。

界面语言可在「设置 > 常规 > 显示语言」中选择：简体中文、繁體中文、English 或日本語，默认跟随 Windows。

AnimeLogon 在系统托盘中显示一个图标。单击图标打开设置；右键单击可以切换最近使用的主题。在设置的「系统托盘」中可以隐藏图标，或更改菜单列出的主题个数。

## 对系统的更改

- **关闭 Windows 锁屏。** 这是显示视频的必要条件。安装期间，Windows 聚焦和锁屏小组件不可用。
- **登录界面背景由 AnimeLogon 设置。** 安装期间无法在 Windows 设置中更改锁屏图片。
- **导入的视频、图片和主题保存在 C 盘，** 位置不可更改。
- **登录时运行系统托盘图标。** 安装程序添加一个对所有账户生效的启动项，卸载时删除。也可以在任务管理器的「启动应用」中禁用。
- **安装名为 AnimeLogon 的 Windows 服务。** 该服务以本地系统账户运行，随 Windows 启动，负责显示锁屏。关闭「启用 AnimeLogon」后服务停止并改为手动启动，卸载时删除。

安装程序会先说明前两项更改，确认后才开始安装。在设置中关闭「启用 AnimeLogon」或卸载 AnimeLogon 后，以上设置将恢复。

## 卸载

打开 **设置 > 应用 > 已安装的应用**，找到 AnimeLogon，选择 **卸载**；也可以在 AnimeLogon 的「设置」中选择「卸载」。卸载时可以选择是否同时删除主题、壁纸和设置。

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
- 请勿提交视频或图片文件，也不要引入许可证与 MIT 不兼容的代码。

## 许可证

MIT © 2026 Nomen (helloyork)。详见 [LICENSE](LICENSE)。

`vendor/webview2` 中包含 Microsoft Edge WebView2 SDK 的头文件和加载器（BSD 3-Clause），加载器 `WebView2Loader.dll` 随 AnimeLogon 一起安装。

`vendor/thorvg` 包含 ThorVG 1.1.2（MIT）和其内嵌 RapidJSON 的许可证，仅场景沙箱链接 ThorVG。来源与构建范围见 [SOURCE.md](vendor/thorvg/SOURCE.md)。

设置程序的界面打包了 React、Fluent UI 等 npm 包（MIT）。构建时会生成这些包的名称、版本和许可证文本。

以上第三方许可证随程序安装在安装目录的 `licenses` 文件夹中，也可以在 AnimeLogon 的「设置」中打开「第三方声明」查看。
