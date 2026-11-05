# 看看 / SeeSee

极速 Windows 看图工具。原生 C++20 + Win32 + Direct2D，**703 KB 单文件，冷启动 80 ms**，双击即看，键盘翻页。

> 解决系统看图工具的三件事：慢、功能不对味、广告多。v1 只做"快速看图"，照片管理留给 v2。

## 现在能做什么

- 双击图片立刻打开；同目录图片自动按自然序加载，键盘一路翻到底
- JPEG 用 DCT 级缩放解码（只解屏幕需要的像素），25.6 MP 的图也能 100 ms 内上屏
- 支持 JPEG / PNG / BMP / GIF / TIFF / ICO / WebP（装了系统解码器还可 AVIF / HEIC / JXL）
- EXIF 方向自动摆正；缩放以光标为锚，放大后可拖动
- 无边框深色界面，左下角显示「尺寸 · 格式 · 体积 · 页码」
- `Ctrl+D` 一键设为默认看图工具（写 HKCU，不需要管理员权限）

## 快捷键

| 键 | 行为 |
| --- | --- |
| `←` `→` `↑` `↓` `PgUp` `PgDn` | 翻页 |
| `Home` / `End` | 第一张 / 最后一张 |
| `0` / `1` | 适应窗口 / 原始大小 |
| `+` / `-` | 放大 / 缩小 |
| 双击 | 适应窗口 ↔ 100% |
| 滚轮 / `Ctrl+滚轮` | 翻页 / 缩放 |
| `F11`、`Esc` | 全屏、退出全屏（再按关窗） |
| `Ctrl+O` / 拖放 | 打开文件 |
| `Ctrl+D` | 设为默认看图工具 |
| `T` | 显示 / 隐藏底部胶片条 |
| `Ctrl+Shift+=` / `Ctrl+Shift+-` | 界面整体放大 / 缩小 |

## 构建

需要 Visual Studio 2022 Build Tools（含 CMake、Ninja、Windows SDK）与 PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1        # 只构建
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Run   # 构建并运行
```

产物：`bin/seesee.exe`（静态链接，无运行时依赖）。

## 命令行

```
seesee.exe [图片路径]        打开图片
seesee.exe --register        设为默认看图工具并打开系统「默认应用」页
seesee.exe --unregister      取消关联
seesee.exe --bench <图片>     性能基准，结果写入 seesee-bench.txt
seesee.exe --shot <png> <图片> 把首帧存成 PNG（UI 回归用）
seesee.exe --help            帮助
```

## 目录

```
src/        应用、渲染、解码、设置、文件关联
resources/  图标、清单、资源脚本
prototype/  UI 高保真原型（HTML 单文件，可直接用浏览器打开）
docs/       需求分析 / 任务计划 / UI 规范
tools/      一次性探针与生成脚本
```

- 需求与性能指标：[`docs/01-requirements.md`](docs/01-requirements.md)
- 计划与里程碑：[`docs/02-roadmap.md`](docs/02-roadmap.md)
- UI 规范：[`docs/03-ui-spec.md`](docs/03-ui-spec.md)

## 状态

v0.2（M0–M5 完成，M6 进行中）：可日常使用的快速看图器，已适配高 DPI 并带底部胶片条、控件自动淡出。剩余 M6：设置面板、内存优化。
