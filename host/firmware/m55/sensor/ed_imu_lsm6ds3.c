#include "ed_sensor.h"

#include "ed_imu_profile.h"
#include "ed_monitor_app.h"
#include "ed_monitor_types.h"

#include <finsh.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <stdio.h>
#include <string.h>

#define ED_IMU_BUS_NAME "i2c0"
#define ED_IMU_WHO_AM_I 0x0fU
#define ED_IMU_CTRL1_XL 0x10U
#define ED_IMU_CTRL3_C 0x12U
#define ED_IMU_FIFO_CTRL1 0x06U
#define ED_IMU_FIFO_CTRL2 0x07U
#define ED_IMU_FIFO_CTRL3 0x08U
#define ED_IMU_FIFO_CTRL4 0x09U
#define ED_IMU_FIFO_CTRL5 0x0aU
#define ED_IMU_FIFO_STATUS1 0x3aU
#define ED_IMU_FIFO_STATUS2 0x3bU
#define ED_IMU_FIFO_STATUS3 0x3cU
#define ED_IMU_FIFO_DATA_OUT_L 0x3eU
#define ED_IMU_OUTX_L_XL 0x28U
#define ED_IMU_DEVICE_ID 0x6aU
#define ED_IMU_QUEUE_DEPTH 64U
#define ED_IMU_FIFO_WATERMARK 24U /* eight XYZ samples: three 16-bit words each */
#define ED_IMU_FIFO_CHUNK 32U /* drain enough data to keep up with 208 Hz */
#define ED_IMU_DIRECT_CHUNK 8U
#define ED_IMU_CAPTURE_STACK 2048U
#define ED_IMU_CAPTURE_PRIORITY 25U

static struct rt_i2c_bus_device *imu_bus;
static rt_uint16_t imu_address;
static ed_sensor_config_t imu_config;
static ed_sensor_stats_t imu_stats;
static rt_bool_t imu_running;
static rt_bool_t imu_thread_ready;
static rt_bool_t imu_queue_initialized;
static rt_bool_t imu_gap_pending;
static rt_bool_t imu_overrun_latched;
static rt_bool_t imu_direct_fallback;
static rt_bool_t imu_wedged;
static uint32_t imu_config_revision;
static struct rt_messagequeue imu_queue;
static rt_uint8_t imu_queue_pool[ED_IMU_QUEUE_DEPTH * sizeof(ed_imu_block_t)]
    __attribute__((aligned(8)));
static struct rt_thread imu_thread;
static rt_uint8_t imu_thread_stack[ED_IMU_CAPTURE_STACK] __attribute__((aligned(8)));
static uint32_t imu_last_tick;
static uint64_t imu_tick_epoch;
static rt_bool_t imu_time_initialized;
static uint64_t imu_rate_start_us;
static uint64_t imu_rate_samples;

static void imu_time_init(void)
{
    rt_base_t level;
    uint32_t tick;
    level = rt_hw_interrupt_disable();
    tick = (uint32_t)rt_tick_get();
    if (!imu_time_initialized)
    {
        imu_tick_epoch = 0U;
        imu_time_initialized = RT_TRUE;
    }
    else if (tick < imu_last_tick)
        imu_tick_epoch += (1ULL << 32);
    imu_last_tick = tick;
    imu_rate_start_us = 0U;
    imu_rate_samples = 0U;
    rt_hw_interrupt_enable(level);
}

uint64_t ed_sensor_now_us(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t tick = (uint32_t)rt_tick_get();
    if (!imu_time_initialized)
    {
        imu_last_tick = tick;
        imu_tick_epoch = 0U;
        imu_time_initialized = RT_TRUE;
    }
    if (tick < imu_last_tick)
        imu_tick_epoch += (1ULL << 32);
    imu_last_tick = tick;
    {
        uint64_t now_us = ((imu_tick_epoch + tick) * 1000000ULL) /
                          RT_TICK_PER_SECOND;
        rt_hw_interrupt_enable(level);
        return now_us;
    }
}

static int imu_read_register(rt_uint8_t reg, rt_uint8_t *data, rt_uint16_t length)
{
    struct rt_i2c_msg messages[2];

    if ((imu_bus == RT_NULL) || (data == RT_NULL) || (length == 0U))
        return -1;
    messages[0] = (struct rt_i2c_msg){imu_address, RT_I2C_WR, 1U, &reg};
    messages[1] = (struct rt_i2c_msg){imu_address, RT_I2C_RD, length, data};
    return rt_i2c_transfer(imu_bus, messages, 2U) == 2 ? 0 : -1;
}

static int imu_write_register(rt_uint8_t reg, rt_uint8_t value)
{
    struct rt_i2c_msg message;
    rt_uint8_t payload[2] = {reg, value};

    if (imu_bus == RT_NULL)
        return -1;
    message = (struct rt_i2c_msg){imu_address, RT_I2C_WR, 2U, payload};
    return rt_i2c_transfer(imu_bus, &message, 1U) == 1 ? 0 : -1;
}

static int imu_queue_init(void)
{
    if (rt_mq_init(&imu_queue, "edimu", imu_queue_pool,
                   sizeof(ed_imu_block_t), sizeof(imu_queue_pool),
                   RT_IPC_FLAG_PRIO) != RT_EOK)
        return -1;
    return 0;
}

static void imu_capture_entry(void *parameter)
{
    (void)parameter;
    while (imu_running)
    {
        rt_uint8_t status[4];
        rt_uint8_t raw[ED_IMU_FIFO_CHUNK * 6U + 4U];
        ed_imu_block_t block;
        uint16_t level;
        uint16_t words;
        uint16_t count;
        uint16_t index;

        if (imu_direct_fallback)
        {
            /* A data-ready-gated read was measured on hardware and rejected:
             * STATUS_REG polling costs one I2C transaction per attempt, and
             * with a 1 ms scheduler tick the per-sample cycle grew past the
             * 4.8077 ms conversion period, dropping the measured rate to
             * ~194 Hz. The free-running read below keeps the rate at the
             * declared nominal. Removing the residual duplicate/skip drift
             * needs the FIFO path or a wired data-ready interrupt. */
            count = ED_IMU_DIRECT_CHUNK;
            for (index = 0U; index < count; ++index)
            {
                if (imu_read_register(ED_IMU_OUTX_L_XL, &raw[index * 6U], 6U) != 0)
                {
                    imu_stats.bus_errors++;
                    imu_gap_pending = RT_TRUE;
                    count = 0U;
                    break;
                }
                if (index + 1U < ED_IMU_DIRECT_CHUNK)
                    rt_thread_mdelay(4U);
            }
            if (count == 0U)
            {
                rt_thread_mdelay(2U);
                continue;
            }
            status[1] = 0U;
            goto samples_ready;
        }
        if (imu_read_register(ED_IMU_FIFO_STATUS1, status, sizeof(status)) != 0)
        {
            imu_stats.bus_errors++;
            imu_gap_pending = RT_TRUE;
            rt_thread_mdelay(2U);
            continue;
        }
        words = (uint16_t)status[0] | (uint16_t)((status[1] & 0x07U) << 8);
        if ((status[1] & 0x60U) != 0U)
        {
            if (!imu_overrun_latched)
            {
                if ((status[1] & 0x40U) != 0U)
                {
                    imu_stats.fifo_overruns++;
                    imu_stats.unknown_gap_count++;
                }
                imu_overrun_latched = RT_TRUE;
                imu_direct_fallback = RT_TRUE;
            }
            imu_gap_pending = RT_TRUE;
        }
        else
            imu_overrun_latched = RT_FALSE;
        if (words == 0U)
        {
            rt_thread_mdelay(2U);
            continue;
        }
        level = (uint16_t)(words / 3U);
        if (level == 0U)
        {
            rt_thread_mdelay(1U);
            continue;
        }
        count = level > ED_IMU_FIFO_CHUNK ? ED_IMU_FIFO_CHUNK : level;
        if (imu_read_register(ED_IMU_FIFO_DATA_OUT_L, raw, (rt_uint16_t)(count * 6U)) != 0)
        {
            imu_stats.bus_errors++;
            imu_gap_pending = RT_TRUE;
            rt_thread_mdelay(2U);
            continue;
        }
samples_ready:
        memset(&block, 0, sizeof(block));
        block.count = count;
        block.sample_period_ns = 1000000000U / 208U;
        block.config_revision = imu_config.config_revision;
        block.first_sample_seq = imu_stats.samples_received;
        block.first_sample_mono_us = ed_sensor_now_us();
        block.flags = imu_direct_fallback ? ED_BLOCK_CLOCK_ESTIMATED :
            (((status[1] & 0x40U) != 0U)
             ? (ED_BLOCK_FIFO_OVERRUN | ED_BLOCK_GAP) : 0U);
        if (imu_gap_pending)
            block.flags |= ED_BLOCK_GAP;
        for (index = 0U; index < count; ++index)
        {
            block.xyz[index][0] = (int16_t)((uint16_t)raw[index * 6U] |
                                           ((uint16_t)raw[index * 6U + 1U] << 8));
            block.xyz[index][1] = (int16_t)((uint16_t)raw[index * 6U + 2U] |
                                           ((uint16_t)raw[index * 6U + 3U] << 8));
            block.xyz[index][2] = (int16_t)((uint16_t)raw[index * 6U + 4U] |
                                           ((uint16_t)raw[index * 6U + 5U] << 8));
        }
        imu_stats.samples_received += count;
        imu_stats.last_sample_mono_us = block.first_sample_mono_us +
            (uint64_t)(count - 1U) * block.sample_period_ns / 1000U;
        if (imu_rate_start_us == 0U)
            imu_rate_start_us = block.first_sample_mono_us;
        imu_rate_samples += count;
        if (block.first_sample_mono_us > imu_rate_start_us &&
            block.first_sample_mono_us - imu_rate_start_us >= 10000000U)
        {
            imu_stats.measured_millihz = (uint32_t)((imu_rate_samples * 1000000000ULL) /
                (block.first_sample_mono_us - imu_rate_start_us));
            imu_rate_start_us = block.first_sample_mono_us;
            imu_rate_samples = 0U;
        }
        if (ed_sensor_submit_block(&block) != 0)
        {
            imu_stats.samples_dropped_known += count;
            imu_gap_pending = RT_TRUE;
        }
        else
            imu_gap_pending = RT_FALSE;
        /* Bound CPU occupancy while draining a full FIFO; the producer is
         * still several times faster than the 208 Hz sensor stream. */
        rt_thread_mdelay(imu_direct_fallback ? 4U : 1U);
    }
    imu_thread_ready = RT_FALSE;
}

int ed_sensor_start(const ed_sensor_config_t *config)
{
    static const rt_uint16_t addresses[] = {0x6aU, 0x6bU};
    rt_uint8_t who_am_i = 0U;
    rt_uint8_t ctrl1 = 0U;
    rt_uint8_t ctrl3 = 0U;
    rt_uint8_t fifo_ctrl1 = 0U;
    rt_uint8_t fifo_ctrl2 = 0U;
    rt_uint8_t fifo_ctrl3 = 0U;
    rt_uint8_t fifo_ctrl4 = 0U;
    rt_uint8_t fifo_ctrl5 = 0U;
    unsigned index;

    if (config == RT_NULL)
        return -1;
    if (imu_running)
        return 0;
    if (imu_thread_ready)
    {
        /* The previous capture thread never returned from an I2C transfer, so
         * its stack is still live and cannot be reused. Starting a second
         * thread over it would corrupt the runtime; a reboot is required. */
        rt_kprintf("edgeimu: capture thread is still blocked after a bus "
                   "fault; reboot the board to recover the sensor\n");
        return -2;
    }
    imu_wedged = RT_FALSE;
    imu_bus = (struct rt_i2c_bus_device *)rt_device_find(ED_IMU_BUS_NAME);
    if (imu_bus == RT_NULL)
        return -1;
    {
        rt_uint32_t bus_clock_hz = 400000U;
        /* Request fast-mode I²C when the BSP driver supports runtime clock
         * control; unsupported drivers keep their configured safe rate. */
        (void)rt_device_control((rt_device_t)imu_bus, RT_I2C_DEV_CTRL_CLK,
                                &bus_clock_hz);
    }
    for (index = 0U; index < sizeof(addresses) / sizeof(addresses[0]); ++index)
    {
        imu_address = addresses[index];
        if (imu_read_register(ED_IMU_WHO_AM_I, &who_am_i, 1U) == 0 &&
            who_am_i == ED_IMU_DEVICE_ID)
            break;
    }
    if (index == sizeof(addresses) / sizeof(addresses[0]))
    {
        imu_stats.bus_errors++;
        return -1;
    }
    /* Clear any FIFO state retained across a warm reset before programming
     * the application profile.  CTRL3_C.SW_RESET self clears. */
    if (imu_write_register(ED_IMU_CTRL3_C, 0x01U) != 0)
        return -1;
    rt_thread_mdelay(20U);
    imu_config = *config;
    imu_config.config_revision = ++imu_config_revision;
    imu_time_init();
    if (imu_write_register(ED_IMU_CTRL1_XL, 0U) != 0 ||
        imu_write_register(ED_IMU_FIFO_CTRL5, 0U) != 0)
        return -1;
    rt_thread_mdelay(5U);
    if (imu_write_register(ED_IMU_CTRL1_XL, ED_IMU_CTRL1_208HZ_4G) != 0 ||
        imu_write_register(ED_IMU_CTRL3_C, 0x44U) != 0 ||
        imu_write_register(ED_IMU_FIFO_CTRL1, (rt_uint8_t)(ED_IMU_FIFO_WATERMARK & 0xffU)) != 0 ||
        imu_write_register(ED_IMU_FIFO_CTRL2, (rt_uint8_t)((ED_IMU_FIFO_WATERMARK >> 8) & 0x07U)) != 0 ||
        imu_write_register(ED_IMU_FIFO_CTRL3, 0x01U) != 0 ||
        imu_write_register(ED_IMU_FIFO_CTRL4, 0x00U) != 0 ||
        imu_write_register(ED_IMU_FIFO_CTRL5, 0U) != 0 ||
        imu_read_register(ED_IMU_CTRL1_XL, &ctrl1, 1U) != 0 ||
        imu_read_register(ED_IMU_CTRL3_C, &ctrl3, 1U) != 0 ||
        imu_read_register(ED_IMU_FIFO_CTRL1, &fifo_ctrl1, 1U) != 0 ||
        imu_read_register(ED_IMU_FIFO_CTRL2, &fifo_ctrl2, 1U) != 0 ||
        imu_read_register(ED_IMU_FIFO_CTRL3, &fifo_ctrl3, 1U) != 0 ||
        imu_read_register(ED_IMU_FIFO_CTRL4, &fifo_ctrl4, 1U) != 0 ||
        imu_read_register(ED_IMU_FIFO_CTRL5, &fifo_ctrl5, 1U) != 0 ||
        ctrl3 != 0x44U ||
        fifo_ctrl1 != (rt_uint8_t)(ED_IMU_FIFO_WATERMARK & 0xffU) ||
        fifo_ctrl2 != (rt_uint8_t)((ED_IMU_FIFO_WATERMARK >> 8) & 0x07U) ||
        fifo_ctrl3 != 0x01U || fifo_ctrl4 != 0x00U || fifo_ctrl5 != 0x00U ||
        ed_imu_profile_from_ctrl1(ctrl1, &imu_config) != 0)
    {
        imu_stats.bus_errors++;
        return -1;
    }
    if (!imu_queue_initialized)
    {
        if (imu_queue_init() != 0)
            return -1;
        imu_queue_initialized = RT_TRUE;
    }
    else if (rt_mq_control(&imu_queue, RT_IPC_CMD_RESET, RT_NULL) != RT_EOK)
        return -1;
    imu_stats.nominal_hz = imu_config.odr_hz;
    imu_gap_pending = RT_FALSE;
    imu_overrun_latched = RT_FALSE;
    /* The E84 board's LSM6DS3 FIFO can wedge its I2C state machine when the
     * FIFO overrun bit is asserted.  Use direct output reads by default;
     * the FIFO path remains available for boards with a verified FIFO. */
    imu_direct_fallback = RT_TRUE;
    imu_running = RT_TRUE;
    if (rt_thread_init(&imu_thread, "edcap", imu_capture_entry, RT_NULL,
                       imu_thread_stack, sizeof(imu_thread_stack),
                       ED_IMU_CAPTURE_PRIORITY, 10U) != RT_EOK)
    {
        imu_running = RT_FALSE;
        return -1;
    }
    imu_thread_ready = RT_TRUE;
    if (rt_thread_startup(&imu_thread) != RT_EOK)
    {
        imu_running = RT_FALSE;
        imu_thread_ready = RT_FALSE;
        return -1;
    }
    return 0;
}

int ed_sensor_stop(void)
{
    uint32_t waited_ms = 0U;
    imu_running = RT_FALSE;
    imu_gap_pending = RT_FALSE;
    while (imu_thread_ready && waited_ms < 2000U)
    {
        rt_thread_mdelay(1U);
        waited_ms++;
    }
    if (imu_thread_ready)
    {
        /* The capture thread did not observe imu_running within the budget,
         * i.e. it is blocked inside the I2C driver. Record it as a distinct
         * condition so `edgeimu info` reports a bus fault rather than a
         * generic sensor error. */
        imu_stats.bus_errors++;
        imu_wedged = RT_TRUE;
        return -2;
    }
    (void)imu_write_register(ED_IMU_CTRL1_XL, 0U);
    (void)imu_write_register(ED_IMU_FIFO_CTRL5, 0U);
    (void)rt_mq_control(&imu_queue, RT_IPC_CMD_RESET, RT_NULL);
    return 0;
}

int ed_sensor_submit_block(const ed_imu_block_t *block)
{
    if (!imu_queue_initialized || (block == RT_NULL) ||
        ed_imu_block_validate(block) != 0)
        return -1;
    if (rt_mq_send(&imu_queue, block, sizeof(*block)) != RT_EOK)
    {
        /* A full message queue is back-pressure, not a FIFO overrun. The
         * capture loop accounts for the lost samples as a known drop, so the
         * overrun counter keeps meaning what its name says. */
        return -2;
    }
    {
        rt_base_t level = rt_hw_interrupt_disable();
        if (imu_queue.entry > imu_stats.queue_high_water)
            imu_stats.queue_high_water = imu_queue.entry;
        rt_hw_interrupt_enable(level);
    }
    return 0;
}

int ed_sensor_read_block(ed_imu_block_t *out, uint32_t timeout_ms)
{
    rt_ssize_t received;

    if (!imu_running || out == RT_NULL)
        return -1;
    received = rt_mq_recv(&imu_queue, out, sizeof(*out),
                          timeout_ms == 0U
                              ? RT_WAITING_NO
                              : rt_tick_from_millisecond(timeout_ms));
    return received == (rt_ssize_t)sizeof(*out) ? 0 : -2;
}

void ed_sensor_get_stats(ed_sensor_stats_t *out)
{
    if (out != RT_NULL)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        *out = imu_stats;
        rt_hw_interrupt_enable(level);
    }
}

static int edgeimu(int argc, char **argv)
{
    ed_sensor_config_t config;
    ed_sensor_stats_t stats;
    ed_snapshot_t snapshot;
    uint32_t windows;
    uint8_t active;
    uint8_t valid;
    uint8_t timed_out;
    uint32_t cal_samples;
    uint8_t cal_active;
    uint8_t cal_complete;
    float cal_gravity;
    float cal_dynamic;
    rt_uint8_t who_am_i;
    rt_uint8_t control_regs[6];
    rt_uint8_t status_regs[6];

    if ((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        /* The autonomous monitor owns sensor startup.  Do not perform a
         * potentially blocking I2C probe in the shell thread. */
        if (ed_monitor_app_is_running())
            return ed_monitor_app_sensor_request(1U);
        memset(&config, 0, sizeof(config));
        strcpy(config.profile_id, "imu_lf_208_v1");
        config.sensitivity_g_per_adc = 0.000122f;
        return ed_sensor_start(&config);
    }
    if ((argc >= 2) && (strcmp(argv[1], "stop") == 0))
        return ed_monitor_app_is_running() ? ed_monitor_app_sensor_request(0U) : ed_sensor_stop();
    if ((argc >= 3) && (strcmp(argv[1], "baseline") == 0) &&
        (strcmp(argv[2], "start") == 0))
    {
        return ed_monitor_app_baseline_start(argc >= 4 &&
                                             strcmp(argv[3], "idle") == 0);
    }
    if ((argc >= 3) && (strcmp(argv[1], "baseline") == 0) &&
        (strcmp(argv[2], "status") == 0))
    {
        if (ed_monitor_app_baseline_status(&windows, &active, &valid,
                                           &timed_out) != 0)
            return -1;
        rt_kprintf("edgeimu: baseline active=%u windows=%u valid=%u timed_out=%u\n",
                   active, (unsigned)windows, valid, timed_out);
        return 0;
    }
    if ((argc >= 3) && (strcmp(argv[1], "baseline") == 0) &&
        (strcmp(argv[2], "commit") == 0))
        return ed_monitor_app_baseline_commit();
    if ((argc >= 3) && (strcmp(argv[1], "baseline") == 0) &&
        (strcmp(argv[2], "load") == 0))
        return ed_monitor_app_baseline_load();
    if ((argc >= 3) && (strcmp(argv[1], "cal") == 0) &&
        (strcmp(argv[2], "start") == 0))
        return ed_monitor_app_calibration_start();
    if ((argc >= 3) && (strcmp(argv[1], "cal") == 0) &&
        (strcmp(argv[2], "selftest") == 0))
    {
        int result;
        result = ed_monitor_app_calibration_status(&cal_samples, &cal_active,
                                                   &cal_complete, &cal_gravity,
                                                   &cal_dynamic);
        if (result != 0)
            return -1;
        if (cal_active != 0U)
        {
            rt_kprintf("edgeimu: cal selftest running samples=%u/416\n",
                       (unsigned)cal_samples);
            return 0;
        }
        if (cal_complete == 0U)
        {
            result = ed_monitor_app_calibration_start();
            rt_kprintf("edgeimu: cal selftest started status=%d\n", result);
            return result;
        }
        result = (cal_gravity >= 0.85f && cal_gravity <= 1.15f &&
                  cal_dynamic <= 0.05f) ? 0 : -1;
        rt_kprintf("edgeimu: cal selftest %s samples=%u gravity=%.4f dynamic_rms=%.4f\n",
                   result == 0 ? "PASS" : "FAIL", (unsigned)cal_samples,
                   (double)cal_gravity, (double)cal_dynamic);
        return result;
    }
    if ((argc >= 2) && (strcmp(argv[1], "regs") == 0))
    {
        if (imu_read_register(ED_IMU_WHO_AM_I, &who_am_i, 1U) != 0 ||
            imu_read_register(ED_IMU_FIFO_CTRL1, control_regs,
                              sizeof(control_regs)) != 0 ||
            imu_read_register(ED_IMU_FIFO_STATUS1, status_regs,
                              sizeof(status_regs)) != 0)
            return -1;
        rt_kprintf("edgeimu: who=0x%02x fifo_ctrl=%02x %02x %02x %02x %02x %02x "
                   "status=%02x %02x %02x %02x %02x %02x\n",
                   who_am_i, control_regs[0], control_regs[1], control_regs[2],
                   control_regs[3], control_regs[4], control_regs[5],
                   status_regs[0], status_regs[1], status_regs[2], status_regs[3],
                   status_regs[4], status_regs[5]);
        return 0;
    }
    if ((argc >= 2) && (strcmp(argv[1], "cal") == 0))
    {
        if (ed_monitor_app_calibration_status(&cal_samples, &cal_active,
                                              &cal_complete, &cal_gravity,
                                              &cal_dynamic) != 0)
            return -1;
        rt_kprintf("edgeimu: cal samples=%u active=%u complete=%u gravity=%.4fg dynamic_rms=%.4fg\n",
                   (unsigned)cal_samples, cal_active, cal_complete,
                   (double)cal_gravity, (double)cal_dynamic);
        return 0;
    }
    if ((argc >= 2) && (strcmp(argv[1], "sample") == 0))
    {
        int status = ed_monitor_app_read_snapshot(&snapshot);
        if (status <= 0)
            return -1;
        rt_kprintf("edgeimu: quality=%u rms_x=%.6f rms_y=%.6f rms_z=%.6f "
                   "vector_rms=%.6f baseline=%u deviation=%.6f\n",
                   (unsigned)snapshot.quality,
                   (double)snapshot.features.values[0],
                   (double)snapshot.features.values[1],
                   (double)snapshot.features.values[2],
                   (double)snapshot.features.values[3],
                   (unsigned)snapshot.baseline_version,
                   (double)snapshot.baseline_deviation);
        return 0;
    }
    ed_sensor_get_stats(&stats);
    rt_kprintf("edgeimu: running=%u nominal_hz=%u measured_millihz=%u samples=%llu drops=%llu gaps=%llu bus_errors=%llu queue_high=%u\n",
               (unsigned)imu_running, (unsigned)stats.nominal_hz,
               (unsigned)stats.measured_millihz,
               (unsigned long long)stats.samples_received,
               (unsigned long long)stats.samples_dropped_known,
               (unsigned long long)stats.unknown_gap_count,
               (unsigned long long)stats.bus_errors,
               (unsigned)stats.queue_high_water);
    if (imu_wedged)
        rt_kprintf("edgeimu: WEDGED capture thread blocked in I2C; "
                   "reboot required before the sensor can restart\n");
    return 0;
}
MSH_CMD_EXPORT(edgeimu, LSM6DS3 IMU monitor and baseline control);
