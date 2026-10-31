# 电脑版数字孪生控制台验收报告

运行标识：`accept-20260915`
执行时间：2026-09-15
对应规格：`docs/specs/desktop-dashboard-v1/01-desktop-dashboard-spec.md`（v0.1）
对应验收规格：`docs/specs/desktop-dashboard-v1/02-desktop-dashboard-acceptance-test-spec.md`（v0.1）

证据目录：`benchmark/results/desktop-dashboard/accept-20260915/`
（`manifest.json`、`case-results.json`、`api.log`、`build.log`、`regression.log`、
`per-case/*/browser-console.log`、`network.har`、`screenshots/`、
`metrics.json`、`metrics-3d05.json`、`metrics-3d06.json`、`checksums.txt`）

## 结果总览

| 状态 | 数量 | 说明 |
|---|---|---|
| PASS | 28 | 自动化用例通过，证据可复核 |
| FAIL | 0 | — |
| MISSING_EVIDENCE | 1 | DD-3D-06：无头浏览器为软件渲染，不能作为 GPU 验收证据 |
| OUT_OF_SCOPE | 1 | DD-UI-05 的“可用 EDV2 分析”分支：服务端尚未实现解析器，规格 §7 允许该项超出范围 |
| NOT_RUN | 0 | — |

环境：Node v24.18.0、Playwright 1.63.0、浏览器 channel `msedge`（无头）、
Python 3.12.10、React 19.1.1、three 0.180.0、Vite 7.1.7。
服务使用隔离数据库 `acceptance.db` 与构建期生成的绑定副本；
模型 SHA-256 与绑定版本记录在 `manifest.json`。

## 关键实测数据

| 项 | 实测 | 证据 |
|---|---|---|
| 场景进出 20 次资源 | 遗留 WebGL 上下文 1（当前页面在用），几何释放 14742 次，第 20 次相对第 2 次无增长 | `metrics-3d05.json` |
| 首屏可用画面（无头软件渲染） | 中位 20 ms，P95 499 ms（10 次） | `metrics-3d06.json` |
| 连续操作 60 秒 | Long Task 0 个，最大帧间隔 30 ms，3621 帧，JS 堆 16 MB，174 次交互 | `metrics.json` |
| 在线阈值语义 | 29 s / 30 s 为 ONLINE，30.001 s 为 STALE，未验证绑定恒为 UNKNOWN | `api.log` |
| SSE 连续 20 条通知 | 汇总接口请求 ≤ 4 次（去抖生效）；断流后 26 秒内轮询 2–6 次 | `per-case/DD-API-06-*/sse-burst.json`、`polling-fallback.json` |

## 未取得证据或超出范围的条目

1. **DD-3D-06（GPU 首屏时间）**：本机为无头浏览器软件渲染，实测值只作记录，
   不构成“验收电脑硬件加速”结论。需在开启硬件加速的 Windows 桌面机上重跑。
2. **DD-UI-05 的“可用 EDV2 分析”分支**：服务端 `GET /api/v1/events/{id}/analysis`
   目前对任何事件都返回 `available:false` 与原因，界面按“不可用”呈现并有截图；
   真实频谱/包络/统计特征需解析器落地后另测。
3. **LAN 走查**：需要至少一台真实绑定设备、真实 MQTT 链路与现场网络，
   本次环境不包含板端，未执行，也不在报告中计为通过。

## 已知限制

- 冻结绑定只包含一台真实设备：`motor-01`（`ed84-001`，已验证）。
  其余五项分别为未接入或待核验，因此“在线”统计恒为 0 或 1，属预期。
- 交付的工厂模型不含贴图（模型包说明无外部贴图依赖），
  因此 DD-3D-05 的贴图释放计数为 0，判据改为“存活贴图不增长”。
- 浏览器控制台会把 `/api/v1/` 的数据 404（例如“尚无留证记录”）记为一条错误日志。
  用例把这类被界面显式处理的数据 404 单独登记（`diagnostics.json` 的 `apiNotFound`），
  只有静态资源 404 才算失败；该规则写在 `tests/ui/evidence.ts` 的注释里。
- 每次验收使用新的 `run_id` 目录与全新数据库；录制在同一 `run_id` 下重复执行会叠加数据。

## 回退步骤

1. 恢复默认入口：`server/app.py` 中把根路由 `RedirectResponse("/dashboard/")` 保持不变即可，
   新版始终挂在 `/dashboard-v2/`，切换评审前不影响旧页。
2. 若要整体下线新版：保留 `server/web-v2/`、`server/asset-bindings.json` 与本次证据目录，
   仅移除 `/dashboard-v2` 的静态挂载及其只读接口即可，旧版 `/dashboard/` 不受影响。
3. 若要回退绑定配置：`ui/desktop/src/config/asset-bindings.json` 为唯一权威来源，
   改回后执行 `npm run validate:bindings` 重新生成服务端副本。

## 复现方式

```powershell
cd EdgeDoctor-RT
npm ci                     # 仓库根：浏览器验收依赖
cd ui\desktop; npm ci; cd ..\..
powershell -ExecutionPolicy Bypass -File scripts/test-ui.ps1 -RunId <新标识>
```

脚本会依次执行绑定校验、lint、类型检查、生产构建、前端单元测试、完整主机回归
（`scripts/test.ps1`）与 Playwright 浏览器用例，并在验收目录写入全部证据与校验和。
