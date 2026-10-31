# EdgeDoctor-RT 当前实现状态

更新时间：2026-09-09（2026-09-15 追加电脑版控制台）

## 电脑版数字孪生控制台（2026-09-15）

规格与验收：`docs/specs/desktop-dashboard-v1/`；验收报告：`docs/desktop-dashboard-acceptance-2026-09-15.md`。

已实现：

- `ui/desktop/`：Vite + React + TypeScript + Three.js，构建产物写入 `server/web-v2/`，
  与旧版 `/dashboard/` 并存于同一 FastAPI 进程，入口切换另行评审。
- 四条路由：工厂总览、实时监测、事件中心、信号分析，覆盖加载中、无数据、服务不可用、
  认证拒绝、数据过期、已恢复未确认、活动报警、命令待回执与命令拒绝九类状态。
- 六区三维总览：模型加载进度、失败降级、点击选择、分区取景、键盘操作、页面隐藏暂停渲染，
  卸载时释放几何/材质/贴图/上下文（验收 20 次进出无增长）。
- 服务端新增三个只读接口：`/api/v1/factory/summary`、`/api/v1/assets/{id}/binding`、
  `/api/v1/events/{id}/analysis`；在线状态由服务端按 30 秒冻结阈值计算，
  只有 `verified` 绑定进入在线统计。
- 冻结绑定 `ui/desktop/src/config/asset-bindings.json` 由 `npm run validate:bindings`
  在构建期校验（schema、唯一性、模型节点存在性、模型 SHA-256），并生成服务端副本。

验收结果：`PASS 28 / FAIL 0 / MISSING_EVIDENCE 1 / OUT_OF_SCOPE 1`。
`MISSING_EVIDENCE` 为 DD-3D-06（无头软件渲染不构成 GPU 验收）；
`OUT_OF_SCOPE` 为 EDV2 频谱分析分支（服务端解析器未实现，界面按“不可用”呈现）。
LAN 走查需要真实设备与现场网络，未执行。

尚未包含：多租户与用户体系、移动端原生应用、持续原始波形上传、工艺控制与自动停机、
预测性剩余寿命、故障部件级定位；频谱、包络与主峰在服务端提供已校验 EDV2 解析前一律显示不可用。


## 已实现

- 板载 LSM6DS3 三轴 IMU：i2c0、208 Hz、±4 g、质量与丢样统计。
- 健康基线：60 个有效窗口、Flash 持久化、启动自动加载。
- 本地报警：稳定触发、恢复、事件 revision、未知异常和信号质量状态。
- MQTT：事件与 telemetry topic、QoS1/PUBACK、断线重连、固定大小 outbox。
- 局域网服务：SQLite WAL、事件去重/逆序保护、趋势聚合、维护前后对比。
- 留证接口：EDV2 上传、CRC 校验、元数据查询和下载。
- 通知：ntfy 适配、配置状态、测试发送接口和网页测试按钮。
- UI：板端六页 HMI、响应式网页看板、通知状态和真实趋势显示。
- 语音：只读状态/最近报警查询和基于真实快照的建议文本。

## 已验证证据

```text
python -m pytest EdgeDoctor-RT/tests -q       43 passed
scripts/test-common-c.ps1                     PASS
scripts/build-firmware.ps1                    scons: done building targets
scripts/flash-bsp.ps1                         Verified OK
board-stability-180s.json                     PASS_STABILITY
```

180 秒实机记录包含 61 次 IMU 采样、194 条 MQTT 消息，队列峰值 4，拒绝 0；之后又完成了最新固件的断开/重连和短时烟测。

## 现场验收入口

```powershell
python EdgeDoctor-RT/scripts/test-board-v2.py --port COM21 --seconds 28800 `
  --output EdgeDoctor-RT/benchmark/results/v2/board-stability-8h.json
```

配置手机通知后，在网页看板点击“测试通知”，或调用 `POST /api/v1/notifications/test`。返回 `CHANNEL_ACCEPTED` 只代表 ntfy 接收，不代表手机已经显示。

## 尚需外部证据

手机锁屏/后台推送、完整 8 小时运行、真实文件系统断电恢复、现场五分类准确率，以及带真实机械故障的原始片段留证，必须在对应部署或仪器条件下完成，当前不虚构为通过。
