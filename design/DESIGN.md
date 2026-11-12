# Fulcrum DESIGN.md — 视觉与交互契约（单一真源）

原型（`design/prototype/`）与三宿主实现（`macos-host/` SwiftUI · `windows/` WinUI 3 · `linux/` GTK4）都遵循本文件；改视觉先改这里，再同步实现。
Tokens 提取自现行实现，保证原型 → 原生实现零翻译成本：`docs/design-system.md`（契约基线）· `docs/cross-platform-interaction.md`（交互语法）· `macos-host/Sources/RivetHost/ContentView.swift`（macOS 字号与 alpha）· `linux/src/main.cpp`（GTK 玻璃底亮暗值与 alpha 层级）· `windows/App.xaml` + `windows/MainWindow.xaml`（XAML amber 层级与字号）· `shared/i18n/{en,zh}.json`（文案单源）。

## Brief（2026-11-11）

- **目标**：非从零重设计。把三宿主已收敛的视觉契约提炼为可评审的 HTML 原型；为「结果列表是主角」的 launcher 形态做 HIG 级打磨；给规划中的 Workflow Pro（多步自动化编排/调试）一个可讨论的形态探索稿。现有 launcher 能力保持免费，原型不出现任何付费墙。
- **原型范围**（6 屏 × 亮/暗 + 自检页）：
  ① `panel.html` 主面板（英雄屏：query 输入态 + 混合 provider 结果）
  ② `states.html` 关键状态三联（默认 ready / 搜索无结果 / 启动中）
  ③ `actions.html` ⌘K 次级操作（剪贴板行 → Copy/Paste/Pin/Delete，前后对照）
  ④ `ai.html` 内联计算 + BYOK AI 流式（running/done/error 三态，error 含 401 原因与重试路径）
  ⑤ `settings.html` 设置窗口（General/Features/AI/Commands/Sync & Updates 五区）
  ⑥ `workflow.html` Workflow Pro 编排概念（探索稿，非承诺）
  ⑦ `audit.html` 原型自检（命中高亮 / 输入法守卫 / 焦点顺序 / 状态矩阵集中验收）
- **Exit 条件**：6 屏 + 自检页关键状态齐（含空态/无结果/启动中/流式中/离线/限流/敏感数据）+ 交互逻辑与 `docs/cross-platform-interaction.md` 一致 + HIG 清单全过 + 用户评审通过。
- **不在范围**：商业/定价与激活 UI（PRICING 未开卖）、支付/权利界面、把 Workflow Pro 当已实现功能呈现、RVT1 契约变更、真实 blur/acrylic/vibrancy 实现（原型以静态玻璃底示意）。

## 设计原则（HIG 内核）

- **一框即一切**：query 是唯一输入面——apps、`find` 文件、`!bang` 网页、`ai` 问答、`add snippet`、`gallery` 插件都从同一框到达；没有模式切换，就没有学习成本。
- **结果列表是主角**：680×460 面板里 chrome 只有 query、发丝线、footer 三件；行高 44 保副标题可读，图标 tile 与 badge 只为扫读服务，装饰不与结果争注意力。
- **键盘即语法**：↑↓ 选择、↵ 执行、Tab 补全、⌘K 次级操作、Esc 阶梯（actions → detail → AI → 清 query → 隐藏）三平台语义一致（⌘K/Ctrl+K 按平台惯例）；hint bar 常驻，高频操作永远有可见快捷键提示。组合态（preedit 未上屏）下 Enter / ↑↓ / Tab 归输入法；⌘K 进入 actions 后 Esc 按 item id 回到原行，不取第一行。
- **反馈及时**：AI 流式有 running（▍ 光标 + 转轮）/done（已复制提示）/error 三态；更新、设置、插件动作的反馈写进 status line，永不静默。
- **原生材质，家族皮肤**：blur/acrylic/vibrancy 是平台机制，玻璃底 + amber 家族色是契约；无 compositor 的 Linux 会话用 ≥98% 不透明底——清晰优先于透明。
- **层级克制**：glass 一层表面 + 发丝线分隔足够，不用阴影堆叠表达层级；accent 只给选中、行图标、badge、↵ chip 与进行态。

## Tokens（与实现 1:1）

| Token | Light | Dark | 用途 |
|---|---|---|---|
| `--glass` | `rgba(250,250,249,.98)` | `rgba(28,28,30,.98)` | 面板玻璃底（GTK 值；macOS vibrancy / Windows acrylic 以此为基线） |
| `--ink` | `#1d1d1f` | `#f2f2f2` | 主文字 |
| `--mute` | `rgba(29,29,31,.66)` | `rgba(242,242,242,.66)` | 行副标题 / hint（macOS secondary） |
| `--faint` | `rgba(29,29,31,.62)` | `rgba(242,242,242,.52)` | status line / 最弱文字（≥4.5:1） |
| `--line` | `rgba(0,0,0,.10)` | `rgba(255,255,255,.10)` | 面板描边 |
| `--hairline` | `rgba(0,0,0,.08)` | `rgba(255,255,255,.08)` | 分隔线 |
| `--accent` | `#D97706` | `#D97706` | 色块用 amber：选中底、tile 底、switch、caret、spin |
| `--accent-text` | `#9A4B06` | `#F0A64B` | 文字/图标用 amber：badge、↵ chip、标题、选中侧栏项、tile 图标（亮暗各自 ≥4.5:1） |
| `--sel` | `rgba(217,119,6,.14)` | 同 | 选中行底 |
| `--hover` | `rgba(0,0,0,.05)` | `rgba(255,255,255,.07)` | 悬浮行底（中性，不与选中抢 amber） |
| `--tile` | `rgba(217,119,6,.12)` | 同 | 图标 tile 底 |
| `--badge` | `rgba(217,119,6,.14)` | 同 | badge / ↵ chip 底 |
| `--danger` | `#d02f26` | `#ff5449` | 仅错误与破坏性操作（源自系统红） |

- alpha 层级对应实现：SwiftUI `.opacity(0.08/0.12/0.14)` · GTK `alpha(#D97706, …)` · XAML `#14/#1F/#24`（pressed 另有 `#2E/#38`）。dark 态沿用同一 alpha（GTK shared_css 已验证）。
- **尺寸**：面板 680 宽、高随结果数增长（240–640，12 行封顶；设置「Maximum results」上限与可视行数一致）· 设置窗 720×520 · 圆角 12（窗）/ 8（行）/ 6（tile）· 行高 ≥44 · 图标 tile 28。
- **图标**：全站功能图标统一为 2px stroke、round join/cap 的线性家族；不再用 `❝`/`✦`/`=`/`>_` 等文字字符充当图标。评审稿中的应用行用中性 app 类别字形，宿主实现替换为真实 dock 图标，不伪造品牌 mark。
- **命中高亮**：query 命中字符用 `--accent-text` + 650 字重（颜色 + 字重双线索），不铺底色；命中发生在副标题或来源时，高亮落在对应字段，行语法不变。
- **字号**：query 19 medium · 行标题 13 medium · 行副标题 11.5 · badge 10 semibold · hint/status 11 · AI/详情正文 12.5。
- **字体**：原型用 `system-ui, -apple-system, "Segoe UI", "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif`；实现端各用原生默认栈。
- **语言**：界面文案单源 `shared/i18n/{en,zh}.json`，改语言即时生效、不重启；原型 UI 文案取 en（i18n canonical），键盘提示按平台语法（macOS ⌘K，Win/Linux Ctrl+K）。

## 组件词汇

`Panel`（680×460 玻璃面板）· `QueryField`（19px + 放大镜，唯一输入面）· `ResultRow`（icon tile + 标题/副标题 + 尾部 badge；↵ chip 仅选中行）· `KindIcon`（28px amber tile + 语义图标；app 行用真实应用图标，Calculator 与 AI 永不共用字形）· `AiStreamPanel`（running/done/error 三态，由 `ai-stream` State 驱动，打字不中断）· `DetailPanel`（FPP2 插件详情：text + metadata 键值）· `HintBar`（footer 快捷键提示，随 Esc 阶梯换文案）· `StatusLine`（动作反馈落点）· `SettingsWindow`（侧栏五区 + 原生表单）· `CommandBindingRow`（alias + 录制热键）· Workflow 探索稿专用：`StepCard`（单步：触发/输入/条件/动作）· `RunTrace`（调试运行轨迹）。

## 不变式

1. amber 分两个 token：`accent` 只做色块（选中底、tile 底、switch、caret、进行态）；`accent-text` 只做文字/图标（badge、↵ chip、AI/详情标题、选中侧栏项、tile 图标），两者亮暗各自 ≥4.5:1。禁止大面积色块与纯装饰。
2. 一行 = 28 tile + 标题/副标题 + 尾部 badge；↵ chip 只出现在选中行；行高 ≥44 不因密度压缩。悬停是中性底，不与选中 amber 抢；actions 层的选中行改用 1.5px 内描边环，层级靠视觉而非读字区分。
3. Esc 阶梯固定：actions → detail → AI → 清 query → 隐藏；HintBar 实时反映当前层（actions 层为「esc back」，并带 Tab 补全与层级标识）。
4. 玻璃可模糊，结果不可牺牲：无 blur 平台 ≥98% 不透明；亮暗成对设计，两态都过对比度自检。
5. 设置与文案单源 `shared/i18n`，键值/描述/可用性三宿主一致；OS 专属项（如 Start at login）留在原生设置、永不进同步 schema；BYOK key 永不镜像同步。
6. 命中必须可见：结果行里的 query 命中片段用 `accent-text` + 650 字重标记；只是「出现」而不标命中，等于让用户自己猜匹配原因。
7. 输入法守卫：compositionstart 后 Enter / ↑ / ↓ / Tab 归输入法，不得执行或移动选择；compositionend 才恢复。Esc 第一次只取消组合，不走层级阶梯。
8. 焦点契约：Tab 顺序 query → 结果列表（整体一个 tab stop）→ modebar 返回；⌘K 焦点落首条 action，Esc 按 item id 回到原行；全站 2px `accent-text` ring（行内 offset -2px），焦点与选中是两种视觉。
9. 敏感数据默认掩码：剪贴板中的凭证以字符掩码呈现并标 `Sensitive`，⌘Y 仅在本机揭示，揭示后才允许复制；揭示不写日志、不进同步。


## 状态矩阵（`prototype/audit.html`）

`audit.html` 是 Exit 条件的自检页，把命中高亮、输入法组合、焦点顺序，以及启动 / 离线 / AI running·done·401·429 / 敏感剪贴板 / 超长 CJK / 结果超上限集中在一页。任何新增状态先补进页内矩阵表，再回填对应屏；**表中出现而屏里找不到的状态视为未收口**。


## 平台映射与差异声明（预钉 · 宿主实现时升级为落点表）

统一层（三平台必须一致，`design/prototype/` 原型已锁定）：信息架构、核心流程、快捷键语义、
语义色与层级词汇。因地制宜层（按平台惯例落地，不预画像素）：

| 维度 | macOS | Windows | Linux |
|---|---|---|---|
| 修饰键 | ⌘ | Ctrl | Ctrl |
| 命令入口 | 原生菜单栏 | 菜单栏/命令行内 | 菜单栏 |
| 字体 | 系统栈 + PingFang SC | 系统栈 + Microsoft YaHei | 系统栈 + Noto Sans CJK |
| 原生控件 | SwiftUI 控件 | WinUI 3 控件 | GTK4 控件 |

差异声明纪律：宿主特例必须先写进本节才能实现；未声明的宿主差异按违约处理
（案例：taskly 漂移台账 D8）。
