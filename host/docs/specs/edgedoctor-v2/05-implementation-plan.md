# EdgeDoctor V2 Implementation Plan

> For agentic workers: 使用 superpowers:executing-plans 按工作包顺序实施。每个工作包独立测试和构建；未要求并行代理。

**Goal:** 在现有EdgeDoctor上实现板载IMU低频监测、基线、可靠事件、MQTT/服务端、留证、趋势、漂亮且诚实的UI与语音查询。

**Architecture:** M55本地诊断和事件状态权威；采集、存储、联网与显示通过有界队列/快照隔离。局域网服务保存历史、派发通知并转发带版本的远程确认。旧KAIST回放保持独立。

**Tech Stack:** RT-Thread、C11/C++、CMSIS-DSP、LVGL 9.2.0、Paho Embedded C、TLS/MQTT 3.1.1、Mosquitto、Python/FastAPI/Paho、SQLite、ntfy、静态网页。

**Spec:** [技术](01-technical-spec.md)、[MQTT](02-mqtt-service-spec.md)、[测试](03-acceptance-test-spec.md)、[UI](04-ui-spec.md)。勾选项描述目标与证据门禁；已实现部分以当前源代码、测试输出和实机记录为准。

## 全局约束

- 官方XiaoZhi v1.4.0、用户app/edgepet工作副本保持；不修改启动/链接/HAL/生成配置/Wi-Fi资源。
- LIVE_IMU为208Hz三轴低频监测；原6.4kHz模型只走已验证KAIST回放，不插值伪兼容。
- 采集/DSP/判定零动态分配；固定RAM新增≤160KiB；TLS新增峰值≤96KiB；UI新增堆≤160KiB；全部预算须实测。
- 每个功能变化后执行host测试和固件构建；不能压制新警告。需要同时跑服务器测试的工作包注明。
- 无需在计划阶段写入设备/账户/网络。部署阶段先生成本地可审阅配置、锁定依赖，再根据已授权项目范围部署；手机凭据/通知授权为部署输入，不伪造完成。
- 本计划所列新增路径相对于EdgeDoctor-RT；测试命令随相应包创建。现有可运行入口是scripts/test.ps1和scripts/build-firmware.ps1。

## 工作包总览

| 包 | 依赖 | 可独立交付成果 | 对应验收 |
|---|---|---|---|
| W0 | 无 | 冻结环境、实机profile与资源审计 | SYS-01..04 |
| W1 | W0 | 公共契约、质量/报警核心与真实快照 | QLT、EVT-01..03 |
| W2 | W1 | IMU采集、物理量、低频特征 | CAP、DIA-01 |
| W3 | W2 | 基线向导后端、偏离判断 | BAS、DIA-02..05 |
| W4 | W1,W2 | 事件持久化、证据与趋势 | EVT-04、EVD、HIS-01 |
| W5 | W1,W4 | MQTT客户端、协议、补传 | NET-01..10 |
| W6 | W5协议冻结 | Broker、告警服务、网页API、通知 | SRV、NTF |
| W7 | W3,W4,W5,W6 | 新板端UI、响应式网页、维修对比 | UI、HIS-02、EXP |
| W8 | W7 | 语音只读工具和播报仲裁 | VOI |
| W9 | W0..W8 | 整机证据包与最终发布差异 | SYS-05..06、全套 |

## W0：基线和硬件资源

文件：新增scripts/inspect-board-v2.py、benchmark/results/v2/inventory/；不编辑vendor文件。

- [ ] 保存git status、已有差异、工具链版本；执行现有两条测试/构建命令，保存原始输出。
- [ ] 串口只读获取线程表、堆/栈、Flash容量、WHO_AM_I及现有传感器配置；核对LSM6DS3准确型号。
- [ ] 在测试目录验证fsync/rename断电语义，生成profile与资源清单；不足时报告具体差额，不能编造通过。
- [ ] 冻结传感器数据手册hash、Paho候选commit、部署依赖锁；记录已有音频优先级和可用入口。
- [ ] 建立应用分支/独立工作区及逐包提交策略，确保用户EdgePet差异可原样恢复。

交付：inventory.json包含器件、ODR能力、FIFO/中断条件、内存预算、Flash配额、SDK/工具链；T-SYS-01..04记录。

## W1：稳定事件与共享数据合同

文件：新增firmware/common/include/ed_monitor_types.h、ed_quality.h、ed_alarm.h；common/src/ed_quality.c、ed_alarm.c；tests/c/test_monitor_core.c；修改runtime/edgedoctor_app.c和ui/edgedoctor_ui.c的快照/报警接线。

输入/输出：ed_snapshot_t含source/quality/diagnosis、物理量、baseline、mono时间、event摘要；ed_alarm_step、ed_alarm_ack、ed_alarm_get_event签名按01。enum采用明确值并加入schema版本，序列化不要依赖enum大小。

- [ ] 先添加下列核心回归，用例失败后实现；测试fixture由本包定义在tests/c/monitor_fixture.h。

```c
/* fixture在本包创建：step_valid(d)每次推进500ms，返回活动事件ID；
   ack_current()提交确认，active_event()返回一致状态副本。 */
for (unsigned i = 0; i < 3; ++i) step_valid(11.0f);
ed_event_t before = active_event();
ack_current();
for (unsigned i = 0; i < 1200; ++i) step_valid(11.0f);
ed_event_t after = active_event();
assert(strcmp(before.event_id, after.event_id) == 0);
assert(after.ack == ED_ACK_ACKNOWLEDGED);
assert(after.condition == ED_CONDITION_ACTIVE);
```

- [ ] 实现质量/事件的纯C状态机、边界值和串行命令处理；加入不同source隔离。
- [ ] UI改成读取状态，不判定/持有事件；确认按钮提交命令并等待结果，移除frame_id重置ACK。
- [ ] 新测试加入scripts/test-common-c.ps1；运行scripts/test.ps1、build-firmware.ps1，保存diff与构建记录。

## W2：真实IMU和特征

文件：新增m55/sensor/ed_sensor.h、ed_imu_lsm6ds3.c、ed_sensor_worker.c；common/src/ed_lf_features.c、common/include/ed_lf_features.h；tools/edgedoctor/low_frequency.py、tests/c/test_lf_features.c、tests/python/test_lf_features.py；修改m55/SConscript添加目录。

接口：ed_sensor_start/stop/read_block/get_stats与ed_core_ingest按01。feature输出固定15维，profile携带实际灵敏度/量程，不读取UI状态。

- [ ] 用fixture生成208Hz正弦：a[n]=0.1*sin(2*pi*10*n/208)，期望单轴RMS约0.0707107g、主频约10Hz；冻结Python黄金向量。
- [ ] 实现有限超时I2C、寄存器回读、FIFO批读、固定块池、序号/时间和统计。
- [ ] 实现去DC/Hann/FFT/能量归一化/15特征；测试纯零、极值、缺样、count=33拒绝。
- [ ] 执行T-CAP-01..05、T-DIA-01，正常联网并发时无丢样；构建与警告记录。

## W3：基线和未知异常

文件：新增common/src/ed_baseline.c、common/include/ed_baseline.h、m55/app/ed_monitor.c；tests/c/test_baseline.c、tests/c/test_diagnosis.c。

接口：int ed_baseline_begin(const ed_baseline_key_t *key)；int ed_baseline_feed(const ed_feature_window_t *window)；int ed_baseline_preview(ed_baseline_t *out)；int ed_baseline_commit(const char *operator_id)；void ed_baseline_cancel(void)。key/window/baseline字段按01第4/5节定义，commit对接W4存储接口。

- [ ] 验证59个有效窗口不能保存，第60个才可预览；插入饱和/idle不增加有效计数，120秒超时保留旧基线。
- [ ] 实现60秒稳定性、m/MAD、epsilon、配置匹配和版本；质量无效冻结计数。
- [ ] 实现6/10/3阈值、3次触发/20次恢复；明确UNKNOWN_ANOMALY与UNCERTAIN。
- [ ] 运行T-BAS、T-DIA-02..05；现场5分类不调用、不填写虚构置信度。

## W4：持久化、证据和趋势

文件：新增common/src/ed_evidence_format.c、ed_trend.c；m55/app/ed_storage.c、ed_evidence.c；common/include/ed_storage_contract.h；tests/c/test_evidence_format.c、test_trend.c；tests/python/test_edv2.py。

接口：int ed_storage_submit_event(const ed_event_revision_t *r)（入队返回，不等Flash）；storage完成后投递ED_STORAGE_COMMITTED/FAILED，驱动sync状态。int ed_evidence_request(const char *event_id)；int ed_evidence_read_chunk(const char *event_id,uint16_t index,uint8_t *buf,size_t cap)（仅存储工作线程调用）。

- [ ] 先用递增ADC/序号fixture测试触发前后1040+1040样本、EDV2大小/CRC/截断错误；文件读写不直接dump结构体。
- [ ] 实现journal双段、boot_counter、事件恢复、基线原子替换、1MiB分区配额与128revision上限。
- [ ] 实现两个证据槽、NO_SLOT/PARTIAL/NO_STORAGE和只淘汰可删除片段；实现1440条分钟趋势。
- [ ] 运行T-EVT-04、T-EVD、T-HIS-01、20次断电测试；故障场景必须可见，文件系统失败不阻塞采集。

## W5：MQTT板端

文件：新增m55/protocol/ed_mqtt_client.c、ed_mqtt_port.c、ed_mqtt_codec.c、ed_sync.c、ed_commands.c；third_party/paho-embedded/与锁文件；tests/python/test_mqtt_contract.py、tools/mqtt_simulate.py、scripts/test-mqtt.ps1。

接口：int ed_mqtt_start(const ed_mqtt_config_t *config)；void ed_mqtt_get_state(ed_connection_snapshot_t *out)；int ed_sync_enqueue(const ed_event_revision_t *revision)；命令通过ed_alarm_ack处理，不能直接改UI。

- [ ] 先实现schema/topic fixtures与模拟Broker用例：收到PUBACK但没有event_receipt时pending仍为1；receipt后pending为0；重复receipt不下溢。
- [ ] 适配Paho Network/Timer有限超时、TLS CA/hostname/time验证、4096B缓存和单线程所有权。
- [ ] 实现重连、state/telemetry/LWT、durable事件补传与应用回执；错误凭据/队列溢出可见。
- [ ] 实现challenge/boot/revision/command_id校验，4KiB/s证据限流/分片重传与趋势补传。
- [ ] 运行T-NET-01..10；每一步构建；测试namespace不得进入正式通知。

## W6：局域网服务与手机通知

文件：按02第9节创建server/目录与scripts/server-up.ps1、server-backup.ps1、mqtt-provision.ps1；tests/server/test_ingest.py、test_commands.py、test_notify.py、test_restore.py。

接口：MQTT payload及HTTP API按02冻结。服务状态机独立于WebSocket/SSE传输状态。SQLite事务成功后才回receipt。

- [ ] 先测event三次重复/逆序与COMMIT前故障：历史唯一、当前态不回退、未提交无receipt。
- [ ] 实现Compose、配置模板、凭据生成/ACL/HTTPS、SQLite迁移、ingest、commands和receipt。
- [ ] 实现API/SSE、分钟聚合、证据接收/CRC、备份恢复、磁盘配额与health检查。
- [ ] 实现ntfy发布和通知任务退避/去重、离线摘要；区分CHANNEL_ACCEPTED和人工确认。
- [ ] 真实手机完成T-NTF-01；iOS/移动网络若未选部署扩展，明确范围，不伪造锁屏达标。
- [ ] 10设备SIM+1板联调，执行T-SRV/T-NET全套，锁定镜像digest和依赖版本。

## W7：UI与网页

文件：04第6节的ui拆分文件、server/web/index.html、app.js、styles.css；tests/c/test_ui_vm.c；tests/ui/场景fixture；prototype/仅作为视觉参考。

接口：void ed_ui_vm_build(const ed_snapshot_t*, const ed_connection_snapshot_t*, ed_ui_vm_t*)；UI只提交typed action，网络/历史读取由工作线程完成。

- [ ] 先写VM矩阵测试：STALE不显示正常、LIVE_IMU隐藏五类概率、MQTT离线不改变机械结论、确认等待durable状态。
- [ ] 实现六页面、统一tokens、44px触摸区、字体子集、报警横幅/详情、基线向导、证据查看。
- [ ] 实现趋势降采样、档案/维修比较、连接轨迹；页面对象有界复用。
- [ ] 实现1440px网页与390px手机版，API确认202显示pending，不乐观确认。
- [ ] 运行T-UI-01..05、T-HIS-02、T-EXP；保存六页实机截图及200次切页性能。

## W8：语音

文件：新增m55/app/ed_voice.cpp、ed_voice_guidance.c；通过XiaoZhi已有McpServer注册扩展，必要的应用层接线差异单独展示；tests/c/test_voice_payload.c。

- [ ] 先用NO_BASELINE/STALE/UNKNOWN_ANOMALY fixture验证响应事实与措辞，禁止填不存在的故障概率。
- [ ] 注册4个只读工具，快照ID/年龄必须返回；建立版本化建议表。
- [ ] 接入扬声器仲裁、每事件播报与污染保护，禁止重新实现音频驱动。
- [ ] 运行T-VOI-01..02以及有/无播报成对采样，确认没有自激报警循环。

## W9：整机发布

- [ ] 全套host/服务器/板端回归，按manifest归档每个ID及原始证据。
- [ ] 完成暖机后≥200样本性能、30分钟连续采集、8小时整机长稳与200次网络重连。
- [ ] 更新README/verification-matrix/旧训练报告口径；实时低频监测与五分类回放分别介绍。
- [ ] 输出最终diff、构建结果、资源map、依赖锁、部署恢复文档、已知限制和未通过项。
- [ ] 只有所选V2范围的必需用例全部有证据才标完成；缺仪器/手机/硬件不替换为模拟PASS。

## 计划自检

CAP/BAS/QLT/DIA→W1..W3；EVT/EVD/HIS→W1/W4/W7；NET→W5；SRV/NTF→W6；EXP/UI→W7；VOI→W8；SYS→W0/W9。所有工作包有指定文件、接口和验收；硬件/内存/存储与手机后台推送的未知条件在W0/W6设实际门禁。
