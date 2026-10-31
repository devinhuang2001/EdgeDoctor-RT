# EdgeDoctor 正确性与自主监测改造 Implementation Plan

> **For agentic workers:** 使用 superpowers:executing-plans 按任务顺序实施；每项独立验证。未勾选为待实施工作。本文件中的代码是拟议改造片段，不是本任务已合入、已编译的固件补丁。

**Goal:** 在保留 KAIST 回放和官方 XiaoZhi 基线的前提下，补齐208Hz板载IMU自主监测、统一报警、可靠同步与真实UI。

**Architecture:** 保留旧的音频/振动回放链，新增独立低频采集和窗口链。采集、monitor、存储、MQTT、LVGL各有单一线程所有者，通过固定容量消息和快照交换数据；网络与文件操作不能进入采集热路径。

**Tech Stack:** RT-Thread、PSoC Edge E84 M55、C11、LVGL 9、现有ST传感器驱动、Python/SCons、pytest。

**Spec:** [本轮核查报告](../../edgedoctor-audit-2026-09-09.md)、[既有技术规格](../../specs/edgedoctor-v2/01-technical-spec.md)、[MQTT规格](../../specs/edgedoctor-v2/02-mqtt-service-spec.md)、[验收规格](../../specs/edgedoctor-v2/03-acceptance-test-spec.md)。本计划补充实际缺口，不替代既有V2范围。

## 全局约束与代码位置

- 保留官方 `Edgi_Talk_M55_XiaoZhi` 基线；不修改启动、链接、HAL、自动生成配置、Wi-Fi资源。
- 每次功能变化后执行firmware build；不压制警告。每任务附相应Host/板端验证，缺失硬件证据不能标PASS。
- 新模块放在现有 `firmware/m55/{sensor,runtime,ui,protocol,logger}`，跨平台纯算法放common。
- 采集、窗口、特征、推理使用静态存储；大数组和线程栈按现有SOCMEM模式放置，检查map增量。
- IRQ只唤醒采集线程；不得在IRQ中做I2C、LVGL、Flash、MQTT、浮点DSP。
- 旧KAIST契约为音频16kHz、振动6.4kHz、20ms chunk、1秒窗口、0.5秒步长。live为208Hz三轴、208点窗口、104点步长、业务频带5–40Hz；禁止把208Hz插值后直接冒充旧模型输入。
- 本文 `firmware/`、`tests/`、`scripts/` 等相对 `D:/CODEX SPACE/infei/xiaozhi/EdgeDoctor-RT`；`BSP/` 指 `D:/CODEX SPACE/infei/edgi-workspace/Edgi_Talk_M55_EdgeDoctor`。
- 既有用户改动（server/web、app/edgepet等）保留；每任务完成检查diff，只提交该任务明确列出的文件，不使用git add -A。

## 数据流与线程所有权

```text
IMU FIFO -> capture[12,3KiB] -> 固定block队列 -> monitor[16,8KiB]
                                                 |
                          208/104窗口 -> LF features -> quality/baseline
                                                 |
                          修正后的ed_alarm + live snapshot
                             |                 |
                       存储请求队列         GUI只读快照
                             |
                       journal worker -> durable completion -> monitor
                             |
                       MQTT worker -> PUBACK -> 等待server receipt
                             |
                       server COMMIT -> event_receipts -> journal同步标记

shell/GUI/MQTT命令 -> monitor命令队列（ACK、基线、采集控制）
shell显示命令 -> UI命令队列（切页、demo开关）
KAIST replay -> 原DSP/模型 -> 独立replay snapshot（不产生正式live事件）
```

优先级数值越小越高；12/16来自既有规格，落地时先检查音频线程优先级，保留其调度保障。网络/存储优先级必须低于采集及monitor。没有新样本时monitor仍按不大于100ms周期更新STALE/DISCONNECTED，不能永远阻塞等采样。

## Task 0：恢复验证入口，先让后续修改能被检查

**修改文件/函数：** `protocol/ed_mqtt_client.c` 顶部include；`scripts/test-common-c.ps1` MQTT编译命令。展开`edgeimu()`和MQTT worker/flush/命令函数时另做纯格式提交，不顺手改变行为。

- [x] RT-only头移入RT条件块：核查期间已由外部工作区更新完成，根目录总runner复测通过，本任务未修改源码。保留以下目标代码作为记录；不要重复修复。common编码/队列接口不能依赖app或硬件：

```c
#include "ed_mqtt_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef EDGEDOCTOR_RT
#include "edgedoctor_app.h"
#include "ed_evidence_capture.h"
#include <rtthread.h>
#include <finsh.h>
#endif
```

- [ ] 测试脚本用绝对构造的include，不依赖调用者工作目录：

```powershell
$mqttInclude = Join-Path $projectRoot 'firmware\m55\protocol'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" `
    "-I$mqttInclude" `
    (Join-Path $projectRoot 'firmware\m55\protocol\ed_mqtt_client.c') `
    (Join-Path $projectRoot 'tests\c\test_mqtt_contract.c') -lm -o $mqttOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

- [ ] 从根工作区和EdgeDoctor-RT目录分别执行test.ps1，应均运行到voice和server；任何后续失败逐个报告，不能只看18项数据测试成功。
- [ ] 执行 `scripts/build-firmware.ps1`；记录是否实际编译了改动源文件。当前审计基线只是up to date，不能拿它替代修改后的编译。

## Task 1：修正量程、身份及基线兼容性

**修改：** `sensor/ed_imu_lsm6ds3.c` 的start/stop、cfg设置、baseline加载；`sensor/ed_sensor.h`。
**新增：** `common/include/ed_imu_profile.h`、`common/src/ed_imu_profile.c`、`tests/c/test_imu_profile.c`。

先记录WHO_AM_I和BOM型号。代码中的ID=0x6A符合TR-C候选，原始LSM6DS3官方ID为0x69；0x6A不是芯片型号唯一标识。SDK已有 `projects/Edgi_Talk_M55_Driver_All/packages/lsm6ds3tr/lsm6ds3tr-c_reg.{c,h}` 可核对。确认型号后冻结驱动版本/许可，不因文件名直接猜型号。

**输出接口：** `int ed_imu_profile_from_ctrl1(uint8_t value, ed_sensor_config_t *out)`，只接受本产品208Hz/±4g，失败返回-1。

```c
/* ed_imu_profile.c: #include "ed_imu_profile.h" */
int ed_imu_profile_from_ctrl1(uint8_t value, ed_sensor_config_t *out)
{
    if (out == NULL || (value & 0xfcU) != 0x58U)
        return -1;
    out->odr_hz = 208U;
    out->range_mg = 4000U;
    out->sensitivity_g_per_adc = 0.000122f;
    return 0;
}
```

头文件包含`ed_monitor_types.h`和`stddef.h`并声明该函数。它只校验ODR/FS；不能用它声称滤波、FIFO、身份已通过。

- [ ] 新增寄存器profile回归：

```c
#include "ed_imu_profile.h"
#include <assert.h>
#include <math.h>
int main(void)
{
    ed_sensor_config_t c = {0};
    assert(ed_imu_profile_from_ctrl1(0x50U, &c) < 0);
    assert(ed_imu_profile_from_ctrl1(0x54U, &c) < 0); /* 01 = ±16g */
    assert(ed_imu_profile_from_ctrl1(0x58U, &c) == 0);
    assert(c.range_mg == 4000U && c.odr_hz == 208U);
    assert(fabsf(8197.0f * c.sensitivity_g_per_adc - 1.0f) < 0.001f);
    return 0;
}
```

将新测试编译步骤接入test-common-c.ps1，使用现有 `-Wall -Wextra -Werror -pedantic`。

- [ ] start中配置值只在完整写入、回读成功后生效。ODR/FS修改片段如下，使用文件现有`rd/wr`：

```c
static int configure_odr_and_range(ed_sensor_config_t *next)
{
    rt_uint8_t old_value, readback;
    if (rd(CTRL1, &old_value, 1U) != 0)
        return -1;
    if (wr(CTRL1, (rt_uint8_t)((old_value & 0x03U) | 0x58U)) != 0)
        return -1;
    if (rd(CTRL1, &readback, 1U) != 0)
        return -1;
    return ed_imu_profile_from_ctrl1(readback, next);
}
```

失败将running置false并发布CONFIG_ERROR；不要让旧配置仍显示running。revision由驱动自身单调递增，不能使用调用者零配置生成相同revision。axis_map初始化为0/1/2，axis_sign为+1/+1/+1；带宽单独按实机驱动核验。stop先让采集线程停止I2C，关闭ODR/FIFO，再清队列，保证无过期信号量计数。

- [ ] 新增 `edgeimu cal`：以正常采集数据做416个连续样本的只读自检，计算原始三轴均值×scale后的模长及去均值RMS。首次门限建议0.90–1.10g且动态RMS<0.02g；这是工程粗检门限，不是厂家校准精度。不能通过自动倍乘把错误隐藏起来。
- [ ] 更换profile标识（包含实际型号、208Hz、4g、滤波/换算版本），旧基线显示BASELINE_MISMATCH。当前配置来自设备配置，不能从基线文件覆盖。加载必须核对schema、CRC、有限数值、至少60窗口、key匹配；文件采用版本化编码，避免裸struct跨编译器持久化。
- [ ] baseline begin传真实mono_us；旧baseline文件保留为诊断证据，新的匹配基线另存。断电安全保存由Task 5的存储协议负责；落盘成功前不显示“保存成功”。
- [ ] build并上板回读CTRL1=0x58的ODR/FS部分、检查1g、重复start/stop、配置失败和旧基线拒绝路径。

## Task 2：把轮询命令改为常驻FIFO和低频窗口

**修改：** `sensor/ed_imu_lsm6ds3.c`、`sensor/ed_sensor.h`、`firmware/m55/SConscript`。
**新增：** `sensor/ed_imu_capture.c`、`runtime/ed_monitor_app.{c,h}`、`common/include/ed_lf_window.h`、`common/src/ed_lf_window.c`、`tests/c/test_lf_window.c`。

**接口：** 现有`ed_sensor_submit_block/read_block`保留；submit只复制完整block并唤醒consumer。read只读队列，timeout=0立即返回、正值按RTOS期限等待，不再直接读OUTXL。真正硬件生产者独占I2C。

- [ ] 建立静态block消息队列取代手写ring/count。以下init必须在启动任何生产者/消费者前完成：

```c
#define BLOCK_DEPTH 16U
static struct rt_messagequeue block_mq;
static rt_uint8_t block_pool[BLOCK_DEPTH *
    (RT_ALIGN(sizeof(ed_imu_block_t), RT_ALIGN_SIZE) + sizeof(void *))]
    __attribute__((section(".cy_socmem_data"), aligned(8)));

static int block_queue_init(void)
{
    return rt_mq_init(&block_mq, "edblock", block_pool,
                     sizeof(ed_imu_block_t), sizeof(block_pool), RT_IPC_FLAG_PRIO);
}

int ed_sensor_submit_block(const ed_imu_block_t *block)
{
    if (ed_imu_block_validate(block) != 0)
        return -RT_EINVAL;
    return rt_mq_send(&block_mq, block, sizeof(*block));
}

int ed_sensor_read_block(ed_imu_block_t *out, uint32_t timeout_ms)
{
    rt_ssize_t n;
    if (out == RT_NULL)
        return -RT_EINVAL;
    n = rt_mq_recv(&block_mq, out, sizeof(*out),
                  timeout_ms == 0U ? RT_WAITING_NO :
                  rt_tick_from_millisecond(timeout_ms));
    return n == sizeof(*out) ? 0 : (n < 0 ? (int)n : -RT_ERROR);
}
```

当前BSP的rt_mq_recv返回消息长度，故判断sizeof；移植其他RT-Thread版本时复核此契约。以上是队列主体，running/start/stop由capture控制协议保证，不能让shell在capture运行时直接重置队列。满队列由capture累计samples_dropped_known并在下一块标GAP；consumer不能把丢数据当作零信号。

- [ ] 已确认TR-C时使用SDK ST驱动配置FIFO，无陀螺、无额外FIFO源。水位8个XYZ样本=24个16位word，不是8word。FIFO模式、batch、ODR核心调用为：

```c
static int configure_fifo(const stmdev_ctx_t *ctx)
{
    if (lsm6ds3tr_c_fifo_mode_set(ctx, LSM6DS3TR_C_BYPASS_MODE) != 0)
        return -1;
    if (lsm6ds3tr_c_fifo_gy_batch_set(ctx, LSM6DS3TR_C_FIFO_GY_DISABLE) != 0)
        return -1;
    if (lsm6ds3tr_c_fifo_xl_batch_set(ctx, LSM6DS3TR_C_FIFO_XL_NO_DEC) != 0)
        return -1;
    if (lsm6ds3tr_c_fifo_watermark_set(ctx, 24U) != 0)
        return -1;
    if (lsm6ds3tr_c_fifo_data_rate_set(ctx, LSM6DS3TR_C_FIFO_208Hz) != 0)
        return -1;
    return lsm6ds3tr_c_fifo_mode_set(ctx, LSM6DS3TR_C_STREAM_MODE);
}
```

`stmdev_ctx_t`回调接现有RT-I2C读写，超时限制10ms。仅这几条调用还不是完整芯片初始化；在已确认器件、BDU/IF_INC、陀螺关闭、滤波校验后执行，并用对应get函数核对结果。需要外部引脚的IRQ不猜pin；没有已确认IRQ时采用规格允许的每10ms查FIFO。

- [ ] capture每次读FIFO level/overrun/pattern，按完整XYZ顺序出最多32点的block，余量继续排空。pattern非X开头或overrun时重同步并显式GAP；未知丢样数量计unknown_gap_count。FIFO读取是硬件采样，10ms轮询只决定搬运时机。
- [ ] monotonic clock统一为64位扩展tick，不能把32位毫秒值直接cast64就声称不会回绕。以FIFO样本序号和ODR恢复时间轴，时间估计标CLOCK_ESTIMATED；10秒统计measured_hz，不能直接回填nominal=208。
- [ ] 新窗口逐一消费block的全部count项；保存原始ADC用于饱和/证据，换算g后做低频特征。窗口接口定义：

```c
typedef struct {
    float xyz[208][3];
    uint16_t count;
} ed_lf_window_t;
void ed_lf_window_init(ed_lf_window_t *window);
/* 1=得到连续208点，0=尚未就绪，负值=无效输入。 */
int ed_lf_window_push(ed_lf_window_t *window, const float xyz[3]);
/* 处理完窗口后保留后104点，开始下一次0.5秒hop。 */
void ed_lf_window_advance(ed_lf_window_t *window);
```

advance实现关键语句为`memmove(w->xyz, w->xyz + 104, 104 * sizeof(w->xyz[0])); w->count=104;`。block序号或config_revision不连续时先init并发布GAPPED，再重新累计208点；禁止拼接缺口两侧的数据成有效窗口。

- [ ] 在test_lf_window.c验证同一416点序列分别按1点、8点、32点分块，输出的3个窗口及LF features逐项一致；207点无结果，208点首结果，312/416点下一结果；中间插GAP后必须重新累计208点。使用10Hz正弦，检查RMS接近幅值/√2及频带能量。
- [ ] monitor独占quality/baseline/alarm，调用`ed_lf_features_compute(...,208,208,...)`。quality填入真实last_sample、gap、饱和数、总线失败起点；只有VALID且匹配baseline才生成有效deviation。其他状态deviation=NAN、diagnosis=NO_BASELINE/MISMATCH/UNAVAILABLE，网络编码为null，不是NaN字符串或健康0分。
- [ ] `edgeimu sample`改成请求/读取最近窗口；不得抢走monitor队列的样本。`edgeimu baseline start/status/commit`改投递monitor命令。证据push由monitor串行调用，文件编码写入移交低优先级存储线程，不调用同步poll写Flash。
- [ ] build；上电不发送sample命令观察连续采样，shell响应、断总线、FIFO溢出、队列满、stop/start都应有明确状态和计数。

## Task 3：修正报警核心并统一live事件来源

**修改：** `common/src/ed_alarm.c`、`common/include/ed_alarm.h`、`tests/c/test_monitor_core.c`、`runtime/edgedoctor_app.c`、Task 2的`ed_monitor_app.c`。

- [ ] 先将以下用例加入现有test_monitor_core.c，利用已有snapshot/tick/accept_store辅助函数，并在main调用：

```c
static void alarm_does_not_recover_above_recovery_threshold(void)
{
    ed_alarm_t c;
    ed_snapshot_t s = snapshot();
    ed_event_t event;
    char id[ED_EVENT_ID_MAX];
    assert(ed_alarm_init(&c, "dev", "boot", 7, 1, accept_store, NULL) == 0);
    s.baseline_deviation = 11.0f;
    tick(&c, &s, 3);
    strcpy(id, stored.event_id);
    s.baseline_deviation = 9.0f;
    tick(&c, &s, 20);
    assert(ed_alarm_get_event(&c, id, &event) == 0);
    assert(event.condition == ED_CONDITION_ACTIVE);
}
```

- [ ] 修改step恢复分支，采用规格阈值语义并为边界=3加测试：

```c
if (s != NULL && s->event.condition == ED_CONDITION_ACTIVE) {
    if (x->baseline_deviation <= c->policy.recovery_deviation) {
        if (s->recovery_count < c->policy.recovery_outputs)
            ++s->recovery_count;
        if (s->recovery_count >= c->policy.recovery_outputs)
            return ed_alarm_condition(c, &x->key, x->source,
                                      ED_EVENT_MECHANICAL, 0,
                                      s->event.severity, now);
    } else {
        s->recovery_count = 0U;
    }
}
```

- [ ] 完成warning/alarm分别连续3次触发和WARNING→ALARM升级，回落但高于3保持活动；无效quality/idle/无匹配baseline不能创建机械事件或推动恢复，清零连续计数。传感器离线应生成SENSOR事件，保留机械事件历史。
- [ ] 修正pending ACK、失败回滚、槽回收和完整身份匹配：存储submit非零时报告STORAGE_ERROR并清ack_pending；匹配当前boot/key/revision；已恢复但未确认仍可ACK。重复command_id应幂等返回，只有“已恢复+已确认+持久完成”的槽可复用。
- [ ] 用现有`ed_alarm_init/step/ack/complete/get_event`统一处理live。删除app_state.live_event/live_active这个独立状态来源，旧app_live_condition/ack_event过渡接口只投递命令，不直接改共享事件。
- [ ] monitor命令新增固定消息：

```c
typedef enum {
    ED_MON_ACK, ED_MON_BASELINE_START, ED_MON_BASELINE_COMMIT,
    ED_MON_CAPTURE_START, ED_MON_CAPTURE_STOP, ED_MON_STORAGE_DONE
} ed_monitor_command_kind_t;
typedef struct {
    ed_monitor_command_kind_t kind;
    ed_ack_request_t ack;
    uint32_t durable_revision;
    int storage_status;
    uint8_t allow_idle;
} ed_monitor_command_t;
int ed_monitor_post(const ed_monitor_command_t *command); /* 复制，满队列返回BUSY */
int ed_monitor_read_snapshot(ed_snapshot_t *out);          /* 复制快照 */
```

ACK接收只返回PENDING；Task 5存储完成消息抵达后调用complete，再更新GUI和发command-results，不能立即回APPLIED。固定消息容量和入队失败可见；不在UI事件回调里等待Flash。
- [ ] build；测试warning/升级/恢复、异常质量、满8槽、存储满/失败、重复ACK、过期revision、重启身份、已恢复未确认状态；回放/demo不得创建正式live事件。

## Task 4：线程安全UI、真实数据绑定、与XiaoZhi解耦

**修改：** `ui/edgedoctor_ui.c/.h`、BSP应用main/SConscript。
**新增：** `ui/ed_ui_commands.{c,h}`、`ui/ed_lvgl_owner.c`、`ui/ed_xiaozhi_ui_bridge.c`。

- [ ] UI命令队列用Task 2同样的静态mq模式，声明如下。UI线程消费后修改普通ui私有字段，移除跨线程volatile读写：

```c
typedef enum { ED_UI_TAB, ED_UI_DEMO, ED_UI_DEMO_ALARM, ED_UI_DEMO_ACK } ed_ui_kind_t;
typedef struct { ed_ui_kind_t kind; uint8_t value; } ed_ui_command_t;
int ed_ui_post(const ed_ui_command_t *command);
```

切页接受0..5；demo值接受0/1；满队列返回错误，shell打印“未受理”。UI info从一致状态快照读取，不直接读正在更新的ui对象。真实报警确认统一走`ed_monitor_post(ED_MON_ACK)`，携带屏幕对应event_id/revision，不能只改ui.alarm_acknowledged。

- [ ] 快照采用线程间mutex复制或每读者独立固定消息队列。先选mutex复制的最小方案：monitor/replay在锁外算完，锁内只复制小快照，UI用RT_WAITING_NO尝试读取、忙时保留上一帧。所有读写双方必须遵守同一锁。采集线程不获取这把锁；绝不锁住DSP、Flash、网络或整个LVGL渲染。
- [ ] 同样审查profiler、spectrum、result，不能只把generation改atomic而继续让普通payload并发访问。common保留纯数据操作，但明确需调用方同步；RT封装提供锁，Host线程测试使用对应锁协议。
- [ ] 保留一个LVGL线程，`ed_lvgl_owner.c`负责lv_init/tick/display/input/font初始化和handler循环，在同一线程调用edgedoctor_ui_init/process。ready信号放在EdgeDoctor界面完成后。main只启动owner和等待ready，不直接跨线程调用LVGL。
- [ ] 在应用构建选择中实现`EDGEDOCTOR_HMI`与原`XIAOZHI_HMI`二选一，不改自动生成rtconfig；EdgeDoctor分支不编译原xiaozhi_ui.c、不创建emoji全屏对象。保留字体/显示输入port等共同资源。
- [ ] 按已有xiaozhi_ui.h的公开函数签名实现bridge，把状态、语音输出、电量信息转成队列消息；保留语音调用方可链接。检查xiaozhi.cpp/speaker.cpp直接qday_show_emoji调用并改到应用桥接入口，不能留下另一条线程直接操作LVGL。原官方工程保持原选择和原实现。
- [ ] UI root使用独立screen，创建和load均在owner线程：

```c
ui.root = lv_obj_create(NULL);
/* 原有build_overview/build_signal等以ui.root为父对象构建。 */
lv_screen_load(ui.root);
```

- [ ] 六页绑定 `ed_snapshot_t`、事件快照、连接快照；live页显示g/RMS/5–40Hz频带及质量，不展示KAIST五分类置信度。回放页明确KAIST_REPLAY，demo明确DEMO；STALE时保留上次值并显示过期时间，不当作新数据。
- [ ] “建立基线”变lv_button并投递BASELINE_START；基线状态显示收集进度/质量拒绝/保存失败。“1小时/24小时”没有板端历史数据源前禁用并说明原因；维修记录/测试告警同样只在后端已接通时启用。不能先涂成蓝色假装可点。
- [ ] 恢复原XiaoZhi源码中的EdgeDoctor钩子；如另行维护其通用缺陷，充电状态用独立bool+基础状态字符串重绘，emoji未知名称显式fallback，s_img_container父对象改为已创建screen。这些改动不要误写入官方冻结基线。
- [ ] build两种UI应用配置；确认各只有一次lv_init、一条handler线程、一个活动UI树。上板触摸/串口并发切页，验证ACK对应稳定event_id，GUI不延迟采样。

## Task 5：MQTT状态机、可靠事件同步与存储线程

**修改：** `protocol/ed_mqtt_client.c/.h`、`protocol/ed_mqtt_socket.c`、`server/mqtt_ingest.py`、`tests/c/test_mqtt_contract.c`、`tests/server/test_mqtt_ingest.py`。
**新增：** `protocol/ed_mqtt_worker.c`、`logger/ed_event_journal.{c,h}`、`tests/c/test_event_journal.c`、`tests/python/test_mqtt_wire.py`。

此任务分成“协议/线程正确性”和“持久回执”两个可独立构建的提交，不能把新增后台线程当作socket竞争的解决办法。

- [ ] MQTT core只含编码和可测试状态；RT shell/worker独立文件。socket/TLS/config/connect/disconnect/poll/flush只由MQTT worker调用，shell命令投递到worker。start/stop必须等待worker退出确认后才允许再start。
- [ ] 先修TLS-only发布拒绝条件：

```c
if ((mqtt_fd < 0 && tls_session == RT_NULL) || topic == RT_NULL ||
    payload == RT_NULL || length > 512U || strlen(topic) > 120U)
    return -1;
```

仅此补丁不代表TLS验证通过；由实际握手结果记录tls_active/peer_verified，端口不能作为安全状态依据。

- [ ] handle_command把所有字符串数组清零；action、command_id必需，ACK还须event_id和严格u32 revision，evidence还须challenge。拒绝缺字段、截断、转义错误、负数/溢出revision。采用长度受限JSON解析器并以测试覆盖空格、转义；不继续使用只匹配紧凑JSON的strstr。收到结果后只投递monitor，不直接修改event。
- [ ] 用一个MQTT接收解析器拆帧并分发PUBLISH/PUBACK/SUBACK/PINGRESP：支持TCP任意分段和多包粘连、remaining-length最多4字节、容量限制、packet_id匹配。publish等待期间到达命令也必须被保存和分发，不能当错误ACK丢掉。
- [ ] poll用select或受限接收期限，空闲等待不超过100ms；disconnect区别EOF/超时/EAGAIN。keepalive到期发送PINGREQ并追踪响应期限。退避按1/2/4/8/16/30秒并加有限抖动；成功握手重置计数。
- [ ] events优先于telemetry，telemetry可按设备合并最近值。保持现有QoS1先修正确性，不将改QoS0当作必要修复。flush预算按消息数和时间双限制，所有网络等待都在worker。状态快照和队列由统一mq/锁协议保护，不能一侧关中断另一侧memmove。
- [ ] 新journal按固定版本字段编码，记录 `{schema,length,sequence,event,crc,commit_marker}`；存储线程追加并确认后，事件进入LOCAL_DURABLE。事件ID包含持久boot计数和序列；boot计数用双槽/版本CRC提交防回滚复用。文件系统sync/close能否保证断电由板端门禁验证。
- [ ] 状态定义明确区分broker与业务回执：

```c
typedef enum {
    ED_DELIVERY_LOCAL_PENDING,
    ED_DELIVERY_LOCAL_DURABLE,
    ED_DELIVERY_BROKER_ACKED,
    ED_DELIVERY_SERVER_PERSISTED
} ed_delivery_state_t;
/* publish的正返回值只转BROKER_ACKED，不调用ed_mqtt_mark_receipt。 */
```

迁移`state.persisted`为broker_acked/server_persisted两个字段；相应更新UI、脚本和协议测试。仅匹配已知device/event_id/revision、服务端status=persisted的回执能提交同步标记。重启扫描完整且CRC正确的记录，忽略未提交尾部，重发未回执revision。

- [ ] server的on_message首参由`_`改为`client`，现有event分支在ingest成功后发布真实回执：

```python
if topic.endswith("/events"):
    event = Event.model_validate(payload)
    if not _topic_allowed(topic, event.device_id):
        return
    receipt = ingest(event, os.environ.get("EDGEDOCTOR_API_TOKEN"))
    if receipt.get("status") != "persisted":
        return
    receipt_topic = topic.rsplit("/", 1)[0] + "/event_receipts"
    client.publish(receipt_topic, json.dumps(receipt), qos=1, retain=False)
```

同步将receipt字段对齐既有NET-02 schema，设备订阅该topic；重复event也须重发receipt，DB提交异常不能发送成功回执。client.publish失败允许设备按相同业务键重试，不重复通知。

- [ ] 存储接口以`ed_alarm_storage_submit_t`为入口，消息复制record及可选ack。ACK入队时冻结expected_revision对应候选revision，存储完成后消息回monitor；失败保留原事件ACK状态。容量128条，满时明确OUTBOX_FULL/存储异常，不静默覆盖未同步事件。
- [ ] 测试PUBACK后仍保留journal、receipt后才同步、receipt丢失重发幂等、断电重启、尾部截断/坏CRC、server落库失败、TLS-only发布、命令穿插PUBACK、弱网、空闲超过keepalive、并发shell stop/start。每次功能提交build。

## Task 6：基线/证据持久化、工程可复现与验收

**修改：** `sensor/ed_evidence_capture.c`、`scripts/test-board-v2.py`、`README.md`、BSP应用集成入口、`docs/verification-matrix.md`。
**新增：** `scripts/test-replay-reference.py`、`scripts/prepare-bsp.ps1`，以及固定版本的应用集成补丁/manifest。

- [ ] 把baseline保存和证据保存接Task 5存储worker，队列满/短写/Flash满返回明确结果。证据缓冲冻结时标注实际pre/post样本数和GAP；不足1040点前史不能输出complete=true。网络request与capture_push由monitor串行处理。
- [ ] 以官方v1.4.0来源+哈希+应用commit重建独立EdgeDoctor BSP；prepare脚本接受SdkPath/AppPath/OutputPath，通过参数导入应用和受版本控制的应用层补丁，避免必须手工创建绝对symlink。不要嵌套vendor整个当前工作区，也不要为清洁目录删除官方SDK。
- [ ] 不改已经生效的 `*.o` 忽略逻辑。BSP构建结果放独立output目录并核对最终ELF/map路径；如要裁剪wakeword，只排除对应构建组，保留DSP依赖的CMSIS和V2所需语音功能，两个配置分别build并检查符号。
- [ ] README区分live/replay/demo，写清上电自动采集、无匹配基线只显示物理量、建立基线/确认事件的操作，以及尚未通过的门禁。BSP入口增加应用说明链接，避免用户按小智配网说明误认为监测已经运行。
- [ ] 把test-board-v2.py从“每次发sample制造数据”改为只发一次start，后续只读info/snapshot。PASS必须同时满足采样计数持续增长、测得频率在冻结容差内、无无声丢样、基线/质量符合场景、网络同步计数进展；断网专项应检查本地采样继续且journal不丢。
- [ ] 为脚本判断函数增加合成输入回归：只有一条sample/只有connected=0/quality始终WARMUP/样本序号不增长/中途串口中断/运行时间不足均不得PASS_STABILITY。这是对验收器的测试，不是伪造板端证据。
- [ ] 新回放脚本接受port、edgp路径和reference-json，校验输入SHA256/模型hash，执行edgeai replay并按结束状态和帧数有界等待；检查预测、Q15分数、掉帧及CSV结果，与冻结参考比较。传感器live输出不参与KAIST指标。
- [ ] 整机验收至少覆盖：开机自主采样、1g、0.5秒窗口输出、I2C中断、满FIFO、基线不匹配、断网补传、Flash失败、报警触发/恢复/确认、触屏并发、原XiaoZhi回归。8小时结果只在完整运行后标PASS，取消/中断保存partial并标INCOMPLETE。

## 执行验证命令与交付要求

从 `D:/CODEX SPACE/infei/xiaozhi` 执行：

```powershell
& .\EdgeDoctor-RT\scripts\test.ps1
& .\EdgeDoctor-RT\scripts\build-firmware.ps1
git diff --check
git diff --stat
```

每项交付应写明变更文件、行为、Host结果、firmware编译结果和实际完成的板端场景。Host通过不代替硬件采样；编译通过不代替GUI/协议互操作；旧的串口轮询记录不代替上电自主监测。先完成Task 0–3作为首个可测里程碑，再做Task 4–6。
