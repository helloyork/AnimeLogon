# 场景壁纸 v1 作者指南

场景是一个图片、视频或 HLSL 底图，加上最多 8 个 HLSL / Lottie 效果层。导入后，主题页可以修改参数和各层的位置、大小、透明度与显隐；这些修改不需要提权，导出主题时会写回场景默认值。

## 最小例子

把下列 `scene.xml` 和 `sky.hlsl` 放在 ZIP 根目录，选择 **不压缩 / stored**，扩展名改为 `.alscene`。设置程序的「导入…」和「更换壁纸」都接受它；导入会创建带默认时钟的主题。

```xml
<scene format="1" name="流动天空" author="你的名字">
  <setting id="tint" kind="color" label="天空颜色" default="#4078cc"/>
  <setting id="speed" kind="number" label="速度" min="0" max="2" step="0.05" default="0.3"/>
  <base shader="sky.hlsl">
    <param slot="0" value="$tint"/>
    <param slot="1" value="$speed"/>
  </base>
</scene>
```

```hlsl
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float wave = 0.8 + 0.2 * sin(uv.x * 6 + time * param[1].x);
    return float4(param[0].rgb * wave, 1);
}
```

不用在源码里重复声明下面的常量块和纹理，导入器会加上它们。

## 素材与 XML

- `scene.xml` 是 UTF-8，最多 64 KB。只接受元素和属性，不接受脚本、文本节点、DOCTYPE 或外部引用。
- 素材文件名只用小写 ASCII 字母、数字、`_`、`-`，例如 `city.png`。也可以放在一级目录 `images/`、`shaders/`、`lottie/` 中。路径不能以 `/` 开头，不能包含 `..`、反斜线或 Windows 保留设备名。
- ZIP 只接受 stored 条目。场景总量最多 256 MiB；只放 XML 实际引用的素材，不放编译出的字节码。
- 最多 4 个 `<image id="city" src="city.png"/>`，支持 BMP、PNG、JPEG 和 WebP。导入时统一转为规范 BMP。
- 底图恰好一个：`<base image="city" fit="fill"/>`、`<base video="clip.mp4" fit="fit"/>` 或 `<base shader="sky.hlsl"/>`。图片引用 image 的 id。视频导入时规范化为 H.264，声音另存，播放声音由全局设置控制。
- `fit` 为 `fill`（裁剪填充）、`fit`（完整显示，留黑边）或 `stretch`（拉伸），默认 fill。它决定场景内部底图的显示方式。
- 效果按 XML 顺序叠加，最多 8 层：`<layer shader="rain.hlsl"/>` 或 `<layer lottie="bird.json"/>`。
- 层的 `blend` 为 normal、add、screen、multiply，默认 normal；`opacity` 是 0–1 的数值或数字设置引用；`when` 是 true/false 或布尔设置引用。
- 不写位置属性时铺满画布。写 `anchor`、`offset-x`、`offset-y`、`height` 中任意一个就按位置摆放。锚点为 top-left、top、top-right、left、center、right、bottom-left、bottom、bottom-right。偏移是画布宽高的百分比（−100 到 100）；高度是画布高度的百分比（大于 0，最多 100）。摆放的 Lottie 保持原始宽高比。

```xml
<image id="city" src="city.png"/>
<base image="city"/>
<layer shader="rain.hlsl" blend="screen" reads="below" opacity="$rain">
  <param slot="0" value="$tint"/>
</layer>
<layer lottie="bird.json" anchor="top-right" offset-x="-4" offset-y="6" height="18" when="$birds">
  <slot id="wing" value="$tint"/>
</layer>
```

## 设置与绑定

支持 number、color、bool、choice，最多 16 项。id 只用小写字母、数字、`_`、`-`，以字母开头；不能用保留字 ref、visible。label 是控件名称，detail 是可选说明。

```xml
<setting id="rain" kind="number" label="雨量" min="0" max="1" step="0.05" default="0.6"/>
<setting id="tint" kind="color" label="色调" default="#8fb4ff"/>
<setting id="birds" kind="bool" label="飞鸟" default="true"/>
<setting id="place" kind="choice" label="动画内部位置" default="0,0,0">
  <option value="0,0,0" label="原位"/>
  <option value="20,40,0" label="偏移"/>
</setting>
```

`$id` 引用设置。参数和槽位也接受直接写的有限数值、颜色 `#RRGGBB` / `#RRGGBBAA` 或 2–4 个逗号分隔的数值；不接受表达式。绑定的默认值和全部 choice 选项必须适合目标类型。透明度绑定到 number 时，整个范围都必须在 0–1 以内。

设置程序还提供每层的直接调整。settings.ini 中的设置键是 `theme.<主题id>.wallpaper.<设置id>`；各层调整为 `p1.anchor`、`p1.offset-x`、`p1.offset-y`、`p1.size`、`p1.opacity`、`p1.visible`，p1 表示第一层。anchor 的额外值 full 表示铺满。直接调整透明度或显隐会覆盖其绑定；再次修改绑定的设置会恢复绑定。

## HLSL 契约

每份源码最多 64 KiB；入口必须是 `float4 main(float2 uv : TEXCOORD0) : SV_Target`。UV 从左上 (0,0) 到右下 (1,1)。只编译 ps_5_0；不支持 include、UAV、计算着色器、额外资源或自行增加常量块。所有作者源码编译和字节码创建都在没有网络能力的 AppContainer 中完成。

```hlsl
cbuffer Scene : register(b0) {
    float time; float2 resolution; float dayfraction;
    float battery; float playback; float playbackState; float reserved;
    float4 param[16];
};
Texture2D<float4> image0 : register(t0);
Texture2D<float4> image1 : register(t1);
Texture2D<float4> image2 : register(t2);
Texture2D<float4> image3 : register(t3);
Texture2D<float4> below  : register(t4);
SamplerState linearClamp : register(s0);
```

这是导入器注入的声明，示例中只需使用这些名字。image0–3 对应 `<image>` 的声明顺序。只有效果层写了 `reads="below"` 才能采样 below；它是该层之前合成好的整幅画面。HLSL 输出使用直通 alpha（RGB 不预乘），由合成器处理透明度。

| 值 | 含义 |
|---|---|
| time | 播放秒数，屏幕关闭时停止推进 |
| resolution | 当前层实际内部纹理的像素宽高，可能随自动降级变小 |
| dayfraction | 本地时刻占一天的比例，0–1 |
| battery | 电量比例 0–1，未知或没有电池为 −1 |
| playback | 播放进度 0–1，未知为 0 |
| playbackState | 0 无播放内容，1 正在播放，2 暂停 |
| param[0–15] | XML 的 param slot；标量在 x，颜色是 RGBA，向量剩余分量为 0 |

播放数据只来自当前用户自己的会话，遵守媒体设置开关；沙箱拿不到歌曲标题、歌手、文件路径或其他用户的数据。预览以打开设置程序的用户身份读取相同的数值。

## Lottie 限制

采用 ThorVG 1.1.2 的 CPU 光栅器。支持形状、预合成和内嵌 PNG/JPEG 图片。文字要先转成形状；图片必须使用 `data:image/png;base64,...` / `data:image/jpeg;base64,...`，e=1 且没有外部 u 路径。不接受 expressions、文字层、视频、音频、字体文件或外部资源。单个 JSON 最多 16 MiB，动画尺寸最大 16384，帧率最大 240。

槽位使用 Lottie 属性里的 `sid`。例如颜色属性 `"c":{"a":0,"k":[1,0,0,1],"sid":"wing"}` 配合 `<slot id="wing" value="$tint"/>`。支持颜色、标量、二维和三维位置；三维位置也接受两个分量并补 0。透明度槽位遵循 Lottie 原始单位（通常 0–100），层的 opacity 仍是 0–1。不支持让 Lottie 读取电量或播放数据。

导入时试画整个动画的分布采样帧。无效 JSON、未知槽位或不匹配的槽位类型会拒收并说明原因。表达式引擎和字体载入器没有编进 scene.exe。

## 预览工具

源码构建会在 `build\x64\bin\Release\`（ARM64 对应其目录）生成 scene_preview.exe 和 scene.exe，两者放在同一目录。工具没有窗口、不安装服务、不需要 UAC。输入是 `.alscene` 或已经规范化、带 wallpaper.ini 的场景目录。

```powershell
.\scene_preview.exe .\sky.alscene .\sky.png --width 1920 --height 1080 --time 2 tint=#ffcc66
.\scene_preview.exe .\sky.alscene .\frames --frames 120 --fps 30 --width 1920 --height 1080
.\scene_preview.exe .\sky.alscene .\noon.png --day 0.5 --battery 0.76 --progress 0.4 --state 1
```

单帧输出 PNG。多帧输出 `000000.png` 起的序列，时间为 `--time + 帧号 / --fps`，通过渲染器确认完成后再保存，不依赖工具运行速度。最多 3600 帧，1–60 fps；输出尺寸最大 7680×4320。数据选项让作者明确指定测试数值，默认没有电池和播放内容。

作者工具以完整分辨率逐帧画，便于检查细节，即使导入试画建议使用静止画面也可以离线输出。设置程序的预览则遵循这台设备的自动性能选择；静止降级时使用同一张导入画面，不继续播放卡片动画。

## 自动性能降级与锁屏

不提供性能旋钮。导入在当前显示器分辨率上测量 CPU/GPU 完成耗时：先选择最高 60 fps，超过预算改为 30 fps，再降低效果层内部尺寸，最低为宽高各 50%。Lottie 和着色器层都能降分辨率，底视频仍由父进程解码。半分辨率仍超预算的有效场景会保留其静止画面。

锁屏先显示导入生成的第 0 秒静止画面，再淡入已经准备好的实时帧。子进程退出、两秒没有新帧或显卡重置时，这次锁屏停在静止画面；连续三次失败后等更换壁纸才重试。屏幕关闭或唤醒淡出后停止渲染。横跨屏幕画一次整幅画布，各屏相同也共享一次渲染；同一场景使用不同参数时分别渲染。

`still.bmp` 和登录背景始终来自导入时的第 0 秒默认画面；参数修改改变实时画面，导出再导入后生成相应的新静止画面。导出保留 HLSL 源码、原始 Lottie 和素材，不携带编译字节码；接收方会重新编译并按自己的硬件评估性能。
