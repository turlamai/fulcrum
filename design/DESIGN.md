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
  ⑤ `settings.html` 完整设置窗口（Workspace / Features / Preferences 分组；含文件索引、网络来源与各功能管理）
  ⑥ `workflow.html` Workflow Pro 编排概念（探索稿，非承诺）
  ⑦ `audit.html` 原型自检（命中高亮 / 输入法守卫 / 焦点顺序 / 状态矩阵集中验收）
- **Exit 条件**：6 屏 + 自检页关键状态齐（含空态/无结果/启动中/流式中/离线/限流/敏感数据）+ 交互逻辑与 `docs/cross-platform-interaction.md` 一致 + HIG 清单全过 + 用户评审通过。
- **不在范围**：商业/定价与激活 UI（PRICING 未开卖）、支付/权利界面、把 Workflow Pro 当已实现功能呈现、RVT1 契约变更、真实 blur/acrylic/vibrancy 实现（原型以静态玻璃底示意）。

## 设计原则（HIG 内核）

- **一框即一切**：query 是唯一输入面——apps、`find` 文件、`!bang` 网页、`ai` 问答、`add snippet`、`gallery` 插件都从同一框到达；没有模式切换，就没有学习成本。
- **结果列表是主角**：680×460 面板里 chrome 只有 query、发丝线、footer 三件；行高 44 保副标题可读，图标 tile 与 badge 只为扫读服务，装饰不与结果争注意力。
- **键盘即语法**：↑↓ 选择、↵ 执行、Tab 在查询与结果之间移动焦点、⌘K 次级操作、Esc 阶梯（actions → detail → AI → 清 query → 隐藏）三平台语义一致（⌘K/Ctrl+K 按平台惯例）；hint bar 常驻，高频操作永远有可见快捷键提示。组合态（preedit 未上屏）下 Enter / ↑↓ / Tab 归输入法；⌘K 进入 actions 后 Esc 按 item id 回到原行，不取第一行。
- **反馈及时**：AI 流式有 running（▍ 光标 + 转轮）/done（已复制提示）/error 三态；更新、设置、插件动作的反馈写进 status line，永不静默。
- **原生材质，家族皮肤**：blur/acrylic/vibrancy 是平台机制，玻璃底 + amber 家族色是契约；无 compositor 的 Linux 会话用 ≥98% 不透明底——清晰优先于透明。
- **层级克制**：glass 一层表面 + 发丝线分隔足够，不用阴影堆叠表达层级；accent 只给选中、行图标、badge、↵ chip 与进行态。

## Tokens（语义；值不在此复制）

值类 token 真源 = 现行实现（`docs/design-system.md` 契约基线 · `macos-host/Sources/RivetHost/ContentView.swift` macOS 字号与 alpha · `linux/src/main.cpp` GTK 玻璃底亮暗值 · `windows/App.xaml` + `windows/MainWindow.xaml` XAML amber 层级）；原型 `design/prototype/shared.css` 与实现值一一对应，本表只记语义与层级。

| Token | 语义 |
|---|---|
| `--glass` | 面板玻璃底（GTK 值；macOS vibrancy / Windows acrylic 以此为基线） |
| `--ink --mute --faint` | 主文字 / 行副标题 hint / status line 三档（faint ≥4.5:1） |
| `--line --hairline` | 面板描边 / 分隔线 |
| `--accent` | amber 色块专用：选中底、tile 底、switch、caret、spin |
| `--accent-text` | amber 文字/图标专用：badge、↵ chip、标题、选中侧栏项、tile 图标（亮暗各自 ≥4.5:1） |
| `--sel --hover --tile --badge` | 选中行 / 中性悬浮（不与 amber 抢）/ 图标 tile / badge·↵ chip 底 |
| `--danger` | 仅错误与破坏性操作（源自系统红） |

- alpha 层级对应实现：SwiftUI `.opacity(0.08/0.12/0.14)` · GTK `alpha(#D97706, …)` · XAML `#14/#1F/#24`（pressed 另有 `#2E/#38`）。dark 态沿用同一 alpha（GTK shared_css 已验证）。
- **尺寸**：面板 680 宽、高随结果数增长（240–640，12 行封顶；设置「Maximum results」上限与可视行数一致）；小屏按工作区 55% 宽 / 65% 高收窄（见「启动窗几何与多显示器」）· 设置窗桌面宽 1080、最高 840，窄屏重排 · 圆角 12（窗）/ 8（行）/ 6（tile）· 行高 ≥44 · 图标 tile 28。
- **图标**：全站功能图标统一为 2px stroke、round join/cap 的线性家族；不再用 `❝`/`✦`/`=`/`>_` 等文字字符充当图标。评审稿中的应用行用中性 app 类别字形，宿主实现替换为真实 dock 图标，不伪造品牌 mark。
- **命中高亮**：query 命中字符用 `--accent-text` + 650 字重（颜色 + 字重双线索），不铺底色；命中发生在副标题或来源时，高亮落在对应字段，行语法不变。
- **字号**：query 19 medium · 行标题 13 medium · 行副标题 11.5 · badge 10 semibold · hint/status 11 · AI/详情正文 12.5。
- **字体**：原型用 `system-ui, -apple-system, "Segoe UI", "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif`；实现端各用原生默认栈。
- **语言**：界面文案单源 `shared/i18n/{en,zh}.json`，改语言即时生效、不重启；原型 UI 文案取 en（i18n canonical），键盘提示按平台语法（macOS ⌘K，Win/Linux Ctrl+K）。

## 组件词汇

`Panel`（680×460 玻璃面板）· `QueryField`（19px + 放大镜，唯一输入面）· `ResultRow`（icon tile + 标题/副标题 + 尾部 badge；↵ chip 仅选中行）· `KindIcon`（28px amber tile + 语义图标；app 行用真实应用图标，Calculator 与 AI 永不共用字形）· `AiStreamPanel`（running/done/error 三态，由 `ai-stream` State 驱动，打字不中断）· `DetailPanel`（FPP2 插件详情：text + metadata 键值）· `HintBar`（footer 快捷键提示，随 Esc 阶梯换文案）· `StatusLine`（动作反馈落点）· `SettingsWindow`（分组侧栏 + 设置搜索 + 原生表单）· `CommandBindingRow`（alias + 录制热键）· Workflow 探索稿专用：`StepCard`（单步：触发/输入/条件/动作）· `RunTrace`（调试运行轨迹）。

## 不变式

1. amber 分两个 token：`accent` 只做色块（选中底、tile 底、switch、caret、进行态）；`accent-text` 只做文字/图标（badge、↵ chip、AI/详情标题、选中侧栏项、tile 图标），两者亮暗各自 ≥4.5:1。禁止大面积色块与纯装饰。
2. 一行 = 28 tile + 标题/副标题 + 尾部 badge；↵ chip 只出现在选中行；行高 ≥44 不因密度压缩。悬停是中性底，不与选中 amber 抢；actions 层的选中行改用 1.5px 内描边环，层级靠视觉而非读字区分。
3. Esc 阶梯固定：actions → detail → AI → 清 query → 隐藏；HintBar 实时反映当前层（actions 层为「esc back」，并提示焦点去向和当前层级）。
4. 玻璃可模糊，结果不可牺牲：无 blur 平台 ≥98% 不透明；亮暗成对设计，两态都过对比度自检。
5. 设置与文案单源 `shared/i18n`，键值/描述/可用性三宿主一致；OS 专属项（如 Start at login）留在原生设置、永不进同步 schema；BYOK key 永不镜像同步。
6. 命中必须可见：结果行里的 query 命中片段用 `accent-text` + 650 字重标记；只是「出现」而不标命中，等于让用户自己猜匹配原因。
7. 输入法守卫：compositionstart 后 Enter / ↑ / ↓ / Tab 归输入法，不得执行或移动选择；compositionend 才恢复。Esc 第一次只取消组合，不走层级阶梯。
8. 焦点契约：Tab 从查询进入选中结果，列表只有一个 Tab 停靠位，Shift+Tab 返回查询；⌘K / Ctrl+K 将焦点交给当前结果的首选操作，Esc 按结果 ID 返回原行；全站 2px `accent-text` ring（行内 offset -2px），焦点与选中是两种视觉。
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

### 已声明的平台差异（2026-11-20 设置页收口）

- **设置 IA**：分组侧栏 Workspace / Features / Preferences，页面集合与行序以「2026-11-20 · 设置与搜索来源补齐」和 `prototype/settings.js` 为准；旧六区侧栏已退役。设置窗全部 chrome 文案走 `shared/i18n` 单源（含侧栏、页标题、hint、占位符、选项与动态行），改语言即时刷新已打开的设置窗。
- **更新交互（统一层）**：任何入口（托盘/应用菜单「检查更新」、启动器 `update.*` 行、设置 Sync 页按钮）都唤起同一个独立更新窗并立即手动检查；窗内语义 checking → up-to-date / available → downloading（进度）→ downloaded（退出并安装）/ failed（重试）。Close 只隐藏窗口，下载继续。设置 Sync 页 = 自动检查说明 + 「立即检查更新」按钮，不再内联更新卡片。
- **更新机制（因地制宜层）**：macOS 用原生 CryptoKit 校验 + RivetUpdate 下载后原子替换并自动重启（打包版 Racket 解不了签名公钥）；Windows/Linux 用后端签名下载，宿主执行安装交接（Windows MSI handoff、Linux AppImage 原子替换），安装前必须拿到后端已验证的票据。
- **更新窗取消与完成态**：macOS 的传输由宿主自己持有，下载中可 Cancel，校验通过后自动重启；Windows/Linux 的传输由后端持有（无取消 RPC），更新窗不提供 Cancel，只有 Close（隐藏、下载继续），下载完成后停在「退出并安装」确认步。
- **Linux 命令热键**：别名三端一致；全局命令热键在 Linux 暂未注册，录制控件置灰并展示声明文案（`settings.hint.commands.linux`）。
- **Linux 片段自动展开**：设置项展示但置灰（`settings.hint.expansion.linux`），保持三端功能地图一致。
- **Start at login**：三端均为本机原生机制（macOS Login Items / Windows 注册表 Run / Linux autostart desktop），不进同步 schema；错误文案按平台键（`settings.login.failed.*`）。

### 启动窗几何与多显示器（2026-11-19 收口，2026-11-24 小屏适配）

统一层：启动窗设计尺寸 680×640（DIP/point/应用像素）是**上限**而非固定值；结果区滚动、窗体不随结果伸缩；
每次唤醒跟随**指针所在显示器**，水平居中、垂直置于工作区上方四分之一处，并按该显示器工作区**留边适配**：
宽度 ≤ 工作区 55%、高度 ≤ 工作区 65%。标准桌面保持 680×640；低分辨率或高缩放屏按比例收窄变矮，
可见结果行减少、列表滚动，字体与行高不变。只做贴边钳制时 680×640 在 1280×800 屏上占满 85% 高度，
所以钳制不再是唯一规则。三宿主都不得用主显示器工作区代替指针显示器。

| 平台 | 尺寸换算 | 混合 DPI/分辨率 |
|---|---|---|
| macOS | AppKit point，天然与 backing scale 无关 | 系统自动按屏幕缩放；唤醒时按目标屏 visibleFrame 的 55%/65% 适配 |
| Windows | PerMonitorV2；物理像素 = DIP × 目标显示器 DPI/96，每次唤醒重算；工作区百分比在同一物理像素单位下比较 | 跨显示器触发 `WM_DPICHANGED`：先让 WinUI 更新渲染缩放，再立即重算 monitor-aware 几何，禁止系统建议矩形二次缩放（否则首次跨屏唤醒尺寸错误） |
| Linux | GTK 应用像素 × 显示缩放因子 | X11 无逐显示器缩放（工具包限制，全局 Xft.dpi），按指针显示器工作区 55%/65% 适配位置与尺寸；Wayland 由合成器负责放置与逐 surface 缩放，客户端不控制坐标 |

### Windows 文件搜索覆盖（2026-11-19 收口）

- **后台 catalog**：home 已知文件夹（含 OneDrive 重定向）+ 全部**就绪固定盘**全深度索引。固定盘枚举用 .NET `DriveInfo`（直读驱动表，毫秒级），**不用** `Get-Volume`——其 CIM/WMI 首调在多盘工作站可超 5 s 预算，超时曾静默丢掉全部固定盘，使 find 对 home 之外的文件全盲。后台永不走网络共享（issue #116，防 NAS 磨盘）。
- **显式 `find` 的平台阶段（9 s 预算，逐层只在上一层零结果时触发）**：① 已知文件夹 Depth 4 → ② Windows Search SYSTEMINDEX → ③ **就绪映射网络盘**有界扫描（Depth 3、128 条上限；显式查询是用户意图，网络盘文件不再对每一层都不可见）→ ④ USERPROFILE Depth 3 兜底。
- **平台阶段永不阻塞应答（2026-11-24）**：实测这段 PowerShell 在真实机器（12 TB 映射盘、282 MB catalog）上每次 7–9 s，且每次进程启动后的 catalog 扫描期间每个 `find` 都同步重跑一遍，用户看到 20 s 应答。现在 Windows 的 `find` **只从 catalog 应答**（持久索引从首批批次起就可用，不再等扫描完成），catalog 未命中或不满一页时，平台阶段由引擎在后台跑：0.6 s 去抖、单飞、只搜最新文本、单字符不触发；命中写入持久索引后通过 revision nudge 让宿主重绘当前查询（Windows 宿主已有该刷新路径）。未命中时先显示「No matching files yet」而非宣称搜索结束。macOS/Linux 的 mdfind/locate 仍同步（毫秒级，且其宿主不随 revision 重绘）。
- **诚实状态行**：idle/empty 文案如实描述覆盖（folders、fixed drives、OS index、ready mapped drives），不承诺未索引的范围；`file index` 行随时可查进度。


## 2026-11-15 · 原型交互补齐

入口为 prototype/panel.html。搜索框可输入、筛选样例；上下键选择、Enter 操作、Esc 返回/清空、⌘/Ctrl+K 操作页、⌘/Ctrl+, 设置。其他 HTML 保留多状态对照用途，不代表后台 AI/系统集成已经接通。

根级 index.html 只负责首次进入主界面；页面切换直接到目标界面，不再保留页面目录。原型运行脚本 prototype-runtime.js / prototype-flow.js 只表达浏览器内的交互合同；正式产品业务仍由既定引擎实现。真实设备、模型和发布服务均未连接。凭据演示不保存密钥，破坏性示例与派发保留确认和取消。

这些是可评审的增量改动，未替用户勾选满意度、HIG 最终验收或设计冻结条件。


## Prototype source checkpoint — 2026-11-15

`index.html` opens the primary prototype directly. Screen-directory launchers have been removed. All local scripts, styles and referenced assets needed by this checkpoint are versioned under this directory; OpenDesign metadata is excluded. This is an editable prototype checkpoint, not a claim that the native product or external services implement these interactions.


## 2026-11-20 · 设置与搜索来源补齐

- 入口：`prototype/settings.html`；依赖 `settings.css`、`settings.js` 和既有 `shared.css`。不加载旧版通用设置事件，避免同一控件被重复绑定。保留现有英文原型文案；language 配置表示产品偏好，本次没有创建整站翻译资源。
- 设计方向继续既有中性表面、双 amber token、线性图标和单层结构；设置密度以可读性优先，表单正文 16px，大号可到 18px。桌面分组侧栏，窄屏改横向分类导航，不隐藏核心项。新增图标使用同一 1.8px 线性家族（设置屏内部统一，延续既有线性风格）。
- Workspace：General、File Search、Applications、Commands & Shortcuts。Features：Clipboard History、Snippets、Quicklinks、Extensions、Window Management、Notes、Focus、Translate、Calendar、Calculator & Units、Emoji & Symbols、Screenshots、System & Scripts。Preferences：AI、Privacy & Permissions、Sync & Updates。
- File Search：每个来源有名称、类型、绝对路径、递归开关、扫描周期、启停与管理入口。支持本地、NAS/网络共享、外部磁盘路径样例。移除只移除来源，不删除文件。全局支持文件内容搜索、隐藏文件、ignore 文件、网络与移动盘、断连结果保留、缓存过滤、低磁盘暂停与自定义 glob 排除。
- 来源状态：Ready to scan → Scanning → Ready；初始样例额外包含 Offline、Permission required、Scan failed。断连需确认样例重连，缺权限需确认样例授权，失败可改路径或重试。已暂停或全局排除的来源不启动扫描。所有扫描、重连、授权均为显式本地样例；没有真实文件或网络访问。
- 管理流程：应用来源/应用启停；逐命令别名和热键（唯一性校验）、自定义命令增删；剪贴板期限/记录类型/排除应用/清空确认；Snippets 与 Quicklinks 增删改、关键词唯一、分组及过滤；扩展逐项启停/配置/授权/移除。其余功能均有具体偏好与相关权限或绑定入口，而非占位分类弹窗。
- 设置搜索：Command/Ctrl+F 聚焦搜索，可从结果直达设置并聚焦；Esc 清空搜索。弹窗 Esc/取消均不提交，关闭后优先回原触发控件，列表重绘后回内容区域。组合态 Enter 不提交；多行文本 Enter 保持换行。
- 导入/导出：JSON v2，导出先选分类；默认只选外观与命令。导入先校验格式、文件大小、字段类型、快捷键/别名/关键词冲突及链接形式，再展示分类与替换范围，确认后才提交。同一状态驱动列表、偏好和导出。
- 数据边界：API key 仅当前页面内存；浏览器保存不含凭据。导出始终排除来源路径、索引、历史、权限、开机启动项和密钥；内容分类包含用户文本和链接，需主动勾选。持久化被禁时改为 Session only，编辑继续可用。
- 真实性：界面提供本地配置交互；开机启动、系统热键、窗口控制、截图、翻译、日历、联网 AI、更新与同步均未接通真实服务。AI 检查只检查必填配置，不宣称联网测试通过；更新改为两处入口共用的样例流程。此页没有账户、定价、支付或扩展市场。
- 自检矩阵补充在 `prototype/audit.html`；记录可达状态与恢复路径，未执行浏览器预览、渲染或自动验收，未代用户标记设计冻结。

## 2026-11-24 · 偏好设置 v2 后端契约（切片 1，#241）

- 一份 schema 驱动一切：`app/core/settings.rkt` 每个键带 `(page section control options)` 元数据；`settings-ui` RPC 把页面/分组/行/控件/选项作为数据下发，三宿主的数据驱动设置窗（切片 2）只渲染、不自建功能表。行序由 `page-row-order` 声明，测试钉住。
- i18n 单源：每个有行的键必须同时有 `settings.<key>`、`settings.hint.<key>`、所在 `settings.section.<x>` 与 `settings.nav.<page>`，每个选项有 `settings.opt.<key>.<option>`（空选项值命名为 `off`）。`tests/settings-v2-test.rkt` 对 en/zh 逐行断言，缺键在 CI 失败而不是在宿主上显示原始键。
- 只加有消费者的键：本切片落地的偏好都已在引擎/剪贴板/后端生效（无结果回退、文件结果开关、剪贴板保留期/类型/默认动作/遮罩、计算器开关与复制格式、表情动作、系统与窗口命令开关、启动历史、自动检查更新）。没有消费者的原型偏好留到切片 3/4，不做死开关。
- 对原型的四处删减（已同步 `prototype/settings.js`）：剪贴板默认动作只留 Paste / Copy；敏感内容只保留「Mask sensitive content」一个开关（移除密码管理器内容、私密会话暂停）；不做「Share usage diagnostics」；不做「Update channel」。
- 命名列表：应用文件夹、隐藏应用、剪贴板排除应用、脚本文件夹、文件忽略规则统一存 `lists.json`（`app/core/lists.rkt`），经 `named-list-items/set/remove` 读写；校验失败以稳定码前缀报错（duplicate-name / path-required / pattern-required / unknown-kind / limit），宿主显示自己的文案。应用文件夹即时进入发现根目录，隐藏应用按名称即时从结果中过滤。
- 导入/导出 JSON v2（`app/core/settings-transfer.rkt`）：`{"format":"fulcrum-settings","version":2,"groups":{appearance,features,commands,content}}`。导出只含 toggle/select（不出自由文本与设备绑定），永不含热键、AI、同步、路径、索引、历史。导入先校验（格式、1 MB 上限、字段类型、命令 ≤200、内容 ≤500、热键/别名冲突含召唤热键），`settings-import` 以 `apply=off` 返回 `plan:<分类>:<数量>`，`apply=on` 整类替换并返回 `applied:<摘要>`。线上不传布尔：开关参数用 `on`/`off`。
- 引擎读偏好经 thunk、每次搜索读取，设置窗改动下一次击键即生效，无需重建引擎；未设置的键读作 `unset`（不是 `#f`），provider 以自己的默认值兜底。

## 2026-11-25 · 偏好设置 v2 宿主契约（切片 2，#241）

- 三宿主数据驱动：设置窗不再各自手写表单。`settings-ui` 以 `("page" <page> <group>)` 行开头给出侧栏（分组 Workspace / Features / Preferences 与页面顺序，均来自后端 `settings-page-groups`），随后逐页 `("section" …)` / `("setting" …)`。宿主只做三件事：按 page 行建分组导航、按 section 行分卡、按 control（toggle / select / text / number / hotkey）选控件；标签、提示、选项名走 `settings.group.*` / `settings.nav.*` / `settings.page.*` / `settings.section.*` / `settings.<key>` / `settings.hint.<key>` / `settings.opt.<key>.<option>`。`number` 行的 options 是闭区间 `(min max)`，宿主微调器的上下限与校验器一致。新增页面或行只改后端 schema 与 i18n。
- 设置搜索：侧栏顶部搜索框，按标签+提示在本机过滤全部行（不发 RPC），结果列出「行标签 · 页面 / 分节」；选中即切到该页、滚动到该行并高亮约 1.8 s；Esc 清空并回到搜索框；Ctrl/Cmd+F 聚焦搜索。空结果给出可试的关键词。
- 宿主自有行：开机启动（设备本地，不进 schema，插在 General 的 hotkey 行之前，AutomationId 保持 `FulcrumStartAtLogin`）；AI 密钥行（PasswordBox + Save，接 `ai.key`，位于 AI 页 schema 行之后，`ai-status` 显示就绪状态）；Sync 页的「Check for Updates Now」继续打开专用更新窗。
- 列表页 extras（schema 之外、宿主原生实现，数据仍走 RPC）：Applications = `named-list-items/set/remove` 的 `app-folders`（名称+绝对路径）与 `apps`（开关=是否可搜索）；Extensions = `plugins-list` + `plugin-set-enabled`；Snippets / Quicklinks = `snippet-*` / `quicklink-*` 增删改（关键词唯一、URL/绝对路径校验）；Clipboard 页底部「Clear history」→ `clipboard-clear`（需确认）；Privacy 页底部「Clear launcher history」→ `history-clear`（需确认）；Sync 页「Export / Import configuration」→ `settings-export` / `settings-import`（宿主文件选择器；导入先 `apply=off` 展示摘要，确认后 `apply=on`）。Commands 与 Workflows 沿用既有原生编辑器，不重做。
- 删除项的确认走宿主原生对话框（ContentDialog / alert / GtkAlertDialog），错误贴近触发控件，不再用窗口底部一条全局红字。
- 本切片明确不做（留后续切片）：File Search 来源管理（切片 3）、Notes/Focus/Translate/Calendar/Screenshots 五页（切片 5）、OS 权限行与「Excluded applications」「Interface size」「Show in menu bar」（切片 4，随各自偏好落地）。

## 文件搜索 · 来源联动

- 主入口使用同名报价单作为样例。每条结果显示来源、目录和离线状态，文件详情显示完整路径和版本。回车打开可用文件的样例预览，⌘K / Ctrl+K 打开该文件的操作与详情。离线条目的回车操作进入详情和恢复路径。
- 设置与搜索共用来源与样例索引状态。只有已启用、有索引且满足递归范围与排除规则的条目能出现；网络与外接来源还受相应总开关控制。离线结果是否出现由保留策略决定。排除规则匹配目录路径、来源相对路径或文件名。
- 从结果进入设置时定位对应来源。模拟重连后用设置页返回键恢复原查询和仍可见的选中结果。如果选中项已隐藏，选择首个可见结果。
- 搜索、授权、连接和扫描使用原型数据。通过自检页的「文件搜索 · 来源与恢复」可进入同名文件、离线恢复、添加和排除的操作场景。

## 桌面驻留与开发交接

- Windows 通知区与 macOS 菜单栏共享「打开 Fulcrum、设置、检查更新、退出」这组命令。最小化与关闭启动器都会隐藏窗口并保留当前查询、选中项和设置。再次打开时恢复搜索上下文。只有「退出 Fulcrum」结束驻留，随后重新启动以空查询进入。
- 软件更新从状态入口或设置页进入，共用已安装版本与当前状态。手动检查反馈正在检查、已是最新版、发现新版本或连接失败。用户确认后开始安装并重启，重启恢复此前的窗口状态。连接失败可以重试；安装期间不重复提交。版本和更新内容均为演示数据。
- 启动器的操作围绕当前结果。文件操作查看来源、完整路径和版本，可继续打开样例；预览退回操作，操作退出时回到对应行。应用、剪贴板、快捷链接等结果从同一快捷键进入各自的样例操作。Tab 沿焦点顺序访问控件，方向键在结果列表移动，Esc 逐级关闭、清空查询、隐藏窗口。输入法组合期间由系统处理组合按键。
- 主题由启动器和设置页共用，最大结果行数控制列表的可见高度，其余结果可以滚动。内容搜索开启后可匹配已连接来源的样例正文；离线条目仍按缓存策略展示路径和来源。高亮显示在实际命中的标题、来源、路径或正文摘要上。
- [自检页](prototype/audit.html)提供可操作的桌面驻留、软件更新、键盘及搜索设置场景。独立的操作、AI 和工作流页面保留为状态设计对照。原生验收覆盖通知区与菜单栏的焦点恢复、后台进程生命周期、全局热键、IME、实际索引与真实更新的下载、安装、失败恢复和重启。Linux 按桌面环境提供的状态入口映射同一组命令，需在宿主中单独验收。
