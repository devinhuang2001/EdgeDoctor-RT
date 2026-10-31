# EdgeDoctor 改造落地状态（2026-09-09）

审计报告 `edgedoctor-audit-2026-09-09.md` 保留为改造前基线。本文件记录本轮已经写入工作区的实现，以及仍必须在真实硬件上验收的门禁。

## 已落地

- `firmware/m55/sensor/ed_imu_lsm6ds3.c`：常驻采集线程、LSM6DS3TR-C 208Hz/±4g 回读、静态块消息队列、FIFO overrun/GAP 标记、单调配置 revision，以及受临界区保护的 64 位 tick 扩展时间接口；E84 板默认使用 FIFO-bypass 的有界直读路径，避免该板 FIFO 满状态锁死 I²C，FIFO 代码保留为兼容路径。
- `firmware/m55/runtime/ed_monitor_app.c`：208 点/104 点窗口消费、低频特征、质量/基线/报警统一链路、快照互斥复制、基线 schema/key/CRC 校验、416 点重力与动态 RMS 自检；stale/disconnect 会生成 `SENSOR` 事件，恢复采样会提交恢复修订；高频采集与窗口特征解耦，采用固定 104 点 hop，每个窗口都更新诊断快照。
- `firmware/common/{include,src}/ed_imu_profile.*`、`ed_lf_window.*`：寄存器 profile 和窗口纯 C 模块，均有 Host 回归测试。
- `firmware/common/src/ed_lf_features.c`：低频 DFT 的 Hanning/twiddle 表一次初始化并放入外部共享 RAM，避免 trig 函数进入实时窗口热路径。
- `firmware/common/src/ed_alarm.c`：恢复阈值使用 `recovery_deviation`，存储提交失败回滚，ACK 入队失败清理 pending。
- `firmware/m55/protocol/ed_mqtt_client.c/.h`、`ed_mqtt_socket.c`：TLS-only publish 条件、事件先于 telemetry、PUBACK 后保留事件直到服务端 receipt、receipt 订阅、TLS 状态由连接结果设置、分段 SUBACK 读取；本地 durable 标记与网络 published 状态分离，ACK 完成结果排队后由 MQTT worker 发布；未确认的 command-result 每轮只尝试一次，避免阻塞 telemetry；重连采用 1–30 秒指数退避并加入有限抖动。
- `server/mqtt_ingest.py`：事件成功落库后发布 `event_receipts`；新增服务端回执测试。
- `firmware/m55/logger/ed_event_journal.*`：固定长度、CRC、commit marker、fsync、启动扫描和未回执事件重排队；boot counter 使用双槽 CRC；新增非阻塞 commit 通知，监控层只在实际 fsync 后标记 `durable` 并发布 revision。
- ACK 采用两阶段本地持久化：先写入 `ack_pending` revision，commit 后再写入 acknowledged revision；最终 revision fsync 后才发布 MQTT 事件与 `command-result`，避免异步 journal 入队被误当作断电安全边界。
- `firmware/m55/ui/edgedoctor_ui.c` 与 BSP `xiaozhi_ui.*`：UI 命令静态队列、monitor 快照绑定、XiaoZhi 扩展桥接、EdgeDoctor 独立 LVGL screen，EdgeDoctor 模式跳过官方 demo 对象树；`edgedoctor_emoji_compat.h` 保留 XiaoZhi 旧通知 ABI 而不重新编译 3D emoji。
- `scripts/test-board-v2.py`、`tests/python/test_board_acceptance.py`：只启动一次自主采集并验证计数、质量、速率、缺口、MQTT 与运行时长；串口握手等待 `msh />`，加入反例回归。
- `scripts/prepare-bsp.ps1`、`scripts/test-replay-reference.py`：提供普通目录 BSP 复制、自动导入 XiaoZhi UI bridge 集成补丁，以及带 SHA256/帧数门禁的回放验收入口；启动文件继续使用官方副本。
- `docs/BSP-entry-EdgeDoctor.md`：作为生成 BSP 的独立 EdgeDoctor 入口说明；官方 XiaoZhi `README_zh.md` 保持原样，避免覆盖厂商基线文档。

## 已验证

- `EdgeDoctor-RT/scripts/test-common-c.ps1`：common、monitor、LF window、baseline、MQTT、voice guidance 全部通过。
- `EdgeDoctor-RT/scripts/test.ps1`：25 项 Python 数据/验收测试、6 组 C 测试、28 项 server 测试通过；从项目目录和工作区根目录执行结果一致。
- 告警状态机补充了持久化提交失败后的槽位回收与确认时间戳；MQTT 服务端命令发布站点改为由 `EDGEDOCTOR_SITE_ID` 配置，避免多站点部署误发到固定 `site`。
- `EdgeDoctor-RT/scripts/build-firmware.ps1`：退出码 0，生成 `rt-thread.elf` 与 `Debug/rtthread.hex`。仍有链接器已有的 RWX LOAD segment 警告，未屏蔽。
- 当前 EdgeDoctor BSP 已将 `applications/edgedoctor` 从绝对 junction 改为普通目录，并复制 `firmware/common`；在不设置 `EDGEDOCTOR_APP_DIR` 的独立 BSP 路径下实际 SCons 构建成功。
- E84 实板：多次刷写均返回 `** Verified OK **`；串口确认 `WHO_AM_I=0x6a`、FIFO 配置回读、监控自主启动。当前 30 秒板测已达到 `204.944 Hz`，43 个有效快照均为 `quality=1`，`drops=0/gaps=0/bus_errors=0/queue_high=0`；MQTT 因板卡当前处于 AP 模式且未配置 STA 凭据仍为未连接，长稳和联网门禁继续保留为 `MISSING_EVIDENCE`。后续时间轴、hop 和 MQTT 修复已重新生成固件，但必须在 USB 调试设备恢复后重新烧录再做联网复测。

## 真实板端门禁

当前仍缺少以下真实硬件证据：1g 静止绝对幅值标定、FIFO 满/总线断开恢复、Flash 断电 journal、TLS 证书链验证、触摸并发和 8 小时稳定性。E84 当前 USB 调试设备未重新枚举，恢复硬件连接后再执行这些场景；在此之前验收矩阵保持 `MISSING_EVIDENCE`。
