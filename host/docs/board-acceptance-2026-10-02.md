# 2026-10-02 烧录与实板验收

结论：已完成授权烧录和本轮可运行的回归、实板闭环测试，但不能宣称全部通过。启动期间的 MQTT 入站溢出和遥测队列丢弃仍阻断稳定性验收。本轮仅检查、烧录现有修复固件，未修改业务代码。

## 固件与环境

- 源码 HEAD：`81e75fe374ed2e13736e1b3ab804a13c9747c24a`。
- `scripts/build-firmware.ps1 -Jobs 4` 成功；冻结文件为 `benchmark/results/desktop-dashboard/lan-20261002/firmware/rtthread-fixed.hex`。
- HEX SHA256：`21bad72407808551894845f0500fea915122fd3113e58cac02296383befa838b`。
- OpenOCD 写入成功，校验 6,637,328 字节，日志明确 `Verified OK`，随后复位启动。
- COM21 / 115200；Wi-Fi 切换并保存为用户授权的 2.4GHz `206`，板端 IP `192.168.1.4`，PC `192.168.1.3`。
- MQTT 测试 broker 端口 `18883`，隔离 API 端口 `8080`，数据库 `tmp/pc-live-20261002/edgedoctor.db`。PC 的 1883 无法绑定，未强行修改系统网络设置。

## 通过项目

- 主机回归：32 项 Python 数据测试、C 公共契约/运行时/日志恢复/MQTT 重试等测试、50 项服务端测试通过，见 `lan-20261002/host-regression.log`。
- 桌面端：lint、类型检查、生产构建、50 项前端单测及 31 项浏览器测试通过。逻辑登记为 28 PASS、1 MISSING_EVIDENCE、1 OUT_OF_SCOPE，见 `run-20261002-live-validation/case-results.json`。
- IMU 持续采样约 208Hz；本轮多个快照均无 drops、gaps、bus_errors，见 `after-flash.json`、`final-board.json`、`reconnect-evidence.json`。
- 遥测最终恢复：`final-api.json` 记录 755 条遥测，其中 448 条静置遥测；静置消息包含 `idle:true`、`alarm:false`，未发送无效 `anomaly_score`。绑定电机为 ONLINE，其余五个未绑定/未验证设备为 UNKNOWN。
- 实际事件 `ed84-001-66-1` 的确认命令与留证命令均进入 APPLIED，未注入人工事件；见 `command-evidence-live.json`。
- 实板证据文件 12,649 字节、原始负载 12,480 字节，下载后通过 EDV2 封装 CRC 校验，challenge 与本次请求一致；元数据为 208Hz、前后各 5 秒、complete:true。文件 `evidence-live.edv2` 的 SHA256 为 `6592f89370c5e1981c6a934328eb5b1787291534dbf6752a466dea6f04c210be`。
- MQTT stop/start 后 30 秒内六个快照均 connected=1，queued=0..2，inbox_dropped=74 和 rejected=464 未继续增加。这仅证明低积压下的短时恢复，不能替代启动突发和长时稳定性验收。

## 未通过和未完成

1. **MQTT 启动突发稳定性失败。** 烧录后初始快照 connected=0、queued=41、inbox_dropped=23；连续 45 秒收到 88 条 MQTT 消息、telemetry=0。随后恢复，但累计 inbox_dropped 增至 74、rejected 增至 464。源码 `mqtt_worker` 每轮只 poll 一条入站消息，却 flush 最多八条出站消息，再休眠一秒；同步发布等待 PUBACK 时收到的 event_receipts 可填满八槽入站 FIFO，导致发布失败和重连。需要另行修复该调度并复测突发积压，不能用最后的 ONLINE 状态抹去前面的失败。
2. **本轮 GPU 性能证据缺失。** 浏览器用例运行成功不等于真实 GPU 指标通过；DD-3D-06 保持 MISSING_EVIDENCE。9 月 26 日有单独硬件 GPU 记录，日期和证据应分开表述。
3. **真实信号分析尚未实现。** 本轮留证下载和 EDV2 封装 CRC 已通过，但分析接口仍返回 available:false，真实波形/频谱解析分支未实现。按冻结的首版范围 DD-UI-05 为 OUT_OF_SCOPE，不能写成该功能已完成。
4. 事件页仍把 baseline deviation / anomaly score 的 `peak_deviation` 标为 g，单位需纠正。
5. 8 小时稳定性及另一台 LAN 客户端验收未运行。`df /flash` 报零空闲块，后续需要进一步评估日志增长与存储余量，不能宣称长时运行安全。

## 备份与操作说明

- 原 Wi-Fi/MQTT 配置已在板端保存备份。
- 旧 synthetic fixture 已用 YMODEM/CRC 转移到 `board-backup/synthetic_smoke_2s.edgp`，SHA256 与 `board-backup/manifest.json` 一致；为释放配置保存空间，随后删除板上这一已备份测试文件。未删除事件日志。
- **旧固件 dump 为全零，不是有效回退备份。** `board-backup/firmware-before.status.json` 明确标记 INVALID_FOR_ROLLBACK。读出检查失败后仍启动烧录是本次操作失误，已向用户说明；不得用该文件回滚。当前已成功写入并校验的冻结 HEX 可用于重烧当前版本。
- 测试 API、broker、ingest 保持运行以便继续板端检查。未修改源码或关闭其他应用。

全部实板证据位于 `benchmark/results/desktop-dashboard/lan-20261002/`。`final-board.json` 的早期 API 查询误用了缺少 `/v1` 的路径，返回 404；API 最终状态以随后保存的正确路径证据 `final-api.json` 为准。
