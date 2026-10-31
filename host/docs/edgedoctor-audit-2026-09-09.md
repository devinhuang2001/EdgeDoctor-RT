# EdgeDoctor 代码核查报告（2026-09-09）

核查对象：当前工作区代码，包括未提交状态；根仓库 HEAD 为 `7a90f8a6bb6cc60091f9288c645722fe6d4c435d`。本次没有修改固件源代码、厂商 BSP 或现有用户改动，没有烧录和操作串口。结论来自源码、官方器件资料、Host 测试和两次 SCons 增量检查。

版本变化说明：核查期间工作区的MQTT与证据模块被外部更新，新增了证据分块上传，并修正了RT-only头文件条件include。本任务没有写入这些源码改动。22:25复核后的关键源码哈希保存于 `tmp/audit-20260909-source-sha256.txt`；下列测试区分初次结果与末次结果。主要采集、量程、报警、UI和MQTT可靠性缺口仍存在；新增证据上传未在本轮做实机端到端验收。

源码根目录：`D:/CODEX SPACE/infei/xiaozhi/EdgeDoctor-RT`。BSP 根目录：`D:/CODEX SPACE/infei/edgi-workspace/Edgi_Talk_M55_EdgeDoctor`。下文 `firmware/`、`scripts/` 等路径相对源码根目录，`BSP/` 相对 BSP 根目录。

## 1. 总体结论

附件指出的主要集成缺口确实存在，但有错误、过时结论，也漏掉了比 UI 美化更优先的正确性问题。当前应按仓库已有 V2 规格修复成“板载 IMU 低频自主监测 + KAIST 独立回放”，不需要重新决定产品定位。

依据：`docs/specs/edgedoctor-v2/README.md:7` 已明确用户选择仅板载 IMU；`01-technical-spec.md:59` 规定 208 点窗口、104 点步长。根 README 仍以回放基准为主，确实存在文档版本口径不一致。

## 2. 对附件逐项核实

| 附件判断 | 核查结论 | 当前证据与修正 |
|---|---|---|
| EdgeDoctor BSP 是应用集成壳，逻辑在外部仓库 | 成立 | `BSP/applications/edgedoctor` 是指向本工作区 `firmware/m55` 的绝对路径链接；`firmware/m55/SConscript` 再引入 common。 |
| 项目只是可复现基准 | 不完整 | 根 README 是此定位，但 V2 规格明确要求真实低频监测。应理解为 V1 回放已存在、V2 集成尚未完成。 |
| `ed_sensor_submit_block()` 没有生产者 | 成立 | `firmware/` 中只有头文件声明和 `sensor/ed_imu_lsm6ds3.c:35` 实现。无后台 FIFO 生产者。 |
| 整个工程没有真实 IMU 数据 | 表述过重 | `ed_sensor_read_block():36` 在 timeout 非零时直接读 OUTXL；`edgeimu sample:40` 可产生真实低频特征、基线、MQTT telemetry/event。缺的是常驻采集及 UI 数据闭环。 |
| 多模态 DSP 只有 replay 喂数据 | 成立 | `runtime/edgedoctor_app.c:113` 的 `replay_chunk()` 是 m55 唯一调用者；sync 没有板端 live 调用。 |
| `edgeimu sample` 阻塞 shell | 成立 | 208 次直接读取，每次 `mdelay(5)`；另含计算和 Flash 写入。不是后台采样。 |
| 量程与换算系数不一致 | 成立，附件修法有错 | CTRL1_XL=0x50 是 208 Hz、±2g；range_mg=4000、scale=0.000122 是 ±4g。±4g 的 FS 位应为二进制 **10**，不是 01；保留低两位时目标上六位为 **0x58**。 |
| 幅值变小一半，静止应读 0.5g | 错误 | 代码执行 raw×scale。实际 ±2g 的 1g 原始值约16384，乘0.000122约 **1.999g**，所以会放大约2倍。应测原始三轴均值向量的重力模长，不是已经去直流的动态 RMS。 |
| 所有基线偏离度也错两倍 | 不能如此推断 | 幅值类特征放大约2倍、能量约4倍；峰度/峰值因子不按此比例变化。`ed_baseline_deviation()` 是带 MAD 和下限项的归一化偏离，旧基线与新数据同错时可能部分抵消。 |
| XiaoZhi UI 直接依赖 EdgeDoctor UI | 成立 | `BSP/.../xiaozhi_ui.c:18,605,615` 直接声明并调用 init/process。先创建 XiaoZhi 对象，再覆盖 EdgeDoctor root。 |
| s_main_container 未初始化 | 成立，但后果描述不准 | 只声明，无赋值，324行 `lv_obj_create(NULL)` 创建的是独立 screen，不是自动成为 active screen 的子对象。它是否影响实际显示要上板验证。 |
| emoji -1 一定导致越界 | 未证实越界 | 确实透传 -1，但下游 `lv_example_virtual3d_animated_emoji.c:2167` 是 switch/default，落入 speaking，不是直接数组索引。建议明确 fallback，但不要描述为已确认越界。 |
| 充电文字重复追加 | 成立，删除描述不准 | 492行每次追加；退出充电时从第一个标记处截断全部后缀，不是仅删除一次标记。 |
| UI volatile 标志和 MQTT state 存在线程竞争 | 成立 | `ui/edgedoctor_ui.c:89` 标志多线程读写，读后清零可丢命令；MQTT队列也不仅仅是状态显示竞争。见下文。 |
| 多个“按钮”实际是 label | 成立 | `build_events:503`、`build_trends:521`、`build_asset:535`、`build_connection:546` 只创建文字，且连接/事件/基线信息是静态占位。 |
| MQTT每5秒flush，固定上限1.6条/秒 | 不准确 | worker 当前 `mdelay(1000)`，但先 poll，明文socket接收超时5秒，再串行 PUBACK。因此轮次取决于poll等待、RTT和失败，不能宣称固定1.6条/秒。 |
| MQTT支持持久outbox与可用TLS发布 | 不成立 | outbox 是静态RAM数组；publish用 mqtt_fd<0 拒绝 TLS-only 连接（TLS分支不设置mqtt_fd）。文件保存只见配置，不见事件journal。 |
| 单行C难维护 | 成立 | sensor和MQTT客户端大量复杂逻辑压在单行，报警核心也有此情况。 |
| .o 已提交，应清理Git | 不成立 | `git ls-files '*.o' '*.elf' '*.dblite'` 无输出；根 `.gitignore` 已忽略 `*.o`，`git check-ignore`确认生效。构建残留不等于已提交。 |
| 两个应用树只差两个文件，且都链接同一应用 | 当前不成立 | EdgeDoctor有链接；当前XiaoZhi工程中 `applications/edgedoctor` 不存在。不能沿用旧的双项目比较结论。 |
| README_zh仍是小智说明 | 成立 | `BSP/README_zh.md:1` 是小智示例说明。应用自己的V2入口应补齐。 |
| main没调wifi_manager_init，可以裁剪网络/唤醒 | 部分错误 | 当前 `BSP/applications/main.c` 确实调用wifi_manager_init，受 `BSP_USING_XiaoZhi` 控制。V2还规划语音；DSP依赖edge-impulse目录内CMSIS，不能整目录删除。 |
| 没有板端自动化冒烟 | 不成立，但验收不足 | 已有 `scripts/test-board-v2.py` 和历史结果；它靠反复发送sample驱动数据，不能证明自主采集，且PASS条件未约束连接成功、质量有效、连续采样及参考回放一致性。 |

器件依据：[ST LSM6DS3TR-C 数据手册，第21、60–61页](https://www.st.com/resource/en/datasheet/lsm6ds3tr-c.pdf)；[ST LSM6DS3驱动的量程枚举](https://github.com/STMicroelectronics/lsm6ds3-pid/blob/master/lsm6ds3_reg.h)；[ST换算函数](https://github.com/STMicroelectronics/lsm6ds3-pid/blob/master/lsm6ds3_reg.c)。寄存器推导已确认，实机幅值尚未重测。

## 3. 需要补进改造范围的问题

### A. 采样率、质量和基线没有形成可靠闭环（P0）

位置：`firmware/m55/sensor/ed_imu_lsm6ds3.c:33–40`。

- 5ms睡眠加I2C耗时不等于208Hz；无DRDY/FIFO保障时可能漏样。timeout参数也没有按期限等待数据。
- block.first_sample_mono_us只是读取时刻；quality的last_sample_mono_us又被“当前处理时刻”覆盖。没有真实传递GAP、饱和、总线故障，质量状态缺乏完整输入。
- 返回block可以包含32个样本，但sample循环仅使用xyz[0]；接上FIFO后直接复用这个循环会丢弃其余样本。
- stop只改软件标志和ring，没有关闭硬件ODR。start的revision基于调用者值加1，多次零配置start会重复revision=1。
- 加载基线只检查read长度，直接将文件key设为当前设备key，未核对schema、CRC、配置和安装信息。量程修复后旧基线不能自动沿用。
- baseline begin传0，超时检查依赖started_mono_us非零，因此120秒超时不生效。重建baseline的版本递增也需要独立持久管理。

### B. 实际报警绕开公共状态机，公共核心也未达到规格（P0）

位置：`runtime/edgedoctor_app.c:276`、`common/src/ed_alarm.c:9`。

sample直接阈值比较后调用app_live_condition；它只有一个live_event，单窗口触发/恢复，没有使用策略的3次触发、20次恢复、质量限制。它与MQTT线程ack_event共同修改同一事件。UI则另有alarm_active/ack状态，只读旧回放snapshot，真实事件、界面确认、远程确认不是同一状态来源。

不能只把函数名换成ed_alarm_step就结束：现有核心在deviation低于alarm=10时就累加恢复次数，没使用recovery=3，也未实现warning=6的完整路径。动态证据：临时Host程序先输入3次11，再输入20次9，输出：

```text
recovery_threshold=3.0 input=9.0 condition=1 (ACTIVE=0 RECOVERED=1)
```

程序按“应该仍ACTIVE”断言返回1，复现错误。源码在 `tmp/audit-alarm-recovery.c`。核心还需审查存储失败传播、确认pending、已恢复未确认事件、槽回收和事件身份，不能把已有单元测试通过当作规格全部完成。

### C. MQTT正确性先于吞吐优化（P0/P1）

位置：`protocol/ed_mqtt_client.c:24–61`、`protocol/ed_mqtt_socket.c:109–228`、`server/mqtt_ingest.py:25`。

1. TLS连接成功后mqtt_fd仍为-1，publish立即拒绝。`tls_valid`只是根据端口填值，不能证明证书验证。
2. outbox RAM队列在PUBACK后删除；`persisted++`其实是broker确认计数。服务端ingest返回落库结果，但MQTT bridge没有发布event_receipts。
3. flush先发telemetry后发event；拥塞时事件可能饥饿。telemetry enqueue局部关中断，但flush的memmove/count减法没在同一同步协议下。shell还能并发connect/disconnect/poll/flush共享socket。
4. publish假定下一帧就是4字节PUBACK，期间收到下行PUBLISH会错误断开；未见PINGREQ/PINGRESP保活。
5. handle_command中event_id/command_id/challenge未初始化且忽略解析失败；缺字段时可能把未定义内容当C字符串使用。必须先校验命令再进入运行时。

### D. “双缓冲就是线程安全”的赞扬需要收回（P1）

`runtime/edgedoctor_app.c:90,238` 和 `common/src/edgedoctor_snapshot.c` 使用普通payload、volatile索引和DMB。DMB只约束顺序，不替代C线程同步，也不保证读者复制期间写者不复用该缓冲。当前单核线程调度下能否触发取决于优先级/抢占；尚未做板端压力复现。应使用明确的RTOS消息/互斥快照协议，host并发测试也需要真实同步。

## 4. 本轮实际验证结果

| 验证 | 结果 |
|---|---|
| `scripts/build-firmware.ps1`，默认EdgeDoctor BSP | 退出0；SCons报告up to date。这是增量检查，不是全量重编译或烧录证明。日志 `tmp/audit-20260909-build.log`。 |
| 根工作区运行 `EdgeDoctor-RT/scripts/test.ps1` | 退出1。18项Python数据测试通过；common、monitor、LF features、baseline四组C通过；MQTT契约编译报 `edgedoctor_app.h: No such file or directory`，后续voice/server未由总runner执行。 |
| 外部修正条件include后，从根工作区再次运行test.ps1 | 退出0：18项数据测试、6组C测试、25项server测试全部通过。日志 `tmp/audit-20260909-host-final.log`。初次头文件错误已不是末次版本的未修复问题。 |
| 从README指定的EdgeDoctor-RT目录运行test-common-c.ps1 | 退出1：前四组C通过，MQTT测试源文件报 `ed_mqtt_client.h: No such file or directory`，相对include路径重复了EdgeDoctor-RT前缀。日志 `tmp/audit-20260909-host-project-cwd.log`。 |
| 外部更新后的末次firmware增量检查 | 退出0，仍为up to date。日志 `tmp/audit-20260909-build-final.log`；不宣称本轮进行了全量重编译。 |
| 单独运行server pytest | 25 passed，退出0。 |
| 告警恢复阈值最小复现 | 编译成功，行为断言失败：输入9仍会恢复，与阈值3不符。 |
| 实机1g、FIFO、TLS、断电、8h稳定性 | 本轮未执行，不能声称通过。历史串口轮询记录不能替代自主采集证据。 |

初次Host日志：`tmp/audit-20260909-host.log`。初次MQTT Host编译失败根因是RT-only头文件无条件include，已在外部更新中修正；测试脚本另有相对include路径依赖当前目录的问题，仍应修正。

## 5. 建议执行顺序

1. 恢复Host测试入口；拆开需要编辑的单行函数，保持行为不变。
2. 核实器件身份、修正量程、寄存器回读、静止重力自检、旧基线失效处理。
3. 接通常驻FIFO采集→208/104低频窗口→质量/基线→修正后的报警核心。
4. 让GUI、命令、报警、快照各有明确线程所有者，移除XiaoZhi反向应用依赖。
5. 修复MQTT/TLS/解析/队列，补journal和服务端回执，最后优化调度与吞吐。
6. 补上真实数据绑定、可用按钮、可复现BSP导入和自主采集验收。

具体文件、接口、关键代码和验收步骤见 [代码改造方案](superpowers/plans/2026-09-09-edgedoctor-remediation.md)。
