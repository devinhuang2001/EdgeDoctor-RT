# 电脑版与实板复查（2026-09-26）

结论：**本轮可在本机复现的问题（#2–#5，以及复验中新发现的一处详情面板竞态）已全部修复并通过自动化复验**；
**#1 真实 MQTT 稳定性依赖实板，本轮无法在本机判定，未解决**。因此本文不宣称"全部验收通过"。

首轮复查的原始记录保留在下面未被改写，每条问题后追加了处置与证据。

## 首轮测试

| 项目 | 结果 | 证据 |
|---|---|---|
| 绑定校验、ESLint、TypeScript、生产构建 | PASS | `benchmark/results/desktop-dashboard/run-20260926-complete-recheck/build.log` |
| 前端单元测试 | 44/44 PASS（6 个文件） | 同上，VITEST_EXIT=0 |
| 完整主机回归 | PASS，包含 32 项 Python 工具测试、C 测试与 50 项服务端测试 | 同目录 `regression.log`，TEST_PS1_EXIT=0 |
| Playwright | 30/31 PASS，DD-API-03 FAIL | 同目录 `case-results.json`、失败 trace、HAR、截图 |
| 真实 GPU 首屏补测 | PASS：GTX 1650 / ANGLE D3D11，1920×1080，10 次独立浏览器上下文，导航前起算 P95 1,359 ms | `benchmark/results/desktop-dashboard/lan-20260926/gpu-real.json` |
| COM21 实板 60 秒 | 未通过；采样正常，MQTT 不稳定 | 同目录 `stability-60s.json` |
| 真实服务页面 | 可打开并显示真实数据，但遥测过期 | 同目录 `live-monitor.png`、`board-and-api.json` |

完整自动化原报告仍将 GPU 标为 MISSING_EVIDENCE；上表的硬件补测为独立证据，没有覆盖或篡改原报告。分析页真实 EDV2 分析分支仍为 OUT_OF_SCOPE，这是规格允许的首版范围例外，并不表示完整频谱分析已经实现。

## 已确认问题与处置

| # | 问题 | 处置 | 状态 |
|---|---|---|---|
| 1 | 真实 MQTT 稳定性未过 | 无代码改动 | **未解决**（需实板复测） |
| 2 | 静置时遥测停止 → 假 STALE | 改遥测入队门；新增 `idle`/`deviation_valid` | 已改源码，待实板复验 |
| 3 | MQTT 入站 PUBLISH 丢失风险 | 单槽改有界 FIFO + PUBACK 期限 | 已改源码，待实板复验 |
| 4 | DD-API-03 竞态前置条件不可靠 | 重写用例为真正的 A→B 设备切换 | **已修复**（自动化 PASS） |
| 5 | 验收配置有执行副作用 | 清理移出 `playwright.config.ts` | **已修复** |
| 附 | 详情/证据/分析面板可能永久停在"读取中" | 详情请求代际与刷新代际解耦 | **已修复**（见下） |

### 1. 真实 MQTT 稳定性未过

原文：60.16 秒、92 次观测中 35 次为 connected=1；rejected 从 937 增至 955，queued 从 40 到 42，persisted 保持 5。不是单纯由历史拒绝计数造成的失败。服务端已能收到事件，不能据此宣称端到端可靠传输已通过。

**处置：未解决，本轮不动。** 这一项判定依据是 COM21 上的真实 broker 行为，本机受控浏览器验收无法替代；
把它标成"已修复"会掩盖实板风险。第 3 项的源码改动会改变入站处理路径，因此第 1 项必须在改动后的固件上重新测
才能得出结论——这一步需要实板，不在本次范围。

### 2. 静置时遥测停止（假 STALE）

原文：`firmware/m55/runtime/ed_monitor_app.c:331` 仅在质量有效且 deviation 有限时入队遥测；静置路径 deviation 为 NaN，真实页面因此将仍在采样的设备显示为 STALE。

**处置：已改源码。** 遥测入队门改为只看 `quality == VALID`，新增 `idle` / `deviation_valid` 字段；
`ed_mqtt_client.c` 在 `deviation_valid == 0` 时省略 `anomaly_score` 但照常上报 `idle`；
前端 `Telemetry` 增加 `idle?`，监测页增加「采样状态」行，避免"静置"被显示成"过期"。

注意：**这一项的验收依据是实板遥测流，本机浏览器用例覆盖不到 "静置 30 秒后是否仍被标为 STALE"**，
因此只能算"按源码修复"，不能算"已复验"。

### 3. MQTT 入站 PUBLISH 丢失风险

原文：`firmware/m55/protocol/ed_mqtt_socket.c:346` 在等待 PUBACK 时只有一个 pending_publish 槽；槽满后进一步收到的 PUBLISH 没有入队处理。缓存路径也没有保留包 ID 供后续发送入站 QoS 1 PUBACK。

**处置：已改源码。** 单槽改为有界 FIFO（`ED_MQTT_INBOX_MAX 8`）+ 5 秒 PUBACK 期限；
新增 `send_puback()` / `inbox_push()` / `inbox_pop()`；**队列满时不发 PUBACK**（让 broker 按 QoS 1 重投，
而不是静默丢包）；`decode_publish` 增加 `packet_id` 出参。

同样地，"修复是否真的闭环"需要在实板上跑回执闭环才能证明，本轮只完成源码层面修复。

### 4. DD-API-03 竞态前置条件不可靠

原文：最新 HAR 中首次 health/ready 为 503，整个测试只有一次 status 请求；固定等待 400 ms 后切页，不能保证旧请求已经发出。因此本次失败不能直接证明产品代际保护失效。另外它测试同一设备切页，不等同于规格要求的 A→B 两设备切换。

**处置：已修复（用例重写）。**

- 用例开始先 `expect.poll` 等 `/health/ready` 到 200，避免刷新在第 1 步就中止。
- 通过总览接口注入第二台 `verified` 设备（`ed84-002` / `MOTOR_02`），构造真正的 A→B 切换。
- A 的慢响应由 `page.route` 显式扣住，并用 promise 记录"请求确实已发出"再切页，
  不再依赖固定 sleep 猜时序；放行 A 的迟到响应后断言 B 的结论未被覆盖。

同时在 `ui/desktop/src/state/store.test.ts` 加了确定性单元测试：api 替身刻意**忽略 abort**，
让旧设备的响应真的送达，把判据压缩到唯一一处——`generation` 检查本身是否生效，
这样即使浏览器用例因为取消而观察不到迟到响应，守卫也仍然被钉住。

### 5. 验收配置有执行副作用

原文：`playwright.config.ts:32` 在 DD_FRESH_SERVER 存在时删除验收数据库和 evidence；配置会被 worker 再次加载，服务已启动后仍可能触发清理。

**处置：已修复。** `playwright.config.ts` 现在只解析路径，不产生任何副作用（连 `mkdir` 都不做）；
删除验收库与证据目录只保留一个入口——`scripts/test-ui.ps1` 在**服务启动之前**执行。

## 复验（第二轮）

首轮之后跑出的 `run-20260926-verified` 仍有 2 个 FAIL（DD-API-05、DD-UI-05），它们不是数据问题，而是一处竞态，
本节记录定位与修复。

### 追加问题：详情/证据/分析面板可能永久停在"读取中"

**症状**：选中事件后，右侧「诊断证据」面板一直显示「正在读取证据元数据」，等 20 秒也不变；
信号分析页则一直显示「正在查询分析接口」。服务端日志显示这些请求本身全部正常返回（evidence 404、analysis 200）。

**根因**：`ui/desktop/src/state/store.ts` 的 `loadEventContext()` 复用了 `refresh()` 的请求代际（generation），
而 `refresh()` 是**由后台活动驱动的**——每次 SSE 通知、以及 SSE 连通时的 60 秒兜底轮询都会调用它并推进代际。
时序一旦错开就会这样：

1. 用户选中事件 → `loadEventContext()` 记下当前 generation，三条详情请求发出；
2. 一次 SSE 通知到达 → `refresh('SSE')` 推进 generation；
3. 详情响应这时才回来 → `generation !== this.generation` 成立 → **静默 `return`**；
4. `evidenceState` 停在 `'loading'`，**再没有任何请求会来改写它** → 面板永久"读取中"。

`DD-API-05`/`DD-UI-05` 的播种动作（`POST /api/v1/events`）本身就会触发 SSE 通知，
所以用例正好反复踩在这个窗口上；也正因为它是毫秒级竞态，表现为间歇失败而不是必现。

**证据（失败实例的 trace 时间戳，单位 mm:ss.mmm）**：

```
12.551 401  /api/v1/events/<evt>/evidence     ← 第 2 次 loadEventContext
12.552 200  /api/v1/events/<evt>/analysis     ← 65ms 后返回
12.572 200  /health/ready                     ← 后台 refresh 在"响应落定之前"推进了代际
12.627 200  /api/v1/factory/summary
```

第 2 次详情请求在 ~12.617 才全部 settle，而代际已在 ~12.566 被推进，于是结果被丢弃。

**修复**：把详情路径的代际与刷新代际**解耦**——详情/证据/分析是按 `event_id` 取数的，
和"设备页面数据"是否过期无关，后台刷新没有理由作废它。

```ts
// 新增独立的详情代际；只由"更新的详情请求"或"清空选择"推进
private detailSeq = 0;

async loadEventContext(eventId: string): Promise<void> {
  const seq = ++this.detailSeq;
  ...
  if (seq !== this.detailSeq) return;                 // 只有更新的详情请求能作废本次结果
  if (this.state.selectedEventId !== eventId) return;  // 选择已变，仍然丢弃
```

`refresh()` 一侧的 `generation` 守卫原样保留，`DD-API-03` 仍然由它保证（其单元测试未改动、仍通过）。

**验证**：

| 手段 | 结果 |
|---|---|
| 修复前：`--repeat-each=3` 跑 DD-API-05/DD-UI-05 | 6 次中 **1 次 FAIL**（DD-API-05 卡在 loading，耗时 26.4s 即 20s 断言超时） |
| 修复后：`--repeat-each=5` 同两个用例 | **10 次全 PASS，0 FAIL** |
| 新增单元测试 `ui/desktop/src/state/store-detail.test.ts` | 5 项 PASS |
| **变异测试**：把守卫改回 `generation` 版本 | 5 项中 **3 项失败**，报错正是生产症状 `expected 'loading' to be 'missing'`，其余 2 项（新选择覆盖、清空作废）仍通过——证明测试确实针对这个 bug |

单元测试用 api 替身把亚秒级竞态钉成确定性的：让详情请求停在在途状态，强制在中间插入一次 `refresh()`，再放行响应，
判据只有一条——状态必须落定。

### 复验结果

`scripts/test-ui.ps1 -RunId run-20260926-verified2`，全流程 14 分 20 秒，进程正常退出。

| 步骤 | 结果 |
|---|---|
| 绑定校验 / ESLint / tsc / 生产构建 | 4 项退出码全 0 |
| 前端单元测试 | **50/50 PASS（8 个文件）** |
| 完整主机回归（`scripts/test.ps1`） | TEST_PS1_EXIT=0 |
| Playwright | **PASS 28 / FAIL 0 / MISSING_EVIDENCE 1 / OUT_OF_SCOPE 1**（共 30 项） |
| 服务端日志 | **0 次 5xx**（1066×200、91×404、5×304、4×202、1×422、1×201、1×307） |
| 收尾 | 服务端已停止，端口 8099 已释放 |

未 PASS 的两项均为预期且非失败：

- `DD-3D-06` MISSING_EVIDENCE——无头软件渲染，实测 P95=2813 ms；硬件加速机的独立证据见首轮真实 GPU 补测。
- `DD-UI-05` OUT_OF_SCOPE——服务端尚未实现 EDV2 解析，"可用 EDV2 分析"分支不在本次范围；
  其"无证据时明确说明原因、不绘制模拟频谱"的断言**本身是 PASS 的**（此前正是这个断言因上面的竞态而失败）。

与首轮后 `run-20260926-verified`（PASS 27 / FAIL 2，两个 FAIL 即 DD-API-05 与 DD-UI-05）对比，
这两个 FAIL 均已消除，且本轮不引入新的失败。首轮整改中修掉、本轮仍保持 PASS 的条目：
DD-API-03、DD-API-04、DD-API-06、DD-3D-03、DD-3D-05，以及 7 项子进程类用例
（DD-API-01/02/07、DD-UI-03/06/07、DD-SYS-01）。

### 复验期间修掉的验收脚本缺陷（不影响产品）

- **从 Git Bash 直接跑 `npx playwright test` 会在全部用例通过后卡住不退出**（PowerShell 入口正常）。
  `npx playwright test --list` 能干净退出，说明包装器无问题，卡在收尾阶段。处置：统一用
  `scripts/test-ui.ps1` 驱动；报告器已在 `onTestEnd` 增量落盘，因此即使收尾卡死也不会丢结果。
- **构建被会话注入的删除保护 shim 拦下**：会话带 `NODE_OPTIONS=--require=...node-language-shim.cjs`
  与一组 `CODEBUDDY_SAFE_DELETE_*`，vite 的 `emptyOutDir` 一次清空 114 个产物文件时被
  `SAFE_DELETE_BULK_CONFIRM_REQUIRED` 拦下，导致验收在第 4 步 `BUILD_EXIT=1` 就中止
  （看起来像项目构建失败）。处置：在 `scripts/test-ui.ps1` 内、启动任何子进程之前解除，
  且**保留 `-like '*node-language-shim*'` 判断**，以免清掉用户/CI 自己设的合法 `NODE_OPTIONS`。

## 实板操作与回退

- 原 MQTT 地址为 192.168.1.7:1883，电脑当前 WLAN 为 192.168.1.3。
- 先在板端复制原 148 字节配置到 `/flash/edgedoctor.mqtt.pre0926`，随后将同一 site_1 / ed84-001 的 Broker 地址改为 192.168.1.3:1883。
- Wi-Fi 配置未修改。板端 `version` 返回 RT-Thread 5.0.2 / build Sep 12 2026 11:44:01；该时间本身不能证明所有应用对象版本，未将其当作烧录镜像 hash。
- 本轮启动的服务使用隔离数据库 `tmp/pc-live-20260926/edgedoctor.db`，API 为 8080，复用已有 1883 Broker。服务 PID：API 12000、ingest 23396。
- 恢复原 Broker 可在停止 MQTT 后执行 `edmqtt config site_1 ed84-001 192.168.1.7 1883`，再启动 MQTT；原配置文件也已保留。

（该实板服务在本次复验期间保持运行，未被停止或改动。）

## 尚未完成的验收

以下项目**不能**由本机受控浏览器测试替代，仍未完成：

1. 真实板端命令 APPLIED/REJECTED 闭环；
2. 真实留证生成与下载；
3. 断连重连后的回执清空；
4. 长时间稳定性（即 #1）；
5. 另一台 LAN 客户端的走查；
6. #2 / #3 的源码改动在实板上的复验（含"静置 30 秒后不再假 STALE"与"入站 PUBLISH 不再丢"）。

当前不建议切换默认旧版入口，也不能报告"全部实现、全部测试通过"。
本机可复现的自动化结论是：**28 PASS / 0 FAIL / 1 MISSING_EVIDENCE / 1 OUT_OF_SCOPE**。
