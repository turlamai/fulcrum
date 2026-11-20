# AGENTS.md

## 家族设计（必读入口）

- 设计/UI 任务先加载 `tlai-product`，必读 `references/policy.md` 与 `references/design-standard.md`（理念、决策规则、UI 验收）。技能缺失时读取 ZCode 真源：macOS/Linux `~/Sync/ZCode/skills/tlai-product/`；Windows `D:/ZCode/skills/tlai-product/`。无法读取须报告规范未核验，不凭记忆跳过。
- 家族规范管共性；本仓现有设计、行为与 token 真源管具体值和产品差异。例外记回原契约，不复制家族规则或新增数值表；已有界面按下一次相关切片渐进对齐。
- 改界面先改原型、落账后实现；按家族 UI 清单与本仓当前技术线的既有门禁验收，原型对照和真实交互证据都要保留；对外宣传使用验收后的实物截图。

## 实现路由（agent 按任务读真源）

- 实现界面以 `design/prototype/` 为准：对照 HTML/CSS/JS 与实物截图验收，不依赖散文复述。
- Token/几何值真源 = `design/prototype/shared.css`（亮暗成对；无 shared.css 的仓 = design/prototype/ 各页样式）；`design/DESIGN.md` 只记语义名、用途与决策。
- 行为边界与跨平台一致性以 `design/DESIGN.md` 的不变式与平台差异节为准。
- 改界面先改原型再改实现（tlai-product）；动共享层（design/、shared/）先 issue 声明，同 commit 落契约。

## Current owner decision — 2026-11-18 quality-first

This is a **non-commercial development product**. The previous 2026-10 authorization, pricing, trial, Pro and Permit/Commerce rollout decisions are canceled. Do not implement or restore checkout, activation, subscription, paid feature gates, trial expiry or update-entitlement windows. Keep source licenses, release-signature verification, privacy protections and tested data migration intact. Focus only on real product functionality, native platform parity, recovery, clear bilingual READMEs and honest website status. Read the [current family policy](https://github.com/turlamai/turlamai.github.io/blob/master/PORTFOLIO.md) before every development slice.

Fulcrum 是三平台键盘启动器：一份 Racket 领域后端经 Rivet RVT1 驱动
macOS SwiftUI、Windows WinUI 3 和 Linux GTK4 原生宿主。

## 结构

- `app/` — 后端入口、搜索引擎、存储、插件和更新逻辑
- `tests/` — Racket 单元、集成与 RVT1 wire tests
- `macos-host/`、`windows/`、`linux/` — 三个平台的第一方原生宿主
- `shared/i18n/` — 跨宿主文案单源；`gallery/` — 第一方插件清单
- `scripts/` — 生成、发布和产品验收脚本

## 完成前必须运行

```bash
raco test tests/
raco test tests/backend-wire-test.rkt
raco rivet schema check rivet-schema.json --json
raco rivet acceptance --json
```

`raco rivet acceptance --json` 是当前平台的产品完成门禁：它必须依次通过
后端契约、真实 RVT1、原生宿主构建、可分发包生成，以及对打包后应用的
原生可访问性操作。GUI 工作不能只以代码审查、单元测试、构建成功或截图
宣称完成。

## 跨平台约束

- 支持矩阵是 macOS、Windows、Linux；改动公共交互时必须同步三宿主，或在
  文档和验收清单中明确记录真实的平台边界。
- 宿主只负责系统集成和原生渲染，搜索、排序、存储、插件、AI 与更新状态
  属于 Racket 后端；不得在某个平台复制一套业务逻辑来绕过 RVT1。
- UI 自动化依赖稳定的 accessibility / automation 标识。改文案或布局时不应
  删除这些标识；新增关键交互时同步扩展 `scripts/e2e/`。
- 测试必须验证可观察结果。默认验收场景通过原生查询框提交 `12*12`，并确认
  后端计算结果 `144` 出现在原生结果列表；wire test 另行证明持久化与事件。
- Rivet 使用不可变提交 pin。升级时同时更新 CI、release、acceptance workflow、
  setup action ref 和生成客户端，并在三个 runner 上跑完产品验收。
- 不提交 `.rivet/`、`dist/`、MSVC/WinUI 生成目录或本机用户数据。

## 发版

- `ci.yml` 负责常规多平台测试与宿主构建；`acceptance.yml` 负责可执行产品验收。
- tag 发版前，CI 与 Product Acceptance 都必须全绿；不得把 `continue-on-error`、
  空测试、跳过 UI 或测试中的强杀进程当作成功证据。
- 发现某平台无法完成同一产品场景时，先修产品或上游 Rivet；临时 workaround
  必须可删除、被测试覆盖，并在 PR 中明确说明边界。

## 组合定位（Portfolio placement）

本产品目前是 **2026-11-18 质量与交付优先的非收费开发项目**。不重新引入旧版价格、购买、试用、Pro 或 Permit/Commerce 门禁；保持现行源码许可与更新签名安全。当前权威为 [产品组合策略](https://github.com/turlamai/turlamai.github.io/blob/master/PORTFOLIO.zh-CN.md)，不是已经删除的产品定价文件。

## 产品交付与下一步（2026-11-18）

先完成真实主流程、错误与恢复、原生平台体验、更新验签及完整中英文文档/产品主页验收。不要实施收费功能，不能用网页或单测代替发行包可用性证据。

## Documentation languages

Public default documentation is English; maintain complete Chinese user paths in `*.zh-CN.md`, with reciprocal links. Update both languages when prices, quotas, installation, recovery, privacy or supported platforms change. API/protocol identifiers and commands remain unchanged; do not maintain a second normative protocol or implementation. Follow the [family language policy](https://github.com/turlamai/turlamai.github.io/blob/master/docs/DOCUMENTATION-LANGUAGES.md). Existing internal notes are not a claim that every document is bilingual.

## Public product documentation

The paired `docs/user-guide.md` / `docs/user-guide.zh-CN.md` own the public installation, first-use, data/recovery, privacy and availability path. Keep the homepage and both README entry points aligned with them when behavior changes. Detailed technical contracts and the current implementation queue retain their existing owners. Do not publish target prices, a purchase link or a ready/secure/platform claim before its release evidence exists.
