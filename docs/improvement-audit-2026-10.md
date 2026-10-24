# Snap-Lite 改进审计报告（2026-10）

> 由 6 个独立子 agent 分工审计产出：架构与代码生成管线 / 性能与资源 / 健壮性与多屏 DPI /
> 竞品功能差距 / UX 与无障碍 / 测试与发布工程化。本报告做了交叉验证、去重与优先级排序。

## 修复记录（本轮已落地）

审计结论已经过 6 轮「子 agent 独立验收」，每轮验收的失败项都在下一轮修掉。
下表是**已合入工作树并通过构建 + 交互测试**的改动。

### 正确性 / 架构

| 项 | 改动 | 文件 |
|---|---|---|
| P0-1 | **消除双份 `snip_window.h`**。此前 `src/app.cpp` 引号包含命中仓库版（`Tool` 无 `Eraser`，Mosaic=4），编译单元命中生成版（有 Eraser，Mosaic=5）—— 同一二进制里两份定义，枚举错位。新写法直接落在 `src/snip_window.h`，生成副本与仓库副本**逐字节一致**，并由校验脚本与单测守住 | `src/snip_window.h`、`scripts/apply_eraser_history_patch.py`、`scripts/verify_generated_sources.py` |
| P0-1b | 删掉 `scripts/optimize_bitmap_history.py`。它整段替换 `BitmapHistory` 类，把 `apply_eraser_history_patch.py` 写的 512MB 上限**完全覆盖丢弃**（实际生效是 64MB），现在策略直接写在源码里 | — |
| P0-2 | **改源码不触发重新生成**已修：8 个脚本 + 12 个模板源文件全部列入 `CMAKE_CONFIGURE_DEPENDS` | `CMakeLists.txt` |
| P0-4 | **OCR 降采样被自己废掉**：`scale = std::max(1.0, scale)` 把上限收敛抵消，长边 >2600px 的选区必然报"识别失败"。改为连续的 `min(3.0, 2560/sourceMax)`，长边恒 ≤2560 | `src/ocr_v2.cpp` |
| P1-19 | 生成脚本里唯一的未检查正则（`re.sub`）改为"配对块恰好命中一次"**且**两个函数符号各恰好出现一次，堵死静默失效 | `scripts/generate_unified_ui_sources.py` |
| — | 删除死代码 `src/ocr.cpp` / `src/ocr_result_panel.cpp` / `src/ocr_winrt_compat.h`（v2 是能力超集），并加入 CI 守卫防止复活 | `src/`、`build.yml` |

### 性能

| 项 | 改动 | 文件 |
|---|---|---|
| P0-3 | **OCR 全量异步化**。此前 `std::async(...).get()` 等于同步，4K 选区 UI 冻结 2–5 秒。现在预处理、落盘、识别全在后台线程，完成后经消息专属窗口回到 UI 线程回调 | `src/ocr.h`、`src/ocr_v2.cpp`、`scripts/add_ocr_action.py` |
| P1-2 | **合成图缓存**。`UiCaptureBitmap()` 此前每次都整屏拷贝（4K 约 33MB），文字标注每敲一个键都要重来一遍。缓存键 = (editRevision, contentRevision, 选区矩形)，并由**每个真正写像素的路径**各自记一次 `contentRevision_`（见下方第四批） | `src/snip_window.h`、`src/snip_window.cpp`、`src/snip_window_original.inc` |
| P1-3 | **橡皮笔画的 GDI churn**。`RestoreOriginalBrush` 此前每个采样点新建 3 个 Region（400px 笔画约 1200 次创建/销毁）。改为笔画级复用：椭圆用 `OffsetRgn` 平移，剪裁区每轮重算 | `scripts/apply_eraser_history_patch.py` |
| P1-4 | **贴图缩放整幅重采样**。每次滚轮刻度都整幅 `StretchBlt`，且 `SetStretchBltMode(HALFTONE)` 设在了目标 DC 上（按 MSDN 应取自源 DC），实际未生效。新增缩放位图缓存，`SetStretchBltMode` 设在源与目标两个 DC 上并改用 `COLORONCOLOR`（截图放大后要看到原始像素，HALFTONE 会把像素平均掉） | `src/pin_window.h`、`src/pin_window.cpp` |

### UX / 健壮性

| 项 | 改动 | 文件 |
|---|---|---|
| P1-15 | 剪贴板被占用时复制不再**静默失败**，改为重试 8×15ms；OCR 面板复制失败给出提示 | `src/capture.cpp`、`src/ocr_result_panel_v2.cpp` |
| P1-16 | 关闭贴图后再按 F3 不再毫无反应，改为提示"这张图片刚刚已经贴过并关闭了" | `src/pin_window.h`、`src/app.cpp` |
| P1-18 | 第二次启动不再静默退出，改为通知已有实例弹提示 | `src/app.cpp` |
| P1-28 | OCR 面板打开后 Esc 失效：给文本框装子类截获 ESC，关闭时把焦点还给截图窗口 | `src/ocr_result_panel_v2.cpp` |
| P1-29 | 热键注册失败不再只靠会被吞掉的气泡，新增托盘菜单「快捷键说明...」，列出全部快捷键并标出失效项 | `src/app.cpp` |
| P1-30 | 工具栏第二行（形状/线宽/字号/**色块**）此前完全没有悬停提示，用户面对一排无名色块；现在补上，并为 7 个色块给出颜色名 | `scripts/add_toolbar_tooltips.py`、`src/editor_toolbar.cpp` |
| — | 退出时清理残留的 `%TEMP%\SnapLite-OCR2-*.png` | `src/app.cpp` |

### 第四批：第三轮验收（VERDICT: FAIL）失败项的修复

第三轮验收独立复核时发现上一批引入/遗留的问题，全部已修，并用**变异测试**验证新测试确实能抓住对应缺陷。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| **高** | **延时截图覆盖层释放后使用**。`FinishOverlay` 用全局 `g_state` 释放状态，而 `Cancel()` 异步投递消息、`Start()` 立刻建新窗口。连续触发两次延时截图时，旧窗口的取消消息晚于新窗口创建才被处理，于是 `delete g_state` 删掉的是**新实例**的状态：倒计时冻结、幽灵窗口、`WM_PAINT` 读已释放内存 | 状态改绑到窗口自己的 `GWLP_USERDATA`（`TakeOverlayState()` 取并清空），全局指针只在"指向的就是本窗口状态"时才清；`Cancel()` 改为同步 `DestroyWindow`；`WM_DESTROY` 兜底释放 | `src/delay_overlay.cpp` |
| **中** | **合成图缓存键覆盖不全**。缓存键是 (editRevision, contentRevision, 选区)，但画笔/马赛克逐段写 `capture_`、橡皮写回原始像素并移除标注、拖动取消还原标注——都不走 `BeginEdit`，键不变。笔画中途重建的"半成品"会被缓存下来并长期显示 | 在**每个真正写像素/写标注的地方**记一次 `contentRevision_`（`DrawPenSegment`、`ApplyMosaic`、`FinishArrowDrag`、`EraseArrows`、橡皮注入的两处）；另加兜底：绘制/拖动进行中一律不复用缓存 | `src/snip_window_original.inc`、`src/snip_window.cpp`、`scripts/apply_eraser_history_patch.py` |
| **中** | **OCR 结果张冠李戴**。在飞标志只比 HWND，而 HWND 会被系统回收复用——旧截图窗口销毁后新窗口可能拿到同一句柄，旧识别结果会挂到新窗口，还会提前清掉新窗口的在飞标志 | 每个窗口实例发进程内唯一序号，以窗口属性 `SnapLite.InstanceId` 挂在 HWND 上；`OcrRequest` 带 `instanceId`，回调同时校验 HWND 与实例序号 | `src/snip_window.h`、`src/ocr.h`、`src/ocr_v2.cpp`、`scripts/add_ocr_action.py` |
| **中** | **第二实例退出误杀主实例的识别**。`Shutdown()` 无条件清理 `%TEMP%\SnapLite-OCR2-*.png`，而第二实例退出也会走 `Shutdown()`，会把主实例**正在识别**用的临时文件删掉 | 只有抢到单实例互斥量的进程（`primaryInstance_`）才清理 | `src/app.cpp`、`src/app.h` |
| **中** | **倒计时覆盖层被截进最终截图**。归零后立刻通知截图，此时覆盖层可能还在屏幕上，或桌面尚未重绘，截出的图顶部中央会多一个数字框 | 先销毁覆盖层 → 强制桌面重绘 → 等待 → 再通知截图 | `src/delay_overlay.cpp` |
| 低 | 覆盖层启动瞬间用户正按住 Esc 会被立刻取消；`Cancel()` 在窗口失效时泄漏状态 | Esc 边沿检测改为实例成员并在创建时取初值；`Cancel()` 兜底释放状态 | `src/delay_overlay.cpp` |
| 低 | 取色、OCR 面板复制仍是"一次失败就放弃"，剪贴板被占用时静默失败 | 统一走 `snaplite::OpenClipboardWithRetry()`（提升到命名空间并在 `capture.h` 声明） | `src/capture.cpp`、`src/capture.h`、`src/ocr_v2.cpp` |
| 低 | 贴图失败提示把"剪贴板被占用"和"没有图片"混成一句话 | `PinWindow::CreateFromClipboard` 改返回 `PinClipboardResult` 枚举，调用方分别提示 | `src/pin_window.h`、`src/pin_window.cpp`、`src/app.cpp` |
| 低 | `src/app.cpp` 局部变量 `message` 遮蔽函数参数（C4457） | 三处统一改名 | `src/app.cpp` |

**新增回归测试**：
- `tests/snip_interaction_test.cpp`：新增「拖动中途重建 + 取消后必须失效」「笔画中途重建」「擦除途中重建」「笔画后再加箭头」等断言。已用变异测试确认——摘掉 `FinishArrowDrag` 的版本号，第 24 项断言失败；摘掉 `DrawPenSegment` 的版本号，第 40 项断言失败。
- `tests/test_build_scripts.py`：新增 `SourceGuardTest`（8 项源码级守卫），把上面这些结构性不变量钉死在测试里：延时覆盖层状态绑定、Cancel 同步销毁、销毁先于截图、第二实例清理门槛、缓存键覆盖每个写像素路径、绘制期间不复用缓存、OCR 实例序号、剪贴板重试统一入口。

### 第五批：第四轮验收（VERDICT: FAIL）失败项的修复

第四轮验收做了 21 组源码级守卫变异 + 6 组 C++ 变异 + 一个独立的窗口属性探针程序，结论是 19 项声称修复中 18 项成立，但抓到一个**未覆盖的截图路径**，另有 3 项测试/校验质量问题。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| **中** | **延时倒计时期间走普通截图路径，覆盖层仍会被拍进图里**。`DelayOverlay::Cancel()` 全仓唯一调用点是 `Start()` 内部，而 `App::StartSnip()` 的三个入口（F1 / 托盘图标 / 托盘菜单「截图」）都不取消覆盖层。覆盖层不是分层窗口，`BitBlt + CAPTUREBLT` 必然把它拍进去 → 截图顶部中央多一个倒计时数字框；倒计时归零后还会再触发一次截图 | `App::StartSnip()` 与 `App::CaptureFullscreen()` 入口先调 `DelayOverlay::Cancel()`；`Cancel()` 与 `FinishOverlay()` 统一走 `DestroyOverlayAndWaitForRepaint()`（销毁 → 作废该区域 → 等桌面重绘），确保覆盖层消失之后才截图 | `src/app.cpp`、`src/delay_overlay.cpp` |
| 低 | OCR 实例序号守卫只查标识符**存在**，不查**比较语义**——把 `if (ReadSnipInstanceId(...) != request.instanceId)` 改成 `if (false)` 守卫仍然通过 | 语义断言改到**生成产物**上（`build/editor_toolbar_anime.cpp` 是真 C++ 文本），要求那几段比较实际在 | `tests/test_build_scripts.py` |
| 低 | `verify_generated_sources.py` 用子串匹配，`PrimaryAction::Ocr` 被改名成 `PrimaryAction::OcrX` 仍能通过 | 必需片段改为**完整标识符**匹配（片段后面不能再跟标识符字符） | `scripts/verify_generated_sources.py` |
| 低 | 「擦除途中重建」断言没有鉴别力：橡皮笔画开始时 `BeginEdit()` 已改 `editRevision_`，且断言位置在重建之前就已被擦掉 | 新增一条鉴别力断言：断言落在**中途重建之后才被擦掉**的那一段上（先画笔画一条，再一次连续橡皮拖动中途重建，最后断言后半段已被擦回原始像素） | `tests/snip_interaction_test.cpp` |
| 信息 | 贴图路径还有一份自己的 0 参数 `OpenClipboardWithRetry()`，与「统一入口」的说法不完全一致 | 删除本地实现，贴图改走 `capture.h` 里的统一入口 | `src/pin_window.cpp` |

**变异验证**：摘掉橡皮路径的**全部三处**版本号（`EraseArrows` + 两处注入）后重跑交互测试 → 第 46 项断言 `erase after mid-erase rebuild reaches the composite` 失败，证明新断言确实能抓住橡皮路径的缓存漏洞。

**新增源码级守卫**（共 11 条）：延时覆盖层状态绑定、`Cancel` 同步销毁、销毁助手必须等重绘、销毁先于截图、**普通截图路径必须取消倒计时**、第二实例清理门槛、缓存键覆盖每个写像素路径（含文字烘焙）、绘制期间不复用缓存、OCR 实例序号（源码 + 产物语义）、剪贴板重试统一入口（含贴图）。

### 第六批：第五轮验收（VERDICT: PASS）遗留的低危项

第五轮验收对 M1/L1/L2/L3/L4 五项修复全部复核通过（**VERDICT: PASS**），另留下 3 条低危 + 2 条信息项，均已处理：

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 低 | **M1 守卫对顺序不敏感**：只断言 `DelayOverlay::Cancel()` 出现在函数体里，把它挪到抓屏之后守卫照样通过，缺陷会原样复现 | 守卫改为断**顺序**：`Cancel()` 的位置必须早于 `SnipWindow::Start(` / `CaptureVirtualScreen()` | `tests/test_build_scripts.py` |
| 低 | 守卫硬编码局部变量名 `DestroyOverlayAndWaitForRepaint(overlay)`，把变量改名就误报 | 改为只断言"销毁助手被调用"，不写死变量名 | `tests/test_build_scripts.py` |
| 低 | **延时截图在已有截图窗口时静默失效**：截图窗口是独占的，到点时新截图会被"已有窗口就返回"的早退吃掉，用户对着空气等 | 已有截图窗口时明确拒绝并提示；`App::StartSnip()` 的取消倒计时提到早退**之前**，这样按 F1 也会把浮在标注窗口上的覆盖层收掉 | `src/app.cpp` |
| 信息 | `contains_token()` 只约束尾部标识符边界，前缀改名（`XPrimaryAction::Ocr`）仍能通过 | 前后边界都约束 | `scripts/verify_generated_sources.py` |

**变异验证**：把 `Cancel()` 挪到抓屏之后 → 守卫 FAIL；把局部变量改名 → 守卫 PASS（无误报）；产物里 `PrimaryAction::Ocr` 改成 `XPrimaryAction::Ocr` → 产物校验器 exit=1。

**第五轮验收的独立证据**：验收方写了一个独立 Win32 探针，实测「覆盖层确实会被拍进屏幕」（红色像素 1030 → 38464）、且 `DestroyWindow` 后像素**立即**恢复（回到 1030），确认 M1 的根因判断成立、修法方向正确（`Sleep(120)` 属于保险而非唯一机制）。

### 第七批：第六、七轮验收（VERDICT: PARTIAL / FAIL）的失败项

第六轮验收（VERDICT: **PARTIAL**）指出：四项声称修复都真实成立，但**本轮自己写下的守卫**只部分满足"改坏源码必须失败、无害重排必须仍然通过"。第七轮验收（VERDICT: **FAIL**）另找到 2 项中危的构建管线完整性缺陷。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 中 | **F3 守卫重蹈 F1 的覆辙：对顺序不敏感**。把整块拒绝逻辑挪到 `DelayOverlay::Start(` 之后，守卫照样 PASS —— 该变异体 `/W4 /permissive-` 零警告编译通过，是可真实发生的回归 | 守卫改为断**顺序**：拒绝必须早于启动倒计时；并断言拒绝块里真的有 `return` | `tests/test_build_scripts.py` |
| 中 | **守卫纯文本化，不验证调用可达性**。注释里写一句 `// DelayOverlay::Cancel();` 就能满足"必须包含"；`if (false) { ... }` 里的调用根本不会执行 | 新增 C++ 源码分析工具：剥离注释、容忍空白与换行、死分支检测、函数体第一层判定、被抽进 helper 时仍能解析到调用点 | `tests/test_build_scripts.py` |
| 低 | **无害重排被误报**：把一行拆成两行、注释里提到 `PostMessageW`、把 `ShowNotice` 换成等价的间接调用、把关键调用包进 helper —— 四种都判失败 | 令牌匹配改为空白不敏感；`assertNotIn` 只扫代码不扫注释；提示断言放宽为"用户可见反馈"启发式；helper 包装由 `resolve_calls` 解析 | `tests/test_build_scripts.py` |
| 低 | **守卫自己写死了窗口类名**：真源改了而调用点没改，守卫拿着同一个旧字面量一起去"通过"，缺陷完全隐形 | 守卫从真源（`src/snip_window_original.inc`）派生类名，真源漂移立刻报错 | `tests/test_build_scripts.py` |
| 中 | **仓库里的 `src/snip_window.cpp` / `src/pin_window.cpp` / `src/ocr_result_panel_v2.cpp` 也是生成模板**，而且仓库版 `snip_window.cpp` 的工具索引是**过期且语义颠倒**的（橡皮↔马赛克↔文字整体错位一格），编译期完全静默 | 产物校验新增工具索引与橡皮实现的语义断言；新增守卫钉死"CMake 编译的必须是生成副本、不能直接编译仓库模板" | `scripts/verify_generated_sources.py`、`tests/test_build_scripts.py` |
| 中 | **`verify_generated_sources.py` 没有接进本地构建**：唯一受支持的本地入口 `build_local.ps1 -Tests` 完全绕开它，"本地绿 / CI 绿 / 产物错"可以同时发生 | 产物校验与 Python 守卫都注册成 ctest 用例，`build_local.ps1 -Tests` 与 CI 走同一套 | `CMakeLists.txt` |
| 低 | **27 条源码级守卫只在 CI 跑**，本地 `ctest` 只有 1 个用例 | 同上，注册成 ctest 用例 | `CMakeLists.txt` |
| 低 | **`App::Shutdown()` 不取消延时倒计时覆盖层**：退出时覆盖层会在进程结束前留在屏幕上闪一下 | 退出时先 `DelayOverlay::Cancel()`，并加守卫 | `src/app.cpp`、`tests/test_build_scripts.py` |
| 低 | **OCR 临时文件名不保证唯一**：`GetTickCount64()` 的分辨率约 15.6ms，同一 tick 内的两次请求会算出同一个路径，两个后台线程互相覆盖 / 删除同一批 PNG（当前基本不可达，属隐患） | 文件名里补一个进程内原子自增序号 | `src/ocr_v2.cpp` |
| 低 | **`SetWindowRgn` 失败路径泄漏 HRGN**（2 处：`ui_theme.h`、`ocr_result_panel_v2.cpp`） | 照抄 `pin_window.cpp` 的写法：成功后交给系统、失败才 `DeleteObject` | `src/ui_theme.h`、`src/ocr_result_panel_v2.cpp` |
| 中 | **预发布版本号必崩**：Inno 的 `VersionInfoVersion` 只接受点分数字，`1.6.0-rc.1` 会让安装包在最后一步编译失败——那时 exe 已构建、CI 时间和 token 都花光了 | `VersionInfoVersion` 改用独立的 `MyAppVersionInfo`（纯数字），带后缀的版本继续用在字符串字段 | `installer/SnapLite.iss`、`.github/workflows/release.yml` |
| 中 | **`.release/` "取最后一个变更文件"会选中被删除的说明文件**：发布说明常是"删旧加新"，`--name-only` 把删除也算进来，按字节序取最后一个可能选中已删文件 → 拿到旧版本号，用旧 tag 重新发布并 `--clobber` 覆盖已发布产物 | 只取**新增**文件（`--diff-filter=A`）；文件名严格匹配；版本解析抽成纯函数 `scripts/resolve_release_version.py`，用 Python 单测覆盖输入矩阵 | `.github/workflows/release.yml`、`scripts/resolve_release_version.py` |
| 中 | **`SetProcessDpiAwarenessContext` 返回值被丢弃**：失败后进程被系统虚拟化，选区、命中半径、贴图尺寸整体错位，且完全静默 | 逐级回退（PMv2 → PM aware → `SetProcessDPIAware`），失败时用托盘气泡明确告知用户（不阻塞） | `src/app.cpp`、`src/app.h`、`src/main.cpp` |

**延时覆盖层第一次有了运行时覆盖**（此前 `delay_overlay.cpp` 与 `app.cpp` 零运行时测试，只有源码级静态守卫）：

- 新增 `tests/delay_overlay_test.cpp`（19 项断言）：覆盖层可见时它自绘的颜色确实落在屏幕上；**App 收到 `WM_SNAPLITE_DELAY_ELAPSED` 的那一刻（也就是它抓屏的同一刻）覆盖层像素必须已经不在屏幕上**；`Cancel()` 必须同步销毁窗口；重复启动不能留下两个覆盖层；没有覆盖层时 `Cancel()` 不能崩。
- 判定用的是"覆盖层贡献了多少像素"而不是"是不是绝对为 0"：本机桌面有 300 多个活动窗口，纯像素差分会出现 21277 像素的假阳性。
- **变异验证**：去掉 `FinishOverlay` 里的"先销毁再通知" → 新断言失败；把 `Cancel()` 改成只投递消息 → "同步销毁"断言失败。

**第七批的守卫变异验证（14 项，全部符合预期）**：

| 变异 | 期望 | 实测 |
|---|---|---|
| `Cancel()` 挪到抓屏之后 | 拦截 | FAIL ✅ |
| 全屏路径 `Cancel()` 挪到抓屏之后 | 拦截 | FAIL ✅ |
| 拒绝块挪到 `DelayOverlay::Start` 之后 | 拦截 | FAIL ✅ |
| 注释诱饵 `// DelayOverlay::Cancel();` | 拦截 | FAIL ✅ |
| 提示但不 `return` | 拦截 | FAIL ✅ |
| `Cancel` 藏进 `if (false)` | 拦截 | FAIL ✅ |
| 销毁调用藏进 `if (false)` | 拦截 | FAIL ✅ |
| 真源类名漂移 | 拦截 | FAIL ✅ |
| 局部变量改名 | 放行 | PASS ✅ |
| 把关键调用包进 helper | 放行 | PASS ✅ |
| 提示换成等价的间接调用 | 放行 | PASS ✅ |
| 注释里提到 `PostMessageW` | 放行 | PASS ✅ |
| 把一行拆成两行 | 放行 | PASS ✅ |

### 第八批：第八轮验收（VERDICT: FAIL / PARTIAL）的失败项

第八轮两个独立验收方分别给出 **FAIL**（4 高 + 6 中 + 4 低）与 **PARTIAL**（7 项）。核心结论是：上一批写下的守卫**大量只断言"字符串出现过"**，把关键调用改指别处、把提示挪出 `if`、把计数器挪出文件名，守卫统统放行。修法统一为**断关系而不是断存在**：断变量的取值落点、断调用所在的语句、断提示所在的 `if` 块、断释放所在的失败分支。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **CMake 守卫对"变量改指模板"完全无感**：把 `SNAPLITE_SNIP_SOURCE` 改回 `src/snip_window.cpp`，守卫照样 PASS —— 而仓库那份模板的工具索引是错位的，编译期一声不吭 | 守卫解析 `set()` 取值并逐层展开 `${}`，要求结果落在 `CMAKE_CURRENT_BINARY_DIR` | `tests/test_build_scripts.py` |
| 高 | **`SetWindowRgn` 守卫不查释放是否在失败路径**：把 `DeleteObject` 挪到成功路径也算通过 | 要求 `DeleteObject(region)` 所在语句的**最内层** `if` 条件里含 `SetWindowRgn` | `tests/test_build_scripts.py` |
| 高 | **OCR 守卫不查计数器是否真的进了文件名** | 要求计数器出现在含 `name +=` 的那条语句里 | `tests/test_build_scripts.py` |
| 高 | **DPI 守卫把 `ShowNotice` 与 `if (!dpiAware_)` 解耦**：提示挪到函数末尾照样通过 | 提示必须落在 `if (!dpiAware_)` 块内；三级回退的**顺序**也要对 | `tests/test_build_scripts.py` |
| 中 | **`code_pattern` 空括号不容错**：3 个关键 needle（`Cancel()` 等）误报 | 匹配器改为令牌化：标识符、字符串字面量整体、单个标点逐个可插空白；`find_calls` 跳过字符串内部（含 raw string） | `tests/test_build_scripts.py` |
| 中 | **`primaryInstance_` 极性翻转漏抓** | 守卫显式拒绝 `!primaryInstance_` | `tests/test_build_scripts.py` |
| 中 | **`Shutdown` 守卫不查无条件性/位置** | 要求相对深度为 1（不在任何 `if` 内），且早于资源拆除 | `tests/test_build_scripts.py` |
| 中 | **`parse_base_version` 因 `^` 未开 `re.MULTILINE` 100% 失效** | 正则加 `re.MULTILINE`，补单测 | `scripts/resolve_release_version.py` |
| 中 | **产物校验器不剥注释**：注释里出现禁用项会假阳性，该出现的要求项在注释里会假阴性 | 校验器自带 `strip_comments`，必需项与禁用项统一剥注释后判断 | `scripts/verify_generated_sources.py` |
| 中 | **发布工作流一个测试都不跑** | `release.yml` 加 `-DSNAPLITE_BUILD_INTERACTION_TESTS=ON` 与 `Run tests` 步骤 | `.github/workflows/release.yml` |
| 低 | **版本比较是字符串前缀**：`1.6.01` 会被当成 `1.6.0` 的后继 | 改按版本分量比较，`numeric_parts` / `same_release_line` | `scripts/resolve_release_version.py` |
| 低 | **`.inc` 橡皮断言只到标识符级** | 补 `++contentRevision_;` 等实现级断言 | `scripts/verify_generated_sources.py` |

**第九批：把"守卫自己会死循环"这件事修掉**（本轮由自建变异测试暴露，非子 agent 报告）：

第 8 批的 19 项守卫变异脚本**第一次真正跑完**，立刻暴露了一个此前无人发现的严重缺陷：

> 守卫的源码分析函数 `functions()` 里有一处 `continue` **没有推进下标**。当源码里出现
> `for (auto& x : {1, 2, 3}) {` 这种"块头括号不配平"的构造时，分析会在同一个 `{` 上
> **永远转下去**。

后果比"守卫误报"严重得多：**守卫最需要报错的场景（关键调用被删掉）恰好会走进这条分支**——CI 上看到的不是一条"退出时必须取消延时倒计时覆盖层"的失败信息，而是一个**跑到超时的 job**。实测该分支单次就能烧掉 500+ 秒 CPU。基线之所以"一直绿"，只是因为基线里关键调用还在，`resolve_calls` 提前返回、根本没调用到 `functions()`。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **守卫在源码被改坏时死循环**，CI 变成超时 job 而不是失败 | `functions()` 的每条分支都推进下标；再加步数预算，超预算立刻报"分析逻辑没有推进下标" | `tests/test_build_scripts.py` |
| 中 | 修复没有回归保护 | 新增 `AnalysisHelperTest`：块头括号不配平时必须终止；关键调用被删时必须返回空列表；抽成 helper 时仍要能找到调用点 | `tests/test_build_scripts.py` |
| 中 | **延时覆盖层的重绘等待靠"睡固定 120ms 去赌"**：机器繁忙时那点时间经常不够，截图就把覆盖层拍进去了 | 改成有上限的轮询：覆盖层出现前先记下该区域的**基线**，销毁后盯着那块区域的像素，覆盖层自己的颜色退干净才算完（上限 800ms） | `src/delay_overlay.cpp` |
| 中 | **逐像素读位图每次要建一个临时 DC**：扫一块 148×104 的区域要建三万次 | 新增批量接口 `ReadBitmapPixels`，一次 `GetDIBits` 读整块；测试耗时从 3.05s 降到 2.28s | `src/capture.h`、`src/capture.cpp` |
| 中 | **延时覆盖层测试不隔离**：别的进程（用户自己开着的延时截图、或上一轮没退干净的测试进程）举着的覆盖层，位置/大小/颜色与本测试的完全一样，量到的像素没有意义 | 测试枚举覆盖层窗口并带出属主 PID：起跑前若存在外来覆盖层就等 2 秒，仍在则**明确报出是谁挡着**；"覆盖层没了"的判定改为查**自己的 HWND** | `tests/delay_overlay_test.cpp` |

**这一轮的实际结论**：此前记录的"延时覆盖层测试 3/20、1/30 概率失败"，根因是**测试自身不隔离**——失败时 `whileVisible=14099` 与 `atNotify=14099` **完全相等**，且失败瞬间能枚举到一个可见的覆盖层窗口，说明量到的其实是**另一个进程**的覆盖层像素。复现与验证：直接跑 30 轮 + 完整 ctest 12 轮共 42 轮全绿；修复后新实现再跑 30 轮同样全绿。

**第八批的守卫变异验证（19 项，全部符合预期）**：

| 变异 | 期望 | 实测 |
|---|---|---|
| CMake 变量改指仓库模板 | 拦截 | FAIL ✅ |
| CMake 注释里提到模板路径 | 放行 | PASS ✅ |
| `Shutdown` 的 `Cancel` 挪进 `if` | 拦截 | FAIL ✅ |
| `Shutdown` 的 `Cancel` 挪到末尾 | 拦截 | FAIL ✅ |
| 删掉 DPI 失败提示 | 拦截 | FAIL ✅ |
| DPI 回退顺序颠倒 | 拦截 | FAIL ✅ |
| 释放藏进 `if (false)` | 拦截 | FAIL ✅ |
| 无条件释放 region | 拦截 | FAIL ✅ |
| 计数器不进文件名 | 拦截 | FAIL ✅ |
| 计数器只留注释 | 拦截 | FAIL ✅ |
| `primaryInstance` 极性翻转 | 拦截 | FAIL ✅ |
| 类名尾随空格 | 拦截 | FAIL ✅ |
| 字符串字面量诱饵 | 拦截 | FAIL ✅ |
| 原始字符串诱饵 | 拦截 | FAIL ✅ |
| 空括号拆行 | 放行 | PASS ✅ |
| 清理调用空括号拆行 | 放行 | PASS ✅ |
| 抓屏调用空括号拆行 | 放行 | PASS ✅ |
| 提示函数改名 | 放行 | PASS ✅ |
| 拒绝块抽成 helper | 放行 | PASS ✅ |

### 第十批：第 8 轮第三份验收（VERDICT: PARTIAL）的失败项

第三份验收方（运行时测试与残留缺陷复审）给出 **PARTIAL**，A 部分（测试有效性）结论成立，**卡住 PASS 的是 CI 保护缺失与几处真实的中低危问题**。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **CI 无头会话没有跳过保护**：GitHub `windows-latest` 跑在会话 0 的服务 window station，那里没有可抓屏的交互式桌面。抓屏**不会失败**（返回黑图），所以"抓不到就跳过"的保护无效，像素数会是 0，断言照样红——**同时挡住 PR 与发布** | 测试开头探测 window station 是否 `WSF_VISIBLE`；没有就以约定退出码 77 跳过，CMake 配 `SKIP_RETURN_CODE 77` | `tests/delay_overlay_test.cpp`、`CMakeLists.txt` |
| 中 | **测试判据依赖桌面历史状态**：验收方实测到**所有相关进程退出后，屏幕上仍留着 14099 个覆盖层同色像素**；同一个二进制会因此先 PASS 后 FAIL | 测之前先强制整屏重绘把那块区域"擦干净"并记基线；擦不干净就**跳过像素断言**（环境问题不算产品缺陷），窗口生命周期的断言照跑 | `tests/delay_overlay_test.cpp` |
| 中 | **撤销历史的 PNG 冷存储在 UI 线程同步编码**：GDI+ 编一张 4K 位图要 **746ms**、1920×1080 也要 **199ms**——每落一笔就冻结零点几秒，是本轮引入的用户可感知回归 | 压缩搬到后台线程：UI 线程保留原图、只把**副本**交给后台编码，编好再换回 PNG；`take_back()` 在条目正在被压缩时仍能拿到可用的位图 | `src/snip_window.h` |
| 中 | **逐像素读位图每次要建一个临时 DC**：扫一块 148×104 的区域要建三万次，测试慢到 3.05s | 新增批量接口 `ReadBitmapPixels`（一次 `GetDIBits` 读整块），测试耗时降到 2.2s | `src/capture.h`、`src/capture.cpp` |
| 低 | **`SetWindowRgn` 失败路径漏改一处**：`src/editor_toolbar.cpp:1617` 仍是 `if (region) SetWindowRgn(...)`，失败时泄漏 HRGN | 与其余四处保持一致：失败才 `DeleteObject` | `src/editor_toolbar.cpp` |
| 低 | **OCR 临时文件清理不按 PID 过滤**：单实例互斥量是 `Local\`（按会话），跨会话的两个"主实例"里一个退出会删掉另一个**正在识别**用的 PNG | 文件名里本就带 PID：属于自己 PID 的删、属于**已不存在**进程的删、属于**还活着**的别的进程的不动 | `src/app.cpp` |
| 低 | **OCR 线程创建抛异常会终止进程**：`std::thread` 构造失败抛 `std::system_error`，调用方没有 try/catch → 整个截图程序 `std::terminate` | 包 try/catch，失败时释放位图并返回 false | `src/ocr_v2.cpp` |
| 低 | **`take_back()` 解码失败会静默卡死撤销**：解码失败时条目不弹出，之后每次 Ctrl+Z 都失败且永远回不去 | 解码失败也弹出条目，让撤销栈往前走 | `src/snip_window.h` |
| 低 | **覆盖层 DPI 与落位取自不同来源**：DPI 取自落在 (0,0) 的隐藏主窗口，位置却用 `MonitorFromWindow(..., MONITOR_DEFAULTTOPRIMARY)`（0×0 窗口与任何显示器都没交集 → 退回主显示器） | 两边都锚在 (0,0) 所在的显示器：DPI 用 `GetDpiForWindow(notifyWindow)`，落位用 `MonitorFromPoint({0,0}, ...)` | `src/delay_overlay.cpp` |
| 低 | **`release.yml` 的注释与实际行为不符**：注释说"测试红不会挡住发布"，实际同一个 job 内 ctest 红会跳过后续打包与发布 | 改正注释：测试红**会**挡住发布，这正是想要的 | `.github/workflows/release.yml` |

**第十批里还挖出两个自己写的 bug**（都是被新加的断言当场抓住的）：

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 中 | **后台压缩会把同一条目排两次队**：条目在后台压完之前一直留着原始位图，看起来仍然"可以压"，于是被反复排队；而结果表以条目 id 为键——**后一个结果把前一个覆盖掉**，在途计数再也降不下来（表现是撤销历史永远压不完） | 条目加「已排队」标记，排队时置位、结果回来时清掉 | `src/snip_window.h` |
| 中 | **测试按类名找覆盖层，会拿到别的进程的窗口**：`FindWindowW` 返回的是**第一个**同类窗口。这台机器上完全可能有另一个 SnapLite 实例正在倒计时（用户开着的、或另一个测试进程正在跑），它的窗口位置/大小/颜色和本测试的一模一样 | 枚举时带出属主 PID，所有窗口断言只算**本进程自己的**覆盖层；别人的存在时跳过像素断言并打 WARN | `tests/delay_overlay_test.cpp` |

**顺带一条教训**：验证脚本/验收 agent **不要和主线共用同一个构建目录的产物**。本轮就因为验收 agent 正在跑 `build\SnapLiteDelayOverlayTests.exe`，主线的链接步骤直接 `LNK1104` 失败——验收方应该在自己的 `build_v*/` 目录里构建和运行。

**这一轮还暴露了一个此前无人发现的严重缺陷**（由自建变异测试第一次真正跑完时暴露）：

> 守卫的源码分析函数 `functions()` 里有一处 `continue` **没有推进下标**。当源码里出现
> `for (auto& x : {1, 2, 3}) {` 这种"块头括号不配平"的构造时，分析会在同一个 `{` 上
> **永远转下去**（实测单个变异就能烧掉 500+ 秒 CPU）。

后果比"守卫误报"严重得多：**守卫最需要报错的场景（关键调用被删掉）恰好会走进这条分支**——CI 上看到的不是一条"退出时必须取消延时倒计时覆盖层"的失败信息，而是一个**跑到超时的 job**。基线之所以"一直绿"，只是因为基线里关键调用还在，`resolve_calls` 提前返回、根本没调用到 `functions()`。已修（每条分支都推进下标 + 步数预算），并补了 `AnalysisHelperTest` 三条回归锁住。

**第十批验证结果**：干净构建 0 警告；ctest 4/4 通过（`snip_interaction` 246 项 / `delay_overlay` 25 项 / `build_scripts` 46 项 / `verify_generated_sources` 38 条）；延时覆盖层测试连续 30 轮全绿（2.2s/轮）；Python 单测 46 项通过；产物校验 38 条断言通过；图标与版本资源校验通过；第 8 批的 19 项守卫变异全部符合预期。

### 第十一批：第 9 轮验收（VERDICT: FAIL / PARTIAL）的失败项

第 9 轮三份验收报告分别给出 **FAIL**（发布链）、**FAIL**（运行时测试）、**PARTIAL**（守卫与撤销历史）。这一批修掉它们点名的缺陷。

#### 发布链（VERDICT: FAIL）

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **没有候选版本时静默回落到项目版本号**：一次"只改了旧说明文件"的提交也会触发发布，于是拿旧 tag 配 `--clobber` 覆盖已发布 release 的产物 | 回落到项目版本号必须**显式声明**（`--allow-base-fallback`）；push 路径一律响亮失败 | `scripts/resolve_release_version.py`、`.github/workflows/release.yml` |
| 高 | **`--diff-filter=A` 漏掉 `git mv` 改名**：改名在 diff 里是 `R100`，只认 `A` 时输出是**空列表**，版本解析于是"没有候选" | 改成 `--diff-filter=AR`（实测：改名时 `--name-only` 输出新路径） | `.github/workflows/release.yml` |
| 中 | **变更文件列表带 UTF-8 BOM 时首行被静默跳过** → 候选为空 → 回落旧版本号 | 读文件用 `utf-8-sig`，并逐行再剥一次 BOM | `scripts/resolve_release_version.py` |
| 中 | **版本选择规则有歧义**：多个说明文件时"随便挑一个" | 规则化：只保留与项目版本**同一条版本线**的候选；同一条线上有多个时**与项目版本完全相同**的优先；仍有多个才报错 | `scripts/resolve_release_version.py` |
| 中 | **`ParseOcrTempFilePid` 的 PID 位数不封顶**：`99999999999999` 在 DWORD 里**静默回绕**成别的 PID，文件归属因此判错 | 位数与数值都封顶，超限直接放弃 | `src/app.cpp` |
| 中 | **`OpenProcess` 打不开被当成"进程已死"**：受保护进程（System 等）是 `ERROR_ACCESS_DENIED`，**不是**"进程没了"——误判就会删掉别人正在用的临时 PNG | 改用进程快照枚举判定"这个 PID 在不在"，并引入 `kUnknown` 状态：判不出来就**不删** | `src/app.cpp` |
| 中 | **`worker.detach()` 在 try 里**：detach 抛异常时 catch 会 `DeleteObject(cropped)`，而 worker 里的 `RunOcrPipeline` **接管并释放**同一块位图 → double free；且 try 作用域结束时 worker 析构发现线程还 joinable → `std::terminate()` | detach 移出 try；万一抛异常改成 join（绝不去碰那块位图） | `src/ocr_v2.cpp` |
| 低 | **覆盖层落位与 DPI 不同源**：落位改用 `MonitorFromPoint` 后，注释声称"0×0 窗口与任何显示器都没交集"——**不成立**（系统会把 WS_OVERLAPPED 的 0×0 撑成约 136×39，矩形完整落在 (0,0) 所在屏上） | 回退到与 DPI 同源的 `MonitorFromWindow(notifyWindow)`，并把注释改成成立的理由 | `src/delay_overlay.cpp` |

#### 运行时测试（VERDICT: FAIL）

验收方用 10 组变异证明：**本轮的核心改动（"靠验证代替赌固定 120ms"）根本没有被运行时测试守住**——把整段等待摘掉，测试照样 PASS 25；把像素判定换成 `if (false)` 诱饵，也照样 PASS。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **等待逻辑本身没被守住**：所有像素断言都发生在"产品已经等完之后"，产品**完全不等**它们照样全绿 | 新增一条**确定性**断言：测试自己造一个盖住区域的顶层窗口（刷成覆盖层自己的面板色），问产品"退干净了吗"，它必须说"没有"，而且必须**等满超时**才回答。等待逻辑被摘掉时当场就红 | `src/delay_overlay.h`、`src/delay_overlay.cpp`、`tests/delay_overlay_test.cpp` |
| 高 | **CI 跳过靠单个未经验证的比特**：CI 上整段跳过等于零覆盖；反过来，"有可见桌面但抓不到画面"时像素数恒为 0，会把 PR 与发布一起挡住 | 用例**分两段**：窗口生命周期那段到处都能跑，像素与等待那段需要可抓屏的桌面；抓屏失败时**跳过像素断言**而不是判红 | `tests/delay_overlay_test.cpp` |
| 中 | **覆盖层颜色常量与产品分叉且无守卫**：产品改颜色，测试判据就失效，而"一直绿的失效判据"比没有判据更糟 | 颜色由产品提供（`DelayOverlay::SurfaceColour/BorderColour`），测试不再另抄一份 | `src/delay_overlay.h`、`tests/delay_overlay_test.cpp` |
| 中 | **"擦不干净就跳过像素断言"会把真回归一起放过** | 补一条**取消之后立刻**判残留的断言（测在"擦桌面"之前），环境脏不脏都成立 | `tests/delay_overlay_test.cpp` |
| 中 | **基线有没有采到从外面看不出来** | 产品暴露 `LastBaselineColours()`，测试直接断言基线确实采到了（采不到时产品会退回"睡固定时长去赌"） | `src/delay_overlay.h`、`tests/delay_overlay_test.cpp` |

**新增的两条运行时断言都做了变异验证**：摘掉等待逻辑（M4）→ 立刻红；不采基线（M5）→ 立刻红；等重绘抽成 helper、基线采集抽成 helper → 仍然放行（等价重构不该误报）。

#### 守卫与撤销历史（VERDICT: PARTIAL）

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **守卫锚点跟着改名一起失效**：`WaitForRegionRepainted` 改名后，守卫报的是**裸 traceback**（`code.index` 抛 ValueError），看不出是哪个锚点没了 | 锚点找不到时给**可读**的失败信息；改名后同步锚点 | `tests/test_build_scripts.py` |
| 高 | **撤销/重做会让位图栈与箭头栈失同步**：`take_back()` 拿不到位图时（条目只剩 PNG、而那张 PNG 解不开），旧代码已经把当前状态压进了 `redo_`/`arrowRedo_` 然后直接 return——位图栈少一条、箭头栈一条没少，之后每次撤销恢复出来的标注都是错位的 | **先取位图、再压对面的栈**：取不到位图就什么都不做，两栈同进同退 | `src/snip_window_original.inc` |
| 高 | **后台压出来的 PNG 从不验证**：PNG 解不开时，失败推迟到**用户按 Ctrl+Z 那一刻**才出现（表现为"撤销之后箭头还在"，且之后每次撤销都错位） | 后台线程在交出去之前**先解一遍**：解不开就整条丢掉，原始位图继续留着，撤销照常可用 | `src/snip_window.h` |
| 中 | **等重绘的守卫断的是"字符串存在"不是"关系"**：删掉超时上限判断、把超时判断挪进 `if (false && …)`、把像素判定换成恒假分支里的诱饵，守卫都还绿 | 改成关系化断言：像素计数的结果必须参与比较、比较的另一端必须是**参数**（基线）而不是写死的数字、超时分支里必须有 `return`、关键判断不能藏在恒假分支里 | `tests/test_build_scripts.py` |
| 中 | **基线守卫不抗 helper 抽取**：把基线采集抽成 `CaptureBaseline()` 会误报 | 用 `resolve_calls` 取代 `find_calls`（容忍"抽成 helper"这种等价重构） | `tests/test_build_scripts.py` |
| 中 | **步数预算本身没有测试** | 预算抽成 `ScanBudget` 类，并补一条"预算真的会触发"的回归 | `tests/test_build_scripts.py` |

**这一批的验证方式**：新写的 6 条定向变异（删超时上限 / 超时判断进恒假分支 / 像素判定换诱饵 / 固定阈值代替基线 / 等重绘抽 helper / 基线采集抽 helper）逐条跑过——前 4 条必须红，后 2 条必须放行，实测 **6/6 符合预期**。

**第十一批验证结果**：ctest 4/4 通过（`snip_interaction` 246 项 / `delay_overlay` 32 项 / `build_scripts` 58 项 / `verify_generated_sources` 38 条）；延时覆盖层测试连续 15 轮全绿（32 项/轮）；Python 单测 58 项通过；产物校验 38 条断言通过；图标与版本资源校验通过；交互测试连续 20 轮全绿。

**一条必须记住的教训**：用 `shutil.copy2` 还原被变异的源文件时，**mtime 会被一起还原成旧的**——ninja 于是认为"没变过"，不重新编译，跑出来的还是**带变异的旧二进制**。本轮有两次"守卫红了"其实是自己留下的陈旧二进制造成的假象，还原之后必须确认重新编译过（比对哈希或强制 touch）。

### 第十二批：第 10 轮验收（VERDICT: PARTIAL / FAIL）的失败项 + 用户实测反馈

第 10 轮三份验收报告：**PARTIAL**（发布链与守卫）、**FAIL**（延时覆盖层运行时）、**C++ 修复与并发**（验收 agent 失联，未产出报告）。这一批修掉前两份点名的缺陷，并吸收用户实测反馈的两条真问题。

#### 用户实测反馈（优先级最高）

用户在双屏环境实测发现两条：**测试程序"一直在死循环"，且"在桌面一测试居然能影响到桌面二"**。两条都不是错觉，根因是同一个：

| 现象 | 根因 | 修法 | 文件 |
|---|---|---|---|
| 桌面二跟着闪 | 桌面窗口**横跨所有显示器**。测试里的 `InvalidateRect(GetDesktopWindow(), nullptr, TRUE)` + `RedrawWindow(..., RDW_ERASE \| RDW_ALLCHILDREN)` 是**整块虚拟桌面**重绘 + 擦背景；产品的"催桌面重绘"同样带 `RDW_ERASE`，而且 800ms 上限内每 30ms 催一次（二十多次） | 催重绘一律**限定区域 + 不擦背景**（`erase=FALSE` + `RDW_NOERASE`），并**限频**到每 200ms 最多一次；测试里的"擦干净"也只重绘目标区域 | `src/delay_overlay.cpp`、`tests/delay_overlay_test.cpp` |
| "一直在死循环" | 验收 agent 与本线在**同一块交互式桌面**上反复跑交互/延时测试；每轮都会创建 TOPMOST 覆盖层窗口、整屏重绘，于是屏幕上反复闪 | 测试临时窗口全部由 RAII 收掉（异常路径也不会在屏幕上留面板）；验收 agent 不得与主线共用构建目录、不得反复跑交互用例 | `tests/delay_overlay_test.cpp` |

新增守卫 `test_desktop_repaint_never_erases_or_covers_the_whole_screen`：对桌面窗口的重绘请求，**区域必须非空、不能带擦除标志**，而且产品**确实催过**——只查实参的话，把两处调用整段删掉反而没人报警。

#### 运行时（VERDICT: FAIL）

第 10 轮验收用 19 组变异证明：上一轮"等待逻辑被守住了"的说法**只在 helper 层成立**——把产品路径上的等待整段摘掉（M1）、把超时改成 0（M1b）、基线不采却报 0（M5b）、基线写死 0（M5c），测试**全绿**。根因是"测试验的是一条旁路 helper 的性质，不是产品的行为"。

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **产品路径的"等重绘"可被整段摘掉而测试全绿** | 产品在 `SNAPLITE_DELAY_TEST_HOOKS` 下暴露**决策**（等了几次 / 等了多久 / 是不是等满了上限 / 是不是退回了睡固定时长 / 催了几次重绘）；测试造一块永远退不干净的区域（覆盖层启动**之后**再盖 TOPMOST 遮挡窗），断言产品**确实等满** | `src/delay_overlay.h`、`src/delay_overlay.cpp`、`tests/delay_overlay_test.cpp` |
| 高 | **"基线采到了"可被常量满足**，且产品可静默退回"睡 120ms 赌" | 暴露的是**决策**而不是数值：新增"是否退回固定等待"标志；并新增一条确定性断言——事先在那块区域贴一块与覆盖层同色的补丁，产品报出的基线必须**与屏幕上量到的对得上** | 同上 |
| 中 | **无头/CI 路径下 `CheckOrSkip` 把"产品彻底坏掉"变成 SKIP**（CI 照样绿） | "产品拒绝启动"与"环境建不出窗口"分开：只有"窗口建不出来"且处于无头会话才跳过，`Start()` 其它失败一律硬失败（产品侧暴露失败原因） | `src/delay_overlay.h`、`src/delay_overlay.cpp`、`tests/delay_overlay_test.cpp` |
| 中 | **催桌面重绘（`InvalidateRect`/`RedrawWindow`）零运行时覆盖** | 产品暴露只读计数器，测试断言"销毁后催过 ≥1 次、轮询期间催过 ≥1 次" | 同上 |
| 中 | **`CanCaptureScreen()` 只看 API 成功**：断连的 RDP / 锁屏会话会抓到一张**全黑**的图，像素断言于是误红 | 采样九处，要求画面里有非黑像素或不止一种颜色，否则按"抓不到画面"跳过像素断言 | `tests/delay_overlay_test.cpp` |
| 中 | **无头路径对"等重绘"零覆盖** | 无头也断言"产品路径真的调用过等待实现"（调用次数 ≥1）——这是 CI 上唯一能守住的等待守卫 | `tests/delay_overlay_test.cpp` |
| 低 | 恒真断言 `Check(true, ...)` 虚增计数 | 改成真断言：没有覆盖层时 `Cancel()` 不该白跑一次"等重绘"，且不该留下窗口 | `tests/delay_overlay_test.cpp` |
| 低 | **测试专用公开 API 编进了产品二进制** | 全部收进 `#ifdef SNAPLITE_DELAY_TEST_HOOKS`；只有测试目标定义该宏，产品目标不定义 | `src/delay_overlay.h`、`CMakeLists.txt` |
| 低 | **残留容忍度 64px 与 DPI 无关**：高 DPI 下每次延时截图都等满 800ms（冻住界面） | 容忍度按覆盖层所在显示器的 DPI 缩放 | `src/delay_overlay.cpp` |
| 低 | 脏桌面 WARN 打印的是 baseline 而非脏像素数（出现 "not clean (0)" 自相矛盾） | 打印实际的脏像素数 | `tests/delay_overlay_test.cpp` |
| 低 | `LastBaselineColours()` 在 `Start()` 失败时也会被更新 | 基线只在覆盖层**真的建出来**之后才对外可见 | `src/delay_overlay.cpp` |

#### 发布链与守卫（VERDICT: PARTIAL）

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 中 | **守卫对"抽 helper"误报**：把"销毁 + 作废"抽成一个 helper 就变红（`find_calls` 与 `resolve_calls` 混用） | 统一用 `resolve_calls`；并把解析器改成**多层传递**（抽两层也不再误报） | `tests/test_build_scripts.py` |
| 中 | **只解析一层 helper 嵌套**：把等待循环再抽一层 → 假红 | `resolve_calls` / `resolve_needle` / `_called_functions` 全部改成传递闭包 | `tests/test_build_scripts.py` |
| 中 | **YAML 守卫用字符串切分判定分支**：引号风格一改、else 换行就假红 | 改成"看每一处 resolver 调用本身带着什么开关"，不再解析 if/else 的排版 | `tests/test_build_scripts.py` |
| 低 | 工作流里的"数字分量版本线比较"无守卫钉住（改成 `StartsWith` 全绿） | 新增断言：不得用字符串前缀比较，必须先剥预发布后缀 | `tests/test_build_scripts.py` |
| 低 | **`.release/` 下任何非说明文件都让发布 job 变红**（加个 README.md 就发不了版） | 无关文件（README/.gitkeep/子目录）静默忽略；"像是说明文件、名字却不合规"的（`v1.6.0.bak`）仍然报错 | `scripts/resolve_release_version.py` |
| 低 | **git 默认给非 ASCII 路径加引号**，变更列表里那种路径匹配不上说明文件 | git 命令加 `-c core.quotepath=false` | `.github/workflows/release.yml` |
| 低 | BOM 保护有两层，只有一层被用例钉住 | 补一条**走 CLI 的端到端**用例（真写带 BOM 的列表文件、真调脚本、看退出码与输出） | `tests/test_build_scripts.py` |

**这一批的验证方式**：
- **14 条定向守卫变异**逐条跑过——G1 整块重绘 / G2 用 `RDW_ERASE` / G3 擦背景 / G4 删掉催重绘 / G9 版本线退化成前缀 / G10 push 路径偷带回落开关 / G11 `.release` 杂文件又变红 / G12 两层 BOM 保护同时删 / G13 测试钩子暴露到产品 / G14 产品目标也定义钩子宏 —— **必须红**；G5 抽 helper / G6 再抽一层 / G7 引号风格 / G8 else 换行 —— **必须放行**。实测 **14/14 符合预期**。
- 运行时侧 6 条变异（M1 删掉等待调用 / M1b 超时改 0 / M5b 基线没采 / M5c 基线写死 / M7 不再催重绘 / M4 等待实现直接返回 true）逐条跑过，**全部按预期变红**。

**第十二批验证结果**：Python 单测 64 项通过；ctest 4/4 通过（`snip_interaction` 246 项 / `delay_overlay` 32 项 / `build_scripts` 64 项 / `verify_generated_sources` 38 条）；延时覆盖层测试 7.1s 通过；产物校验与图标、版本资源校验通过。

### 第十三批：第 11 轮验收（B=FAIL 运行时 / C=FAIL C++ / A=PARTIAL 发布链）的修复

第 11 轮三份验收报告的结论：**A（发布链与守卫）PARTIAL**（5 中 + 8 低）、**B（延时覆盖层运行时）FAIL**（3 条已实测的假绿路径）、**C（C++ 修复与并发）FAIL**（1 个真 bug + 2 处新缺陷）。本批把这些全部修掉。

#### B：运行时（VERDICT: FAIL）—— 3 条假绿路径

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **"催过桌面重绘"验的是计数器，不是 API 真被调用**：删掉 `InvalidateRect`/`RedrawWindow` 后计数照样在涨，49 项断言全绿 | 计数**由 API 的返回值驱动**（`RepaintDesktopRegion` 返回 Win32 的真实返回值，失败就不计数）；并新增"最近一次重绘请求的区域"钩子，测试断言它是**有界区域且就是覆盖层那一小块** | `src/delay_overlay.cpp`、`src/delay_overlay.h`、`tests/delay_overlay_test.cpp` |
| 高 | **无头/CI 路径下"产品彻底坏掉"仍变绿**（`HEADLESS=1` 时 0 退出并报 "PASS"） | 任何一组断言被跳过 → 退出码 **77**（ctest 认成 Skipped 而不是 Passed），摘要逐条列出跳过了哪几组 | `tests/delay_overlay_test.cpp` |
| 高 | **别的进程正在倒计时 → 产品路径断言整段被静默跳过**，摘要还写 "PASS: 31" | 同上：跳过即 77，并点名"外来覆盖层"这个原因 | 同上 |
| 中 | **"基线"是条空断言**：基线在覆盖层**可见时**量（≈15392），断言上限比它还大 → 永远不可能失败 | 基线挪到"擦干净之后、覆盖层出现之前"量；取消后的对比改成"重新起一次覆盖层再取消" | 同上 |
| 中 | 测试写死残留容忍度 64 / 等待上限 800，产品却按 DPI 缩放、参数可调 | 产品暴露 `LastResidueTolerance()` / `LastWaitTimeoutMs()`，测试按产品报的来判 | `src/delay_overlay.h` |
| 低 | `SNAPLITE_DELAY_TEST_HEADLESS=`（空串）不生效 | 改用 `GetLastError() != ERROR_ENVVAR_NOT_FOUND` 判定"变量存在" | `tests/delay_overlay_test.cpp` |
| 低 | 无头 WARN 文案把"环境变量强制"说成"窗口站不可见" | 两种来源分开写 | 同上 |
| 低 | 圆角半径 20、顶部偏移 48 没按 DPI 缩放 | 都按覆盖层所在屏的 DPI 缩放 | `src/delay_overlay.cpp` |
| 低 | `InvalidateRect` + `RedrawWindow` 功能重复，同一块区域作废两次 | 只留一次（`RedrawWindow` 的 `RDW_INVALIDATE` 等价于 `InvalidateRect(..., FALSE)`） | 同上 |
| 低 | `Start()` 失败时基线保持 -1，没有断言覆盖 | 新增一条不需要桌面的断言：非法秒数必须**明确失败**且不留基线 | `tests/delay_overlay_test.cpp` |

#### C：C++ 修复与并发（VERDICT: FAIL）

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 中 | **撤销/重做的两个栈会失同步**（实测：位图栈 2、箭头栈 3）——`take_back()` 解码失败时**已经把条目弹掉**，`Undo()` 却提前 `return` 不弹箭头栈；下一次撤销会取回更老的位图、恢复更新的箭头（"撤销之后箭头还在"，且越撤越错） | 取不到位图时**同步弹掉对应的那一条箭头**；两栈长度不平时多出来的箭头只能从**队尾**扔（从队首扔会把后面每一对的配对整体错开一位） | `src/snip_window_original.inc`、`src/snip_window.h` |
| 中 | **`EnsureWorker()` 异常不安全**：先置 `workerStarted_=true` 再起线程，线程创建失败时异常穿出窗口过程（那路上没有 try/catch）→ `std::terminate`；`workerState_` 还会是空的 | 整段包进 try/catch，失败返回 false；调用方放弃压缩这一条（原始位图继续留着，撤销照常可用） | `src/snip_window.h` |
| 低 | `detach` 失败后的兜底 `worker.join()` **自己也会抛**，调用点没有 try/catch → 兜底代码把进程杀了 | 去掉 join 兜底（detach 抛了说明线程已经不是 joinable 状态，析构不会 terminate） | `src/ocr_v2.cpp` |
| 低 | OCR 临时文件清理**每个文件做一次全进程快照**（文件一多退出就变慢），且快照与枚举之间有竞态（新实例刚建的文件会被当成"主人已死"而删掉） | 先把文件**全部**列完，再取**一次**快照；快照不可信时一律不删别人的文件 | `src/app.cpp` |
| 低 | 三个 DPI API 全失败就报"未能开启 DPI 感知"——但清单里已声明感知时它们会以 `ERROR_ACCESS_DENIED` 失败，感知其实是**开着**的 | 失败后读回 `GetThreadDpiAwarenessContext()` 的实际状态再判定 | `src/app.cpp` |
| 低 | OCR 完成消息未被处理时 payload 泄漏 | 登记"待处理"表，退出时统一回收；所有权靠"从表里摘走"转移，不会二次释放 | `src/ocr_v2.cpp`、`src/ocr.h`、`src/app.cpp` |
| 低 | 延时覆盖层**恒定落在主屏**（通知窗口钉在 (0,0)），用户在副屏干活时倒计时跑到了另一块屏 | 落位改用**光标所在**的那块屏，DPI 从**同一块**屏取（`GetDpiForMonitor`），拿不到时退回通知窗口所在屏 | `src/delay_overlay.cpp` |
| 信息 | `BitmapHistory::clear()` 的注释与实现相反（"位图归后台线程"其实是副本） | 改正注释 | `src/snip_window.h` |

#### A：发布链与守卫（VERDICT: PARTIAL）

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 中 | **变量展开即可绕过守卫**：`--allow-base-fallback` 藏进 `$flags` + `@flags`，push 路径带回落开关仍全绿 | 回落开关在整个工作流里**只能出现一次**（藏进变量再展开同样算出现），且必须出现在手动那条命令里 | `tests/test_build_scripts.py` |
| 中 | **"解析失败 → job 变红"没有守卫**：删掉 `$LASTEXITCODE` 检查后，pwsh 仿真显示步骤把项目版本号写进了 `GITHUB_OUTPUT` | 新增守卫：每一条解析命令后面必须自己检查 `$LASTEXITCODE` 并 `throw`（检查窗口到下一条命令为止） | 同上 |
| 中 | **"必须主动作废覆盖层所在区域"被间接链满足**：删掉销毁后那次重绘，守卫仍被轮询期那次兑现 | 断"**销毁之后、等待之前**的那一次"，并排除"等重绘"那个调用点本身 | 同上 |
| 中 | **销毁/等待守卫不查可达性**：`if (false) { DestroyWindow(hwnd); }` 能通过 | 对销毁/作废/等待三个调用点都补 `in_dead_branch` 检查 | 同上 |
| 中 | **恢复路径上 tag 与产物不对应**：tag 保留旧的，产物按新 commit 构建后 `--clobber` 覆盖 | tag 已存在且指向别的 commit 时**直接失败**，除非显式勾 `force_rebuild` | `.github/workflows/release.yml` |
| 低 | `core.quotepath=false` 无守卫钉住 | 新增断言 | `tests/test_build_scripts.py` |
| 低 | `assertNotIn("--diff-filter=A$", …)` 是**空断言**（找的是字面量 `A$`） | 改正则 `--diff-filter=A(?![R])`，且只看**代码**（注释里本来就写着这句话） | 同上 |
| 低 | YAML 守卫对 python 写法敏感（`python -X utf8` / `./scripts/…` / `py -3` / 注释里出现命令 / 多加一条调用，5 种合法写法假红） | 识别逻辑改成"逻辑命令"扫描：反引号续行算同一条、注释不算调用、解释器写法放宽；条数不再写死 | 同上 |
| 低 | `.release/` 子目录语义与文档矛盾（子目录里的合规名被当说明文件、坏名报错） | 只认 `.release/` **顶层**；子目录里的一切静默忽略 | `scripts/resolve_release_version.py` |
| 低 | BOM 的两层保护只有一层被钉住（删掉 `utf-8-sig` 全绿） | 删掉多余的 `utf-8-sig`，只留**逐行剥**这一层，并如实写成"单层防线" | 同上 |
| 低 | `functions()` 不区分同名函数，`_called_functions` 按名字塌缩 | 改成按 (名字, 区间) 记 | `tests/test_build_scripts.py` |

**这一批的验证方式**：
- **27 条定向守卫变异**逐条跑过——新增 16 条（N1 变量藏开关 / N2 删掉失败检查 / N3 删掉销毁后作废 / N4 销毁藏进死分支 / N5 整段流程藏进死分支 / N6 去掉 quotepath / N7 删掉重绘 API / N8 撤销不同步 / N9 线程启动不包 try / N15 删掉逐行剥 BOM / N16 子目录又被当说明文件 / G3 重绘藏进死分支 / G11 不比对 tag 的 commit）——**必须红**；N10–N14（python 各种写法 / 注释里出现命令 / 多加一条带检查的调用）与 G4（把"销毁+作废"抽成 helper）——**必须放行**。实测 **27/27 符合预期**。
- 运行时侧 12 条变异（含新增的 M8 删掉重绘 API、M9 区域为空、M10 上限改 1ms、M11 不报告上限、M12 容忍度不缩放）逐条跑过。


### 第十四批：第 12 轮验收（B=FAIL 运行时 / A=FAIL 发布链与守卫）+ 一个被确认的撤销栈真缺陷

第 12 轮两份报告回来了：**B（延时覆盖层与跨屏）VERDICT: FAIL**（1 高 + 1 中 + 2 低/信息）、**A（发布链与守卫）VERDICT: FAIL**（12 类"必须红"全红、16 类"必须绿"全绿，但另外构造出 **7 条确凿的"改坏了仍全绿"漏判**）。第三份（C++ 修复与并发）在跑完探针、**确认了一个撤销栈配对缺陷**之后掉线（`net::ERR_CONNECTION_CLOSED`），缺陷本身由主会话接手复核并修掉。

#### B：运行时（VERDICT: FAIL）—— 恒真断言与"假装成功"

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 高 | **"没有整块重绘桌面"是条恒真断言**：`LastRepaintRegion()` 记的是**传参区域**，不是真正交给 `InvalidateRect` 的矩形。变异 M3b（实参改 `nullptr`，整屏重画）与 M3c（`erase=TRUE`）运行时仍全绿 | 记录器**放进实参表达式里**：`RepaintTarget(&region)` / `RepaintEraseFlag()`。绕开这两个函数就等于"没有这次请求"，运行时当场红 | `src/delay_overlay.cpp` |
| 中 | **"计数由 API 返回值驱动"挡不住"假装成功"**：`RepaintDesktopRegion` 若无条件 `return true`，计数照涨、测试照绿 | 产品新增**强制失败**注入（`ForceRepaintFailure`）：让"Windows 拒绝了这次请求"可以被造出来，测试问出"请求被拒时你还算催过吗"。答"不算"才算过 | `src/delay_overlay.cpp`、`src/delay_overlay.h`、`tests/delay_overlay_test.cpp` |
| 低 | 外来覆盖层/桌面脏时，跳过清单只写"the pixel assertions"，实际**产品路径那组**也被跳过却未点名 | 跳过清单逐条点名 | `tests/delay_overlay_test.cpp` |
| 低 | `g_lastRepaintRequested` 是死状态 | 删掉 | `src/delay_overlay.cpp` |

> 顺带修掉一个自己写出的 bug：记录器最初把**传参**又盖回"实参"上面（`RequestDesktopRepaint` 里 `g_lastRepaintRegion = region;`），等于把上一轮的恒真断言原样请回来。现在区域**只**由 `RepaintTarget` 写。

#### A：发布链与守卫（VERDICT: FAIL）—— 7 条"打错靶"的断言

上一轮把"字面量出现次数"当成了不变量。这轮全部改成**问真实行为**：

| 漏判 | 为什么打错靶 | 修法 |
|---|---|---|
| M01c | 回落开关藏进变量**并用拼接拼出来**（`$flags = '--allow-base' + '-fallback'`），字面量一次都不出现 | 把 push 路径的命令**展开变量**之后再查：引号、`+`、空白都归一化，拼接出来的开关照样认得出 |
| M05b | tag 闸门里的 `throw` 换成 `Write-Host`，两个正则照样成立，但 job 不再变红 | 断"闸门块里有一条**真会执行**的 `throw`"（不是字符串、不是 `if ($false)` 里） |
| P07 | `throw` 藏进 `if ($false)` | 同上（识别 PowerShell 版的死分支） |
| P06 | 检查窗口读的是**含注释的原文**，一句 `# …必须检查 $LASTEXITCODE 并 throw` 就能顶替 | 窗口改用**剥掉注释之后**的代码；并要求 `throw` 与 `$LASTEXITCODE` 在**同一个 if 块**里 |
| M11b | 只查"计数被某个变量支配"，挡不住"调 API 但忽略返回值、无条件 `return true`" | 静态：重绘函数的返回值必须**由 API 的返回值派生**；运行时：强制失败注入（见上表） |
| N02 | 基线取样用 `resolve_calls`，会沿 `Start→Cancel→…→CountOverlayColours` 传递链兑现，真基线删了守卫照样绿 | 只认 `Start` 本体（或它**直接**调用的、非拆卸路径上的 helper）里的取样点 |
| N03 | 轮询歇息只判"循环体里有 `Sleep(`"，而循环**必须在这个函数里**；原来只判了"循环起点在该函数结束之前"，文件前面别的函数里的循环也能顶替 | 循环必须落在该函数区间内；轮询的歇息必须**在像素判定之后**、且不能用兜底的那次固定等待顶替 |

另外两条本轮自测发现的：**V1** 工作流的版本线比较退回字符串比较（与 resolver 的分量口径不一致）、**L1** resolver 放行带前导零的版本分量（`1.06.0`）——两条都补了守卫。

#### 撤销/重做：位图与标注的配对会错开一位（**真缺陷**）

第 12 轮 C++ 验收在掉线前用探针**确认**了这条（"两栈长度相等，但配对整体错开一位"），主会话复核了机制：

- 位图栈有 **64MB / 128 条**的裁剪，裁剪（`BitmapHistory::Trim()` → `DropFront()`）丢的是**队首**；
- 标注栈不会跟着丢 → 两栈长度可能仍然相等，配对却错开一位；
- 更糟的是下一次编辑时那句"把多出来的标注**从队尾**扔掉"，会把**新**的那套标注状态扔掉 → 之后每次撤销恢复出来的都是旧的那一套（实测表现：撤销之后箭头还在，而且越撤越错）。

**修法不是"再同步一次"，而是让配对变成结构性的**：标注状态**存在历史条目里**（`BitmapHistory` 变成带 `Companion` 的模板），位图与标注同生共死——裁剪丢掉的是"位图+标注"一整条，取回失败丢掉的也是一整条。`Undo`/`Redo` 里那套"取不到位图就手动弹一下标注栈"的补丁整体删掉了（它正是上一轮按错误理解加的）。同时把守卫从"多出来的标注只能从队尾扔"改成"标注状态必须在条目里、不能有平行的标注栈"。

| 文件 | 改动 |
|---|---|
| `src/snip_window.h` | `BitmapHistory` 模板化（`Companion`），条目带标注状态；新增测试专用钩子 `SetMaxEntriesOverrideForTest`（把上限压到 2 条才能测到裁剪路径） |
| `src/snip_window_original.inc` | `BeginEdit`/`Undo`/`Redo` 改成"取/压都是一整条"；删掉 `arrowUndo_` / `arrowRedo_` 两个平行栈 |
| `tests/snip_interaction_test.cpp` | 新增"裁剪之后取回的帧必须带着**它自己**的那份状态"运行时断言（对照组：6 条正常压缩往返也逐条校验编号） |
| `tests/test_build_scripts.py` | 守卫改为断言结构性不变量（条目里有 `Companion`、没有平行标注栈、取/压都一次取全） |
| `CMakeLists.txt` | 交互测试目标定义 `SNAPLITE_EDIT_TEST_HOOKS`（产品目标不定义，钩子不进产品二进制） |

#### 低危项

| 级别 | 问题 | 修法 | 文件 |
|---|---|---|---|
| 低 | 端到端 BOM 用例的 docstring 仍写"剥 BOM 有两处实现"，与实现（只有逐行剥这一层）矛盾 | 如实改成"只有一层" | `tests/test_build_scripts.py` |
| 低 | 工作流的版本线比较是**字符串**比较，resolver 是**分量**比较：`1.06.0` 被 resolver 判成同一条线，工作流却当成不同线而抛错（两层口径不一致） | 工作流改成按数字分量比较（`[int]$_` 逐分量）；resolver 侧**拒绝前导零分量**（`1.06.0` 几乎一定是手误） | `.github/workflows/release.yml`、`scripts/resolve_release_version.py` |
| 低 | `git diff` / `git fetch` 的退出码没检查：失败时列表为空，后面被报成"没有发布说明文件"，真正的原因被埋掉 | 补 `$LASTEXITCODE` 检查并 `throw` | 同上 |

**这一批的验证方式**：
- **13 条守卫变异**逐条跑过：9 条"必须红"（M01c / M05b / P07 / P06 / M11b / N02 / N03 / V1 / L1）全部变红；4 条"必须绿"（G1 换引号风格 / G2 改报错文案 / G3 局部变量改名 / G4 把基线取样抽成 helper）全部放行。实测 **13/13 符合预期**。
- 运行时侧新增三组断言：重绘实参（记录器）、强制失败下的计数、裁剪之后的标注配对。

> **未改但已记录**：
> - `.release/*.md` 的**内容**至今没有进发布页（发布走 `gh release create --generate-notes`），这些文件目前只是"版本触发器"。要不要把它们当发布说明发出去，取决于产品意图，本轮没动。
> - **恢复路径（`gh release upload --clobber`）不更新 release 的 prerelease 标记**——这一条经推演是**不成立的**：tag 名里就带着版本号（`v$releaseVersion`），同一个 tag 永远对应同一个版本、同一份 prerelease 判定，所以恢复路径上那个标记不可能是错的。没有可改的东西，也就没有改。
> - **CI 上延时覆盖层用例恒为 Skipped**（GitHub 的 `windows-latest` 跑在会话 0 的服务 window station，抓不到交互式桌面）。这是环境限制，不是缺陷：退出码 77 会把它如实报成 Skipped 而不是 Passed。真正的覆盖来自本地/CI 上那 246→259 项的原生交互测试。


### 第十五批：第 13 轮验收（A=FAIL 发布链与守卫 / C=PASS）的修复

第 13 轮的三份报告：

- **A 卷（发布链与源码级守卫）**：**FAIL**。上一批的 13 条定向变异它重做了一遍，**全部符合预期**（说明上一批的修法确实堵住了"明显形态"）；但它另造 17 条"打错靶"变异，**12 条仍然全绿**、**2 条假红**。其中 5 条用 pwsh 真实仿真证明了后果。
- **C 卷（C++ 撤销栈与并发）**：**PASS**（无 P0/P1）。留下 1 项 P2 覆盖缺口 + 5 项 P3 观察。
- **B 卷（延时覆盖层运行时）**：本轮没派（C 卷的桌面测试还没跑完，两个桌面测试并发会互相破坏）。

#### A 卷：守卫的"修法太浅"——12 条漏判 + 2 条假红

上一批的修法普遍停在**字面量 / 形态**层面，没有钉住语义，换一种形态就能绕过。本批把断言从"这段代码**长得像**吗"改成"这段代码**真的会**这么做吗"。

| 编号 | 上一轮的漏判（验收实测） | 本批的修法 |
|---|---|---|
| H1 | 回落开关从手动路径挪到 push 路径 → push 静默回落、job 绿、写出旧 tag | 按**读的是哪个文件**分组：带开关的那条读的文件必须**不是** git 算出来的那份，不带开关的那条必须读的就是它。按字面量分组时，两条 invocation 依然是"一带一不带"，守卫看不出来 |
| H2 | `Write-Host "resolver failed; throw not reached"` —— 字符串里的 "throw" 也算数 | 新增 PowerShell 字面量遮罩（单引号 `''`、双引号 `""`、反引号转义各按各的规则），找 throw 之前先把字符串遮成空格 |
| H3 | `$LASTEXITCODE -ne 0` 写成 `-eq 0` → 正常发布**必然失败** | 凡是"条件提到 `$LASTEXITCODE`、块里有一条直接 throw"的判断，条件都必须是**失败条件**（`-ne 0` / `-gt 0` / 真值判断） |
| H4 | throw 藏进 `if ($env:SNAPLITE_NEVER -eq 'yes')`（不是字面 `$false`）→ 闸门形同虚设，job 照样绿 | throw 必须**直接**属于闸门块（不许藏在更深的嵌套 if 里）；恒假条件识别扩展到 `0` / `$null` 和纯字面量的 `-and` / `-or` 表达式 |
| H5 | `--diff-filter=AR` 改成 `ARD`（把被删的文件也算进来）→ 选中被删文件的老 tag 并用它重发 | 过滤器的**字符集合必须恰好是 {A, R}**（原来是子串匹配，"ARD" 里就含有 "AR"） |
| M1 | 变量赋值取"首个匹配"，前面放个诱饵空赋值就能骗过 | 取**全部**赋值；展开时优先用"这段文本里自己赋的值"，避免同名变量在两条分支里被混起来 |
| M2 | 反引号续行把赋值截断，拼接出来的开关认不出来 | 取值时把反引号续行接完 |
| M3 | 重绘函数多一条无条件 `return true` 就能绕过"至少有一条 return 由 API 决定" | 每一条 return 都要过：API 调用**之后**的必须由 API 返回值派生；**之前**的提前退出只能是假值 |
| M4 | 基线取样 / 轮询歇息藏进 `if (false)` 里 | 两处都补 `in_dead_branch` 检查 |
| M5 | `'true'` 改 `"true"`、或把操作数对调 → 语义等价的改写让守卫变红（假红） | 正则放宽成 `['\"]true['\"]`，并允许操作数顺序互换 |
| M6 | `git diff` 删掉 `--name-only` | 断言落在剥过注释的代码上 |
| M7 | `core.quotepath=false` 只写在注释里 | 同上（断言原来查的是原文） |
| L1 / L2 | "出现过一次 `-split`" / "出现过一次 `[int]$_`"被别处满足 | 改成"发布版本和项目版本**各自**都要剥后缀"、"两侧都要转数字分量" |
| L3 | 条件里带 `)` 的字符串 → 整个 if 块被静默丢掉（假红方向） | 括号配对改为在**遮罩之后**数，条件文本仍取原文 |
| L4 | 花括号不配平时守卫以 ERROR 结束 | 解析不了的块跳过而不是抛异常；另加一条"每段 run 块花括号必须配平"的用例 |
| L5 | 赋值不在行首时展开失效 | 赋值识别不再要求行首 |

产品侧也补了对应的一道防线（**不只是守卫**）：

| 文件 | 改动 | 理由 |
|---|---|---|
| `.github/workflows/release.yml` | "Read release version" 步骤开头显式 `$ErrorActionPreference = 'Stop'` | PowerShell 默认 `Continue`：一条非终止错误（比如对 `$null` 调 `.Trim()`）**不中断脚本**，脚本带着旧值继续往下走，最后拿旧 tag 配 `--clobber` 覆盖已发布的产物。A 卷 pwsh 仿真实测：Continue 下 job 是绿的，`GITHUB_OUTPUT` 里照样写出 `version=1.6.0` / `tag=v1.6.0` |
| 同上 | `$releaseVersion` 不再预置成 `$baseVersion`（改成空串） | 预置成项目版本号就是一个放大器：解析那一步一旦没拦住，这里会安静地保留旧版本号 |
| 同上 | resolver 没输出时显式 `throw`；两条路径之后统一断言 `$releaseVersion` 非空 | 不再靠下一行的 `.Trim()` 去撞 null |

#### C 卷的 P2 覆盖缺口 F1：产品自己那条"裁剪 → 编辑 → 撤销"路径没有运行时覆盖

C 卷的结论：裁剪配对那条测试用的是 `BitmapHistory<int>`，而产品用的是 `BitmapHistory<std::vector<ArrowAnnotation>>`。**测试钩子是按模板实例化存的静态量**（每份实例化各有一份），于是测试压的上限根本落不到产品那一份上——把平行标注栈原样装回产品（变异 M3），**259 项运行时断言全绿**，只有源码文本守卫会红。

修法：

| 文件 | 改动 |
|---|---|
| `src/snip_window.h` | `SnipWindow` 增加 `SetHistoryEntryLimitForTest(unsigned)`（`#ifdef SNAPLITE_EDIT_TEST_HOOKS`），它指向**产品真正用的那个实例化** `BitmapHistory<std::vector<ArrowAnnotation>>` |
| `tests/snip_interaction_test.cpp` | 新增 6 条产品级运行时断言：上限压到 2 → 连做 3 次带标注的编辑 → 撤销 → 恢复出来的标注必须**逐条对得上**（第一、第二个箭头在，第三个不在） |
| `tests/test_build_scripts.py` | 守卫同步改：条件编译块**可能不止一个**（原来只认第一个，多一个块就会误报"钩子跑到了条件编译块之外"）；另加一条"测试入口必须指向产品真正用的那个实例化" |

**这一批的验证方式**：

- **ctest 4/4 全绿**：`snip_interaction` **265 项**（259 + 6）/ `delay_overlay` 68 项 / `build_scripts` **75 项**（73 + 2）/ `verify_generated_sources` 38 条。
- **30 条守卫变异**逐条跑过（`%TEMP%\snaplite_guard_mut5.py`）：**22 条"必须红"全红**（上一批 13 条 + A 卷 12 条漏判中的 9 条 + 4 条），**8 条"必须绿"全绿**（4 条无害重排 + 4 条等价改写）。实测 **30/30 符合预期**。其中 A 卷点名的 2 条**假红**（`'true'` 换引号风格、操作数对调）已消除。
- **运行时变异 M3-PAR**（把平行标注栈原样装回产品，在 TEMP 副本上重编后跑真实交互测试）：必须**红**，且红在**新增的那条产品级断言**上——实测符合预期。
- 产品二进制纯净性：`SetHistoryEntryLimitForTest` / `SetMaxEntriesOverrideForTest` 等符号**不在** `SnapLite.exe` 里（产品目标不定义 `SNAPLITE_EDIT_TEST_HOOKS`）。

> **未改但已记录**（C 卷的 5 项 P3 观察）：`IsDpiAwarenessEffective()` 注释里"清单已声明感知"的前提在本仓库不成立（无 `.manifest`），该分支在当前构建下不可达，逻辑本身无害；每个 `BitmapHistory` 实例自带一个后台压缩线程（每个截图窗口 2 个常驻线程）；6 条历史里通常只有 2 条转 PNG（受 64MB 上限与在途上限约束，不构成正确性问题）；PNG 解码失败路径（整条丢弃、状态复位、栈不卡）设计合理。


### 第十六批：第 14 轮验收（A=FAIL 发布链与守卫 / C=PASS）的修复

第 14 轮的三份报告：

- **A 卷（发布链与源码级守卫）**：**FAIL**。上一批的 30 条定向变异它重做了一遍，**30/30 全部符合预期**（说明上一批的修法确实堵住了"明显形态"）；但它另造 32 条"打错靶"变异，结果是 **9 条真实绕过**、**3 条覆盖缺口**、**10 条假红**。
- **C 卷（撤销栈产品级）**：**PASS**（无 P0/P1）。留下 2 项 🟠 覆盖缺口 + 3 项 🟡 观察。
- **B 卷（延时覆盖层运行时）**：本轮没派（桌面测试不能并发跑）。

#### A 卷：上一批的修法"换一种形态就能绕过"

上一批把断言从"这段代码**长得像**吗"改成了"这段代码**真的会**这么做吗"，但仍有一批地方停在**字面量 / 形态**层面：A 卷换个形态就绕过去了。本批继续往下挖。

| 编号 | 第 14 轮实测的漏判 | 本批的修法 |
|---|---|---|
| N-02 | PowerShell 的 **here-string**（`@'…'@` / `@"…@`）没被字面量遮罩罩住 → 注释里写一句 `throw` 就算数 | `_powershell_mask_literals` 增加 here-string 识别（终止符必须独占一行） |
| N-04 / N-05 | 退出码条件写成**复合条件**（`-and` / `-or`）绕过极性断言。两种后果都实测有：恒假的 `-and $false` → 闸门**永不触发**；恒真的 `-or` → **正常发布被误杀** | `_powershell_exit_code_is_failure` 重写：只认**单纯**形式（`-ne`/`-gt` + `0`/`'0'`/`"0"`/`$null`，允许操作数对调）；**含 `-and`/`-or` 的复合条件一律不认** |
| N-01b / N-06b / N-07 / N-08b / N-19 | 变量赋值还有别的形态：`${name}` 写法、**下标赋值**、`Set-Variable -Name … -Value …` | `_powershell_variable_values` 三种形态都收 |
| N-28 | 回落开关被**环境变量顶掉**（`$env:XXX` 覆盖了参数） | 回落开关改用**白名单**判定：`_resolver_extra_arguments(text, allowed_flags)`——push 那条只允许 `--base-version` / `--changed-files`，splat、多余位置参数、没见过的开关都算"多出来" |
| N-14 / N-15 / N-16 | 产品侧三处改动**一条守卫都没有** | 新增 `test_release_version_step_fails_loudly_on_any_error`（"Read release version" 步骤必须 (a) 在调用解析脚本**之前**设 `$ErrorActionPreference='Stop'`；(b) 有 ≥2 处 `-not $resolved` 且每处含真会执行的 throw；(c) 有非空闸门且含真会执行的 throw）；另加 `FORCE_REBUILD` 必须**直接映射**（挡 `|| true`） |
| N-25 / N-26 / N-17 / N-18 / N-08 / N-27 / N-23 / N-11 / N-13 / N-09 | 10 条**假红**（无害改写让守卫变红） | 判据改成"**问条件真假**"而不是正则匹配：新增 `_powershell_condition_is_true` / `_powershell_conditional_spans`；`_powershell_real_throws` 改为排除"**可能不执行**的块"（if 条件非恒真、else/while/for/foreach/catch/try/switch 的块），但**不再**排除普通分组块、`do` 块、`& { }` → `& { throw }` / `do { throw } while($false)` / `if ($true) { throw }` 不再假红（`try { throw } catch {}` 仍判红，是真阳性） |
| F-2 | `test_history_limit_test_hook_targets_the_product_stack` 用**邻近式匹配**，放个诱饵就能骗过 | 改成**函数体级**断言：体内容错后 `BitmapHistory<` 计数必须恰为 1、必须含 `BitmapHistory<std::vector<ArrowAnnotation>>` 与 `SetMaxEntriesOverrideForTest`，并断言产品 `undo_`/`redo_` 就是该实例化 |

#### C 卷的 F-1 / F-3

- **F-1（🟠）**：第 15 批加的 6 条产品级断言只证明了"撤销能恢复标注"，**证明不了"裁剪真的发生了"**（`BeginEdit` 存的是编辑前的标注，与裁没裁剪正交）。修法：在 `SetHistoryEntryLimitForTest(0)` 之前追加 2 次撤销 + 1 条探针断言 `the trimmed oldest step is really gone: the first arrow survives three undos`——钩子没落到产品栈时它会红。
- **F-3（🟡）**：`tests/snip_interaction_test.cpp:300-302` 的 GDI 泄漏断言 flaky（C 卷实测 16 次误红 1 次）。修法：新增 helper `MinGdiObjects()`（30 次采样取最小值），断言改成"刷新前采样 → 40 次刷新 → 采样 → 再 40 次刷新 → 采样 → 取较小者 ≤ before+1"。

**这一批的验证方式**：

- **ctest 4/4 全绿**：`snip_interaction` **265 项**（259 + 6）/ `delay_overlay` 68 项 / `build_scripts` **75 项**（73 + 2）/ `verify_generated_sources` 38 条。
- **22 条 A 卷定向变异**逐条跑过（`%TEMP%\snaplite_guard_mut6.py`）：11 条"必须红"全红、11 条"必须绿"全绿，**22/22 符合预期**；还原后基线绿。
- **运行时变异 M3-PAR**（把平行标注栈原样装回产品）：红在**新增的产品级断言**上（"259 全绿"变成红），还原后基线绿。
- 守卫套件 **76 项全绿**（75 + 1 条 F-2 收紧）。

### 第十七批：第 14 轮验收（B=FAIL 延时覆盖层运行时）的修复

第 14 轮 B 卷结论：**FAIL**。19 条变异里 **4 条"必红"没红**，其中最要命的一条是——

> **200ms 催重绘限频（用户实测"在桌面一测试居然能影响到我桌面二，桌面二在闪"的直接修复点）在运行时与静态两层同时是绿的。**
> 去掉限频之后 800ms 窗口内催重绘从 **4 次变成 18 次**，而运行时 68 项、静态 75 项全绿。

其余三条：轮询不歇息（`Sleep` 被删）运行时绿；"抓不到画面→退回固定等待"分支**一条断言都没有**；运行时对"基线取样时机"无鉴别力。

#### 产品的做法：把**决策**和**行为**都报出来

按本项目一贯的原则（"产品暴露**决策**而非数值"），覆盖层新增 6 个只读钩子（`src/delay_overlay.h` / `.cpp`，全部在 `SNAPLITE_DELAY_TEST_HOOKS` 内）：

| 钩子 | 报的是什么 | 为什么需要 |
|---|---|---|
| `LastRepaintNudgeIntervalMs()` | 催重绘的**限频间隔**（决策参数） | 测试不能写死 200ms：产品合法地调到 300ms 不该假红。次数上限由产品自己报的间隔算出来 |
| `LastWaitPollRepaintCount()` | **最近一次等待**里催了几次 | 限频有没有生效，先看次数 |
| `LastPollNudgeGapMs()` | 最近一次等待里，**相邻两次催重绘的最小间隔** | 次数没超也可能骗人（"每次都催、只在最后一次前停手"）；这条断的是**间隔**本身 |
| `LastWaitRestIntervalMs()` | 轮询歇息间隔（决策参数） | 歇息歇没了 = 忙等烧 CPU |
| `LastWaitRestMs()` | 最近一次等待里**真的**花在歇息上的总时长 | **量出来的**（`Sleep` 前后的时钟差），不是数出来的——把那行 `Sleep` 删掉，计数照样在涨而时长归零 |
| `LastWaitSampleCount()` | 最近一次等待里取了几次样 | 次数多到不像"歇过"就是在忙等 |

> **为什么"量出来"比"数出来"可靠**：本项目已经被"计数写在调用点旁边"咬过一次——把 API 调用整段删掉，计数照样在涨，运行时 49 项全过。歇息时长记在 `Sleep` 的**前后**，删掉 `Sleep` 就归零。

#### 运行时新增 8 条断言（`tests/delay_overlay_test.cpp`）

1. 限频间隔 > 0；
2. 限频间隔 ≥ **绝对下限 100ms**（产品报 1ms 来骗过次数上限的路被堵死；200→300ms 合法不假红）；
3. 本次等待的催重绘次数 ≤ `超时 / 限频间隔 + 2`；
4. 相邻两次催重绘的间隔 ≥ 产品报的限频值；
5. 歇息间隔 > 0；
6. 歇息总时长 ≥ `歇息间隔 × 2`（删掉 `Sleep` → 0 → 红）；
7. 取样次数 ≤ `耗时 / 歇息间隔 + 6`（不忙等）；
8. **基线必须采自覆盖层显示之前的屏幕内容**（比"显示之前那块画面的内容"——不是"在窗口创建之前"）。

#### 静态守卫：新增 2 条 + 修 5 条

| 守卫 | 作用 |
|---|---|
| `test_delay_overlay_rate_limits_the_repaint_nudges`（新） | 轮询期催重绘的调用点必须落在一个**有内容的**条件里：不能是 `if (true)`（恒真 = 不限频），也不能是 `if (false)`（恒假 = 不催）；而且条件里必须有比较（抽成 `ShouldNudge(now, last)` 也算——判定会去看那个 helper 的函数体） |
| `test_delay_overlay_falls_back_when_the_picture_cannot_be_captured`（新） | 等待实现里必须有一条"样本拿不到"的分支（与负数比较）且**通向 return**；或写成"样本可用"、出路在 `else` 里。抓屏失败时空转到超时会**把界面冻住 800ms** |
| `test_delay_overlay_captures_a_baseline_before_showing`（改） | 判据从"在 `CreateWindowExW` 之前取"改成"在 `ShowWindow` 之前取"，并加一条"覆盖层不能带 `WS_VISIBLE` 创建"。**`WS_POPUP` 窗口建出来时还没显示**，那时候取样同样是对的——上一版的判据把无害重排判成了缺陷（假红） |
| `test_desktop_repaint_never_erases_or_covers_the_whole_screen`（改） | 实参检查能**穿透 helper 的形参**（`IssueDesktopInvalidate(rect, erase)` → 追到调用点传的实参）；`_effective_argument` 新增 `fn_span` 作用域 |
| `test_delay_overlay_repaint_counter_needs_a_real_request`（改） | "请求结果变量"收**全部**候选而不是第一个（抽 helper 后链路上有两处接住返回值，只取第一个会挑错对象）；能认转发者（`RepaintDesktopRegion` 转发 `IssueDesktopInvalidate`）；类型名不只认 `bool`（`BOOL` 也认）；`_dominated_by` 要求**闸门与被支配的位置在同一个函数里** |
| `body_span`（工具修正） | 跳过**前向声明**（声明后面跟的是分号不是花括号，按"下一个 `{`"找会一路找到后面另一个函数的头上，把别人的函数体当成自己要查的） |

#### 关于"产品二进制纯净性"的判据（上一轮验收指出的方法论问题）

> 第 14 轮 B 卷用"扫产品 exe 的字节找符号名"作为判据，并**实测证明该判据恒真**：MSVC Release 的 exe 里根本不含符号名（函数名只活在 obj/PDB 里，而 PDB 不随产品发布）。把 `SNAPLITE_DELAY_TEST_HOOKS` 加进产品目标、编出一个**故意泄漏**的产品，exe 字节扫描**仍然是 0 命中**，而干净产品的 obj 里钩子符号数 = 0、泄漏版 = 18。

所以守卫改成**源码级**门禁（这才真正有效）：

1. 头文件里这组接口的**声明**在 `#ifdef SNAPLITE_DELAY_TEST_HOOKS` 块内（**全部 25 个**，上一版只查了 2 个，漏了 20 多个）；
2. **实现**（在 `delay_overlay.cpp` 里）同样在 `#ifdef` 块内——只看头文件会漏掉这一半：声明挡住了、实现没挡住，符号照样进产品；
3. 没有任何源文件无条件 `#define` 这个宏；
4. CMake 里只有测试目标定义这个宏，产品目标不定义。

**这一批的验证方式**：

- **16 条静态守卫变异**逐条跑过（`%TEMP%\snaplite_b14_mut.py`，每条都在 TEMP 副本上跑完整守卫套件）：**16/16 全部符合预期**——
  - 7 条"**必须红**"全红：`if (true)` 去限频、删掉轮询歇息、删掉"抓不到画面"的兜底、整块桌面重绘、擦背景、忽略 API 返回值、删掉超时分支；
  - 2 条"**必须红**"是本批**顺带补上的旧缺口**（第 14 轮 B 卷实测这两条静态层是绿的）：删掉"请求没成功就到此为止"那道闸门、把计数搬到调用点——现在都红了；
  - 7 条"**必须绿**"全绿：前向声明 + 代码前移、常量整体改名、轮询歇息写死字面量、基线取样挪到建窗之后（无害）、把重绘整段抽成 helper、限频间隔改成 0（交给运行时抓）。
- **运行时变异**（`%TEMP%\snaplite_b14_rt.py`，每条都在 TEMP 副本上**重新编译**再跑真实桌面测试，串行）：**6/6 全部符合预期**。

  | 编号 | 精确改法 | 运行时 | 静态 | 实测红在哪一条 |
  |---|---|---|---|---|
  | 基线 | —— | 绿 | 绿 | `PASS: 76 delay overlay checks` + `Ran 78 tests` |
  | **M5** | 限频判断改成 `if (true) {` | **红** | **红** | 运行时第 58 项：`the product rate-limits the repaint nudges; nudges=18 max=6 interval=200ms timeout=800ms`（去掉限频后 800ms 内催了 **18 次**） |
  | **M5b** | 限频常量改成 `0` | **红** | 绿 | 运行时第 56 项：`the product reports the repaint nudge interval it uses`（静态层不判数值，交给运行时抓） |
  | **M7** | 删掉 `Sleep(kRepaintPollMs);` | **红** | **红** | 运行时第 61 项：`the wait really rested between the pixel checks; rested=0ms interval=30ms` |
  | **M9b** | 基线取样挪到 `ShowWindow` 之后 | **红** | **红** | 运行时第 42 项：`the baseline is not a hard-coded zero; reported=0 expected=15392` |
  | **M11** | `if (remaining < 0)` 改成 `if (false) {` | 绿 | **红** | 抓屏失败在运行时**无法触发**（要断开显示链路），所以只能由静态守卫兜住 |

  > M9b 的失败信息与第 14 轮 B 卷实测的**完全一致**（`reported=0 expected=15392`），说明这一条行为被稳定复现。

- **ctest 4/4 全绿**：`snip_interaction` 266 项 / `delay_overlay` **76 项**（68 + 8）/ `build_scripts` **78 项**（76 + 2）/ `verify_generated_sources` 38 条。
- 产品二进制纯净性（**obj 级**，不是 exe 字节级）：`build_mine\CMakeFiles\SnapLite.dir\src\delay_overlay.cpp.obj` 里**钩子符号数 = 0**，而 `SnapLiteDelayOverlayTests` 的同名 obj 里 = **28**。exe 字节扫描对两者都是 0 命中（恒真判据）。
  > 注意扫描要用**精确符号**（名字后面不能跟标识符字符）：朴素的子串搜索会把产品自己的 `WaitForOverlayColoursGoneImpl` 误报成测试钩子 `WaitForOverlayColoursGone`。
- 产品二进制：`SnapLite.exe` 仍为 **454144 字节**（本批只加测试钩子，产品构建不含它们）。

另外断了一件**相反**的事：**记录器不是测试符号**。`RepaintTarget` / `RepaintEraseFlag` 是产品自己的函数（它们在实参表达式里，必须在产品构建里一直存在），把它们当"测试钩子"藏进 `#ifdef` 会让运行时测试彻底失去鉴别力（计数不再涨，两边对不上）。守卫现在明确要求这两个函数**不许**出现在条件编译块里。

### 第十八批：第 15 轮验收（A=FAIL 发布链与守卫）的修复

第 15 轮 A 卷的变异矩阵：**35 条里 30 条符合预期，5 条不匹配**——其中 **3 条是真实绕过**、**2 条是假红**。

| 编号 | 第 15 轮实测 | 根因 | 本批的修法 |
|---|---|---|---|
| **B6** 🔴 | 抽 helper 之后，调用点传 `erase=TRUE` → 守卫**绿** | `_effective_argument` 的形参穿透里有一条 `if resolved.startswith("&")` 的过滤——那条过滤是给"**矩形必须有边界**"用的，却一并套到了**擦除标志**上：擦除标志解析出来的 `"TRUE"` 不以 `&` 开头 → 被丢弃 → 回退成形参名 `"erase"` → 比对 `"ERASE" != "TRUE"` → 绿 | `_effective_argument` 增加 `address_only` 参数：**矩形**要求解析结果以 `&` 开头，**擦除标志**不要求（否则擦背景就没人拦了） |
| **E5** 🔴 | 限频比较写成恒真 `if (now - last >= 0)` → 守卫**绿** | 守卫只问"条件里有没有比较"，没问"**阈值是不是正的**"；而且 `lastRepaintAt` 是从 `now` **派生**的（`lastRepaintAt = now;`），不在"从时钟读来的变量"集合里 | 新增 `_rate_limit_threshold_is_positive()`：跟**时钟变量**比的那一项，其阈值必须 > 0；`_time_variables()` 改为算**传递闭包**（`x = y;` 且 `y` 是时钟变量 → `x` 也是） |
| **F5** 🔴 | 兜底分支被钉成恒假 `if (remaining < 0 && kRepaintTimeoutMs < 0)` → 守卫**绿** | 守卫只问"有没有这条分支、块里有没有 return"，没问"这条分支**会不会执行**" | 新增 `_condition_is_dead()`：能静态求值的常量比较（`kRepaintTimeoutMs` = 800 → `800 < 0` 恒假）→ 整个 `&&` 恒假 → 死分支 → 红 |
| **F2** 🟠 | 兜底分支改成"条件=样本可用、出路在 `else` 里" → 守卫**红**（假红） | ① `_else_block()` **差一错误**：`block_end` 本身就是右花括号的下标，从它自己开始找 `else` 永远找不到；② 判定只认"样本在左边"，认不出**操作数对调** | `_else_block()` 从 `block_end + 1` 开始找；新增 `_sample_availability()`：能认出 `0 > remaining` 与 `remaining < 0` 等价，也能认出"条件=可用、出路在 else" |
| **F3** 🟠 | 兜底条件操作数对调 `if (0 > remaining)` → 守卫**红**（假红） | 同上（只认一种方向） | 同上 |
| **F2 连带** 🟠 | 同一条改动让**另一条**守卫也红：`test_delay_overlay_repaint_wait_is_verified_and_bounded` 的"比较的另一端必须是基线参数" | 那条守卫只看**紧跟**像素计数之后的第一条判断；把 `if (a >= 0 && a <= 基线 + 容忍度)` 拆成嵌套两层是**等价重构**，第一条判断里就只剩 `a >= 0` 了 | 放宽成"**整个等待实现里**，样本必须与某个参数比较过"；同时补一条守卫变异（`M12`：把基线比较换成写死的数字）确认**没有放松过头**——它仍然红 |

**这一批的验证方式**：

- **23 条静态守卫变异**逐条跑过（`%TEMP%\snaplite_b14_mut.py`）：**23/23 全部符合预期**——13 条"必须红"全红（含第 15 轮实测的 3 条真实绕过与 2 条历史缺口）、10 条"必须绿"全绿（含第 15 轮实测的 2 条假红）。
- 守卫套件 **78 项全绿**。

### 第十九批：第 15 轮验收（B=PASS 延时覆盖层运行时）里新挖出来的 8 条

第 15 轮 B 卷判 **PASS**（任务书必测的 16 条变异 16/16 符合期望），但它自己又造了 **28 条**变异，打穿运行时层，得出 **3 条 🟠 + 5 条 🟡**。这一批逐条修。

| 编号 | 第 15 轮实测 | 本批的修法 |
|---|---|---|
| **P1** 🟠 | 限频彻底拆掉、再把上报的间隔/次数/间隔值**一起造假**（N2b）→ 运行时 76 项全绿。根因：次数上限是拿**产品自报的间隔**算出来的 | 运行时改用**测试自己量的墙钟** + **测试侧的下限**算上限：`nudges <= 1 + 外部墙钟 / 100ms`（这条**不看产品自报的任何一个数**），并在"等待跑满上限"的场景加 `nudges >= 2`（一次长等待至少催两次，否则"让桌面重画那块像素"这条依托就退化成摆设） |
| **P2** 🟠 | "量出来的歇息时长"与"取样次数"两条一起被改成"数出来的"（N5）→ 运行时全绿 | 加一条**不依赖产品自报间隔**的上界：`歇息总时长 <= 等待本身的墙钟`。谎报时"轮数"是真的，歇息就会超出墙钟 → 红 |
| **P3** 🟠 | 限频间隔调到 60000ms（一次等待只催一次）→ **运行时与静态两层同时放绿** | 同 P1 的 `nudges >= 2`（只等满上限的等待才要求）；静态层另加"限频比较的**阈值必须 > 0**"（`if (now - last >= 0)` 恒真 = 不限频） |
| **P4** 🟡 | 运行时断言的"真相源"是产品自己的记录器：记录器写错 → **误报**成产品做错；记录器撒谎 → **瞒过**真实缺陷 | **定位为设计边界，不改**：能看穿"真正交给 Win32 的实参"的是**静态层**（它会顺着 `_effective_argument` 追到 `RepaintEraseFlag()` 的返回值）；运行时层只能看产品上报的值。这一条写进审计报告，不再假装运行时能分辨 |
| **P5** 🟡 | 静态守卫对等价重构仍假红 3 处：换时钟（`GetTickCount64` → `QueryPerformanceCounter`）、给产品记录器改名、轮询循环改 `do-while` | ① `_wait_implementation` 认**任意**时钟 API（直接调用或**包一层**的函数，都算传递闭包）；`_time_variables` 同步放宽；② 记录器**不再按名字认**——改成"谁在**写** `g_lastRepaint*` 这几个记录位，谁就是记录器"（只看"提到"会把钩子的 getter 也算进去）；③ `loop_blocks` 支持 `do { ... } while (...)` |
| **P6** 🟡 | 静态守卫只比"取样位置 vs `ShowWindow`"，不查"**上报的基线是不是取样之后赋的**"（M9b2：取样挪到上报赋值之后 → 运行时红、静态绿） | 追加一条：上报基线的赋值必须**在取样之后**（按"取样存进了哪个变量 → 谁把它报出去"来配对，不写死变量名） |
| **P7** 🟡 | 新钩子 `LastWaitPollRepaintCount` 数的是"打算催"而不是"催成了"，与由 API 返回值驱动的 `LastPollRepaintCount` 语义不一致，无人交叉校验 | 把新计数挪到**同一道闸门后面**（`RequestDesktopRepaint` 里"请求真的发出去了"之后），改成由 Win32 的返回值驱动 |
| **P8** 🟡 | 歇息断言没有前置条件：等待偶尔提前结束时（区域意外变干净、或抓屏失败走兜底分支）歇息自然很小 → **同一个二进制两跑结论不同** | 歇息/取样两条断言加上前置条件（只在"等待确实跑满了上限"时才判），下界也放宽成"至少歇过一个完整间隔"（原来是"两次"，太紧） |

**这一批的验证方式**：

- **静态守卫变异 27 条**全量跑过（`%TEMP%\snaplite_b14_mut.py`）：第 15 轮 A 卷实测的 5 条 + B 卷新造的 4 条静态变异全部按预期。
- **运行时变异 9 例**（`%TEMP%\snaplite_b14_rt.py`）：每例都在 TEMP 副本上**重新编译**再跑真实桌面测试，串行。
- **ctest 4/4 全绿**：`snip_interaction` 266 项 / `delay_overlay` **79 项**（76 + 3）/ `build_scripts` 78 项 / `verify_generated_sources` 38 条。

### 第二十批：第 15 轮验收（A=FAIL 发布链与守卫）的 4 条已确认缺陷

第 15 轮 A 卷的结论是 **FAIL**：它自造 39 条变异，**34 条符合预期、5 条不匹配**（其中 3 条在并发修复后已闭环，2 条当时仍开放）。它另外指出 4 条**已确认缺陷**（P1–P4）与 2 条低优先级问题（P5/P6）。这一批逐条修。

| 编号 | 第 15 轮实测 | 本批的修法 |
|---|---|---|
| **P1** 🔴 | **`_else_block()` 被并发修复截断**：方法体只剩 `index = block_end + 1`（**没有 return**），原来的实现被挤到类体末尾、成了 `_rate_limit_threshold_is_positive` 的 `return True` 之后的**死代码**。探针实测 `_else_block(...)` → `None`，于是"出路在 else 里"那种写法（守卫自己在注释里声称支持）**从来没生效过** | 把被挤走的 11 行**搬回** `_else_block` 方法体。（这条是**我自己的补丁**把新方法插在方法体中间造成的——审阅 agent 独立发现） |
| **P2** 🔴 | 钩子实现放进 **`#else` 分支** → 守卫**绿**。根因：`#ifdef` → `#endif` 用的是"找第一个 `#endif`"，**不区分 `#else`**；而宏未定义时 **`#else` 分支是会被编进产品的** | 新增 `compiled_spans()` / `excluded_spans()`：按**配对栈**处理 `#if/#ifdef/#ifndef/#else/#elif/#endif`，算出"给定宏未定义时哪些区间会被编译"。钩子声明与实现必须在**被挡掉**的区间里；记录器必须在**会被编译**的区间里 |
| **P3** 🔴 | 新计数 `++g_lastWaitPollRepaintCount` **不受**"必须被 API 返回值支配"这条约束保护：判据的正则 `\+\+\s*g_last(?:Destroy\|Poll)RepaintCount` 匹配不到 `g_lastWaitPollRepaintCount` → 把它搬到调用点，静态层**全绿** | 正则补上 `WaitPoll`：`g_last(?:WaitPoll\|Destroy\|Poll)RepaintCount` |
| **P4** 🟠 | helper 形参穿透**只检查第一个调用点**：抽 helper 后再加一个调用点传 `(&wholeDesktop, TRUE)`（整块桌面 + 擦背景）→ 守卫**绿** | `_effective_argument` 拆成 `_effective_arguments`（返回**所有**调用点解析出来的值）；矩形与擦除标志**逐个**检查 |
| **P5** 🟠 | 报错里显示的是 helper 的**形参名**（"rect"），不是真正传进去的值（"nullptr"）→ 排查时指向错的地方 | 报错里带上**真正传下去的值**（`实参 … → 真正传下去的是 …`） |
| **P6** 🟡 | `#ifdef`→`#endif` 的配对不认**嵌套**：块中间插一对嵌套 `#ifdef` → 守卫把内层 `#endif` 当成外层结束 → 之后的钩子被判成"在条件编译块之外"（假红） | 与 P2 一并解决（配对栈） |

**这一批的验证方式**：

- **静态守卫变异 30 条**全量跑过（`%TEMP%\snaplite_b14_mut.py`）：第 15 轮 A 卷实测的 5 条 + B 卷新造的 4 条静态变异 + 本批新加的 3 条（`#else` 绕过 / 第二个调用点 / 新计数搬家）全部按预期。
- 守卫套件 **78 项全绿**。

| 功能 | 说明 |
|---|---|
| **延时截图** | 托盘菜单「延时截图」提供 3/5/10 秒。屏幕顶部居中显示倒计时覆盖层，点击或 Esc 取消。这补上了竞品分析里的第一功能缺口（用来抓右键菜单、吐司提示这类"刚出现就消失"的界面） |
| **统一构建入口** | `scripts/build_local.ps1` 自动定位 VS Build Tools、导入 `vcvars64`、调用 CMake + Ninja，并做 `-Clean` 的路径边界校验 |
| **构建期生成管线的回归测试** | `tests/test_build_scripts.py`（58 项，纯 Python 不依赖 Windows 桌面）：完整跑一遍生成链、校验产物内容、验证"篡改产物能被校验器抓到"，外加**源码级守卫**把结构性不变量钉死（断言写的是**关系**：谁在谁之前、比较的另一端是谁、关键判断不能藏在恒假分支里） |
| **生成产物内容校验** | `scripts/verify_generated_sources.py`：38 条断言，含"生成的头文件必须与仓库逐字节一致""产物里的工具索引映射必须是对的" |
| **CI 真正跑测试** | 审计时 222 项交互断言**编译了但从不执行**（修复过程中增至 246 项，延时覆盖层断言增至 32 项）。`build.yml` 与本地 `build_local.ps1 -Tests` 现在跑的是同一套 4 个 ctest 用例 |
| **发布版本解析可单测** | `scripts/resolve_release_version.py`：纯函数，把"这次该发哪个版本号"从 YAML 里拿出来，用 Python 单测覆盖输入矩阵；**没有候选版本时响亮失败**，不静默回落 |

### 未做 / 待续

- **`.inc` 与 `editor_toolbar.cpp` 仍是模板**：`src/snip_window_original.inc` 里的橡皮实现、`src/editor_toolbar.cpp` 的常量仍由脚本注入，仓库读到的不等于编译产物。同类的还有 `src/snip_window.cpp`、`src/pin_window.cpp`、`src/ocr_result_panel_v2.cpp`（第七批补进这份清单）。这不产生 ODR 风险（已消除头文件那一份），但"读源码 ≠ 读产物"在这些文件上依然存在。彻底收敛需要把剩下的脚本产出一次性合并回 `src/`，属于较大的重构。**第七批已加两道防线**：产物校验会断言产物里的工具索引映射是对的，守卫会钉死"CMake 编译的必须是生成副本"。
- **遮罩绘制仍是整屏拷贝**（P1-1）：`Paint()` 每帧整屏 blit，4K 拖拽 15–30ms/帧。改成脏区 blit 需要同时限制遮罩的绘制范围，否则半透明遮罩会重复叠加 —— 属于渲染路径的精细改造，需要像素级回归验证。
- **马赛克仍有一点 GDI churn**：`ApplyMosaic` 每次涂抹新建一个 DC。这是**每次鼠标移动一次**，不是每个采样点，代价可以忽略；真正严重的"每个采样点 3 个 Region"已经在橡皮路径上改成了笔画级复用。
- **选区在首次标注后即锁定**：一旦画过任何东西，选区边框就不能再拖（`HitSelection` 在 `edited_` 为真时直接返回 `DragMode::None`）。这意味着"合成图缓存键里的选区矩形"在正常使用中几乎不会变化——保留它是为了正确性，不是为了性能。
- **延时截图的 `Esc` 取消是全局轮询**：覆盖层是 `WS_EX_NOACTIVATE`，不抢键盘焦点，收不到按键消息，只能靠 `GetAsyncKeyState(VK_ESCAPE)` 的边沿检测识别。代价是**在其它程序里按 `Esc` 也会取消这次倒计时**。可靠取消请点覆盖层。改成"只有本进程在前台时才认 Esc"会误伤"切到别的应用抓瞬时界面"这个主要用途，因此保留现状并在此记录。
- **OCR 取不到"用户刚输入但尚未烘焙的文字标注"**：OCR 之前只提交文字编辑、不做烘焙，而烘焙会清掉几何标注。改法涉及产品取舍（要不要为了取字牺牲标注），本轮不做，记录在此。
- **仍需真机验证**：Windows OCR 引擎的真实像素上限（代码依据是注释里的 2600）、4K 下 OCR 冻结时长的实测值、混合 DPI 机器上的选区对齐与面板截断。**延时截图是否残留倒计时覆盖层已不再是挂账**：第七批用运行时测试在真实桌面上断言过了（覆盖层贡献的像素在抓屏时刻必须归零）。
- **CI 构建路径已本地验证**：本机没有 VS 2022 生成器（只有 VS 2026 工具链），改用 **Ninja Multi-Config** 近似 CI 的多配置流程。**仍未验证的只有一点**：GitHub `windows-latest` 的无头会话能否跑通真实窗口交互测试（这是会话可用性问题，不是构建问题）。

---

## 0. 结论摘要

**项目整体是健康的**：GDI 生命周期管理干净、撤销历史已有压缩策略、frame buffer 已复用、
运行时零外部依赖、Windows OCR 免安装是真差异化。**没有发现架构级崩坏或大面积内存泄漏。**

**但存在一个结构性问题盖过所有其它发现**：`src/` 里的源码不是编译器读的那份。
7 个 Python 脚本在 CMake *configure 期* 改写 C++ 源码，编译的是改写产物。
后果是**读源码 ≠ 读产物**，且改源码不触发重新生成。这个问题一旦不解决，
下面每一条功能改进都要**同时改两份**。

按"多 agent 独立命中同一处 = 高置信度"排序，最该动的五件事：

| # | 事项 | 命中 agent | 收益 |
|---|---|---|---|
| 1 | 源码真源劈叉：模板 / 生成物 / 枚举错位 | 1, 4, 5, 6 | 消除后续所有改动的双倍成本 |
| 2 | OCR 全程阻塞 UI 线程，4K 下冻结 2–5 秒 | 2, 3 | 把"卡死"变成"可用" |
| 3 | OCR 降采样被自己废掉，长边 >2600px 直接报错 | 2 | 修一个真 bug，4K 选区 OCR 从不可用变可用 |
| 4 | 交互测试编译了但**从不执行**，222 项断言没进过 CI | 6 | 1 行 CI 改动接住所有回归 |
| 5 | 遮罩绘制每帧整屏拷贝，4K 拖拽 15–30ms/帧 | 2 | 脏区 blit，4K 拖拽提速 10–20× |

---

## 1. P0 — 必须先处理

### P0-1 源码真源被劈成两份，枚举值已经错位

**证据**：
- `CMakeLists.txt:23-103` 共 7 次 `execute_process`，全部在 configure 阶段。
- `src/app.cpp:7` `#include "snip_window.h"` → MSVC 引号包含先搜 includer 所在目录 → 命中**仓库版**
  `src/snip_window.h`（`Tool` = None, Rectangle, Arrow, Pen, Mosaic, Text → **Mosaic=4, Text=5**）；
  而编译单元 `generated_eraser_history/src/snip_window.cpp` 包含同目录的**生成版**
  （多出 `Eraser` → **Mosaic=5, Text=6**）。
- `BitmapHistory` 在仓库版是 `class BitmapHistory : public std::vector<HBITMAP>`（`src/snip_window.h:43`），
  被 `scripts/optimize_bitmap_history.py:36-259` 整类换成压缩版。
- 模板常量与产物不一致：仓库 `src/editor_toolbar.cpp:30,34,35` = `kPrimaryHeight 42 / kToolCount 6 /
  kToolWidth 78`；编译产物 = `52 / 7 / 50`。仓库版**根本没有"橡皮"工具**，也没有 OCR 按钮。
- `src/snip_window.cpp` 用魔法数字 `UiSetTool(2/3/4/5)` 映射工具
  （`scripts/apply_eraser_history_patch.py:311-321`），与 `Tool` 枚举解耦。

**实际影响**：
1. 任何新增的 `src/` 内 TU 一旦写 `Tool::Mosaic`，编译出的立即数是 4，而生成版是 5 →
   **点马赛克变成别的工具，不报任何错**。
2. `BitmapHistory` 两版 sizeof 恰好都是 24 字节纯属巧合，一旦在 `undo_`/`redo_` 之后加成员
   （`src/snip_window.h:253-258`），偏移立刻错位 → 堆越界。
3. 照着 `src/` 读代码的人会认为 OCR 和橡皮不存在（agent 4、5 均独立踩到）。
4. 改 `src/editor_toolbar.cpp` / `pin_window.cpp` / `ocr_result_panel_v2.cpp` **不直接生效**。

**改法**：
1. 把压缩版 `BitmapHistory` 手工落地进 `src/snip_window.h`；
2. 删除 `scripts/optimize_bitmap_history.py` + `CMakeLists.txt:37-45`；
3. 删除 `apply_eraser_history_patch.py:19-26`（它对 `snip_window.h` 的替换被上一个脚本整段覆盖，
   **`kMaxStoredBytes=512MB` 从未进入二进制**，实际是 64MB）；
4. 从 `PATCHED_FILES` 移除 `snip_window.h`，生成目录不再持有头文件副本 → ODR 分歧从根上消失。

**回归项**：撤销/重做深度、Ctrl+Z 行为、内存占用上限。

### P0-2 改源码不触发重新生成（configure 期生成的经典坑）

**证据**：`CMakeLists.txt:23-103` 全在 configure 阶段；全文无 `add_custom_command`、无 `DEPENDS`、
无 `CMAKE_CONFIGURE_DEPENDS`。`build_release/build.ninja` 的 `RERUN_CMAKE` 依赖表里**没有 `scripts/*.py`**，
且仍钉着 `SNAP_LITE_VERSION=L"1.4.0"`（当前 CMakeLists 已是 1.6.0）。

**影响**：改 `src/` 或改 Python 脚本后 `cmake --build build` **不会重新生成**，编译旧产物。
「改了没反应」排查成本极高，IDE 里同样如此。

**改法**：
```cmake
# 全部 9 个脚本各加一行
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  scripts/generate_eraser_history_sources.py scripts/optimize_bitmap_history.py ...)
```
长期形态：把 7 个脚本串成一条 `add_custom_command(OUTPUT ... DEPENDS scripts/*.py src/*.cpp ...)`。

**注意**：P0-1 做完后生成链会从 7 步收敛到 2–3 步，依赖声明才好维护。

### P0-3 OCR 全程阻塞 UI 线程

**证据**：
- `src/ocr_v2.cpp:622-624`：`std::async(std::launch::async, ...).get()` —— **`.get()` 立刻阻塞调用线程**，
  `std::async` 的异步在这里等于同步。
- `src/ocr_v2.cpp:183-298` `PrepareBitmap`（Otsu + 3× StretchBlt）也在 UI 线程。
- `src/ocr_v2.cpp` 落盘 3 张临时 PNG（GDI+ 编码 33 MB）也在 UI 线程。

**实测代价**（4K 选区）：PrepareBitmap 3× ≈ 120–275ms，PNG 编码 3× ≈ 0.9–2.7s，
解码+Recognize ≈ 1–2.5s → **合计冻结 2–5 秒**（1080p 0.8–2 秒）。
UI 表现为"未响应"、等待光标不闪烁、**无法取消**。

**改法**：
1. `PrepareBitmap` + 落盘 + 识别整体搬到 worker 线程；
2. 完成时 `PostMessage(WM_OCR_DONE)` 回主线程更新面板；
3. 面板加"识别中…"状态与取消按钮；
4. `BitmapDecoder` 支持 `IStream` → 去掉 3 个临时文件与 3 次磁盘写。

**收益**：冻结 2–5s → 0。这是用户能直接感知的最痛点。

### P0-4 OCR 降采样被自己废掉（真 bug）

**证据**：`src/ocr_v2.cpp:196-198`
```cpp
scale = std::min(scale, 2600.0 / sourceMax);
scale = std::max(1.0, scale);   // ← 这一行把上一步抵消了
```
长边 > 2600px 的选区**不会缩小**，送进 `OcrEngine.RecognizeAsync` 超过引擎的
`MaxImageDimension=2600` 会抛异常 → 提示"OCR 识别失败"。

**影响**：4K 屏上截任何宽区域**必然失败**。而这恰好是 OCR 最该发挥的场景（长文档、表格）。

**改法**：删掉 `std::max(1.0, scale)` 那一行；同时给引擎留出余量（如降到 2400）。
**附带收益**：预处理位图 33 MB → 15 MB。

### P0-5 交互测试编译了但从不执行

**证据**：`.github/workflows/build.yml:35,38` 打开 `SNAPLITE_BUILD_INTERACTION_TESTS=ON` 并编译
`SnapLiteInteractionTests`，但**全仓库 workflow 里 `ctest` 出现 0 次**。
`tests/snip_interaction_test.cpp` 约 222 项真实 Win32 断言从未进过 CI；
`release.yml:65` 更甚，连测试目标都不编译。

**可行性**：测试全程用 `SendMessageW`（`:27`）+ `SetKeyboardState` 伪造 Ctrl（`:46-55`），
**不需要物理鼠标**；`windows-latest` 有 window station/desktop，窗口创建大概率可用。
唯一风险是无头会话下 `CaptureVirtualScreen` 返回 nullptr 导致 `tests:343` 断言失败。

**改法**（`build.yml` Build 之后加一行）：
```yaml
- name: Run interaction tests
  run: ctest --test-dir build -C Release --output-on-failure
```
首次可能红一次，按上述 null 路径加个 skip 即可。

---

## 2. P1 — 明确该改

### 2.1 性能

| # | 问题 | 证据 | 代价 | 改法 | 收益 |
|---|---|---|---|---|---|
| P1-1 | 遮罩每帧整屏拷贝 | `snip_window_original.inc:979` `BitBlt(frameDc,0,0,W,H,...)`，`frameDc_` **无裁剪区**（`snip_window.cpp:817-830` 只 SelectObject）；`:1040` 再整屏拷到窗口 | 4K **15–30ms/帧**，1080p 4–8ms | `Paint()` 用 `GetUpdateRgn` 取脏区，只 blit 脏矩形 | 4K **25ms → 1–3ms（10–20×）** |
| P1-2 | `UiCaptureBitmap()` 每次调用整屏刷新 | `snip_window.cpp:795-815` 每次 = CloneBitmap + 整屏 BitBlt + 重画全部箭头；调用点 `edit_repaint_fix.h:123`（**每个按键**）、`editor_toolbar_anime.cpp:639`（每次 WM_ERASEBKGND） | 4K 每按键 **+5–15ms** | 给箭头集合加版本号，版本未变直接返回缓存的 `arrowComposite_` | 每按键省 5–15ms |
| P1-3 | 橡皮/马赛克每采样 GDI churn | `snip_window_original.inc:88-129` `RestoreOriginalBrush` 每个采样点新建 2 DC + 3 Region；400px 移动 ≈ 100 采样 = **500 对 GDI 对象创建/销毁**；`snip_window.cpp:577-588` `EraseArrows` 每次还调 `HitArrow`（每次 `GetDC/ReleaseDC` + 每标注一个 `Gdiplus::Graphics`） | 15–40ms | DC/clip 区提到笔画级复用；命中测试改纯几何 | **500 对 → ~10 对；15–40ms → 3–8ms** |
| P1-4 | 贴图缩放整幅重采样 | `pin_window.cpp:899-915` 每次 `WM_PAINT`（=每滚轮刻度）`StretchBlt` 整幅；且 `SetStretchBltMode(dc, HALFTONE)` 设在**目标** DC（:901）而源是 `memory`（:900）—— 按 MSDN 拉伸模式取自**源** DC，HALFTONE 很可能未生效 | 4K 贴图 **50–200ms/刻度** | 缩放比例不变时复用缓存位图；模式改 `COLORONCOLOR`（或把 HALFTONE 移到源 DC） | 50–200ms → 5–20ms |
| P1-5 | 冷存压缩失败会重复编码 PNG | `optimize_bitmap_history.py:224-242`：`if (compressed.size() >= entry.rawBytes) continue;` 时 `entry.bitmap` 非空、`entry.png` 为空 → 下次 push **重新编码同一张图**，第 n 次编辑触发 O(n) 次全分辨率编码 | 每次多 1 次 PNG 编码（1080p 80–200ms） | 失败时也要标记状态（存 raw 或标记"不可压缩"），避免重复编码 | 消除 O(n²) |
| P1-6 | `TrimWorkingSet` 反而拖慢下一次截图 | `app.cpp:194-199` 每次截图后调用，4K 下次截图需重新缺页调入全部位图 | 首次触摸 **+50–200ms** | 改空闲超时再 trim，或直接去掉 | |
| P1-7 | `CaptureVirtualScreen` 每次新建整屏 DDB | `capture.cpp:210-255` 4K 33MB，未复用（v1.5.1 的"复用"只覆盖编辑器 frame buffer 与 arrowComposite） | 每次截图 33MB 分配 | 复用一块全屏 DDB | |

**已做得好的地方（别改回去）**：
- GDI 生命周期整体干净：`Create*` 全部配对 `Delete*`，`SelectObject` 全部换回原对象，
  `WM_NCDESTROY → delete this → 析构` 一次性释放全部资源。
- 撤销历史：`kHotEntries=2` + PNG 冷存 + 128 条/64MB 每栈，**压缩比 6–8×，省掉约 85% 历史内存**。
- 画笔/马赛克**原地烧录**进 `capture_`，每次鼠标移动不产生整幅临时位图。
- 橡皮从 pristine 克隆做 patch 级 BitBlt 还原，同样没有整幅临时位图。
- `arrowComposite_` 只分配一次并复用。
- **GDI 句柄不是真实风险**：稳态 ≈15 个，最坏 ≈270 个，Win11 上限 10000/进程 —— 要泄漏 37 倍才会耗尽。

### 2.2 健壮性

| # | 问题 | 证据 | 改法 |
|---|---|---|---|
| P1-8 | ToolbarHost 生命周期 UAF 隐患 | `editor_toolbar.cpp:1425` + `snip_window_original.inc:1328` + `editor_toolbar.cpp:1540`：`FinishWithText → UiFinish → DestroyWindow → WM_NCDESTROY → delete host`（ToolbarHost 已释放），而 `ExecutePrimaryAction`/`HandleToolbar` 栈帧仍持有已释放的 `this` | `SnipWindow::Finish` 改为"先取回调 → 置 pending → 由 WM_NCDESTROY 之后触发"；或 ToolbarHost 改 `shared_ptr` |
| P1-9 | `SetProcessDpiAwarenessContext` 返回值未检查 | `main.cpp:6` | 检查返回值，失败时回退 `SYSTEM_AWARE` 并提示 |
| P1-10 | 显示器拔插 / 分辨率变化完全未处理 | 全库无 `WM_DISPLAYCHANGE` / `WM_SETTINGCHANGE`；`screen_` 在构造时缓存 `GetVirtualScreen()`（`snip_window_original.inc:221`）→ 覆盖层位置失效、选区可能落在已拔掉的屏、贴图窗口跑到不可见坐标 | 处理这两个消息并重算 `screen_` |
| P1-11 | OCR 面板固定尺寸 vs 缩放字体 | `ocr_result_panel_v2.cpp:16-17` 尺寸固定 430×380，字体按 `GetDpiForWindow` 缩放（`:261-276`）→ 150%/200% 屏文字截断、按钮重叠 | 尺寸与字体统一按 DPI 缩放 |
| P1-12 | 贴图跨 DPI 变糊 | `pin_window.cpp:744-780` `ResizeForZoom` 按 `bitmapWidth_*zoom_` 算像素，**不含 DPI 因子** → 100% 屏拖到 200% 屏被 DWM 拉伸 | 乘 DPI 因子 |
| P1-13 | 标注粗细不缩放但命中半径缩放 | `snip_window.cpp:63` `gStrokeWidth` 直接当像素；命中半径 `MulDiv(10,dpi,96)` **缩放了**（`snip_window_original.inc:375,1225`）→ 视觉与手感不匹配 | 统一 `MulDiv(width, dpi, 96)` |
| P1-14 | 文字烘焙用错屏幕的 DPI | `snip_text_overlay.cpp:24` 用**截图窗口** DPI（面积最大屏）而非**源选区所在屏** DPI → 主屏 100% + 副屏 200% 时烘焙进 PNG 的字明显偏小 | `MonitorFromPoint` 取源屏 DPI |
| P1-15 | 剪贴板被占用 → 静默失败 | `capture.cpp:349-355` `OpenClipboard` 无重试（`pin_window.cpp:332` 却有重试，行为不一致）；`app.cpp:469-478` 只提示"截图已保存"，**不告知复制失败**。`ocr_result_panel_v2.cpp:371,376` `CopyUnicodeTextToClipboard` 返回值被忽略 → "复制选中/全部"**点了没反应** | 统一带重试；失败给出可行动提示 |
| P1-16 | 贴图关闭后按 F3 静默失效 | `pin_window.cpp:721-726` `gDismissedClipboardSequence` 会话级永久抑制；`app.cpp:449-453` 只在返回 false 时提示 → 用户毫无反应 | 区分"已忽略重复序列"与"成功"，后者给气泡提示 |
| P1-17 | `DecodePng` 失败 → 撤销永久卡死 | `snip_window.h:61-76` `take_back` + `snip_window_original.inc:1229-1236`：`DecodePng` 失败时 `take_back()` 不 pop，而 `redo_.push_back()` 已先执行 | 失败时回滚 redo 栈并保留条目 |
| P1-18 | 第二实例启动完全静默退出 | `app.cpp:209-217` 互斥量命中即 `return 0` | `FindWindow` + 自定义消息激活已有实例，或弹气泡 |

**关于 NULL HDC 检查的分歧**：`pin_window.cpp:868-869`（`CreateCompatibleDC` 未判空）与
`snip_window_original.inc:1249-1251`（`ApplyMosaic`）确实是未检查的访问违例路径，
但性能 agent 独立测算出句柄稳态仅 15 个、上限 10000 —— **触发条件极罕见**。
结论：**按 P1 处理，不按 P0**，但改动只有 2 行，顺手做掉。

### 2.3 静默失效与 CI 盲区

| # | 问题 | 证据 | 改法 |
|---|---|---|---|
| P1-19 | 唯一的正则替换会静默失效 | `generate_unified_ui_sources.py:37-54` 用 `re.sub(...)` **未检查是否命中**（同文件 `8-12` 的 `replace_once` 是严格的）→ 一旦重命名 `SliderRect()`/`MenuRowRect()`，贴图菜单布局静默回退旧版，**构建全绿、日志无异常** | 改用同文件其它 8 处一致的 `replace_once`；或 `re.sub` 后断言 `"kMenuWidth - 28, 109"` 已出现。**1 行改动，零风险** |
| P1-20 | 脚本名与实际行为不符（今天即 no-op） | `apply_unified_toolbar_theme.py:22-28` 唯一动作是"若 anime 头里没有 `ui_theme.h` 则补一行 include"，而 `src/toolbar_icon_render_gdi_anime.h:10` 本来就有 → **今天完全不改动文件**，却仍打印 "Applied shared Snap-Lite UI palette" | 重命名或删除；真正调色板改写在 `generate_anime_toolbar_source.py:324-335` |
| P1-21 | 生成管线只查退出码不查产物内容 | `CMakeLists.txt:23-103` 8 个 `execute_process` 只看返回码，无任何内容断言 | 新增 `scripts/verify_generated_sources.py`：断言 `generated_eraser_history/src/snip_window.h` 含 `class BitmapHistory {` 且含 `kMaxStoredBytes`；`editor_toolbar_anime.cpp` 含 `PrimaryAction::Ocr` 与 `L"取字"`；`generated_ui/pin_window.cpp` 含 `ui_theme.h` |
| P1-22 | exe 无 VERSIONINFO | `resources/SnapLite.rc.in:1-4` 只有 3 行（include + ICON + RCDATA）→ Explorer 属性看不到文件版本/产品名 | 补 `VERSIONINFO` 块，用 `${PROJECT_VERSION}` 展开 |
| P1-23 | git 卫生严重超标 | `.gitignore` 只有 12 行、只忽略 `build/`；实测 `build_release/`、`build_verify/`、`.tmp-cmake-check/`、`.tmp-mingw-check/`、`extracted_icon.png`、`dist/`、`package/` **全部未忽略**；`build_verify/` 下 9 个文件（含 `.ninja_deps`、甚至 `.omc/state/agent-replay-*.jsonl` 代理日志）**在 git index 内**，另有 27 个 `.omc/` 运行时文件被 track | `.gitignore` 补 `build*/`、`.tmp-*/`、`dist/`、`package/`、`extracted_icon.png`；`git rm -r --cached build_verify` |
| P1-24 | 三套构建目录并存且工具链不一致 | `build/`（VS2022，CI 同款）、`build_release/`（**MinGW GCC 15.1.0 + Ninja**，日常开发）、`build_verify/`（已删）并存；README 写的生成器与实际日常用法不一致 | 统一到一套；本地也用 VS2022 或 CI 也用 Ninja，避免"本地过 CI 挂" |
| P1-25 | `workflow_dispatch` 无法指定版本 | `release.yml:41,43` 无 inputs → 非 push 时 `releaseVersion` 永远等于 baseVersion | 加 `version` input |
| P1-26 | 预发布版本号可能让 Inno 编译失败 | `release.yml:59,100` 支持含 `-` 的 prerelease 并直传 `$VERSION` → `installer/SnapLite.iss:32` `VersionInfoVersion` 要求点分数字版本 | `VersionInfoVersion` 用 base 版本，另设 `VersionInfoProductVersion` 放 prerelease |

**发布流程里做得好的部分**：`release.yml` 的版本号单一来源 + `StartsWith` 强校验（`:55-57`）
强制 CMake 版本与 `.release/` 说明一致；`gh release create --clobber` 幂等可重跑；
SHA256 校验和已生成；`installer/` 路径与 `release.yml` 完全对得上（是活资产不是死文件）；
`scripts/check_pe_icon.py` 质量很高（解析 PE 头、RT_GROUP_ICON/RT_ICON 结构、PNG 尺寸一致性、
最小 32×32，并用 `shell32.ExtractIconExW` 实测 Shell 提取）。

### 2.4 UX / 无障碍

| # | 问题 | 证据 | 价值 | 难度 |
|---|---|---|---|---|
| P1-27 | 纯键盘无法完成一次标注，更无法建选区 | 工具栏 `WS_EX_NOACTIVATE` 且点击后 `SetFocus(parent_)`（`editor_toolbar.cpp:1586-1591`, `:450`）；颜色/线宽/字号只在第二行自绘控件；选区阶段方向键只挪光标，`Enter` 要求 `selected_` 已成立 | 高（无障碍硬指标） | 中 |
| P1-28 | OCR 面板打开后 Esc/Enter 语义断裂 | `ocr_result_panel_v2.cpp` `PanelProc` 无 `WM_KEYDOWN`（`:248-405`）；面板创建后 `SetFocus(edit)`（`:327`）+ `SetForegroundWindow`（`:491`）抢焦点 → 此时 Esc/Enter 全失效，而工具栏仍提示"取消截图 Esc"（`editor_toolbar.cpp:767`） | 高 | 低 |
| P1-29 | 热键注册失败只靠气泡通知 | `app.cpp:270-282` → `Shell_NotifyIconW(NIIF_INFO)`；Win11 气泡常被专注助手吞掉、不弹模态 → 用户可能永远不知道 F1 失效 | 高 | 低 |
| P1-30 | 第二行工具栏无任何提示 | `scripts/add_toolbar_tooltips.py:52-128` 是**自绘 STATIC 弹窗**（非 `TOOLTIPS_CLASS`）→ 屏幕阅读器不朗读；`HoverTooltipLabel` 只覆盖 `hoverTool_`/`hoverAction_`（`:70-78`），**第二行 8 形状/3 填充/4 线宽/7 箭头/7 字号/7 色块全无提示**；色块 label 本身为空串（`editor_toolbar.cpp:150-156`）→ 用户面对一排无名色块 | 高 | 低 |
| P1-31 | 不跟随深色模式 / 不满足高对比度 | 全库无 `WM_SETTINGCHANGE`/`WM_THEMECHANGED`/`ShouldAppsUseDarkMode`；调色板硬编码奶油粉（`ui_theme.h:11-23`、`editor_toolbar.cpp:796` `RGB(252,250,247)`）；无 `GetSysColor` 匹配 | 中高 | 中 |
| P1-32 | 工具栏无障碍名 | 子窗口标题为空串（`editor_toolbar.cpp:1586-1591`）→ 辅助技术读不到任何名称 | 中 | 低 |
| P1-33 | 贴图右键菜单是自绘的，无菜单语义/助记键 | `pin_window.cpp:155-168,283-330` 是 `DrawTextW` 画的行，不是真实菜单 | 中 | 低 |
| P1-34 | `R/A/P/M/T/E` 工具热键从未出现在任何 UI 或文档 | `docs/annotation-shortcuts.md` 只写了对象级快捷键 | 中 | 低 |
| P1-35 | 撤销/重做按钮无禁用态、撤销无视觉反馈 | `editor_toolbar.cpp:830-844` 绘制循环只处理 hover/pressed，全文件无 `disabled` 分支；`UnifiedUndo`（`:1380-1391`）只还原位图 | 中 | 低 |
| P1-36 | 贴图 `Ctrl+滚轮` 透明度零反馈 | `pin_window.cpp:795-798` 每次 13/255 突变 → 用户不知道当前多少 | 中 | 低（约 40 行） |
| P1-37 | 遮罩"闪现" | 全库无 `SetTimer`/`AnimateWindow`/渐变；`kDimIdle=66`/`kDimSelected=148`（`snip_window_original.inc:32-33,912`）瞬间切换 → 用户眼睛要重新适应对比度 | 中 | 中（约 60 行） |
| P1-38 | 撤销后的对象生命周期隐患 | `editor_toolbar.cpp:1425` 等 | 中 | 中 |

### 2.5 死代码清理

| 文件 | 状态 | 依据 |
|---|---|---|
| `src/ocr.cpp` (19KB) | **死代码**，定义 `ExtractTextFromBitmapRegion`（`:496`）与 `ocr_v2.cpp` 符号重复 | CMake 未列、scripts 未读、无人 include |
| `src/ocr_result_panel.cpp` (9KB) | **死代码**，定义 `ShowOcrResultPanel`（`:242`）与 v2 重复 | 编译的是 `generated_ui/ocr_result_panel_v2.cpp` |
| `src/ocr_winrt_compat.h` | **死代码**，全仓零 include | |
| `src/editor_toolbar.cpp` / `src/snip_window.cpp` / `src/pin_window.cpp` / `src/ocr_result_panel_v2.cpp` | **生成器输入模板**，不直接编译 | `CMakeLists.txt:47,105,118,121-122` |

**风险**：grep 时看到两套 OCR 实现，极易改错文件（改 `ocr.cpp` 根本不生效）；
若有人误加进 `add_executable` → 链接期 duplicate symbol。
**注意**：删除 `ocr.cpp` / `ocr_result_panel.cpp` 前需人工 diff 确认 v2 没漏移植能力。

---

## 3. 功能缺口（竞品对照）

### 3.1 先修正一处认知

`src/` 里读代码会以为"橡皮和 OCR 不存在"，**实际都存在**（构建期注入）。
另外 README 漏写了两项已实现的功能：**13×13 放大镜**（`snip_window_original.inc:455-499`）
和**取色器**（`:1336-1349`）。README 应当补上。

### 3.2 P0 级功能建议

| 功能 | 竞品出处 | 实现难度 | 影响面 | 为什么值得做 |
|---|---|---|---|---|
| **延时截图**（3/5/10s） | Win11 自带 `Alt+D`；ShareX Auto capture | **低** | `app.cpp` 加 Timer + 托盘项 | 成本最低的一件；补上"截右键菜单/刚消失的 toast"这个最高频的"截不到"场景 |
| **贴图内二次标注 + 贴图管理** | Snipaste `Space`；PixPin 便签标注 | 中 | `pin_window.cpp` + 复用 `editor_toolbar.cpp` 引擎 + 新增贴图列表窗 | **Snap-Lite 的产品名和核心场景就是贴图，但贴图现在是只读终点**——主流程断了一截。贴图已持有 `HBITMAP`，标注引擎可直接复用，边际成本远低于新写功能 |
| **截图历史面板** | Snipaste `,`/`.`；ShareX History | 中低 | 新增 `history_panel.cpp`；`app.cpp` 加托盘项 | 已有 `BitmapHistory` 基础设施与压缩策略可复用；"刚才那张图我再要一下"是截图工具第二高频真实需求 |

### 3.3 P1 级功能建议

| 功能 | 竞品出处 | 难度 | 说明 |
|---|---|---|---|
| 快捷键自定义 | Snipaste 偏好设置；PixPin Config→Shortcut | 低-中 | `app.cpp:24-27` 的 4 个常量改注册表 + 设置对话框（`ChooseColorW`/Folder Picker 已在用）。F1 常被占用（`app.cpp:270-282` 已有降级提示） |
| 模糊（高斯模糊）/ 荧光笔 / 聚光灯 | Snipaste Blur+Marker；ShareX Blur/Spotlight/Highlight | 低-中 | 马赛克实现（`ApplyMosaic`）已在，模糊可照抄；荧光笔是半透明画笔变体 |
| 活动窗口 / 指定显示器截图 | ShareX Active window / Active monitor | 低 | 窗口识别已实现（`snip_window_original.inc:178-197`），只需 `GetForegroundWindow()` + `GetWindowRect` 走 `CaptureRect` |

### 3.4 P2 级（记录，暂不做）

滚动长截图（PixPin/ShareX/FastStone，成本=其余两项之和）· 贴图旋转/翻转/穿透/阴影 ·
文件名模板 + 多格式输出 · 贴图分组/多贴图列表 · 上传/分享目标（本地工具加账号体系是反命题）。

### 3.5 明确**不建议**做

- **录屏 / GIF** —— Win11 自带 `Win+Shift+R`，重复建设。
- **OCR / 文字识别** —— 已有，且用系统 OCR 比 ShareX 的 Tesseract 更省事。
- **跨平台 / Electron 重构** —— 纯原生单 EXE 是 Snap-Lite 的差异化之一。

### 3.6 差异化优势（应强化为卖点）

1. **剪贴板四格式同时写入**（`capture.cpp:331-406`：CF_BITMAP + CF_DIB + PNG + CF_HDROP）——
   竞品多数只写一种。这直接支撑"可直接粘到 CLI"的卖点。
2. **Windows 系统 OCR，零安装** —— 免 Python、免模型下载。ShareX 走 Tesseract、PixPin 用自研模型，
   Snap-Lite 是"装完即用、不挑环境"的。
3. **中文优先 + 多预处理变体择优**（`ocr_v2.cpp:512-580`：zh-Hans 优先，彩色/增强对比/二值化三路打分择优）——
   **比系统自带 Snipping Tool 更强**，应作为核心差异点写进 README 和发布说明。
4. **几何标注可二次编辑**（`docs/annotation-shortcuts.md`：Ctrl+D 复制、Tab 切换、Ctrl+Home/End 层级、
   方向键 1px 微调）—— 多数竞品的标注是"烧死进位图"的。
5. **纯原生单 EXE**，无 Electron/.NET/Python 运行时，内存占用低。
6. **橡皮恢复原始像素**（不是"擦成白块"），比多数竞品细致。

---

## 4. 建议路线图

### 阶段 A：先止损（约 1 周，零新功能）

让"改代码能生效"和"CI 能挡住坏构建"成立。

1. `generate_unified_ui_sources.py` 的 `re.sub` 改 `replace_once` + 加命中断言（P1-19，1 行）
2. 删死代码 `ocr.cpp` / `ocr_result_panel.cpp` / `ocr_winrt_compat.h`（P2 前置，需先人工 diff）
3. `.gitignore` 补全 + `git rm -r --cached build_verify`（P1-23）
4. 加 `CMAKE_CONFIGURE_DEPENDS`（P0-2）
5. CI 加 `ctest`（P0-5）+ 零依赖 Python 单测锁生成管线锚点（P1-21）
6. 补 exe VERSIONINFO（P1-22）

### 阶段 B：收敛真源（约 1 周）

7. 压缩版 `BitmapHistory` 手工落地进 `src/snip_window.h`，删 `optimize_bitmap_history.py` +
   `apply_eraser_history_patch.py` 对 `snip_window.h` 的替换 → **双份头与两脚本争抢一次解决**（P0-1）
8. anime 版工具栏尺寸常量和动作宽度归回 `src/editor_toolbar.cpp`，移除 `generate_anime_toolbar_source.py` 中的常量改写；
   工具索引改 `enum` 而非魔法数字（P0-1 续）
9. 重新审视是否保留"Python 改写 C++"作为长期机制。README 明确写了这是有意设计，
   但若目标是"源码唯一真源"，推荐把剩下 2–3 个脚本的产出一次性合并回 `src/`，然后删除整条生成链。

> **回归清单**：撤销/重做深度、Ctrl+Z、橡皮、形状 8×3 矩阵、马赛克、OCR 按钮、工具栏布局宽度。

### 阶段 C：性能与体验（约 2 周）

10. OCR 全量异步化（P0-3）—— **用户可感知的最大痛点**
11. 删掉 `ocr_v2.cpp:197` 的 `std::max(1.0, scale)`（P0-4）—— 4K OCR 从报错变可用
12. `Paint()` 脏区 blit（P1-1）+ `UiCaptureBitmap()` 版本缓存（P1-2）
13. 橡皮/马赛克 GDI churn 降级（P1-3）+ 贴图缩放缓存（P1-4）
14. UX 三件套：OCR 面板补 Esc/焦点归还（P1-28）、热键失败改模态提示（P1-29）、
    第二行工具栏补 tooltip 与色块名（P1-30）
15. 静默失败全部改成可行动提示（P1-15、P1-16、P1-18）

### 阶段 D：功能扩展（按需求驱动）

16. 延时截图 → 贴图内二次标注 + 贴图管理 → 截图历史面板
17. 可选：快捷键自定义、高斯模糊、活动窗口截图

---

## 5. 本报告未覆盖 / 需人工确认

1. **`/FI` 强制包含的宏劫持**：`window_bounds_fix.h:41` `#define GetWindowRect(...)`、
   `drag_repaint_fix.h:32` `#define InvalidateRect(...)`。
   `GetWindowRect` 被改写为 DWM 可见边框，会影响"自动识别窗口"的选区边界 —— **只做了静态阅读，未运行验证**。
   建议去宏化，但**必须先有回归测试兜底**。
2. **已完成旧 OCR 实现与 v2 人工对照**：旧版的裁剪、Otsu 预处理、候选识别、质量评分、剪贴板复制均在 v2 保留；结果面板保留选择复制/全部复制，并新增异步回调、Esc 关闭与焦点归还。旧 `ocr_winrt_compat.h` 没有独立业务能力。
3. **`windows-latest` 无头会话下交互测试是否真能通过** —— P0-5 的唯一未知，需一次 CI 实验。
4. **Inno Setup 对 `VersionInfoVersion=1.6.0-rc.1` 的实际反应** —— 未实测。
5. **`optimize_bitmap_history.py` 的 PNG 压缩路径实际收益** —— v1.5.x "perf" 宣称的核心，需一次真实测量。
6. **4K 整屏选区点"取字"的实际冻结时长** —— 上面 2–5s 是估算，需 `QueryPerformanceCounter` 实测。
7. **混合 DPI 真机验证**：跨屏选区对齐、副屏 200% 烘焙字号、OCR 面板 150% 截断、
   贴图 100%→200% 变糊、截图进行中拔副屏、125%/150% 下线条手感。
8. **多贴图共存**：所有贴图都是 `WS_EX_TOPMOST`（`pin_window.cpp:693`）→ 后建者永久压住先建者，
   且 `zoom_`/`opacity_` 是每实例成员、**不持久化**（全库唯一注册表写入点只有截图目录，
   `capture_settings.cpp:11-12`）→ 新贴图一律 1.0×/100%。
9. **`.release/` 目录格式混乱**：41 个文件混着 `.md`(24) / `.txt`(16) / 无扩展名(`v1.2.1`)，
   `release.yml:46` 用"取最后一个"策略，无法表达一次发多版本。
10. **代码签名**（P1 之外）：无 Authenticode → 用户下载 exe/安装包会被 SmartScreen 拦截。
    需证书，属一次性投入，建议单独排期。

## 第十五轮收尾：本地实现与验证（2026-10-23）

本轮按 `TODO.md` 收敛阶段 A/B/C，保留用户选择的构建期 Python 生成链。Python 仍只在 CMake 配置阶段运行，生成的应用仍是原生 C++；工具栏尺寸改由 `src/editor_toolbar.cpp` 保存，生成脚本不再重写这些值，工具索引改为 `SnipWindow::UiToolIndex`。此调整不改变当前已经生成并运行的工具栏外观。

### 本轮代码收尾

- `SnipWindow::Paint()` 用 `PAINTSTRUCT::rcPaint` 限制离屏合成和回屏复制范围，并保存/恢复 DC 状态。
- `BitmapHistory` 保持源码内实现；工具栏布局常量回到仓库源码，数值索引统一改用枚举。
- 修正源码守卫对未定义宏 `#else` 分支的判断。之前 `compiled_spans()` 在计算 `#else` 时误把当前分支算入父条件，导致一个产品编译路径上的测试钩子漏判；现在明确按父级条件计算，并增加回归测试。
- 中文 PowerShell 下新配置 CMake/Ninja 时，CMake 3.30 可能把 MSVC `/showIncludes` 前缀解码成乱码，Ninja 因而记录不到头文件依赖。本地 `zh-CN` 构建入口传入已验证的前缀，CMake 将其应用于 Ninja。全新构建已确认交互测试源记录 141 个依赖。
- 手工对照旧 OCR 与 v2 实现，确认图像裁剪、Otsu 预处理、候选识别与质量评分、文本复制能力都已保留；v2 还增加异步完成回调，结果面板保留选择复制/全部复制并补上 Esc 关闭和焦点归还。

### 验证结果

- 全新 `build_mine/fresh_config_prefix_fixed_20261023` 构建通过；`ctest` **4/4 全绿**：`snip_interaction` 266、`delay_overlay` 79、`build_scripts` 81、`verify_generated_sources` 46。
- 静态守卫变异套件全量复跑 **mismatches=0**。第一轮的 H4b 变异曾暴露 `#else` 漏判；修复后 H4b 单项和完整变异集均按预期报红。
- 撤销栈产品级变异：G5/G6/G7 报红，G8 等价写法通过，M3-NOH 在“裁掉的最早步骤必须确实消失”探针报红；恢复后基线通过。旧验收副本的 MSVC 依赖扫描缺失，基线复跑时强制重编了测试入口；正式本地构建现在会正确记录头文件依赖。

双屏/混合 DPI、CI 会话 0 的交互分支、真实 4K OCR 耗时与 OCR 语言包端到端仍需对应设备或环境；代码签名仍需证书。提交、推送和覆盖安装版未执行。
