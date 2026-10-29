# 字体文件（生成字库用）

`../gen_font.ps1` 会读取本目录的字体，重新生成 `Countdown/font_cn12.h`。
字体源文件体积较大，**Fusion Pixel 的 TTF 不随仓库提交**（约 7 MB），按下面下载即可。

## 1. Fusion Pixel Font 12px · **等宽模式**（中文点阵）

- 项目主页：https://github.com/TakWolf/fusion-pixel-font
- 许可证：SIL OFL 1.1（见 `OFL-FusionPixel.txt`）
- 版本：`2026.10.25`
- **下载地址**：
  https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.10.25/fusion-pixel-font-12px-monospaced-ttf-v2026.10.25.zip
- 压缩包约 51 MB，里面包含 7 个语言的 TTF；**只需要**：
  `fusion-pixel-12px-monospaced-zh_hans.ttf`（约 7.0 MB）
- 解压后放到本目录，文件名保持不变：
  `tools/fonts/fusion-pixel-12px-monospaced-zh_hans.ttf`

> **为什么用等宽模式而不是比例模式？**
> 屏幕是固定的 12px 网格：汉字占 12px、ASCII 占 6px。等宽模式保证「字形完全落在字面框内、行高等于字号」，不会裁字；比例模式的 ASCII 宽度不固定，放进 6px 格子会被切掉。

## 2. JetBrains Mono Bold（数字）

- 项目主页：https://github.com/JetBrains/JetBrainsMono
- 许可证：SIL OFL 1.1（见 `OFL-JetBrainsMono.txt`）
- 版本：`2.304`
- 下载地址（如需重新获取）：
  https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
- 压缩包内路径：`fonts/ttf/JetBrainsMono-Bold.ttf`
- **本仓库已附带该文件**，无需重复下载。

## 重新生成字库

方式一：双击 `tools/gen_font.cmd`（会自动调用 PowerShell 并暂停显示结果）。

方式二：在项目根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/gen_font.ps1
```

成功后会在 `Countdown/font_cn12.h` 生成新的点阵字库（约 110 KB），然后重新编译固件即可。

## 说明

- 两个字体都通过 `PrivateFontCollection` 直接加载，**不需要安装到系统**。
- 生成的是**点阵数据**，只包含源码里实际用到的字符（约 750 个），不是完整字体。
- 像素字体必须按 **12px（或其整数倍）** 渲染才能保证不糊；脚本已固定 12px。
- 换字体后字形会变化，建议上机确认可读性。
