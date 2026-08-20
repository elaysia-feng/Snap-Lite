# Snap-Lite

面向 **Windows 11 x64** 的轻量截图 + 贴图工具。

目标不是只做一个 `BitBlt` 截图 Demo，而是保留 Snipaste 类工具真正高频的能力，同时坚持：**原生、快速、低占用、无 Electron / Python / .NET Runtime**。

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

### 标注

选区完成后进入轻量编辑器：

- 矩形
- 椭圆
- 箭头
- 画笔
- 马赛克
- 撤销 / 重做
- `Enter` 完成，`Esc` 取消

快捷键：

| 工具 | 快捷键 |
| --- | --- |
| 矩形 | `R` |
| 椭圆 | `O` |
| 箭头 | `A` |
| 画笔 | `P` |
| 马赛克 | `M` |
| 撤销 | `Ctrl + Z` |
| 重做 | `Ctrl + Y` |

### 输出

- 自动复制截图到剪贴板
- 自动保存 PNG 到 `图片/Snap-Lite/`
- `Ctrl + Shift + F` 全屏截图

### 贴图

- `F3` 将剪贴板图片贴到桌面
- 始终置顶
- 鼠标拖动移动
- 鼠标滚轮缩放
- `Ctrl + 滚轮` 调整透明度
- 右键快速设置透明度
- 双击关闭贴图

### 桌面应用能力

- 系统托盘常驻
- 单实例运行
- 原生 Win32 全局热键
- 无第三方 GUI 框架

## 1.0 必须完成的功能

这些属于核心功能，不会为了“轻量”而删除：

- [ ] 文字标注
- [ ] 马克笔 / 高亮笔
- [ ] 高斯模糊
- [ ] 橡皮擦
- [ ] 颜色、线宽、字体大小可调
- [ ] 截图历史回放
- [ ] 贴图旋转 / 镜像翻转
- [ ] 贴图鼠标穿透
- [ ] 贴图缩略图模式
- [ ] 贴图重新进入标注
- [ ] 自定义全局快捷键
- [ ] 设置页：自动保存、保存目录、文件名规则
- [ ] 开机启动
- [ ] 更完善的 UI 元素识别（包含非 Win32 控件）

OCR、云同步、账号系统等重功能默认不进入核心版本。

## 技术实现

```text
C++17
Win32 API
GDI / GDI+
Windows Shell API
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
轻量标注编辑器
 ↓
复制剪贴板 + PNG 保存
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
├── CMakeLists.txt
├── README.md
└── src/
    ├── main.cpp
    ├── app.h / app.cpp
    ├── capture.h / capture.cpp
    ├── snip_window.h / snip_window.cpp
    ├── editor_window.h / editor_window.cpp
    └── pin_window.h / pin_window.cpp
```

## 设计原则

1. Windows 11 优先，不为老系统兼容增加大量分支。
2. 原生 API 优先，不为了 UI 引入大型运行时。
3. 高频截图 / 标注 / 贴图功能必须完整。
4. 非核心功能按插件化或可选能力扩展，避免常驻进程越来越重。
