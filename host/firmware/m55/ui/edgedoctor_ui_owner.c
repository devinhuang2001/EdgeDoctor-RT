#include "edgedoctor_ui.h"
#include "xiaozhi_ui.h"

#include <lvgl.h>
#include <rtthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <finsh.h>

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
    ED_UI_OWNER_CLEAR,
    ED_UI_OWNER_STRESS
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
static struct rt_semaphore owner_stress_done;
static volatile int owner_stress_result;
static volatile uint32_t owner_stress_elapsed_ms;
/* Created once by the LVGL owner and retained for the screen lifetime. */
static lv_font_t *owner_font_small;
static lv_font_t *owner_font_medium;
static lv_font_t *owner_font_large;

extern void lv_port_disp_init(void);
extern void lv_port_indev_init(void);
extern const unsigned char xiaozhi_font[];
extern const int xiaozhi_font_size;

static void owner_init_fonts(void)
{
#if LV_USE_TINY_TTF
    owner_font_small = lv_tiny_ttf_create_data(xiaozhi_font, xiaozhi_font_size, 16);
    owner_font_medium = lv_tiny_ttf_create_data(xiaozhi_font, xiaozhi_font_size, 22);
    owner_font_large = lv_tiny_ttf_create_data(xiaozhi_font, xiaozhi_font_size, 28);
#endif
    if (owner_font_small == RT_NULL || owner_font_medium == RT_NULL ||
        owner_font_large == RT_NULL)
        rt_kprintf("[edui] Chinese font initialization incomplete; using available font fallback\n");
    /* Prefer another Chinese size over the Latin-only LVGL default if one
     * allocation failed. Aliases are never deleted independently. */
    if (owner_font_small == RT_NULL)
        owner_font_small = owner_font_medium != RT_NULL ? owner_font_medium : owner_font_large;
    if (owner_font_medium == RT_NULL)
        owner_font_medium = owner_font_small;
    if (owner_font_large == RT_NULL)
        owner_font_large = owner_font_medium;
}

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
        if (command.kind == ED_UI_OWNER_STRESS)
        {
            owner_stress_result = edgedoctor_ui_stress_switch((uint32_t)command.value,
                                                               (uint32_t *)&owner_stress_elapsed_ms);
            rt_sem_release(&owner_stress_done);
        }
    }
}

static void owner_entry(void *parameter)
{
    (void)parameter;
    lv_init();
    lv_tick_set_cb(&rt_tick_get_millisecond);
    lv_port_disp_init();
    lv_port_indev_init();
    owner_init_fonts();
    edgedoctor_ui_init(owner_font_small, owner_font_medium, owner_font_large);
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
    rt_sem_init(&owner_stress_done, "eduist", 0, RT_IPC_FLAG_PRIO);
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

static int edui_stress(int argc, char **argv)
{
    int count = 100;
    if ((argc < 2) || (strcmp(argv[1], "stress") != 0))
        return -1;
    if (argc == 3)
        count = atoi(argv[2]);
    if ((count <= 0) || (count > 10000) || owner_post(ED_UI_OWNER_STRESS, count, RT_NULL) != 0)
        return -1;
    if (rt_sem_take(&owner_stress_done, rt_tick_from_millisecond(15000U)) != RT_EOK)
        return -1;
    rt_kprintf("edui stress switches=%d elapsed_ms=%u result=%d\n", count,
               (unsigned int)owner_stress_elapsed_ms, owner_stress_result);
    return owner_stress_result;
}
MSH_CMD_EXPORT(edui_stress, EdgeDoctor UI owner stress: edui_stress stress [count]);

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
