# Review 修复记录 — 2026-09-13

本轮修复前次 review 列出的 7 项问题。最新镜像已通过 KitProg3 烧录并回读校验。

## 修复差异

| 问题 | 实现变化 | 回归证据 |
| --- | --- | --- |
| 配置 API Token 后命令回执丢失 | `server/mqtt_ingest.py` 将服务端 Token 传入 `command_result()`，保持 HTTP 认证要求 | `test_mqtt_command_receipt_with_api_token` 验证 PENDING → APPLIED、重复回执及无 Token HTTP 拒绝 |
| 未收到数据库回执的事件不再重传 | MQTT 队列记录发布时间，10 秒后重试；断线时清除已发布标记；仅 persisted 回执删除事件 | `test_mqtt_retry.c` 验证重连、超时重试及回执后的停止重试 |
| 日志坏尾阻断后续回放 | 恢复时截断半条/CRC 错误的尾记录并 fsync；读取或修复失败时禁止继续追加；运行中写失败后再次修复 | `test_journal_recovery.c` 使用真实文件验证短记录、整条损坏和部分写入失败后的回放 |
| 启动计数未持久化仍被使用 | 检查读、定位、写、fsync 和 close；失败返回 0；损坏计数不重置为 1；告警核心拒绝无效启动身份 | boot 故障注入及 `invalid_identity_cannot_issue_event` |
| CONNECT 缺少用户名密码 | 配置支持凭据，CONNECT 编码用户名/密码标志与可变长度；增加 `edmqtt auth`；兼容旧配置文件，配置通过临时文件保存 | `test_mqtt_connect.c` 检查报文字段、长度边界、旧文件迁移与坏输入 |
| 无效窗口没有清空连续告警计数 | 有效、无效及空闲窗口均送入告警核心；断流和 gap 显式重置连续计数；快照保留 idle 状态 | `test_monitor_runtime.c` 执行实际运行时代码，覆盖饱和、STALE、空闲的重置 |
| `edgeimu start` 没有执行传感器恢复 | 启停通过监测线程的固定消息队列执行；停止/未探测时让出 CPU，避免忙循环 | 运行时集成测试覆盖探测失败后重试、停止后重新启动 |

另外，`build-firmware.ps1` 会备份并同步 EdgeDoctor 应用自己的 BSP SConscript 入口，避免使用旧副本触发 `RTT_ROOT` 错误。官方启动代码、链接脚本、HAL、生成配置及 Wi-Fi 资源未修改。

`provision-board-mqtt.ps1` 增加可选 `-MqttUser`、`-MqttPassword`，先停止 MQTT worker 再配置凭据；不输出凭据回显。BSP 的 shell 命令上限仍为 79 字节，过长输入明确拒绝。

## 验证结果

- `scripts/test.ps1`：退出码 0；32 项 Python 测试、29 项服务端测试、全部 C 回归通过。
- C 回归使用 `-Wall -Wextra -Werror`；新增运行时测试替代 RT 调度/I²C 边界，日志测试使用主机真实文件与 I/O 故障注入。
- `scripts/build-firmware.ps1`：各项固件变更后构建成功；最后一次增量验证退出码 0，目标为最新。
- 最后一次链接尺寸：text 6,115,636，data 520,620，bss 3,953,356 字节。
- 原有链接警告：`rt-thread.elf has a LOAD segment with RWX permissions`，未屏蔽或通过修改链接脚本消除。
- PowerShell 语法解析通过；本轮相关已跟踪文件 `git diff --check` 通过。
- 测试日志：`build/review-tests.log`；最终增量构建日志：`build/review-build.log`。
- 固件：`D:/CODEX SPACE/infei/edgi-workspace/Edgi_Talk_M55_EdgeDoctor/Debug/rtthread.hex`。
- HEX SHA-256：`19ba01b92c8f6bdf7a53e06b67bfbe7a5b5338c26d442e7c5b3ed71beece7e10`。

## 验证边界

本轮已完成短时板端验证：COM21（KitProg3）烧录回读成功；`edgeimu info` 报告 `nominal_hz=208`、`measured_millihz=210252`、`drops=0`、`gaps=0`、`bus_errors=0`；`edgeimu stop` 后 `running=0`，随后 `edgeimu start` 后 `running=1`。MQTT 报告 `connected=1`、`rejected=0`。板端日志中有一次 `I2C bus operation not supported`，来源是 BSP 不支持动态 I2C 时钟控制，启动仍成功。

仍未完成真实断电、带认证 broker 的完整板端 ACK 闭环或长时间稳定性测试；主机文件故障注入不能替代板载文件系统的断电验收。最后一次烧录日志见 `benchmark/results/v2/flash-review-20260913-final.log`，短时串口采样见 `benchmark/results/v2/board-smoke-final-20260913.json`。
