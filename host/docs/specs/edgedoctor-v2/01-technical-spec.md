# EdgeDoctor V2 技术规格

版本 0.1，2026-09-06，状态：设计目标。与 [MQTT 规格](02-mqtt-service-spec.md)、[验收规格](03-acceptance-test-spec.md)、[UI 规格](04-ui-spec.md) 一起执行。

## 1. 需求编号与基本约束

| ID | 必须实现的行为 |
|---|---|
| CAP-01 | 板载 IMU 实际采集，报告实际采样率、物理幅值、丢样与配置 |
| BAS-01 | 操作者确认健康工况后学习基线，绑定设备/安装/工况并版本化 |
| QLT-01 | 区分断线、过期、饱和、丢样、静止、有效信号与模型不适用 |
| DIA-01 | 低频基线异常检测；无法确定时不强制给出五类标签 |
| EVT-01 | 与 UI 无关的报警状态机，确认与恢复分离，事件身份稳定 |
| EVD-01 | 报警前后原始片段、完整性校验、一键查看/回放 |
| HIS-01 | 设备档案、24 小时板端趋势、服务端长期趋势和维修比较 |
| EXP-01 | 展示频谱、物理特征、基线偏离和触发规则 |
| VOI-01 | 基于真实快照的语音查询与受仲裁的报警播报 |
| SYS-01 | 采集不被 UI/网络/存储阻塞，资源有界，断网继续运行 |

使用官方 Edgi_Talk_M55_XiaoZhi v1.4.0 基线。新业务优先置于 firmware/m55/app、sensor、protocol、ui 和 firmware/common。保持用户的 app/edgepet 修改不动。不修改启动、链接脚本、HAL、生成配置、Wi-Fi 资源。若 M0 发现必须修改硬件配置，应先形成具体最小差异及原因，再处理该依赖；不能静默改变官方基线。

本版本在 M55 内完成，不要求 M33 协同或 U55。每个功能变化执行现有 host 测试与固件构建，警告不得屏蔽。文档中建议的 CLI、API、文件在实施阶段创建，不能作为当前可用命令。

## 2. 数据通路与线程

```text
IMU FIFO → 采集线程 → 固定块池 → DSP/质量/诊断线程 → 核心快照
                          └→ 原始滚动缓冲             ├→ UI 视图模型
                                                      ├→ 报警事件管理
                                                      │   ├→ 留证/存储工作队列
                                                      │   └→ 持久待发记录 → MQTT
                                                      └→ 趋势聚合/语音只读工具
```

MQTT/语音不得直接更新 LVGL，不得在回调内读写传感器。LVGL 所有对象访问在现有 UI 线程执行。共享快照使用短时间 RT-Thread 优先级继承互斥保护复制，禁止持锁访问文件/网络/图表；目标临界区 ≤100 µs。原始块通过固定池所有权交接，不依赖 volatile 代替同步。

| 工作 | 建议优先级（数值越小越高） | 栈预算 | 周期/限制 |
|---|---:|---:|---|
| FIFO 采集 | 12 | 3 KiB | 水位通知，或 10 ms 读取 FIFO 状态的绝对周期任务 |
| DSP / 质量 / 报警判定 | 16 | 8 KiB | 每 104 样本输出一次，约 0.5 s |
| 现有音频/网络系统 | 原值 | 原值 | M0 审计优先级；以上新优先级不得挤占原音频关键任务 |
| 存储 | 23 | 4 KiB | 批量写入；队列满显式记数 |
| MQTT | 24 | 8 KiB | 单连接所有者，读写超时，重连退避 |
| 语音业务适配 | 25 | 4 KiB | 通过现有音频仲裁器提交 |
| UI | 复用现有线程 | 原值 | 10 Hz 视图更新，图表最多 5 Hz |

优先级是起始配置，M0 记录现有线程列表后可在应用代码内调整；最终值随验收报告冻结。网络协议栈自身优先级不在本方案中修改。

## 3. CAP-01：板载 IMU profile

### 3.1 固件默认 profile：imu_lf_208_v1

- 实机 WHO_AM_I、总线名和地址写入 inventory。根据当前代码，候选为 i2c0 上 0x6A/0x6B 的 LSM6DS3；探测失败不写配置。
- 三轴加速度，ODR=208 Hz、量程±4 g；关闭不需要的陀螺采样。按对应数据手册设置并回读 BDU、自动地址递增、FIFO 连续模式与滤波。
- 目标模拟抗混叠带宽50 Hz，业务分析5–40 Hz；根据实机寄存器能力验证配置，配置不符使 profile 无效。不能通过提高 ODR 宣称板载安装适用于工业轴承高频诊断。
- FIFO 中以完整 XYZ 六字节样本为单位读取，水位为8个三轴样本；有可用 IRQ 则 IRQ 仅唤醒线程，否则每10 ms检查 FIFO。采样时基来自传感器 ODR，不由轮询间隔伪造。
- I2C 事务超时10 ms，失败只在采集线程处理。超过200 ms没有新样本即数据过期；硬件总线不能提供有限超时则 CAP-01 不放行。
- 记录原始 int16 ADC；LSM6DS3 ±4 g 对应换算系数须以冻结数据手册/实机配置核实后写入 profile，不在客户端写死成通用值。
- 208点=1秒诊断窗口、104点步长。20 ms不再要求整数样本：旧 KAIST EdgePack 分块保持不变，新增 IMU 通路允许可变块长。
- 使用单调时间锚定 FIFO 批次与样本序号，区分 nominal_sample_rate 与 measured_sample_rate。每10秒按样本数/单调时间估计实测率；UTC未同步时保持 null，不编造日期。

### 3.2 基础接口（新建公共契约）

```c
#define ED_IMU_BLOCK_MAX 32u
typedef struct {
    uint64_t first_sample_seq;
    uint64_t first_sample_mono_us;
    uint32_t sample_period_ns;
    uint32_t config_revision;
    uint16_t count;             /* 1..32 */
    uint16_t flags;             /* GAP, FIFO_OVERRUN, CLOCK_ESTIMATED */
    int16_t xyz[ED_IMU_BLOCK_MAX][3];
} ed_imu_block_t;

int ed_sensor_start(const ed_sensor_config_t *config);
int ed_sensor_stop(void);
int ed_sensor_read_block(ed_imu_block_t *out, uint32_t timeout_ms);
void ed_sensor_get_stats(ed_sensor_stats_t *out);
int ed_core_ingest(const ed_imu_block_t *block); /* 固定池提交，不能等文件或网络 */
int ed_core_snapshot(ed_snapshot_t *out);       /* 一致快照 */
```

ed_sensor_config_t 包含 profile_id、ODR、量程、带宽、axis_map[3]、axis_sign[3]、安装修订号；不允许随意写寄存器值。ed_sensor_stats_t 包含 nominal_hz、measured_millihz、samples_received、samples_dropped_known、fifo_overruns、unknown_gap_count、bus_errors、queue_high_water、last_sample_mono_us。FIFO 丢失数无法准确恢复时记录 unknown_gap_count，不伪造精确丢样数。

块池16×32样本，单生产者/单消费者队列。满时丢弃最旧待消费块并记 GAP；不得覆盖消费者持有块。GAP 后清空诊断窗口重新预热。

### 3.3 单位与处理

存储原始ADC+灵敏度/量程/映射。计算轴向去均值的动态加速度，单位g；换算 m/s² 时使用9.80665。窗口加周期 Hann，208点补零到256点 FFT；峰值频率分辨能力按1秒观测窗解释，补零不提高物理分辨率。

另行保留 raw mean/DC 向量以观察姿态。更换安装方向不会因逐窗口去均值而自动变成同一工况。

## 4. QLT-01 / DIA-01：质量和不确定性

### 4.1 状态相互独立

snapshot.source ∈ LIVE_IMU、KAIST_REPLAY、EVIDENCE_REPLAY、DEMO。

snapshot.quality ∈ WARMUP、VALID、STALE、DISCONNECTED、SATURATED、GAPPED、CONFIG_ERROR；另有 idle 标志。quality不是模型置信度，也不包装成没有标定的“信号质量百分比”。

snapshot.diagnosis ∈ NO_BASELINE、BASELINE_MISMATCH、WITHIN_BASELINE、UNKNOWN_ANOMALY、KNOWN_CLASS、UNCERTAIN、UNAVAILABLE。实时板载 profile 不运行 KAIST 分类器；KNOWN_CLASS 仅允许验证过的 profile/model 组合，V2默认仅 KAIST_REPLAY。

### 4.2 确定规则

| 条件 | 行为 |
|---|---|
| 未累计208个连续有效样本 | WARMUP，显示进度，不显示“正常” |
| 200 ms无新样本 | STALE，保留上次值但变灰并显示数据年龄 |
| 探测失败或持续1秒总线读取失败 | DISCONNECTED，传感器事件，不判机械故障 |
| 任一轴绝对 ADC ≥32700 的样本比例≥1% | SATURATED，该窗口不参与基线/机械诊断 |
| FIFO overrun、序号断裂、显式块丢弃 | GAPPED，重新预热；统计已知/未知丢样分别显示 |
| 动态向量RMS<0.005 g连续10秒 | idle，显示“静止/振动很弱”，不是传感器断线，也不是证明设备健康 |
| profile/安装/工况与基线不匹配 | BASELINE_MISMATCH，仍显示物理量，停止基线异常判断 |

上述阈值是开发默认值，写入版本化 monitor policy；必须通过健康运行误报测试后冻结。VALID 连续2秒解除质量故障，但不自动抹除已有机械事件。

### 4.3 低频特征与基线偏离

15维特征：RMS_X/Y/Z、向量RMS、向量峰值、峰值因子、三轴峰度最大值、8个频带能量。向量RMS=sqrt(mean(x²+y²+z²))；峰值因子=向量峰值/max(向量RMS,0.001g)。峰度为各轴四阶中心矩/二阶中心矩²，近静止轴标记不可用，不能产生NaN。

8个半开频带边界为[5,8,11,14,18,23,29,35,40]Hz，最后一段含40Hz。频带能量采用功率谱积分并用Hann能量归一化；与Python参考共用contract与golden。分析频带之外的原始数据仍保存在证据中。

基线每维保存中位数m和MAD；s=max(1.4826×MAD, epsilon)，z=abs(f-m)/s。epsilon：RMS/峰值0.002g、峰值因子0.1、峰度0.2、频带能量1e-6 g²。派生 baseline_deviation=max(z)；缺失维不参加计算，少于12维可用则 UNCERTAIN。

默认触发：deviation≥6连续3个有效输出（约1秒观察跨度）进入WARNING；≥10连续3个输出升级ALARM；<3连续20个输出（约9.5秒跨度）恢复。idle/质量无效暂停机械事件恢复计数，超过2秒中断清零连续计数。数值偏离不解释为故障概率，UNKNOWN_ANOMALY的名称为“振动偏离基线，类型未确定”。

KAIST回放若最高softmax<0.70或前两类差<0.15，输出UNCERTAIN；这只是分类拒答策略，不能声称具有已验证开放集检测能力。现场未知故障识别能力需独立数据验收，不在V2冒认。

## 5. BAS-01：设备基线

实体键：asset_id + sensor_profile_hash + mount_revision + operating_profile_id。设备ID代表板子；asset_id代表被监测机器；一台板子同一时刻只监测一个asset。最多8个设备档案，每个最多3个工况基线。工况由操作员选择，不从未接入的转速传感器伪造RPM。

基线向导：确认设备/安装/工况 → 操作者声明当前健康运行 → 2秒预热 → 收集60个不重叠的有效1秒窗口 → 稳定性校验 → 预览 → 保存新版本。最长120秒；超时、出现严重报警、idle超过5秒、质量有效窗口不足均失败。6组各10窗口的向量RMS均值，其最大/最小比值不得超过1.15；最小值低于idle阈值则拒绝建立运行基线。

原基线在新基线提交前一直有效。新版本采用双文件generation+CRC、写临时文件/fsync/原子替换；启动选最新有效版本，断电不得留下半份基线。不自动在线更新基线，以免把故障学习成正常。更换传感器配置/安装/设备/工况立即使旧基线不匹配，界面给出重新选择或重建入口。

基线元数据：baseline_id、version、asset_id、profile_hash、mount_revision、operating_profile_id、创建UTC/null、创建单调时刻与boot_id、operator、有效窗口数、15维统计、8频带均值、policy_version、CRC32。最多保留当前+上一版本，支持回退，回退写审计记录。

## 6. EVT-01：事件状态机

两条正交状态：condition=ACTIVE/RECOVERED，ack=UNACKNOWLEDGED/ACKNOWLEDGED。组合可显示“待确认/已确认·仍异常/已恢复·待确认/已归档”。无效信号不得隐式恢复机械故障。

event_id = device_id + boot_counter + monotonically_increasing_event_seq；boot_counter在启动前双副本CRC原子递增，无法安全持久化时报告身份错误并禁用远程命令，不能重复使用旧ID。恢复出的跨重启活动事件仍沿用原event_id。每个状态变化生成revision（从1递增），唯一键(event_id,revision)。

同一asset+source+事件类型只有一个活动事件。机械基线异常、采集故障、存储故障分开；首次触发创建事件，持续帧只更新内存峰值，严重度升级才发新revision。确认不停止采集、不清故障、不删除证据；恢复按连续健康条件产生新revision。活动状态变化和确认操作由诊断线程串行处理，避免本地/远程确认竞态。

每设备上限8个活动事件。超限将同类质量问题合并为一个汇总事件并记 overflow_count；机械严重事件优先。提醒：未确认事件每10分钟最多一次，已确认事件只在升级/恢复时通知。一个10分钟恒定故障应只产生一个事件ID。

```c
int ed_alarm_step(const ed_snapshot_t *s, uint64_t now_us);
int ed_alarm_ack(const ed_ack_request_t *request, ed_command_result_t *out);
int ed_alarm_get_event(const char *event_id, ed_event_t *out);
```

ed_ack_request_t={command_id,event_id,expected_revision,actor,origin,target_boot_id,received_mono_us}。不存在返回NOT_FOUND；revision不符返回CONFLICT及当前revision；已确认重复请求返回ALREADY_ACKNOWLEDGED且不新增revision。持久化成功后才返回确认成功；UI先显示“确认中”。网络发送与本地确认成功不互相等待。

## 7. EVD-01：故障证据

板载profile原始速率208×3×2=1248 B/s。持续5秒滚动缓存6240 B；首次机械事件触发时保留前5秒，采集后5秒，完整10秒载荷12480 B。预热不足/丢样/重启/采集断线时保留现有片段，明确PARTIAL与缺失区间。

维护2个固定证据槽，每槽最多16 KiB；槽忙时同一事件只记录追加峰值，不重新录片。第三个并发事件只留元数据并标记NO_SLOT，不能阻塞采集。质量事件可保留前片段但不得伪造后半段。

新增EDV2文件格式，旧EDGP不变：magic[4]="EDV2"、version u16=2、header_bytes u16、metadata_len u32、payload_len u32、metadata_crc32 u32、payload_crc32 u32；固定头24字节，小端。后随≤2048 B UTF-8 JSON元数据、int16 XYZ载荷。metadata含event_id、profile、灵敏度、样本率分数、起始seq/单调时间、UTC映射、pre/post实际长度、GAP区间、原诊断与baseline版本、complete标志。禁止直接把C结构体内存写成文件头。

写入.tmp、校验、fsync后rename；启动扫描.tmp恢复可校验部分或删除无效文件并记错误，不能当作完整证据。

“回放证据”默认查看原始记录与当时判定；可选重算明确标记当前算法/基线版本。实时采集继续，回放使用独立低优先级上下文，算力不足降为静态查看，不能停监测。回放不生成正式机械报警、不发送正式通知，不覆盖实时快照。

证据上传采用MQTT二进制分片，协议见02。禁止将波形base64塞进状态消息。留证片段失败不影响本地告警，但必须可见。

## 8. HIS-01 / EXP-01：历史、档案与解释

板端每分钟聚合一条：asset/profile/baseline、开始时间映射、RMS最小/均值/最大、最大deviation、有效/无效秒数、事件数。每条固定64 B，保存1440条=92160 B（24小时）；掉电只允许损失尚未提交的当前分钟，旧段CRC正确。

服务端保存1 Hz遥测30天、1分钟聚合365天、事件365天、证据30天，默认10设备；维护任务按配额清理并保留审计记录。离线缺口不得插值成正常，重连补传分钟聚合，每条有asset/boot/period_seq去重键。

设备档案含名称、类型、安装位置、轴映射、安装修订号、手工工况、当前基线、维修记录。维修记录只记时间、操作、备注，不自动恢复报警。前后对比只在同profile/安装/工况且两侧各≥30分钟有效数据时启用；显示日期范围、有效率和RMS变化，不推断剩余寿命。

诊断依据页显示当前与基线频谱叠加、RMS/峰值/峰值因子/峰度、偏离最大的3个特征、触发阈值和持续时长。频谱默认相同物理刻度，不能各自归一化后制造差异。标注“观测依据，不等同于故障因果解释”。LIVE_IMU隐藏五类概率图，显示基线偏离；KAIST_REPLAY才显示类别概率。

## 9. VOI-01：语音

新增只读工具self.edgedoctor.get_status、get_recent_events(limit≤5)、get_event(event_id)、get_guidance(event_id)。每个返回snapshot_id、asset、source、quality、age_ms、diagnosis、baseline_version、事件原文摘要；get_guidance只返回版本化本地建议表，禁止生成未提供的转速、故障类、健康百分比或寿命。

工具读取快照≤100 ms；数据年龄>2秒明确说“数据已过期”。无基线说“尚未建立运行基线”；LIVE_IMU异常说“振动偏离基线，具体故障类型未确定”。断开XiaoZhi服务时，触屏查询仍可用；不承诺离线语音识别。

报警播报走现有扬声器仲裁；优先级：既有音频安全/交互规则 → 一次性严重报警 → 常规语音回复。没有确认可用的抢占接口时排队≤10秒，超时用提示音/屏幕，不私自重置音频驱动。每事件首次严重报警播报一次，同事件冷却60秒；确认可停止重复提醒但不改变故障状态。

扬声器播放时设置audio_contaminated；机械耦合使IMU也可能受影响，暂停基线学习，冻结恢复计数。用播报开/关成对试验确定受影响区间，默认播放后500 ms保护期；受保护窗口标记UNCERTAIN并不计机械触发连续数，传感器断线/饱和检查继续。界面说明监测受播报影响，不能显示这段“正常”。

## 10. 内存、Flash、时间和故障策略

| 应用新增预算 | 上限/说明 |
|---|---|
| 原始块池 | 8 KiB（含元数据/对齐） |
| 预录+证据槽 | 40 KiB |
| 特征、基线样本与诊断状态 | 24 KiB |
| 事件/命令/MQTT TX RX缓冲 | 32 KiB；MQTT包≤4096 B，JSON≤2048 B |
| 新线程栈合计 | 27 KiB |
| 其他状态和趋势缓存 | 29 KiB |
| 固定RAM合计目标 | ≤160 KiB，必须由map与sizeof核对 |
| TLS额外瞬时堆目标 | ≤96 KiB，实测；不能用静态预算代替TLS峰值 |
| 新LVGL对象/字体/绘制缓冲 | 单列，新增UI运行堆目标≤160 KiB；字体Flash目标≤1 MiB，复用现有显示buffer |

M0实测空闲内存必须覆盖新增峰值并保留至少25%余量；不满足时先减少UI缓存/证据并重新评审指标，不修改链接脚本挤空间。

`/flash/edgedoctor/`配额1 MiB：配置/档案/基线64 KiB、事件journal/待发128 KiB、趋势双段192 KiB、证据640 KiB。部署要求该挂载点扣除既有资源后可用空间≥1.25 MiB；不足时报STORAGE_CAPACITY，不承诺持久留证验收通过。禁止清理Wi-Fi文件或XiaoZhi资源。

事件revision紧凑二进制≤512 B，待发最多128条，内存发送索引32条；journal采用预分配段。超过容量不保证无损：保留已有未确认记录，停止新增持久记录并显式报告OUTBOX_FULL及lost_event_revisions；本地报警继续，服务器重连收到当前状态与丢失计数。该状态必须红/橙提示，不能静默覆盖。

证据最多40份，每份≤16 KiB；仅淘汰已恢复且已同步/导出的最旧片段。未同步/活动片段不可自动删除，空间不足只记元数据与NO_STORAGE。趋势和日志分段批写，事件触发/确认/恢复必须durable提交；fsync/rename语义在M0实机验证，失败不能声称断电安全。

单调时钟用于持续时间；UTC用于展示与服务端排序辅助。UTC未同步不阻止监测，记录boot_id+mono_us。校时不修改历史单调时间；远程命令使用boot/会话绑定，详见02。

目标性能：采集块提交P99≤2 ms；每诊断输出DSP+判定P95≤5 ms/P99≤10 ms；最后样本就绪到核心结果P95≤30 ms；UI触摸反馈P95≤100 ms；这些均不包含1秒采集窗口。每块耗时全量统计，禁止只测输出帧那一次调用。

## 11. 实施文件边界

保留旧edgedoctor_onboard_model、EdgePack解析和KAIST特征合同。修改edgedoctor_app.c仅负责数据源协调，拆出UI报警。新增common/include/ed_monitor_types.h及common/src/ed_quality.c、ed_baseline.c、ed_alarm.c、ed_evidence_format.c、ed_trend.c；新增m55/sensor/ed_imu_lsm6ds3.c、m55/app/ed_monitor.c、ed_storage.c、ed_voice.cpp；新增m55/protocol下MQTT模块。UI拆文件见04。

## 12. 参考依据

- [ST LSM6DS3 AN4650](https://community.st.com/ysqtg83639/attachments/ysqtg83639/mems-sensors-forum/21052/2/AN4650.PDF)：ODR、滤波与FIFO配置背景；实际实施冻结完整手册与器件ID。
- [LVGL threading](https://docs.lvgl.io/9.3/details/integration/adding-lvgl-to-your-project/threading.html)：LVGL需要调用方保证线程访问串行。本工程保持已有9.2.0，不因引用文档升级SDK。
