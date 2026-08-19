# Snap-Lite

一个面向 Windows 的轻量截图工具。核心目标是：**启动快、常驻占用低、依赖少、截图操作直接**。

## 当前功能

- 区域截图：鼠标拖动选择区域
- 全屏截图：支持 Windows 虚拟桌面 / 多显示器
- 截图后自动复制到剪贴板
- 自动保存 PNG
- 系统托盘常驻
- 全局快捷键
- 单实例运行
- 无第三方运行时、无 Electron、无 Python 环境

## 快捷键

| 操作 | 快捷键 |
| --- | --- |
| 区域截图 | `Ctrl + Shift + A` |
| 全屏截图 | `Ctrl + Shift + F` |
| 取消区域截图 | `Esc` / 鼠标右键 |

也可以单击托盘图标开始区域截图，右键托盘图标打开菜单。

## 截图保存位置

默认保存到：

```text
图片/Snap-Lite/
```

文件名示例：

```text
SnapLite_2026-08-19_19-42-18-125.png
```

## 技术实现

Snap-Lite 使用 **C++17 + Win32 API + GDI/GDI+**：

```text
全局快捷键  -> RegisterHotKey
屏幕捕获    -> GDI BitBlt
区域选择    -> Win32 TopMost Overlay Window
剪贴板      -> Win32 Clipboard API
PNG 保存    -> Windows GDI+
系统托盘    -> Shell_NotifyIcon
```

程序本身不依赖额外 GUI 框架和第三方运行时。

## 构建

推荐 Windows 10/11 + Visual Studio 2022（安装 Desktop development with C++）。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成文件：

```text
build/Release/SnapLite.exe
```

运行 `SnapLite.exe` 后程序进入系统托盘，不会弹出主窗口。

## 项目结构

```text
Snap-Lite/
├── .github/workflows/build.yml
├── CMakeLists.txt
├── README.md
└── src/
    └── main.cpp
```

## 设计原则

Snap-Lite 暂时不加入 OCR、云同步、账号系统、大型截图编辑器等重功能。后续功能优先考虑：窗口自动吸附、简单标注、自定义快捷键和开机启动，同时继续保持单文件原生程序的方向。
