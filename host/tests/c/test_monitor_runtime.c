#include <assert.h>
#include <io.h>
#define fsync _commit
#include "../../firmware/m55/runtime/ed_monitor_app.c"
#include "../../firmware/m55/sensor/ed_imu_lsm6ds3.c"

jmp_buf ed_test_idle;
static int bus_available;
static struct rt_i2c_bus_device bus;
static uint8_t registers[256];
rt_device_t rt_device_find(const char *name) { (void)name; return bus_available ? &bus : NULL; }
int rt_device_control(rt_device_t device,int cmd,void *value) { (void)device;(void)cmd;(void)value;return 0; }
int rt_i2c_transfer(struct rt_i2c_bus_device *device, struct rt_i2c_msg *msg, unsigned count)
{
    (void)device;
    if (count==1) registers[msg[0].buf[0]]=msg[0].buf[1];
    else memcpy(msg[1].buf,registers+msg[0].buf[0],msg[1].len);
    return (int)count;
}
int ed_event_journal_init(const char *path) { (void)path;return 0; }
int ed_event_journal_submit(const ed_event_t *event) { (void)event;return 0; }
int ed_event_journal_take_committed(ed_event_t *event) { (void)event;return -1; }
int ed_event_journal_replay(ed_event_journal_replay_fn fn) { (void)fn;return 0; }
uint64_t ed_event_journal_boot_counter(void) { return 1; }
int ed_mqtt_configured_device(char *out,size_t cap) { (void)out;(void)cap;return -1; }
int ed_mqtt_enqueue_revision(ed_event_t *event) { (void)event;return 0; }
int ed_mqtt_enqueue_command_result(const ed_command_result_t *r) { (void)r;return 0; }
/* Record what the monitor reports instead of discarding it: the idle path must
 * still produce telemetry, and it must not invent an anomaly score for it. */
static ed_mqtt_telemetry_t last_telemetry;
static int telemetry_calls;
int ed_mqtt_enqueue_telemetry(const ed_mqtt_telemetry_t *t)
{
    if (t) { last_telemetry = *t; telemetry_calls++; }
    return 0;
}
void ed_evidence_capture_push(int16_t x,int16_t y,int16_t z) { (void)x;(void)y;(void)z; }
void ed_evidence_capture_mark_gap(void) {}

static void interrupted_window(void)
{
    monitor.alarm.candidate_valid=1; monitor.alarm.candidate_count=2;
    monitor.alarm.slots[0].used=1; monitor.alarm.slots[0].recovery_count=19;
    monitor.window.count=ED_LF_WINDOW_SAMPLES;
    monitor.saturated_samples=2;
    monitor_process_window(1000000);
    assert(monitor.snapshot.quality==ED_QUALITY_SATURATED);
    assert(monitor.alarm.candidate_count==0);
    assert(monitor.alarm.slots[0].recovery_count==0);
    monitor.alarm.candidate_count=2; monitor.alarm.candidate_valid=1;
    monitor.alarm.slots[0].recovery_count=19;
    monitor.no_block_since_us=1000000;
    monitor_update_no_block(1300000);
    assert(monitor.snapshot.quality==ED_QUALITY_STALE);
    assert(monitor.alarm.candidate_count==0 && monitor.alarm.slots[0].recovery_count==0);
    monitor.alarm.candidate_valid=1; monitor.alarm.candidate_count=2;
    monitor.alarm.slots[0].recovery_count=19;
    monitor.window.count=ED_LF_WINDOW_SAMPLES;
    monitor.saturated_samples=0;
    monitor.quality.idle_tracking=1; monitor.quality.idle_since_us=1;
    monitor_process_window(12000000);
    assert(monitor.snapshot.idle && monitor.snapshot.diagnosis==ED_DIAG_UNAVAILABLE);
    assert(monitor.alarm.candidate_count==0 && monitor.alarm.slots[0].recovery_count==0);
    /* 静置窗口仍在采样，必须照常上报遥测：服务端用遥测回执判定在线，
     * 之前"没有偏离度就不发"会把一台正在采样的设备在控制台上显示为过期。
     * 同时它不能被塞一个 0 分：0 是一个真实测量值，会把"没有结论"伪装成"结论正常"。 */
    assert(telemetry_calls > 0);
    assert(last_telemetry.idle == 1u);
    assert(last_telemetry.deviation_valid == 0u);
    assert(last_telemetry.alarm == 0u);
    assert(last_telemetry.rms_vector_g == monitor.features.values[3]);
}
static void sensor_retry(void)
{
    char *start[]={"edgeimu","start"};
    char *stop[]={"edgeimu","stop"};
    assert(ed_monitor_app_is_running());
    bus_available=0;
    assert(ed_sensor_start(&monitor.sensor_config)<0);
    bus_available=1; registers[ED_IMU_WHO_AM_I]=ED_IMU_DEVICE_ID;
    assert(edgeimu(2,start)==0);
    monitor_process_commands();
    assert(imu_running);
    imu_thread_ready=RT_FALSE; /* fake scheduler has returned from capture */
    assert(edgeimu(2,stop)==0);
    monitor_process_commands();
    assert(!imu_running);
    assert(edgeimu(2,start)==0);
    monitor_process_commands();
    assert(imu_running);
}
int main(int argc,char **argv)
{
    assert(ed_monitor_app_init()==0);
    if (argc>1 && strcmp(argv[1],"sensor")==0) sensor_retry();
    else interrupted_window();
    puts("monitor runtime integration: passed"); return 0;
}
