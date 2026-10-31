#include "edgedoctor_ui.h"
#include "xiaozhi_ui.h"

#include <lvgl.h>
#include <rtthread.h>
#include <stdbool.h>
#include <string.h>

#define ED_UI_OWNER_STACK_BYTES 10240U
#define ED_UI_OWNER_QUEUE_DEPTH 16U
#define ED_UI_OWNER_TEXT_BYTES 96U

typedef enum {
    ED_UI_OWNER_STATUS = 1,
    ED_UI_OWNER_OUTPUT,
    ED_UI_OWNER_EMOJI,
    ED_UI_OWNER_AP,
    ED_UI_OWNER_CONNECTING,
    ED_UI_OWNER_BATTERY,
    ED_UI_OWNER_CHARGING,
    ED_UI_OWNER_CLEAR
} ed_ui_owner_command_kind_t;

typedef struct {
    ed_ui_owner_command_kind_t kind;
    int value;
    char text[ED_UI_OWNER_TEXT_BYTES];
} ed_ui_owner_command_t;

static struct rt_semaphore owner_ready;
static struct rt_messagequeue owner_queue;
static rt_uint8_t owner_queue_pool[ED_UI_OWNER_QUEUE_DEPTH *
                                   sizeof(ed_ui_owner_command_t)]
    __attribute__((aligned(8)));
static struct rt_thread owner_thread;
static rt_uint8_t owner_stack[ED_UI_OWNER_STACK_BYTES]
    __attribute__((aligned(8)));
static uint8_t owner_initialized;

extern void lv_port_disp_init(void);
extern void lv_port_indev_init(void);

static void owner_copy_text(char *destination, size_t capacity,
                            const char *source)
{
    size_t length;
    if (destination == RT_NULL || capacity == 0U)
        return;
    length = strlen(source == RT_NULL ? "" : source);
    if (length >= capacity)
        length = capacity - 1U;
    memcpy(destination, source == RT_NULL ? "" : source, length);
    destination[length] = '\0';
}

static int owner_post(ed_ui_owner_command_kind_t kind, int value,
                      const char *text)
{
    ed_ui_owner_command_t command;
    if (!owner_initialized)
        return -1;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    command.value = value;
    owner_copy_text(command.text, sizeof(command.text), text);
    return rt_mq_send(&owner_queue, &command, sizeof(command)) == RT_EOK ? 0 : -2;
}

static void owner_consume_commands(void)
{
    ed_ui_owner_command_t command;
    while (rt_mq_recv(&owner_queue, &command, sizeof(command), RT_WAITING_NO) ==
           (rt_ssize_t)sizeof(command))
    {
        /* EdgeDoctor owns the screen. Voice/AP/battery updates are retained in
         * this bounded bridge so callers never touch LVGL from their threads;
         * the industrial HMI exposes its own connection and health state. */
        (void)command;
    }
}

static void owner_entry(void *parameter)
{
    (void)parameter;
    lv_init();
    lv_tick_set_cb(&rt_tick_get_millisecond);
    lv_port_disp_init();
    lv_port_indev_init();
    edgedoctor_ui_init(RT_NULL, RT_NULL, RT_NULL);
    rt_sem_release(&owner_ready);
    while (1)
    {
        uint32_t period_ms;
        owner_consume_commands();
        edgedoctor_ui_process();
        period_ms = lv_task_handler();
        if (period_ms == LV_NO_TIMER_READY || period_ms > 100U)
            period_ms = 20U;
        rt_thread_mdelay(period_ms);
    }
}

void xiaozhi_ui_init(void)
{
    if (owner_initialized)
        return;
    rt_sem_init(&owner_ready, "edui", 0, RT_IPC_FLAG_PRIO);
    if (rt_mq_init(&owner_queue, "eduiq", owner_queue_pool,
                   sizeof(ed_ui_owner_command_t), sizeof(owner_queue_pool),
                   RT_IPC_FLAG_FIFO) != RT_EOK)
        return;
    if (rt_thread_init(&owner_thread, "edui", owner_entry, RT_NULL,
                       owner_stack, sizeof(owner_stack), 25U, 10U) != RT_EOK)
        return;
    owner_initialized = 1U;
    if (rt_thread_startup(&owner_thread) != RT_EOK)
        owner_initialized = 0U;
}

rt_err_t xiaozhi_ui_wait_ready(rt_int32_t timeout)
{
    return rt_sem_take(&owner_ready, timeout);
}

void xiaozhi_ui_set_extension(xiaozhi_ui_extension_init_fn init,
                              xiaozhi_ui_extension_process_fn process)
{
    (void)init;
    (void)process;
}

void xiaozhi_ui_set_status(const char *status)
{
    (void)owner_post(ED_UI_OWNER_STATUS, 0, status);
}

void xiaozhi_ui_set_output(const char *output)
{
    (void)owner_post(ED_UI_OWNER_OUTPUT, 0, output);
}

void xiaozhi_ui_set_emoji(const char *emoji)
{
    (void)owner_post(ED_UI_OWNER_EMOJI, 0, emoji);
}

void xiaozhi_ui_set_adc(const char *adc_str)
{
    (void)owner_post(ED_UI_OWNER_STATUS, 0, adc_str);
}

void xiaozhi_ui_clear_info(void)
{
    (void)owner_post(ED_UI_OWNER_CLEAR, 0, RT_NULL);
}

void xiaozhi_ui_show_ap_config(const char *ap_info)
{
    (void)owner_post(ED_UI_OWNER_AP, 0, ap_info);
}

void xiaozhi_ui_show_connecting(void)
{
    (void)owner_post(ED_UI_OWNER_CONNECTING, 0, RT_NULL);
}

void xiaozhi_ui_update_battery(int level)
{
    (void)owner_post(ED_UI_OWNER_BATTERY, level, RT_NULL);
}

void xiaozhi_ui_update_charging_status(bool is_charging)
{
    (void)owner_post(ED_UI_OWNER_CHARGING, is_charging ? 1 : 0, RT_NULL);
}

void xiaozhi_ui_update_ble_status(bool connected)
{
    (void)connected;
}

void init_ui(void) { xiaozhi_ui_init(); }
rt_err_t wait_ui_ready(rt_int32_t timeout) { return xiaozhi_ui_wait_ready(timeout); }
void clean_info(void) { xiaozhi_ui_clear_info(); }
void xiaozhi_ui_chat_status(char *string) { xiaozhi_ui_set_status(string); }
void xiaozhi_ui_chat_output(char *string) { xiaozhi_ui_set_output(string); }
void xiaozhi_ui_update_emoji(char *string) { xiaozhi_ui_set_emoji(string); }
void xiaozhi_ui_update_adc(char *string) { xiaozhi_ui_set_adc(string); }
