# Snap-Lite

面向 **Windows 11 x64** 的轻量截图 + 贴图工具。

目标不是只做一个 `BitBlt` 截图 Demo，而是保留 Snipaste 类工具真正高频的能力，同时坚持：**原生、快速、低占用、无 Electron / Python / .NET Runtime**。

## 下载

优先从 GitHub Releases 获取预编译版本，不需要安装 CMake、Visual Studio 或其他运行时。

当前正式版：`v1.2.0`

Release Assets：

```text
SnapLite.exe                   直接运行
SnapLite-1.2.0-win-x64.zip    便携压缩包
SHA256SUMS.txt                 文件校验
```

当前 Release 面向 **Windows 11 x64**。

## 当前已实现

### 截图

- `F1` 开始截图
- 鼠标悬停自动识别窗口 / Win32 控件区域
- 单击快速选择识别区域
- 鼠标拖动自由选区
- 多显示器虚拟桌面截图
- Windows 11 Per-Monitor V2 高 DPI
- 放大镜像素预览
- `C` 复制当前像素 HEX 颜色
- 方向键 1px 微调鼠标位置，`Shift + 方向键` 10px
- `Esc` / 右键取消
- 蓝白二次元科技风截图 UI，与 Snap-Lite 少女图标统一配色

### 标注

选区完成后显示独立的二次元风编辑工具条：

- 矩形
- 箭头
- 画笔
- 马赛克
- 文字
- 颜色选择
- 文字字号选择：12 / 14 / 16 / 20 / 24 / 32 / 48 pt
- 撤销 / 重做
- 贴图
- 保存
- 另存为
- 复制
- 取消

工具按钮支持 hover 说明，鼠标移到按钮上会直接显示该按钮的用途和快捷键。

文字输入阶段使用透明背景，最终写入截图时只保留文字本身；文字颜色和字号均跟随工具条当前设置。

快捷键：

| 工具 | 快捷键 |
| --- | --- |
| 矩形 | `R` |
| 箭头 | `A` |
| 画笔 | `P` |
| 马赛克 | `M` |
| 文字 | `T` |
| 撤销 | `Ctrl + Z` |
| 重做 | `Ctrl + Y` |
| 完成复制 | `Enter` |
| 取消 | `Esc` |

撤销历史采用双重内存限制：最多保留 6 个位图状态，并将位图历史控制在约 32 MB 预算内。大尺寸选区不会再因为连续标注保留 20 份完整位图而快速膨胀内存。

### 输出

- 自动复制截图到剪贴板
- 自动保存 PNG
- 可在托盘菜单中设置默认截图保存目录
- 默认保存目录持久化到当前用户配置，不需要管理员权限
- 编辑工具条提供“另存为”，可自行选择文件名和位置
- `Ctrl + Shift + F` 全屏截图

截图完成时会同时向 Windows 剪贴板写入多种表示：

```text
CF_HDROP   -> 已保存 PNG 文件
CF_BITMAP  -> Windows Bitmap
CF_DIB     -> 设备无关位图
PNG        -> 原始 PNG 数据
```

这样普通 GUI 应用和支持图片输入的 AI CLI 都可以选择自己支持的格式读取。

### AI CLI 直接粘贴

目标使用链：

```text
F1 截图
  ↓
选择区域 / 标注
  ↓
复制
  ↓
切回 AI CLI
  ↓
Ctrl + V
  ↓
图片直接进入 CLI 上下文
```

目前重点兼容：

- Codex CLI（Windows Terminal / PowerShell）
- Codex CLI（WSL，使用 Windows 剪贴板回退）
- 其他能够直接读取系统剪贴板图片的 CLI / TUI

纯文本 CLI 如果自身完全不支持图片输入，Snap-Lite 无法让它凭空获得图片能力。

### 贴图

- `F3` 将剪贴板图片贴到桌面
- 始终置顶
- 鼠标拖动移动
- 鼠标滚轮缩放
- `Ctrl + 滚轮` 调整透明度
- 右键快速设置透明度
- 双击 / 右键关闭贴图
- 关闭后不会因为再次按 `F3` 重新创建同一份剪贴板贴图；复制新图片后才允许重新贴出

### 桌面应用能力

- 系统托盘常驻
- 单实例运行
- 原生 Win32 全局热键
- 内置应用 / 托盘图标
- 托盘菜单可选 **开机自启动**，默认关闭
- 开机启动使用当前用户 `HKCU` Run 项，不需要管理员权限
- 托盘菜单可设置 / 打开默认截图目录
- 截图处理完成后主动回收不再活跃的工作集页面
- 无第三方 GUI 框架

## 后续计划

- [ ] 马克笔 / 高亮笔
- [ ] 高斯模糊
- [ ] 橡皮擦
- [ ] 线宽可调
- [ ] 截图历史回放
- [ ] 贴图旋转 / 镜像翻转
- [ ] 贴图鼠标穿透
- [ ] 贴图缩略图模式
- [ ] 贴图重新进入标注
- [ ] 自定义全局快捷键
- [ ] 更完整的设置页：自动保存、文件名规则等
- [ ] 更完善的 UI 元素识别（包含非 Win32 控件）

OCR、云同步、账号系统等重功能默认不进入核心版本。

## 技术实现

```text
C++17
Win32 API
GDI / GDI+
Windows Shell API
Windows Registry API
Common Controls / Common Dialogs
```

核心链路：

```text
F1
 ↓
截取虚拟桌面
 ↓
窗口/控件自动识别 + 放大镜/取色
 ↓
选择区域
 ↓
二次元风浮动编辑工具条
 ↓
标注 / 颜色 / 字号 / 保存 / 另存为 / 复制
 ↓
可直接粘贴到支持图片的 AI CLI
 ↓
F3 可将剪贴板图片贴回桌面
```

## 构建

Windows 11 + Visual Studio 2022，安装 **Desktop development with C++**：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成：

```text
build/Release/SnapLite.exe
```

## 项目结构

```text
Snap-Lite/
├── .github/workflows/build.yml
├── .github/workflows/release.yml
├── resources/
│   ├── SnapLite.ico
│   ├── SnapLite.rc.in
│   └── resource.h
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp
    ├── app.h / app.cpp
    ├── anime_toolbar.h / anime_toolbar.cpp
    ├── capture.h / capture.cpp
    ├── capture_settings.cpp
    ├── snip_window.h / snip_window.cpp
    ├── snip_window_original.inc
    ├── editor_window.h / editor_window.cpp
    └── pin_window.h / pin_window.cpp
```

## 设计原则

1. Windows 11 优先，不为老系统兼容增加大量分支。
2. 原生 API 优先，不为了 UI 引入大型运行时。
3. 高频截图 / 标注 / 贴图功能必须完整。
4. AI CLI 图片粘贴属于核心兼容能力，不做成额外插件。
5. 非核心功能按可选能力扩展，避免常驻进程越来越重。
