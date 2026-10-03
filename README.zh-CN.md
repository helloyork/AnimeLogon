# AnimeLogon

[![Build](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml/badge.svg?branch=develop)](https://github.com/helloyork/AnimeLogon/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

AnimeLogon 使用自己绘制的叠层替换了 Windows 10/11 的锁屏界面，这使得你可以在上面放置视频和组件等内容。

[English](README.md)

## 规划功能

- [x] 在锁屏和开机登录界面全屏播放视频
- [x] 自定义时钟样式
- [ ] 小组件

## 系统要求

- Windows 10 或 Windows 11，x64 或 ARM64
- 安装需要管理员权限

不支持 Windows N 和 KN 版本。

## 安装

> 目前尚未发布正式版本。如需试用，请[从源码构建](#从源码构建)。

1. 从 [Releases](https://github.com/helloyork/AnimeLogon/releases) 下载最新版本。
2. 运行 `install.exe`，按提示完成安装。
3. 打开 AnimeLogon 设置，在「主题」页导入视频或图片。

## 使用

锁定电脑（<kbd>Win</kbd>+<kbd>L</kbd>）或重新启动后，锁屏显示当前使用的主题。按任意键或点击鼠标，即显示 Windows 登录界面。

如需更换，在设置的「主题」页导入视频、图片或主题包（.altheme），再选择「使用」。主题可以导出为 .altheme 文件分享给他人。

## 对系统的更改

- **关闭 Windows 锁屏。** 这是显示视频的必要条件。安装期间，Windows 聚焦和锁屏小组件不可用。
- **登录界面背景由 AnimeLogon 设置。** 安装期间无法在 Windows 设置中更改锁屏图片。
- **导入的视频、图片和主题保存在 C 盘，** 位置不可更改。

卸载 AnimeLogon 后，以上设置将恢复。

## 卸载

打开 **设置 > 应用 > 已安装的应用**，找到 AnimeLogon，选择 **卸载**。

## 从源码构建

环境要求：

- Visual Studio 2022 或更高版本，安装「使用 C++ 的桌面开发」工作负载（构建 ARM64 还需安装「MSVC ARM64 生成工具」）
- CMake 3.25 或更高版本

```powershell
git clone https://github.com/helloyork/AnimeLogon.git
cd AnimeLogon
cmake --preset x64
cmake --build --preset x64-release
```

输出目录为 `build\<预设>\bin\<配置>\`。

| 配置预设 | 构建预设 |
|---|---|
| `x64` | `x64-debug`、`x64-release` |
| `arm64` | `arm64-debug`、`arm64-release` |

可选参数：

| 参数 | 说明 |
|---|---|
| `-DANIMELOGON_BUILD=<n>` | 写入版本信息的构建号，默认为 `0`。 |
| `-DCMAKE_GENERATOR_INSTANCE=<路径>` | 安装了多个 Visual Studio 时，指定使用哪一个。 |

## 路线图

规划中：

- 自定义锁屏元素（小组件），采用声明式格式定义
- 动态场景渲染

## 参与贡献

- 拉取请求请提交到 `develop` 分支。`master` 只通过来自 `develop` 的拉取请求更新。
- CI 会为每个拉取请求构建 x64 和 ARM64 版本。
- 请勿提交视频或图片文件，也不要引入许可证与 MIT 不兼容的代码。

## 许可证

MIT © 2026 Nomen (helloyork)。详见 [LICENSE](LICENSE)。

`vendor/micula` 中包含 [Micula](https://github.com/helloyork/micula)（MIT）。
