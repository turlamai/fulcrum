# Fulcrum DESIGN.md — 视觉与交互契约（单一真源）

原型（`design/prototype/`）与三宿主实现（`macos-host/` SwiftUI · `windows/` WinUI 3 · `linux/` GTK4）都遵循本文件；改视觉先改这里，再同步实现。
Tokens 提取自现行实现，保证原型 → 原生实现零翻译成本：`docs/design-system.md`（契约基线）· `docs/cross-platform-interaction.md`（交互语法）· `macos-host/Sources/RivetHost/ContentView.swift`（macOS 字号与 alpha）· `linux/src/main.cpp`（GTK 玻璃底亮暗值与 alpha 层级）· `windows/App.xaml` + `windows/MainWindow.xaml`（XAML amber 层级与字号）· `shared/i18n/{en,zh}.json`（文案单源）。

## Brief（2026-11-11）

- **目标**：非从零重设计。把三宿主已收敛的视觉契约提炼为可评审的 HTML 原型；为「结果列表是主角」的 launcher 形态做 HIG 级打磨；给规划中的 Workflow Pro（多步自动化编排/调试）一个可讨论的形态探索稿。现有 launcher 能力保持免费，原型不出现任何付费墙。
- **原型范围**（6 屏 × 亮/暗）：
  ① `panel.html` 主面板（英雄屏：query 输入态 + 混合 provider 结果）
  ② `states.html` 关键状态三联（默认 ready / 搜索无结果 / 启动中）
  ③ `actions.html` ⌘K 次级操作（剪贴板行 → Copy/Paste/Pin/Delete，前后对照）
  ④ `ai.html` 内联计算 + BYOK AI 流式（running/done 两态）
  ⑤ `settings.html` 设置窗口（General/Features/AI/Commands/Sync & Updates 五区）
  ⑥ `workflow.html` Workflow Pro 编排概念（探索稿，非承诺）
- **Exit 条件**：6 屏关键状态齐（含空态/无结果/启动中/流式中）+ 交互逻辑与 `docs/cross-platform-interaction.md` 一致 + HIG 清单全过 + 用户评审通过。
- **不在范围**：商业/定价与激活 UI（PRICING 未开卖）、支付/权利界面、把 Workflow Pro 当已实现功能呈现、RVT1 契约变更、真实 blur/acrylic/vibrancy 实现（原型以静态玻璃底示意）。

## 设计原则（HIG 内核）

- **一框即一切**：query 是唯一输入面——apps、`find` 文件、`!bang` 网页、`ai` 问答、`add snippet`、`gallery` 插件都从同一框到达；没有模式切换，就没有学习成本。
- **结果列表是主角**：680×460 面板里 chrome 只有 query、发丝线、footer 三件；行高 44 保副标题可读，图标 tile 与 badge 只为扫读服务，装饰不与结果争注意力。
- **键盘即语法**：↑↓ 选择、↵ 执行、Tab 补全、⌘K 次级操作、Esc 阶梯（actions → detail → AI → 清 query → 隐藏）三平台语义一致（⌘K/Ctrl+K 按平台惯例）；hint bar 常驻，高频操作永远有可见快捷键提示。
- **反馈及时**：AI 流式有 running（▍ 光标 + 转轮）/done（已复制提示）/error 三态；更新、设置、插件动作的反馈写进 status line，永不静默。
- **原生材质，家族皮肤**：blur/acrylic/vibrancy 是平台机制，玻璃底 + amber 家族色是契约；无 compositor 的 Linux 会话用 ≥98% 不透明底——清晰优先于透明。
- **层级克制**：glass 一层表面 + 发丝线分隔足够，不用阴影堆叠表达层级；accent 只给选中、行图标、badge、↵ chip 与进行态。

## Tokens（与实现 1:1）

| Token | Light | Dark | 用途 |
|---|---|---|---|
| `--glass` | `rgba(250,250,249,.98)` | `rgba(28,28,30,.98)` | 面板玻璃底（GTK 值；macOS vibrancy / Windows acrylic 以此为基线） |
| `--ink` | `#1d1d1f` | `#f2f2f2` | 主文字 |
| `--mute` | `rgba(29,29,31,.60)` | `rgba(242,242,242,.62)` | 行副标题 / hint（macOS secondary） |
| `--faint` | `rgba(29,29,31,.38)` | `rgba(242,242,242,.40)` | status line / 最弱文字 |
| `--line` | `rgba(0,0,0,.10)` | `rgba(255,255,255,.10)` | 面板描边 |
| `--hairline` | `rgba(0,0,0,.08)` | `rgba(255,255,255,.08)` | 分隔线 |
| `--accent` | `#D97706` | `#D97706` | 选中/行图标/badge/↵ chip/进行态（两态同值） |
| `--sel` | `rgba(217,119,6,.14)` | 同 | 选中行底 |
| `--hover` | `rgba(217,119,6,.08)` | 同 | 悬浮行底 |
| `--tile` | `rgba(217,119,6,.12)` | 同 | 图标 tile 底 |
| `--badge` | `rgba(217,119,6,.14)` | 同 | badge / ↵ chip 底 |
| `--danger` | `#d02f26` | `#ff5449` | 仅错误与破坏性操作（源自系统红） |

- alpha 层级对应实现：SwiftUI `.opacity(0.08/0.12/0.14)` · GTK `alpha(#D97706, …)` · XAML `#14/#1F/#24`（pressed 另有 `#2E/#38`）。dark 态沿用同一 alpha（GTK shared_css 已验证）。
- **尺寸**：面板 680×460 · 设置窗 720×520 · 圆角 12（窗）/ 8（行）/ 6（tile）· 行高 ≥44 · 图标 tile 28。
- **字号**：query 19 medium · 行标题 13 medium · 行副标题 11.5 · badge 10 semibold · hint/status 11 · AI/详情正文 12.5。
- **字体**：原型用 `system-ui, -apple-system, "Segoe UI", "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif`；实现端各用原生默认栈。
- **语言**：界面文案单源 `shared/i18n/{en,zh}.json`，改语言即时生效、不重启；原型 UI 文案取 en（i18n canonical），键盘提示按平台语法（macOS ⌘K，Win/Linux Ctrl+K）。

## 组件词汇

`Panel`（680×460 玻璃面板）· `QueryField`（19px + 放大镜，唯一输入面）· `ResultRow`（icon tile + 标题/副标题 + 尾部 badge；↵ chip 仅选中行）· `KindIcon`（28px amber tile + 语义图标；app 行用真实应用图标，Calculator 与 AI 永不共用字形）· `AiStreamPanel`（running/done/error 三态，由 `ai-stream` State 驱动，打字不中断）· `DetailPanel`（FPP2 插件详情：text + metadata 键值）· `HintBar`（footer 快捷键提示，随 Esc 阶梯换文案）· `StatusLine`（动作反馈落点）· `SettingsWindow`（侧栏五区 + 原生表单）· `CommandBindingRow`（alias + 录制热键）· Workflow 探索稿专用：`StepCard`（单步：触发/输入/条件/动作）· `RunTrace`（调试运行轨迹）。

## 不变式

1. `accent` 只用于：选中底、图标 tile、badge、↵ chip、AI/详情标题、进行态；禁止大面积色块与纯装饰。
2. 一行 = 28 tile + 标题/副标题 + 尾部 badge；↵ chip 只出现在选中行；行高 ≥44 不因密度压缩。
3. Esc 阶梯固定：actions → detail → AI → 清 query → 隐藏；HintBar 实时反映当前层（actions 层为「esc back」）。
4. 玻璃可模糊，结果不可牺牲：无 blur 平台 ≥98% 不透明；亮暗成对设计，两态都过对比度自检。
5. 设置与文案单源 `shared/i18n`，键值/描述/可用性三宿主一致；OS 专属项（如 Start at login）留在原生设置、永不进同步 schema；BYOK key 永不镜像同步。
