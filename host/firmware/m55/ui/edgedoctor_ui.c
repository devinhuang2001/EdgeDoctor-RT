#include "edgedoctor_ui.h"

#include "edgedoctor_app.h"
#include "ed_monitor_app.h"
#include "ed_mqtt_client.h"
#include "edgedoctor_backend.h"
#include "edgedoctor_logger.h"
#include "edgedoctor_machine_image.h"

#include <finsh.h>
#include <lvgl.h>
#include <math.h>
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#if defined(RT_USING_WIFI) && !defined(ED_UI_HOST_PREVIEW)
#include <wlan_mgnt.h>
#endif

#define UI_REFRESH_MS 100U
#define UI_ALARM_BLINK_MS 500U
#define UI_CRITICAL_Q15 26214U
#define UI_TAB_COUNT 6U
#define UI_TREND_POINTS 30U

#define COLOR_CANVAS 0xF3F5F7U
#define COLOR_SURFACE 0xFFFFFFU
#define COLOR_BORDER 0xDDE2E7U
#define COLOR_TEXT 0x111820U
#define COLOR_MUTED 0x66717CU
#define COLOR_NAV 0x121920U
#define COLOR_RED 0xE32222U
#define COLOR_RED_DARK 0xB81419U
#define COLOR_GREEN 0x15A36DU
#define COLOR_BLUE 0x347FE8U
#define COLOR_AMBER 0xE58A12U
#define COLOR_TRACK 0xE8ECEFU

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *tabview;
    lv_obj_t *nav;
    lv_obj_t *nav_buttons[UI_TAB_COUNT];
    lv_obj_t *nav_labels[UI_TAB_COUNT];
    lv_obj_t *header_state[UI_TAB_COUNT];
    lv_obj_t *overview_state;
    lv_obj_t *overview_result;
    lv_obj_t *overview_detail;
    lv_obj_t *overview_health_arc;
    lv_obj_t *overview_health_value;
    lv_obj_t *overview_dsp;
    lv_obj_t *overview_model;
    lv_obj_t *overview_log;
    lv_obj_t *latency_chart;
    lv_chart_series_t *latency_series;
    lv_obj_t *audio_chart;
    lv_obj_t *vibration_chart;
    lv_chart_series_t *audio_series;
    lv_chart_series_t *vibration_series;
    lv_obj_t *signal_audio_state;
    lv_obj_t *signal_vibration_state;
    lv_obj_t *diagnosis_banner;
    lv_obj_t *alarm_icon;
    lv_obj_t *alarm_title;
    lv_obj_t *alarm_detail;
    lv_obj_t *alarm_button;
    lv_obj_t *alarm_button_label;
    lv_obj_t *diagnosis_type;
    lv_obj_t *diagnosis_confidence;
    lv_obj_t *diagnosis_anomaly;
    lv_obj_t *diagnosis_frame;
    lv_obj_t *diagnosis_bars[EDGEDOCTOR_CLASS_COUNT];
    lv_obj_t *diagnosis_values[EDGEDOCTOR_CLASS_COUNT];
    lv_obj_t *profile_latency;
    lv_obj_t *profile_runtime;
    lv_obj_t *profile_storage;
    const lv_font_t *font_small;
    const lv_font_t *font_medium;
    const lv_font_t *font_large;
    uint32_t last_refresh_ms;
    uint32_t last_spectrum_frame;
    uint32_t last_result_frame;
    uint32_t alarm_key;
    uint32_t alarm_blink_ms;
    uint32_t last_demo_ms;
    uint32_t demo_frame;
    uint8_t alarm_active;
    uint8_t alarm_acknowledged;
    uint8_t alarm_blink_on;
    uint8_t active_tab;
} edgedoctor_ui_state_t;

static edgedoctor_ui_state_t ui;
/* UI-owned bounded caches: no history IO or allocations in capture paths. */
#define UI_HISTORY_CAPACITY 1440U
static ed_monitor_ui_state_t monitor_view;
static ed_snapshot_t monitor_snapshot;
static uint8_t monitor_ready, snapshot_ready;
static lv_obj_t *event_count_label, *event_empty, *event_rows[ED_ACTIVE_EVENTS_MAX];
static lv_obj_t *event_titles[ED_ACTIVE_EVENTS_MAX], *event_meta[ED_ACTIVE_EVENTS_MAX];
static lv_obj_t *event_status[ED_ACTIVE_EVENTS_MAX], *event_filter[2];
static lv_obj_t *event_detail, *event_detail_text, *event_ack_button, *event_action;
static char selected_event[ED_EVENT_ID_MAX];
static uint32_t ack_submitted_ms;
static uint8_t ack_submitted;
static uint8_t show_all_events;
static lv_obj_t *asset_name, *asset_mount, *asset_profile, *asset_state;
static lv_obj_t *baseline_state, *baseline_info, *baseline_rms, *baseline_time;
static lv_obj_t *baseline_button, *baseline_button_label, *baseline_progress;
static lv_obj_t *baseline_bar, *baseline_details, *baseline_details_text, *baseline_view_button;
static lv_obj_t *wifi_state, *mqtt_status, *sync_status, *sync_steps, *system_details;
static lv_obj_t *demo_button_label;
static lv_obj_t *trend_chart, *trend_note, *trend_mean, *trend_peak, *trend_valid;
static lv_obj_t *trend_range[2], *trend_metric[2];
static lv_chart_series_t *trend_series, *trend_baseline;
static uint8_t trend_day, trend_deviation;
static uint32_t panel_refresh_ms;
typedef struct { uint64_t minute; float sum, peak, deviation; uint32_t baseline_version; uint16_t valid, total, deviation_valid; } ui_history_t;
static ui_history_t history[UI_HISTORY_CAPACITY]
    __attribute__((section(".cy_socmem_data"), aligned(8)));
static uint64_t history_snapshot_id;
static uint8_t history_started;
static uint32_t snapshot_received_ms;
static uint8_t demo_alarm_requested;
static uint8_t alarm_ack_requested;
static uint8_t requested_tab;
/* Production boots on real snapshots; demo data is opt-in via `edgeui demo on`. */
static uint8_t demo_data_enabled;

typedef enum {
    ED_UI_COMMAND_TAB = 0,
    ED_UI_COMMAND_DEMO,
    ED_UI_COMMAND_ALARM,
    ED_UI_COMMAND_ACK,
    ED_UI_COMMAND_BASELINE
} ed_ui_command_kind_t;

typedef struct {
    ed_ui_command_kind_t kind;
    uint8_t value;
} ed_ui_command_t;

static struct rt_messagequeue ui_command_queue;
static rt_uint8_t ui_command_pool[8U * sizeof(ed_ui_command_t)]
    __attribute__((aligned(8)));
static uint8_t ui_command_queue_ready;

static void set_header_state(const char *text, uint32_t color);
static void refresh_alarm(const edgedoctor_result_t *result, uint8_t have_result);
static void paint_alarm_banner(void);
static void set_alarm(uint8_t active, uint32_t key, const char *detail);

static int post_ui_command(ed_ui_command_kind_t kind, uint8_t value)
{
    ed_ui_command_t command = {kind, value};
    if (!ui_command_queue_ready ||
        rt_mq_send(&ui_command_queue, &command, sizeof(command)) != RT_EOK)
        return -1;
    return 0;
}

static void consume_ui_commands(void)
{
    ed_ui_command_t command;
    while (rt_mq_recv(&ui_command_queue, &command, sizeof(command),
                      RT_WAITING_NO) == (rt_ssize_t)sizeof(command))
    {
        switch (command.kind)
        {
        case ED_UI_COMMAND_TAB:
            requested_tab = command.value <= 5U ? (uint8_t)(command.value + 1U) : 0U;
            break;
        case ED_UI_COMMAND_DEMO:
            demo_data_enabled = command.value != 0U;
            break;
        case ED_UI_COMMAND_ALARM:
            demo_alarm_requested = command.value != 0U;
            alarm_ack_requested = 0U;
            break;
        case ED_UI_COMMAND_ACK:
            alarm_ack_requested = 1U;
            break;
        case ED_UI_COMMAND_BASELINE:
            (void)ed_monitor_app_baseline_start(0U);
            break;
        default:
            break;
        }
    }
}

static const char *class_names[EDGEDOCTOR_CLASS_COUNT] = {
    "正常", "轴承内圈", "轴承外圈", "不对中", "不平衡"
};

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (ui.font_small != NULL)
    {
        lv_obj_set_style_text_font(label, ui.font_small, 0);
    }
    return label;
}

static void set_font(lv_obj_t *object, const lv_font_t *font)
{
    if (font != NULL)
    {
        lv_obj_set_style_text_font(object, font, 0);
    }
}

static lv_obj_t *make_plain(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static void style_page(lv_obj_t *page)
{
    lv_obj_set_style_bg_color(page, lv_color_hex(COLOR_CANVAS), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 8, 0);
    lv_obj_set_style_pad_gap(page, 8, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_gap(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x9AA3ACU), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_offset_y(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_section_title(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = make_plain(parent);
    lv_obj_t *accent;
    lv_obj_t *label;
    lv_obj_set_size(row, LV_SIZE_CONTENT, 24);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 7, 0);
    accent = lv_obj_create(row);
    lv_obj_set_size(accent, 4, 18);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_radius(accent, 2, 0);
    label = make_label(row, title, COLOR_TEXT);
    set_font(label, ui.font_small);
    return label;
}

static void build_header(lv_obj_t *page, uint32_t index)
{
    lv_obj_t *header = make_plain(page);
    lv_obj_t *brand = make_plain(header);
    lv_obj_t *logo;
    lv_obj_t *brand_text = make_plain(brand);
    lv_obj_t *title;
    lv_obj_set_size(header, LV_PCT(100), 54);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(brand, 334, 52);
    lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(brand, 8, 0);
    logo = lv_obj_create(brand);
    lv_obj_set_size(logo, 38, 38);
    lv_obj_set_style_bg_color(logo, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(logo, 0, 0);
    lv_obj_set_style_radius(logo, 9, 0);
    title = make_label(logo, "E", COLOR_SURFACE);
    set_font(title, ui.font_medium);
    lv_obj_center(title);
    lv_obj_set_size(brand_text, 282, 50);
    lv_obj_set_flex_flow(brand_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(brand_text, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    title = make_label(brand_text, "EdgeDoctor-RT", COLOR_TEXT);
    set_font(title, ui.font_small);
    make_label(brand_text, "工业设备智能诊断", COLOR_MUTED);
    ui.header_state[index] = make_label(header, "设备正常", COLOR_GREEN);
    set_font(ui.header_state[index], ui.font_small);
    lv_obj_set_width(ui.header_state[index], 112);
    lv_obj_set_style_text_align(ui.header_state[index], LV_TEXT_ALIGN_RIGHT, 0);
}

static void make_metric_line(lv_obj_t *parent, const char *name, lv_obj_t **value)
{
    lv_obj_t *row = make_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), 31);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(row, name, COLOR_MUTED);
    *value = make_label(row, "--", COLOR_TEXT);
    set_font(*value, ui.font_small);
}

static lv_obj_t *make_chart(lv_obj_t *parent, uint32_t points, uint32_t color,
                            lv_chart_series_t **series)
{
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_width(chart, LV_PCT(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(chart, points);
    lv_chart_set_div_line_count(chart, 4, 7);
    lv_obj_set_style_bg_color(chart, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    *series = lv_chart_add_series(chart, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
    return chart;
}

static void build_overview(lv_obj_t *page)
{
    lv_obj_t *card;
    lv_obj_t *row;
    lv_obj_t *health;
    lv_obj_t *metrics;
    lv_obj_t *image;
    style_page(page);
    build_header(page, 0U);

    card = make_card(page, 200);
    row = make_plain(card);
    lv_obj_set_size(row, LV_PCT(100), 26);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_section_title(row, "设备概览");
    make_label(row, "KAIST-002184", COLOR_MUTED);
    image = lv_image_create(card);
    lv_image_set_src(image, &edgedoctor_machine_image);
    lv_obj_set_size(image, 440, 148);
    lv_obj_align(image, LV_ALIGN_BOTTOM_MID, 0, 0);

    row = make_plain(page);
    lv_obj_set_size(row, LV_PCT(100), 158);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 8, 0);
    health = make_card(row, LV_PCT(100));
    lv_obj_set_width(health, 174);
    make_section_title(health, "设备健康度");
    ui.overview_health_arc = lv_arc_create(health);
    lv_obj_set_size(ui.overview_health_arc, 105, 105);
    lv_obj_align(ui.overview_health_arc, LV_ALIGN_CENTER, 0, 12);
    lv_arc_set_range(ui.overview_health_arc, 0, 100);
    lv_arc_set_value(ui.overview_health_arc, 0);
    lv_arc_set_rotation(ui.overview_health_arc, 135);
    lv_arc_set_bg_angles(ui.overview_health_arc, 0, 270);
    lv_obj_remove_flag(ui.overview_health_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(ui.overview_health_arc, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.overview_health_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.overview_health_arc, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ui.overview_health_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_opa(ui.overview_health_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    ui.overview_health_value = make_label(ui.overview_health_arc, "--", COLOR_TEXT);
    set_font(ui.overview_health_value, ui.font_large);
    lv_obj_center(ui.overview_health_value);

    metrics = make_card(row, LV_PCT(100));
    lv_obj_set_flex_grow(metrics, 1);
    make_section_title(metrics, "核心指标");
    make_metric_line(metrics, "DSP 耗时", &ui.overview_dsp);
    make_metric_line(metrics, "推理模型", &ui.overview_model);
    make_metric_line(metrics, "运行日志", &ui.overview_log);

    card = make_card(page, 200);
    row = make_plain(card);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_section_title(row, "实时趋势");
    ui.overview_state = make_label(row, "等待数据", COLOR_GREEN);
    ui.latency_chart = make_chart(card, UI_TREND_POINTS, COLOR_BLUE,
                                  &ui.latency_series);
    row = make_plain(card);
    lv_obj_set_size(row, LV_PCT(100), 27);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.overview_result = make_label(row, "诊断链路待机", COLOR_TEXT);
    ui.overview_detail = make_label(row, "最近 30 个处理窗口", COLOR_MUTED);
}

static lv_obj_t *make_signal_card(lv_obj_t *page, const char *title,
                                  const char *subtitle, uint32_t points,
                                  uint32_t color, lv_chart_series_t **series,
                                  lv_obj_t **status)
{
    lv_obj_t *card = make_card(page, 292);
    lv_obj_t *row = make_plain(card);
    lv_obj_t *chart;
    lv_obj_set_size(row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_section_title(row, title);
    *status = make_label(row, "等待采样", color);
    chart = make_chart(card, points, color, series);
    row = make_plain(card);
    lv_obj_set_size(row, LV_PCT(100), 26);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(row, subtitle, COLOR_MUTED);
    make_label(row, "归一化幅值", COLOR_MUTED);
    return chart;
}

static void build_spectrum(lv_obj_t *page)
{
    style_page(page);
    build_header(page, 1U);
    ui.audio_chart = make_signal_card(page, "声音信号", "40 个梅尔频带",
                                      EDGEDOCTOR_UI_AUDIO_POINTS, COLOR_BLUE,
                                      &ui.audio_series, &ui.signal_audio_state);
    ui.vibration_chart = make_signal_card(page, "振动信号", "64 个双轴频带",
                                          EDGEDOCTOR_UI_VIBRATION_POINTS, COLOR_RED,
                                          &ui.vibration_series,
                                          &ui.signal_vibration_state);
}

static void alarm_button_event(lv_event_t *event)
{
    if ((lv_event_get_code(event) == LV_EVENT_CLICKED) && (ui.alarm_active != 0U))
    {
        alarm_ack_requested = 1U;
    }
}

static void test_alarm_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        (void)post_ui_command(ED_UI_COMMAND_ALARM, demo_alarm_requested ? 0U : 1U);
}

static void build_diagnosis_banner(lv_obj_t *page)
{
    lv_obj_t *text_box;
    lv_obj_t *icon_text;
    ui.diagnosis_banner = lv_obj_create(page);
    lv_obj_set_size(ui.diagnosis_banner, LV_PCT(100), 104);
    lv_obj_set_style_bg_color(ui.diagnosis_banner, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(ui.diagnosis_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.diagnosis_banner, 0, 0);
    lv_obj_set_style_radius(ui.diagnosis_banner, 8, 0);
    lv_obj_set_style_pad_all(ui.diagnosis_banner, 10, 0);
    lv_obj_set_style_pad_gap(ui.diagnosis_banner, 10, 0);
    lv_obj_set_flex_flow(ui.diagnosis_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.diagnosis_banner, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(ui.diagnosis_banner, LV_OBJ_FLAG_SCROLLABLE);
    ui.alarm_icon = lv_obj_create(ui.diagnosis_banner);
    lv_obj_set_size(ui.alarm_icon, 56, 56);
    lv_obj_set_style_bg_color(ui.alarm_icon, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(ui.alarm_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.alarm_icon, 0, 0);
    lv_obj_set_style_radius(ui.alarm_icon, 28, 0);
    lv_obj_remove_flag(ui.alarm_icon, LV_OBJ_FLAG_SCROLLABLE);
    icon_text = make_label(ui.alarm_icon, "!", COLOR_GREEN);
    set_font(icon_text, ui.font_large);
    lv_obj_center(icon_text);
    text_box = make_plain(ui.diagnosis_banner);
    lv_obj_set_height(text_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text_box, 1);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    ui.alarm_title = make_label(text_box, "暂无严重报警", COLOR_SURFACE);
    set_font(ui.alarm_title, ui.font_medium);
    ui.alarm_detail = make_label(text_box, "诊断引擎正在持续监测", COLOR_SURFACE);
    ui.alarm_button = lv_button_create(ui.diagnosis_banner);
    lv_obj_set_size(ui.alarm_button, 92, 48);
    lv_obj_set_style_bg_color(ui.alarm_button, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(ui.alarm_button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.alarm_button, 6, 0);
    lv_obj_add_event_cb(ui.alarm_button, alarm_button_event, LV_EVENT_CLICKED, NULL);
    ui.alarm_button_label = make_label(ui.alarm_button, "确认", COLOR_RED);
    lv_obj_center(ui.alarm_button_label);
    lv_obj_add_flag(ui.alarm_button, LV_OBJ_FLAG_HIDDEN);
}

static void make_detail_line(lv_obj_t *parent, const char *name, lv_obj_t **value)
{
    lv_obj_t *row = make_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), 35);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(row, name, COLOR_MUTED);
    *value = make_label(row, "--", COLOR_TEXT);
    set_font(*value, ui.font_small);
}

static void style_diagnosis_bar(lv_obj_t *bar)
{
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
}

static void build_diagnosis(lv_obj_t *page)
{
    uint32_t index;
    lv_obj_t *card;
    style_page(page);
    build_header(page, 2U);
    build_diagnosis_banner(page);
    card = make_card(page, 185);
    make_section_title(card, "故障详情");
    make_detail_line(card, "故障类型", &ui.diagnosis_type);
    make_detail_line(card, "置信度", &ui.diagnosis_confidence);
    make_detail_line(card, "异常评分", &ui.diagnosis_anomaly);
    make_detail_line(card, "数据窗口", &ui.diagnosis_frame);
    card = make_card(page, 310);
    make_section_title(card, "诊断依据");
    for (index = 0U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        lv_obj_t *row = make_plain(card);
        lv_obj_t *name;
        lv_obj_set_size(row, LV_PCT(100), 50);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 7, 0);
        name = make_label(row, class_names[index], COLOR_TEXT);
        lv_obj_set_width(name, 112);
        ui.diagnosis_bars[index] = lv_bar_create(row);
        lv_obj_set_height(ui.diagnosis_bars[index], 12);
        lv_obj_set_flex_grow(ui.diagnosis_bars[index], 1);
        lv_bar_set_range(ui.diagnosis_bars[index], 0, 32767);
        lv_bar_set_value(ui.diagnosis_bars[index], 0, LV_ANIM_OFF);
        style_diagnosis_bar(ui.diagnosis_bars[index]);
        ui.diagnosis_values[index] = make_label(row, "--", COLOR_MUTED);
        lv_obj_set_width(ui.diagnosis_values[index], 62);
        lv_obj_set_style_text_align(ui.diagnosis_values[index], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void build_profiler(lv_obj_t *page)
{
    lv_obj_t *card;
    style_page(page);
    build_header(page, 3U);
    card = make_card(page, 205);
    make_section_title(card, "性能监控");
    ui.profile_latency = make_label(card, "暂无性能样本", COLOR_TEXT);
    set_font(ui.profile_latency, ui.font_small);
    lv_obj_set_style_text_line_space(ui.profile_latency, 8, 0);
    card = make_card(page, 190);
    make_section_title(card, "运行统计");
    ui.profile_runtime = make_label(card, "等待处理窗口", COLOR_TEXT);
    set_font(ui.profile_runtime, ui.font_small);
    lv_obj_set_style_text_line_space(ui.profile_runtime, 8, 0);
    card = make_card(page, 190);
    make_section_title(card, "系统状态");
    ui.profile_storage = make_label(card, "正在读取系统状态", COLOR_TEXT);
    set_font(ui.profile_storage, ui.font_small);
    lv_obj_set_style_text_line_space(ui.profile_storage, 8, 0);
}

/* Reference-inspired light pages keep a fixed header/navigation and scroll only
 * their bounded content area. Explicit widths keep translated text in its card. */
static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *label = make_label(parent, text, color);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *light_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t callback, uintptr_t data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *label;
    lv_obj_set_size(button, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xC5D9F7), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 4, 0);
    label = make_label(button, text, COLOR_BLUE);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)data);
    return button;
}

static lv_obj_t *light_row(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *row = make_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void badge_style(lv_obj_t *label, uint32_t color, uint32_t background)
{
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(label, 6, 0);
    lv_obj_set_style_pad_hor(label, 8, 0);
    lv_obj_set_style_pad_ver(label, 5, 0);
}

static lv_obj_t *light_page(lv_obj_t *page, uint32_t index, const char *title)
{
    lv_obj_t *header, *logo, *label, *body;
    style_page(page);
    header = light_row(page, 40);
    logo = make_label(header, "E", COLOR_BLUE);
    set_font(logo, ui.font_large);
    label = make_label(header, "EdgeDoctor-RT", COLOR_TEXT);
    lv_obj_set_flex_grow(label, 1);
    ui.header_state[index] = make_label(header, "等待数据", COLOR_MUTED);
    label = wrap_label(page, title, COLOR_TEXT);
    set_font(label, ui.font_large);
    body = make_plain(page);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, 0);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(body, 10, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    return body;
}

static void selected_button(lv_obj_t *button, uint8_t selected)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(selected ? 0xDDEBFF : COLOR_SURFACE), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(selected ? 0xABCDFB : COLOR_BORDER), 0);
}

static void event_filter_event(lv_event_t *event)
{
    show_all_events = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    panel_refresh_ms = lv_tick_get() - 1000U;
}

static void event_open_event(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index >= monitor_view.event_count) return;
    ack_submitted = 0;
    lv_label_set_text(event_action, "");
    rt_snprintf(selected_event, sizeof(selected_event), "%s", monitor_view.events[index].event_id);
    lv_obj_remove_flag(event_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(event_detail, LV_ANIM_OFF);
    panel_refresh_ms = lv_tick_get() - 1000U;
}

static void event_ack_event(lv_event_t *event)
{
    unsigned i;
    (void)event;
    for (i = 0; i < monitor_view.event_count; ++i)
    {
        const ed_event_t *item = &monitor_view.events[i];
        ed_ack_request_t request;
        if (strcmp(item->event_id, selected_event) != 0) continue;
        memset(&request, 0, sizeof(request));
        rt_snprintf(request.command_id, sizeof(request.command_id), "ui-%lu", (unsigned long)lv_tick_get());
        rt_snprintf(request.event_id, sizeof(request.event_id), "%s", item->event_id);
        request.expected_revision = item->revision;
        request.origin = ED_ORIGIN_LOCAL;
        rt_snprintf(request.actor, sizeof(request.actor), "%s", "local-ui");
        request.received_mono_us = monitor_snapshot.mono_us;
        if (ed_monitor_app_post_ack(&request) == 0) {
            ack_submitted = 1;
            ack_submitted_ms = lv_tick_get();
            lv_obj_add_state(event_ack_button, LV_STATE_DISABLED);
            lv_label_set_text(event_action, "确认请求已提交，等待保存");
        } else lv_label_set_text(event_action, "提交失败，请稍后重试");
        return;
    }
    lv_label_set_text(event_action, "事件已不在当前记录中");
}

static void build_events(lv_obj_t *page)
{
    unsigned i;
    lv_obj_t *body = light_page(page, 2U, "事件中心");
    lv_obj_t *row = light_row(body, 44);
    lv_obj_t *legacy;
    event_filter[0] = light_button(row, "待处理", event_filter_event, 0);
    event_filter[1] = light_button(row, "全部", event_filter_event, 1);
    lv_obj_set_width(event_filter[0], LV_PCT(49));
    lv_obj_set_flex_grow(event_filter[1], 1);
    lv_obj_set_width(event_filter[1], 0);
    event_count_label = wrap_label(body, "读取事件记录…", COLOR_MUTED);
    event_empty = make_card(body, LV_SIZE_CONTENT);
    wrap_label(event_empty, "暂无事件数据", COLOR_TEXT);
    wrap_label(event_empty, "监测中的异常会记录在这里", COLOR_MUTED);
    for (i = 0; i < ED_ACTIVE_EVENTS_MAX; ++i)
    {
        lv_obj_t *title_row;
        event_rows[i] = make_card(body, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(event_rows[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(event_rows[i], 4, 0);
        title_row = light_row(event_rows[i], 34);
        event_titles[i] = make_label(title_row, "", COLOR_TEXT);
        lv_obj_set_width(event_titles[i], 0);
        lv_obj_set_flex_grow(event_titles[i], 1);
        lv_label_set_long_mode(event_titles[i], LV_LABEL_LONG_DOT);
        set_font(event_titles[i], ui.font_medium);
        event_meta[i] = wrap_label(event_rows[i], "", COLOR_MUTED);
        event_status[i] = make_label(title_row, "", COLOR_AMBER);
        badge_style(event_status[i], COLOR_AMBER, 0xFFF1DE);
        lv_obj_add_flag(event_rows[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(event_rows[i], event_open_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    wrap_label(body, "确认表示已收到报警；恢复由监测结果判断。全部显示本机保留的最近记录。", COLOR_MUTED);
    event_detail = make_card(body, LV_SIZE_CONTENT);
    wrap_label(event_detail, "事件详情", COLOR_TEXT);
    event_detail_text = wrap_label(event_detail, "", COLOR_MUTED);
    event_ack_button = light_button(event_detail, "确认收到", event_ack_event, 0);
    event_action = wrap_label(event_detail, "", COLOR_MUTED);
    lv_obj_add_flag(event_detail, LV_OBJ_FLAG_HIDDEN);
    /* Retain replay/classifier diagnostics, accessible below live events. */
    legacy = make_plain(body);
    lv_obj_set_size(legacy, LV_PCT(100), LV_SIZE_CONTENT);
    build_diagnosis(legacy);
    lv_obj_add_flag(lv_obj_get_child(legacy, 0), LV_OBJ_FLAG_HIDDEN);
    /* build_diagnosis historically overwrote the real page header pointer. */
    ui.header_state[2] = lv_obj_get_child(lv_obj_get_child(page, 0), 2);
}

static void trend_control_event(lv_event_t *event)
{
    uintptr_t value = (uintptr_t)lv_event_get_user_data(event);
    if (value < 2U) trend_day = (uint8_t)value;
    else trend_deviation = (uint8_t)(value - 2U);
    panel_refresh_ms = lv_tick_get() - 1000U;
}

static void build_trends(lv_obj_t *page)
{
    lv_obj_t *body = light_page(page, 3U, "振动趋势");
    lv_obj_t *row = light_row(body, 44);
    lv_obj_t *card;
    trend_range[0] = light_button(row, "1小时", trend_control_event, 0);
    trend_range[1] = light_button(row, "24小时", trend_control_event, 1);
    lv_obj_set_width(trend_range[0], LV_PCT(49));
    lv_obj_set_width(trend_range[1], 0); lv_obj_set_flex_grow(trend_range[1], 1);
    card = make_card(body, 410);
    row = light_row(card, 44);
    trend_metric[0] = light_button(row, "RMS", trend_control_event, 2);
    trend_metric[1] = light_button(row, "基线偏离", trend_control_event, 3);
    lv_obj_set_width(trend_metric[0], LV_PCT(49));
    lv_obj_set_width(trend_metric[1], 0); lv_obj_set_flex_grow(trend_metric[1], 1);
    trend_note = wrap_label(card, "本次运行 · 等待有效采样", COLOR_MUTED);
    trend_chart = make_chart(card, 60, COLOR_BLUE, &trend_series);
    trend_baseline = lv_chart_add_series(trend_chart, lv_color_hex(COLOR_MUTED), LV_CHART_AXIS_PRIMARY_Y);
    wrap_label(card, "时间 → 最近所选区间（右端为现在）", COLOR_MUTED);
    row = light_row(card, 64);
    trend_mean = make_label(row, "区间均值\n—", COLOR_TEXT);
    trend_peak = make_label(row, "区间峰值\n—", COLOR_TEXT);
    trend_valid = make_label(row, "窗口有效率\n—", COLOR_TEXT);
    lv_obj_t *summary[3] = {trend_mean, trend_peak, trend_valid};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_width(summary[i], 0);
        lv_obj_set_flex_grow(summary[i], 1);
        lv_obj_set_style_text_align(summary[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(summary[i], 10, 0);
    }
    card = make_card(body, LV_SIZE_CONTENT);
    wrap_label(card, "维修前后对比", COLOR_TEXT);
    wrap_label(card, "尚无维修记录。趋势保留本次运行数据，重启后清空；缺失区间不补值。", COLOR_MUTED);
}

static void baseline_action_event(lv_event_t *event)
{
    int result;
    (void)event;
    if (monitor_view.baseline_active && monitor_view.baseline_windows >= 60U)
        result = ed_monitor_app_baseline_commit();
    else result = ed_monitor_app_baseline_start(0U);
    lv_label_set_text(baseline_progress, result == 0 ? "请求已提交，等待监测线程处理" : "请求失败，请稍后重试");
}

static void baseline_view_event(lv_event_t *event)
{
    (void)event;
    if (lv_obj_has_flag(baseline_details, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(baseline_details, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_view(baseline_details, LV_ANIM_OFF);
    } else lv_obj_add_flag(baseline_details, LV_OBJ_FLAG_HIDDEN);
}

static void build_asset(lv_obj_t *page)
{
    lv_obj_t *body = light_page(page, 4U, "设备与基线");
    lv_obj_t *card = make_card(body, LV_SIZE_CONTENT);
    asset_name = wrap_label(card, "设备档案", COLOR_TEXT);
    set_font(asset_name, ui.font_medium);
    asset_state = make_label(card, "等待监测数据", COLOR_MUTED);
    badge_style(asset_state, COLOR_MUTED, 0xEDF0F4);
    make_metric_line(card, "传感器", &asset_profile);
    lv_label_set_text(asset_profile, "板载 IMU · 208 Hz");
    make_metric_line(card, "轴方向", &asset_mount);
    lv_label_set_text(asset_mount, "X / Y / Z");
    asset_mount = wrap_label(card, "安装 / 工况  未读取", COLOR_MUTED);
    card = make_card(body, LV_SIZE_CONTENT);
    baseline_state = wrap_label(card, "运行基线 · 未读取", COLOR_TEXT);
    set_font(baseline_state, ui.font_medium);
    baseline_info = wrap_label(card, "等待基线状态", COLOR_MUTED);
    make_metric_line(card, "基线 RMS", &baseline_rms);
    baseline_time = wrap_label(card, "建立时间  —", COLOR_MUTED);
    baseline_progress = wrap_label(card, "仅在安装固定、工况稳定时建立基线", COLOR_MUTED);
    baseline_bar = lv_bar_create(card);
    lv_obj_set_size(baseline_bar, LV_PCT(100), 7);
    lv_bar_set_range(baseline_bar, 0, 60);
    lv_obj_set_style_bg_color(baseline_bar, lv_color_hex(0xEAF2FF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(baseline_bar, lv_color_hex(COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_t *actions = light_row(card, 44);
    baseline_view_button = light_button(actions, "查看基线", baseline_view_event, 0);
    lv_obj_set_width(baseline_view_button, LV_PCT(49));
    baseline_button = light_button(actions, "建立基线", baseline_action_event, 0);
    lv_obj_set_width(baseline_button, 0);
    lv_obj_set_flex_grow(baseline_button, 1);
    baseline_button_label = lv_obj_get_child(baseline_button, 0);
    baseline_details = make_card(body, LV_SIZE_CONTENT);
    set_font(wrap_label(baseline_details, "基线详情", COLOR_TEXT), ui.font_medium);
    baseline_details_text = wrap_label(baseline_details, "尚无基线", COLOR_MUTED);
    lv_obj_add_flag(baseline_details, LV_OBJ_FLAG_HIDDEN);
    card = make_card(body, LV_SIZE_CONTENT);
    wrap_label(card, "维修记录", COLOR_TEXT);
    wrap_label(card, "尚无维修记录，记录服务暂未接入", COLOR_MUTED);
}

static void toggle_system_event(lv_event_t *event)
{
    (void)event;
    if (lv_obj_has_flag(system_details, LV_OBJ_FLAG_HIDDEN))
    { lv_obj_remove_flag(system_details, LV_OBJ_FLAG_HIDDEN); lv_obj_scroll_to_view(system_details, LV_ANIM_OFF); }
    else lv_obj_add_flag(system_details, LV_OBJ_FLAG_HIDDEN);
}

static void build_connection(lv_obj_t *page)
{
    lv_obj_t *body = light_page(page, 5U, "连接与同步");
    lv_obj_t *card = make_card(body, LV_SIZE_CONTENT);
    lv_obj_t *saved_header = ui.header_state[3];
    set_font(wrap_label(card, "Wi-Fi", COLOR_TEXT), ui.font_medium);
    wifi_state = wrap_label(card, "网络状态接口尚未接入", COLOR_MUTED);
    card = make_card(body, LV_SIZE_CONTENT);
    set_font(wrap_label(card, "MQTT 服务", COLOR_TEXT), ui.font_medium);
    mqtt_status = wrap_label(card, "读取连接状态…", COLOR_MUTED);
    card = make_card(body, LV_SIZE_CONTENT);
    set_font(wrap_label(card, "事件同步", COLOR_TEXT), ui.font_medium);
    sync_steps = wrap_label(card, "本地保存 → Broker 接收 → 服务端落库", COLOR_MUTED);
    sync_status = wrap_label(card, "等待同步状态", COLOR_MUTED);
    demo_button_label = lv_obj_get_child(light_button(body, "本地报警演示", test_alarm_button_event, 0), 0);
    light_button(body, "系统详情 / 收起", toggle_system_event, 0);
    system_details = make_plain(body);
    lv_obj_set_size(system_details, LV_PCT(100), LV_SIZE_CONTENT);
    build_profiler(system_details);
    lv_obj_add_flag(lv_obj_get_child(system_details, 0), LV_OBJ_FLAG_HIDDEN);
    ui.header_state[3] = saved_header;
    lv_obj_set_width(ui.profile_storage, LV_PCT(100));
    lv_label_set_long_mode(ui.profile_storage, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(system_details, LV_OBJ_FLAG_HIDDEN);
}

static void paint_navigation(uint8_t active)
{
    uint32_t index;
    if (active >= UI_TAB_COUNT)
    {
        return;
    }
    ui.active_tab = active;
    for (index = 0U; index < UI_TAB_COUNT; ++index)
    {
        uint32_t background = (index == active) ? COLOR_RED : COLOR_NAV;
        uint32_t foreground = (index == active) ? COLOR_SURFACE : 0xC7CDD2U;
        lv_obj_set_style_bg_color(ui.nav_buttons[index], lv_color_hex(background), 0);
        lv_obj_set_style_bg_color(ui.nav_buttons[index], lv_color_hex(background),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_text_color(ui.nav_labels[index], lv_color_hex(foreground), 0);
    }
}

static void nav_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
        uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
        lv_tabview_set_active(ui.tabview, index, LV_ANIM_OFF);
        paint_navigation(index);
    }
}

static void build_navigation(lv_obj_t *parent)
{
    static const char *names[UI_TAB_COUNT] = {"总览", "信号", "事件", "趋势", "设备", "连接"};
    uint32_t index;
    ui.nav = lv_obj_create(parent);
    lv_obj_set_size(ui.nav, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(ui.nav, lv_color_hex(COLOR_NAV), 0);
    lv_obj_set_style_bg_opa(ui.nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.nav, 0, 0);
    lv_obj_set_style_radius(ui.nav, 0, 0);
    lv_obj_set_style_pad_all(ui.nav, 0, 0);
    lv_obj_set_style_pad_gap(ui.nav, 0, 0);
    lv_obj_set_flex_flow(ui.nav, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(ui.nav, LV_OBJ_FLAG_SCROLLABLE);
    for (index = 0U; index < UI_TAB_COUNT; ++index)
    {
        ui.nav_buttons[index] = lv_button_create(ui.nav);
        lv_obj_set_height(ui.nav_buttons[index], LV_PCT(100));
        lv_obj_set_flex_grow(ui.nav_buttons[index], 1);
        lv_obj_set_style_bg_opa(ui.nav_buttons[index], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ui.nav_buttons[index], 0, 0);
        lv_obj_set_style_outline_width(ui.nav_buttons[index], 0, 0);
        lv_obj_set_style_radius(ui.nav_buttons[index], 0, 0);
        lv_obj_set_style_shadow_width(ui.nav_buttons[index], 0, 0);
        lv_obj_add_event_cb(ui.nav_buttons[index], nav_button_event,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)index);
        ui.nav_labels[index] = make_label(ui.nav_buttons[index], names[index], COLOR_SURFACE);
        set_font(ui.nav_labels[index], ui.font_small);
        lv_obj_center(ui.nav_labels[index]);
    }
    paint_navigation(0U);
}

void edgedoctor_ui_init(const lv_font_t *font_small,
                        const lv_font_t *font_medium,
                        const lv_font_t *font_large)
{
    lv_obj_t *bar;
    lv_obj_t *content;
    if (ui.root != NULL)
    {
        return;
    }
    memset(&ui, 0, sizeof(ui));
    if (rt_mq_init(&ui_command_queue, "edui", ui_command_pool,
                   sizeof(ed_ui_command_t), sizeof(ui_command_pool),
                   RT_IPC_FLAG_PRIO) != RT_EOK)
        return;
    ui_command_queue_ready = 1U;
    ui.font_small = font_small;
    ui.font_medium = font_medium;
    ui.font_large = font_large;
    ui.root = lv_obj_create(NULL);
    lv_obj_set_size(ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ui.root, 0, 0);
    lv_obj_set_style_bg_color(ui.root, lv_color_hex(COLOR_CANVAS), 0);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui.root, 0, 0);
    lv_obj_set_style_border_width(ui.root, 0, 0);
    lv_obj_set_style_radius(ui.root, 0, 0);
    lv_obj_set_flex_flow(ui.root, LV_FLEX_FLOW_COLUMN);
    if (font_small != NULL)
    {
        lv_obj_set_style_text_font(ui.root, font_small, 0);
    }
    lv_obj_remove_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);
    ui.tabview = lv_tabview_create(ui.root);
    lv_obj_set_width(ui.tabview, LV_PCT(100));
    lv_obj_set_flex_grow(ui.tabview, 1);
    lv_tabview_set_tab_bar_position(ui.tabview, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(ui.tabview, 0);
    content = lv_tabview_get_content(ui.tabview);
    lv_obj_set_style_bg_color(content, lv_color_hex(COLOR_CANVAS), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    bar = lv_tabview_get_tab_bar(ui.tabview);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_NAV), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_NAV), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_RED),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bar, lv_color_hex(0xC7CDD2U), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bar, lv_color_hex(COLOR_SURFACE),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    if (font_small != NULL)
    {
        lv_obj_set_style_text_font(bar, font_small, LV_PART_ITEMS);
        lv_obj_set_style_text_font(bar, font_small,
                                   LV_PART_ITEMS | LV_STATE_CHECKED);
    }
    lv_obj_set_style_border_width(bar, 0, 0);
    build_overview(lv_tabview_add_tab(ui.tabview, "概览"));
    build_spectrum(lv_tabview_add_tab(ui.tabview, "信号"));
    build_events(lv_tabview_add_tab(ui.tabview, "事件"));
    build_trends(lv_tabview_add_tab(ui.tabview, "趋势"));
    build_asset(lv_tabview_add_tab(ui.tabview, "设备"));
    build_connection(lv_tabview_add_tab(ui.tabview, "连接"));
    build_navigation(ui.root);
    lv_screen_load(ui.root);
}

static const char *result_status(const edgedoctor_result_t *result)
{
    switch (result->status)
    {
    case EDGEDOCTOR_RESULT_OK: return "诊断完成";
    case EDGEDOCTOR_RESULT_MODEL_UNAVAILABLE: return "特征已就绪";
    case EDGEDOCTOR_RESULT_DSP_ERROR: return "信号处理异常";
    default: return "窗口预热中";
    }
}

static const char *live_quality_text(ed_quality_t quality)
{
    switch (quality)
    {
    case ED_QUALITY_VALID: return "有效";
    case ED_QUALITY_WARMUP: return "窗口预热";
    case ED_QUALITY_STALE: return "数据过期";
    case ED_QUALITY_DISCONNECTED: return "传感器断开";
    case ED_QUALITY_GAPPED: return "采样有缺口";
    case ED_QUALITY_SATURATED: return "传感器饱和";
    default: return "配置错误";
    }
}

static void refresh_live_snapshot(const ed_snapshot_t *snapshot)
{
    ed_event_t active_event;
    uint32_t health = 0U;
    uint32_t anomaly = 0U;
    if (snapshot == NULL)
        return;
    if (isfinite(snapshot->baseline_deviation))
    {
        float score = snapshot->baseline_deviation * 10.0f;
        if (score > 100.0f) score = 100.0f;
        if (score > 0.0f) anomaly = (uint32_t)score;
        health = 100U - anomaly;
    }
    set_header_state(snapshot->quality == ED_QUALITY_VALID ? "板载监测" : "数据质量异常",
                     snapshot->quality == ED_QUALITY_VALID ? COLOR_GREEN : COLOR_AMBER);
    lv_label_set_text_fmt(ui.overview_state, "窗口 %llu",
                          (unsigned long long)snapshot->snapshot_id);
    lv_label_set_text_fmt(ui.overview_result, "低频监测  %s",
                          live_quality_text(snapshot->quality));
    lv_label_set_text(ui.overview_dsp, "208 Hz / 5–40 Hz");
    lv_label_set_text_fmt(ui.overview_log, "RMS %.5f g",
                          (double)snapshot->features.values[3]);
    lv_label_set_text_fmt(ui.signal_audio_state, "板载 IMU  %.5f g",
                          (double)snapshot->features.values[3]);
    lv_label_set_text_fmt(ui.signal_vibration_state, "偏离度 %s",
                          isfinite(snapshot->baseline_deviation) ? "有效" : "—");
    lv_label_set_text_fmt(ui.diagnosis_frame, "#%llu",
                          (unsigned long long)snapshot->snapshot_id);
    lv_label_set_text(ui.overview_model, "低频基线");
    lv_obj_set_style_text_color(ui.overview_model, lv_color_hex(COLOR_BLUE), 0);
    lv_arc_set_value(ui.overview_health_arc, (int32_t)health);
    lv_label_set_text_fmt(ui.overview_health_value, "%u", (unsigned int)health);
    lv_label_set_text(ui.overview_detail,
                      snapshot->diagnosis == ED_DIAG_BASELINE_MISMATCH
                          ? "已有基线与当前传感器配置不匹配"
                          : snapshot->baseline_version != 0U
                              ? "板载物理特征 · 基线偏离"
                              : "尚未建立匹配基线");
    lv_label_set_text(ui.diagnosis_type,
                      snapshot->diagnosis == ED_DIAG_BASELINE_MISMATCH ? "基线偏离" :
                      snapshot->diagnosis == ED_DIAG_WITHIN_BASELINE ? "基线范围内" : "无可用基线");
    lv_label_set_text_fmt(ui.diagnosis_confidence, "RMS %.5f g",
                          (double)snapshot->features.values[3]);
    lv_label_set_text_fmt(ui.diagnosis_anomaly, "%u / 100", (unsigned int)anomaly);
    if (ed_monitor_app_read_active_event(&active_event) > 0)
    {
        if (alarm_ack_requested != 0U && active_event.ack == ED_UNACKNOWLEDGED &&
            active_event.ack_pending == 0U)
        {
            ed_ack_request_t request;
            memset(&request, 0, sizeof(request));
            rt_snprintf(request.command_id, sizeof(request.command_id), "ui-%lu",
                        (unsigned long)lv_tick_get());
            rt_snprintf(request.event_id, sizeof(request.event_id), "%s",
                        active_event.event_id);
            request.expected_revision = active_event.revision;
            request.origin = ED_ORIGIN_LOCAL;
            request.received_mono_us = snapshot->mono_us;
            if (ed_monitor_app_post_ack(&request) == 0)
                alarm_ack_requested = 0U;
        }
        set_alarm(1U, active_event.revision, "低频基线偏离");
        ui.alarm_acknowledged = (uint8_t)(active_event.ack == ED_ACKNOWLEDGED ||
                                         active_event.ack_pending != 0U);
        paint_alarm_banner();
    }
    else
    {
        alarm_ack_requested = 0U;
        set_alarm(0U, 0U, NULL);
    }
}

static uint8_t result_is_critical(const edgedoctor_result_t *result,
                                  const char **detail)
{
    if (result->status == EDGEDOCTOR_RESULT_DSP_ERROR)
    {
        *detail = "信号处理失败  请检查输入";
        return 1U;
    }
    if ((result->status == EDGEDOCTOR_RESULT_OK) &&
        ((result->anomaly_q15 >= UI_CRITICAL_Q15) ||
         ((result->predicted_class != EDGEDOCTOR_CLASS_NORMAL) &&
          (result->predicted_class < EDGEDOCTOR_CLASS_COUNT) &&
          (result->confidence_q15 >= UI_CRITICAL_Q15))))
    {
        *detail = (result->predicted_class < EDGEDOCTOR_CLASS_COUNT)
            ? class_names[result->predicted_class] : "异常评分超过严重阈值";
        return 1U;
    }
    return 0U;
}

static void paint_alarm_banner(void)
{
    lv_obj_t *icon_text = lv_obj_get_child(ui.alarm_icon, 0);
    if (ui.alarm_active == 0U)
    {
        lv_obj_set_style_bg_color(ui.diagnosis_banner, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_text_color(icon_text, lv_color_hex(COLOR_GREEN), 0);
        lv_label_set_text(ui.alarm_title, "暂无严重报警");
        lv_label_set_text(ui.alarm_detail, "诊断引擎正在持续监测");
        lv_obj_add_flag(ui.alarm_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(ui.alarm_icon, LV_OPA_COVER, 0);
        return;
    }
    lv_obj_set_style_bg_color(ui.diagnosis_banner,
                              lv_color_hex(ui.alarm_acknowledged
                                               ? COLOR_RED_DARK : COLOR_RED), 0);
    lv_obj_set_style_text_color(icon_text, lv_color_hex(COLOR_RED), 0);
    lv_obj_remove_flag(ui.alarm_button, LV_OBJ_FLAG_HIDDEN);
    if (ui.alarm_acknowledged != 0U)
    {
        lv_label_set_text(ui.alarm_title, "严重报警  已确认");
        lv_label_set_text(ui.alarm_button_label, "已确认");
        lv_obj_add_state(ui.alarm_button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(ui.alarm_icon, LV_OPA_COVER, 0);
    }
    else
    {
        uint32_t now = lv_tick_get();
        lv_label_set_text(ui.alarm_title, "严重报警");
        lv_label_set_text(ui.alarm_button_label, "确认报警");
        lv_obj_remove_state(ui.alarm_button, LV_STATE_DISABLED);
        if ((uint32_t)(now - ui.alarm_blink_ms) >= UI_ALARM_BLINK_MS)
        {
            ui.alarm_blink_on ^= 1U;
            ui.alarm_blink_ms = now;
        }
        lv_obj_set_style_opa(ui.alarm_icon,
                             ui.alarm_blink_on ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

static void set_alarm(uint8_t active, uint32_t key, const char *detail)
{
    uint8_t was_active = ui.alarm_active;
    if (active == 0U)
    {
        ui.alarm_active = 0U;
        ui.alarm_acknowledged = 0U;
        paint_alarm_banner();
        return;
    }
    if ((ui.alarm_active == 0U) || (ui.alarm_key != key))
    {
        ui.alarm_acknowledged = 0U;
        ui.alarm_blink_on = 1U;
        ui.alarm_key = key;
        ui.alarm_blink_ms = lv_tick_get();
    }
    ui.alarm_active = 1U;
    lv_label_set_text(ui.alarm_detail, detail);
    if (was_active == 0U)
    {
        lv_tabview_set_active(ui.tabview, 2U, LV_ANIM_OFF);
    }
    paint_alarm_banner();
}

static void refresh_alarm(const edgedoctor_result_t *result, uint8_t have_result)
{
    const char *detail = NULL;
    uint8_t critical = 0U;
    uint32_t key = 0U;
    if (demo_alarm_requested != 0U)
    {
        critical = 1U;
        key = UINT32_MAX;
        detail = "轴承外圈故障  置信度 92%";
    }
    else if (have_result != 0U)
    {
        critical = result_is_critical(result, &detail);
        key = result->frame_id;
    }
    set_alarm(critical, key, detail);
    if ((alarm_ack_requested != 0U) && (ui.alarm_active != 0U))
    {
        ui.alarm_acknowledged = 1U;
        alarm_ack_requested = 0U;
        paint_alarm_banner();
    }
    if (ui.alarm_active == 0U)
    {
        alarm_ack_requested = 0U;
    }
}

static void set_header_state(const char *text, uint32_t color)
{
    uint32_t index;
    for (index = 0U; index < UI_TAB_COUNT; ++index)
    {
        lv_label_set_text(ui.header_state[index], text);
        lv_obj_set_style_text_color(ui.header_state[index], lv_color_hex(color), 0);
    }
}

static int32_t clamp_chart(int32_t value)
{
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static void refresh_demo(void)
{
    static const uint16_t normal_scores[EDGEDOCTOR_CLASS_COUNT] = {
        30801U, 983U, 328U, 328U, 327U
    };
    static const uint16_t alarm_scores[EDGEDOCTOR_CLASS_COUNT] = {
        1311U, 655U, 30146U, 328U, 327U
    };
    const uint16_t *scores = demo_alarm_requested ? alarm_scores : normal_scores;
    edgedoctor_result_t result;
    uint32_t index;
    uint32_t now = lv_tick_get();
    memset(&result, 0, sizeof(result));
    result.frame_id = 2184U + ui.demo_frame;
    result.status = EDGEDOCTOR_RESULT_OK;
    result.backend = EDGEDOCTOR_BACKEND_REFERENCE;
    result.predicted_class = demo_alarm_requested
        ? EDGEDOCTOR_CLASS_BEARING_OUTER : EDGEDOCTOR_CLASS_NORMAL;
    result.confidence_q15 = scores[result.predicted_class];
    result.anomaly_q15 = demo_alarm_requested ? 28507U : 2621U;
    memcpy(result.class_scores_q15, scores, sizeof(result.class_scores_q15));

    set_header_state(demo_alarm_requested ? "报警演示" : "演示模式",
                     demo_alarm_requested ? COLOR_RED : COLOR_AMBER);
    lv_label_set_text(ui.overview_state, "演示数据");
    lv_label_set_text(ui.overview_result,
                      demo_alarm_requested ? "检测到严重故障" : "设备状态良好");
    lv_label_set_text(ui.overview_detail,
                      demo_alarm_requested ? "轴承外圈  92.0%" : "正常  94.0%");
    lv_label_set_text_fmt(ui.overview_dsp, "3.%u ms",
                          (unsigned int)(1U + (ui.demo_frame % 4U)));
    lv_label_set_text(ui.overview_model, "板载 MLP");
    lv_obj_set_style_text_color(ui.overview_model, lv_color_hex(COLOR_GREEN), 0);
    lv_label_set_text(ui.overview_log, "板载 Flash");
    lv_obj_set_style_text_color(ui.overview_log, lv_color_hex(COLOR_GREEN), 0);
    lv_arc_set_value(ui.overview_health_arc, demo_alarm_requested ? 13 : 92);
    lv_label_set_text(ui.overview_health_value, demo_alarm_requested ? "13" : "92");
    lv_label_set_text(ui.signal_audio_state, "演示数据");
    lv_label_set_text(ui.signal_vibration_state, "演示数据");
    lv_label_set_text(ui.diagnosis_type,
                      demo_alarm_requested ? "轴承外圈故障" : "正常");
    lv_label_set_text(ui.diagnosis_confidence,
                      demo_alarm_requested ? "92.0%" : "94.0%");
    lv_label_set_text(ui.diagnosis_anomaly,
                      demo_alarm_requested ? "87.0 / 100" : "8.0 / 100");
    lv_label_set_text_fmt(ui.diagnosis_frame, "#%u", (unsigned int)result.frame_id);
    for (index = 0U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        uint16_t score = scores[index];
        lv_bar_set_value(ui.diagnosis_bars[index], score, LV_ANIM_ON);
        lv_label_set_text_fmt(ui.diagnosis_values[index], "%u.%u%%",
                              score * 100U / 32767U,
                              (score * 1000U / 32767U) % 10U);
    }
    lv_label_set_text(ui.profile_latency,
                      "演示窗口  2184\nDSP P50  3 ms\nDSP P95  4 ms\n模型推理 P95  1 ms");
    lv_label_set_text(ui.profile_runtime,
                      "当前队列  0\n峰值队列  2\n丢帧数量  0\n数据连续性  正常");
    lv_label_set_text(ui.profile_storage,
                      "模型  板载 KAIST MLP\n模型存储  固件 Flash\n界面资源  固件 Flash\n运行日志  板载 Flash\nSD 卡  不使用");

    if ((uint32_t)(now - ui.last_demo_ms) >= 350U)
    {
        int32_t *audio = lv_chart_get_y_array(ui.audio_chart, ui.audio_series);
        int32_t *vibration = lv_chart_get_y_array(ui.vibration_chart,
                                                  ui.vibration_series);
        for (index = 0U; index < EDGEDOCTOR_UI_AUDIO_POINTS; ++index)
        {
            int32_t value = 12 + (int32_t)((index * 7U + ui.demo_frame * 3U) % 24U);
            if ((index > 7U && index < 12U) || (index > 25U && index < 29U))
            {
                value += demo_alarm_requested ? 48 : 20;
            }
            audio[index] = clamp_chart(value);
        }
        for (index = 0U; index < EDGEDOCTOR_UI_VIBRATION_POINTS; ++index)
        {
            int32_t value = 8 + (int32_t)((index * 11U + ui.demo_frame * 5U) % 20U);
            if ((index > 30U && index < 38U) || (index > 48U && index < 53U))
            {
                value += demo_alarm_requested ? 68 : 28;
            }
            vibration[index] = clamp_chart(value);
        }
        lv_chart_set_next_value(ui.latency_chart, ui.latency_series,
                                (int32_t)(3U + (ui.demo_frame % 3U)));
        lv_chart_refresh(ui.audio_chart);
        lv_chart_refresh(ui.vibration_chart);
        ui.demo_frame++;
        ui.last_demo_ms = now;
    }
    refresh_alarm(&result, 1U);
}

static void refresh_result(void)
{
    edgedoctor_result_t result;
    ed_snapshot_t live_snapshot;
    uint32_t index;
    int read_status;
    if (demo_data_enabled != 0U)
    {
        refresh_demo();
        return;
    }
    if (!edgedoctor_app_is_replaying() &&
        (ed_monitor_app_read_snapshot(&live_snapshot) > 0))
    {
        refresh_live_snapshot(&live_snapshot);
        return;
    }
    read_status = edgedoctor_app_read_result(&result);
    if (read_status <= 0)
    {
        set_header_state(edgedoctor_app_is_replaying() ? "正在回放" : "等待数据",
                         edgedoctor_app_is_replaying() ? COLOR_BLUE : COLOR_MUTED);
        lv_label_set_text(ui.overview_state,
                          edgedoctor_app_is_replaying() ? "窗口预热中" : "等待数据");
        refresh_alarm(&result, 0U);
        return;
    }
    set_header_state(edgedoctor_app_is_replaying() ? "正在回放" : "数据在线",
                     edgedoctor_app_is_replaying() ? COLOR_RED : COLOR_GREEN);
    lv_label_set_text_fmt(ui.overview_state, "帧 %u", (unsigned int)result.frame_id);
    lv_label_set_text(ui.overview_result, result_status(&result));
    lv_label_set_text_fmt(ui.overview_dsp, "%u ms",
                          (unsigned int)((result.preprocess_us + 500U) / 1000U));
    lv_label_set_text(ui.overview_log, "正常");
    lv_obj_set_style_text_color(ui.overview_log, lv_color_hex(COLOR_GREEN), 0);
    lv_label_set_text_fmt(ui.signal_audio_state, "帧 %u", (unsigned int)result.frame_id);
    lv_label_set_text_fmt(ui.signal_vibration_state, "帧 %u", (unsigned int)result.frame_id);
    lv_label_set_text_fmt(ui.diagnosis_frame, "#%u", (unsigned int)result.frame_id);
    if ((result.status == EDGEDOCTOR_RESULT_OK) &&
        (result.predicted_class < EDGEDOCTOR_CLASS_COUNT))
    {
        uint32_t health = 100U - (result.anomaly_q15 * 100U / 32767U);
        lv_label_set_text(ui.overview_model, "已安装");
        lv_obj_set_style_text_color(ui.overview_model, lv_color_hex(COLOR_GREEN), 0);
        lv_arc_set_value(ui.overview_health_arc, (int32_t)health);
        lv_label_set_text_fmt(ui.overview_health_value, "%u", (unsigned int)health);
        lv_label_set_text_fmt(ui.overview_detail, "%s  %u.%u%%",
                              class_names[result.predicted_class],
                              result.confidence_q15 * 100U / 32767U,
                              (result.confidence_q15 * 1000U / 32767U) % 10U);
        lv_label_set_text(ui.diagnosis_type, class_names[result.predicted_class]);
        lv_label_set_text_fmt(ui.diagnosis_confidence, "%u.%u%%",
                              result.confidence_q15 * 100U / 32767U,
                              (result.confidence_q15 * 1000U / 32767U) % 10U);
        lv_label_set_text_fmt(ui.diagnosis_anomaly, "%u.%u / 100",
                              result.anomaly_q15 * 100U / 32767U,
                              (result.anomaly_q15 * 1000U / 32767U) % 10U);
    }
    else
    {
        lv_label_set_text(ui.overview_model, "未安装");
        lv_obj_set_style_text_color(ui.overview_model, lv_color_hex(COLOR_AMBER), 0);
        lv_arc_set_value(ui.overview_health_arc, 0);
        lv_label_set_text(ui.overview_health_value, "--");
        lv_label_set_text(ui.overview_detail, "特征有效  等待验证模型");
        lv_label_set_text(ui.diagnosis_type,
                          result.status == EDGEDOCTOR_RESULT_DSP_ERROR
                              ? "信号处理异常" : "模型未安装");
        lv_label_set_text(ui.diagnosis_confidence, "--");
        lv_label_set_text(ui.diagnosis_anomaly, "--");
    }
    if (result.frame_id != ui.last_result_frame)
    {
        uint32_t latency_ms = (result.total_us + 500U) / 1000U;
        if (latency_ms > 100U)
        {
            latency_ms = 100U;
        }
        lv_chart_set_next_value(ui.latency_chart, ui.latency_series,
                                (int32_t)latency_ms);
        ui.last_result_frame = result.frame_id;
    }
    for (index = 0U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        uint16_t score = result.class_scores_q15[index];
        lv_bar_set_value(ui.diagnosis_bars[index], score, LV_ANIM_OFF);
        if (result.status == EDGEDOCTOR_RESULT_OK)
        {
            lv_label_set_text_fmt(ui.diagnosis_values[index], "%u.%u%%",
                                  score * 100U / 32767U,
                                  (score * 1000U / 32767U) % 10U);
        }
        else
        {
            lv_label_set_text(ui.diagnosis_values[index], "--");
        }
    }
    refresh_alarm(&result, 1U);
}

static void refresh_spectrum(void)
{
    edgedoctor_ui_spectrum_t spectrum;
    uint32_t index;
    int32_t *audio_values;
    int32_t *vibration_values;
    if ((demo_data_enabled != 0U) ||
        (edgedoctor_app_read_spectrum(&spectrum) <= 0) ||
        (spectrum.frame_id == ui.last_spectrum_frame))
    {
        return;
    }
    audio_values = lv_chart_get_y_array(ui.audio_chart, ui.audio_series);
    vibration_values = lv_chart_get_y_array(ui.vibration_chart, ui.vibration_series);
    for (index = 0U; index < EDGEDOCTOR_UI_AUDIO_POINTS; ++index)
    {
        audio_values[index] = spectrum.audio[index];
    }
    for (index = 0U; index < EDGEDOCTOR_UI_VIBRATION_POINTS; ++index)
    {
        vibration_values[index] = spectrum.vibration[index];
    }
    ui.last_spectrum_frame = spectrum.frame_id;
    lv_chart_refresh(ui.audio_chart);
    lv_chart_refresh(ui.vibration_chart);
}

static void refresh_profile(void)
{
    edgedoctor_profile_snapshot_t profile;
    edgedoctor_logger_stats_t logger_stats;
    edgedoctor_result_t result;
    if (demo_data_enabled != 0U)
    {
        return;
    }
    edgedoctor_app_profile(&profile);
    edgedoctor_logger_stats(&logger_stats);
    lv_label_set_text_fmt(ui.profile_latency,
                          "处理窗口  %u\nDSP P50  %u ms\nDSP P95  %u ms\n总耗时 P95  %u ms",
                          (unsigned int)profile.frames,
                          (unsigned int)((profile.preprocess_p50_us + 500U) / 1000U),
                          (unsigned int)((profile.preprocess_p95_us + 500U) / 1000U),
                          (unsigned int)((profile.total_p95_us + 500U) / 1000U));
    if (edgedoctor_app_read_result(&result) > 0)
    {
        lv_label_set_text_fmt(ui.profile_runtime,
                              "当前队列  %u\n峰值队列  %u\n丢帧数量  %u\n数据连续性  %s",
                              result.queue_depth, profile.max_queue_depth,
                              (unsigned int)result.dropped_frames,
                              result.dropped_frames == 0U ? "正常" : "异常");
    }
    else
    {
        lv_label_set_text(ui.profile_runtime,
                          "当前队列  0\n峰值队列  0\n丢帧数量  0\n数据连续性  等待数据");
    }
    lv_label_set_text_fmt(ui.profile_storage,
                          "日志路径  %s\n已写入  %u\n丢弃记录  %u\n写入错误  %u\n模型后端  未安装",
                          edgedoctor_logger_path(),
                          (unsigned int)logger_stats.written,
                          (unsigned int)logger_stats.dropped,
                          (unsigned int)logger_stats.write_errors);
}

static const char *event_name(const ed_event_t *event)
{
    switch (event->kind) {
    case ED_EVENT_SENSOR: return "采集连接异常";
    case ED_EVENT_STORAGE: return "存储异常";
    case ED_EVENT_TREND: return "振动趋势变化";
    case ED_EVENT_MOUNT: return "安装状态变化";
    default: return event->severity == ED_SEVERITY_ALARM ? "振动偏离基线" : "振动预警";
    }
}

static const char *event_state_text(const ed_event_t *event)
{
    if (event->ack_pending) return "确认保存中";
    if (event->condition == ED_CONDITION_RECOVERED)
        return event->ack == ED_ACKNOWLEDGED ? "已恢复 · 已确认" : "已恢复 · 待确认";
    return event->ack == ED_ACKNOWLEDGED ? "已确认 · 仍异常" : "待确认";
}

static void refresh_events(void)
{
    unsigned i, pending = 0, visible = 0;
    uint8_t found = 0;
    selected_button(event_filter[0], !show_all_events);
    selected_button(event_filter[1], show_all_events);
    for (i = 0; i < ED_ACTIVE_EVENTS_MAX; ++i)
    {
        const ed_event_t *item = &monitor_view.events[i];
        uint8_t needs_action;
        uint32_t color;
        if (i >= monitor_view.event_count) {
            lv_obj_add_flag(event_rows[i], LV_OBJ_FLAG_HIDDEN); continue;
        }
        needs_action = item->condition == ED_CONDITION_ACTIVE || item->ack == ED_UNACKNOWLEDGED;
        if (needs_action) pending++;
        color = item->condition == ED_CONDITION_RECOVERED ? COLOR_MUTED :
            item->severity == ED_SEVERITY_ALARM ? COLOR_RED : COLOR_AMBER;
        if (show_all_events || needs_action) {
            visible++;
            lv_obj_remove_flag(event_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(event_rows[i], lv_color_hex(color), 0);
            lv_label_set_text(event_titles[i], event_name(item));
            lv_label_set_text_fmt(event_meta[i], "%s · 运行 %llu:%02llu · 详情 >",
                item->key.asset_id, (unsigned long long)(item->created_mono_us / 3600000000ULL),
                (unsigned long long)((item->created_mono_us / 60000000ULL) % 60ULL));
            lv_label_set_text(event_status[i], event_state_text(item));
            badge_style(event_status[i], color, item->condition == ED_CONDITION_RECOVERED ? 0xEDF0F4 :
                item->severity == ED_SEVERITY_ALARM ? 0xFDE7EB : 0xFFF1DE);
        } else lv_obj_add_flag(event_rows[i], LV_OBJ_FLAG_HIDDEN);
        if (selected_event[0] && strcmp(item->event_id, selected_event) == 0) {
            found = 1;
            lv_label_set_text_fmt(event_detail_text,
                "%s\n编号  %s\n状态  %s\n峰值偏离  %.2f\n基线版本  %u\n本地记录  %s\n证据回放尚未接入",
                event_name(item), item->event_id, event_state_text(item), (double)item->peak_deviation,
                (unsigned)item->baseline_version, item->durable ? "已保存" : "等待保存");
            if (ack_submitted && !item->ack_pending && item->ack != ED_ACKNOWLEDGED &&
                (uint32_t)(lv_tick_get() - ack_submitted_ms) > 5000U) {
                ack_submitted = 0;
                lv_label_set_text(event_action, "确认未保存或记录已更新，请重试");
            }
            if (item->ack == ED_ACKNOWLEDGED || item->ack_pending || ack_submitted) lv_obj_add_state(event_ack_button, LV_STATE_DISABLED);
            else lv_obj_remove_state(event_ack_button, LV_STATE_DISABLED);
            if (item->ack == ED_ACKNOWLEDGED) { ack_submitted = 0; lv_label_set_text(event_action, "确认已保存；恢复由监测结果判断"); }
        }
    }
    if (!found && selected_event[0]) {
        lv_label_set_text(event_detail_text, "此事件已不在本机保留记录中");
        lv_obj_add_state(event_ack_button, LV_STATE_DISABLED);
    }
    if (monitor_ready) lv_label_set_text_fmt(event_count_label, "%u 项待处理 · 本机保留 %u 条", pending, (unsigned)monitor_view.event_count);
    else lv_label_set_text(event_count_label, "事件记录暂不可用");
    if (visible) lv_obj_add_flag(event_empty, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_remove_flag(event_empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(event_empty, 0), monitor_ready ? "当前无待处理事件" : "等待监测服务");
    }
    /* Live event detail replaces replay-only classifier panels. */
    if (!demo_alarm_requested && !demo_data_enabled && !edgedoctor_app_is_replaying())
        lv_obj_add_flag(lv_obj_get_parent(ui.diagnosis_banner), LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lv_obj_get_parent(ui.diagnosis_banner), LV_OBJ_FLAG_HIDDEN);
}

static void history_capture(void)
{
    ui_history_t *bucket;
    uint64_t minute;
    float rms;
    if (!snapshot_ready || monitor_snapshot.source != ED_SOURCE_LIVE_IMU ||
        (history_started && history_snapshot_id == monitor_snapshot.snapshot_id)) return;
    history_started = 1;
    history_snapshot_id = monitor_snapshot.snapshot_id;
    minute = monitor_snapshot.mono_us / 60000000ULL;
    bucket = &history[minute % UI_HISTORY_CAPACITY];
    if (bucket->minute != minute || !bucket->total) {
        memset(bucket, 0, sizeof(*bucket)); bucket->minute = minute;
    }
    if (bucket->total < UINT16_MAX) bucket->total++;
    rms = monitor_snapshot.features.values[3];
    if (monitor_snapshot.quality != ED_QUALITY_VALID || !isfinite(rms) || rms < 0) return;
    if (bucket->valid < UINT16_MAX) {
        bucket->valid++; bucket->sum += rms;
        if (rms > bucket->peak) bucket->peak = rms;
        if (isfinite(monitor_snapshot.baseline_deviation) && monitor_snapshot.baseline_version) {
            if (bucket->baseline_version && bucket->baseline_version != monitor_snapshot.baseline_version) {
                bucket->deviation_valid = 0;
                bucket->deviation = 0;
            }
            bucket->baseline_version = monitor_snapshot.baseline_version;
            bucket->deviation_valid = 1;
            if (monitor_snapshot.baseline_deviation > bucket->deviation)
                bucket->deviation = monitor_snapshot.baseline_deviation;
        }
    }
}

static void refresh_trends(void)
{
    uint32_t count = trend_day ? 1440U : 60U, i, valid = 0, total = 0, present = 0;
    uint64_t now = (monitor_snapshot.mono_us +
        (uint64_t)(uint32_t)(lv_tick_get() - snapshot_received_ms) * 1000ULL) / 60000000ULL;
    float sum = 0, peak = 0, max_plot = trend_deviation ? 1.0f : 0.01f;
    int32_t *values, *baseline;
    float reference = monitor_ready && monitor_view.baseline_valid ? monitor_view.baseline.median[3] : 0;
    selected_button(trend_range[0], !trend_day); selected_button(trend_range[1], trend_day);
    selected_button(trend_metric[0], !trend_deviation); selected_button(trend_metric[1], trend_deviation);
    values = lv_chart_get_y_array(trend_chart, trend_series);
    baseline = lv_chart_get_y_array(trend_chart, trend_baseline);
    for (i = 0; i < 60U; ++i) {
        values[i] = LV_CHART_POINT_NONE;
        baseline[i] = LV_CHART_POINT_NONE;
    }
    for (i = 0; i < count; ++i) {
        uint64_t age = count - 1U - i;
        uint32_t point = i * 59U / (count - 1U);
        const ui_history_t *bucket;
        float value;
        if (age > now) continue;
        bucket = &history[(now - age) % UI_HISTORY_CAPACITY];
        if (bucket->minute != now - age || !bucket->total) continue;
        present++; total += bucket->total;
        if (!bucket->valid) continue;
        valid += bucket->valid; sum += bucket->sum;
        if (bucket->peak > peak) peak = bucket->peak;
        if (trend_deviation && !bucket->deviation_valid) continue;
        value = trend_deviation ? bucket->deviation : bucket->peak;
        if (value > max_plot) max_plot = value;
        if (values[point] == LV_CHART_POINT_NONE || value * 10000.0f > values[point])
            values[point] = (int32_t)fminf(value * 10000.0f, 1000000000.0f);
        if (!trend_deviation && monitor_view.baseline_valid && isfinite(reference) &&
            bucket->baseline_version == monitor_view.baseline.version) {
            baseline[point] = (int32_t)(reference * 10000.0f);
            if (reference > max_plot) max_plot = reference;
        }
    }
    lv_chart_set_range(trend_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)fminf(max_plot * 12000.0f, 1200000000.0f) + 1);
    lv_chart_refresh(trend_chart);
    lv_label_set_text_fmt(trend_note, "本次运行 · 分钟峰值 · %s\n纵轴 0–%.3f %s · 已记录 %u 分钟",
        trend_deviation ? "偏离度" : "蓝色当前 / 灰色当前基线", (double)(max_plot * 1.2f), trend_deviation ? "" : "g", (unsigned)present);
    if (valid) {
        lv_label_set_text_fmt(trend_mean, "RMS 均值\n%.3f g", (double)(sum / valid));
        lv_label_set_text_fmt(trend_peak, "RMS 峰值\n%.3f g", (double)peak);
    } else {
        lv_label_set_text(trend_mean, "RMS 均值\n—");
        lv_label_set_text(trend_peak, "RMS 峰值\n—");
    }
    if (total) lv_label_set_text_fmt(trend_valid, "窗口有效率\n%u%%", (unsigned)(valid * 100U / total));
    else lv_label_set_text(trend_valid, "窗口有效率\n—");
}

static void refresh_panels(void)
{
    ed_mqtt_state_t mqtt;
    ed_mqtt_ui_state_t transport;
    int status = ed_monitor_app_read_ui_state(&monitor_view);
    if (status > 0) monitor_ready = 1;
    else if (status < 0) monitor_ready = 0;
    status = ed_monitor_app_read_snapshot(&monitor_snapshot);
    if (status > 0) {
        if (!snapshot_ready || history_snapshot_id != monitor_snapshot.snapshot_id)
            snapshot_received_ms = lv_tick_get();
        snapshot_ready = 1;
    }
    history_capture();
    if ((uint32_t)(lv_tick_get() - panel_refresh_ms) < 1000U) return;
    panel_refresh_ms = lv_tick_get();
    lv_label_set_text(demo_button_label, demo_alarm_requested ? "结束报警演示" : "本地报警演示");
    refresh_events(); refresh_trends();
    if (snapshot_ready) {
        lv_label_set_text(asset_name, monitor_snapshot.key.asset_id);
        lv_label_set_text_fmt(asset_state, "板载监测 · %s", live_quality_text(monitor_snapshot.quality));
        badge_style(asset_state, monitor_snapshot.quality == ED_QUALITY_VALID ? COLOR_GREEN : COLOR_AMBER,
            monitor_snapshot.quality == ED_QUALITY_VALID ? 0xE6F5EB : 0xFFF1DE);
        lv_label_set_text_fmt(asset_mount, "安装版本  %u\n当前工况  %s", (unsigned)monitor_snapshot.key.mount_revision,
            monitor_snapshot.key.operating_profile_id);
    }
    if (monitor_ready) {
        if (monitor_view.baseline_timed_out)
            lv_label_set_text(baseline_progress, "采集超时，请检查工况与信号质量后重试");
        else if (monitor_view.baseline_active)
            lv_label_set_text_fmt(baseline_progress, "有效采集 %u / 60 个窗口；完成后自动保存",
                (unsigned)monitor_view.baseline_windows);
        else if (monitor_view.baseline_valid)
            lv_label_set_text(baseline_progress, "基线已保存；重新建立需保持稳定工况");
        if (monitor_view.baseline_valid) lv_obj_remove_state(baseline_view_button, LV_STATE_DISABLED);
        else lv_obj_add_state(baseline_view_button, LV_STATE_DISABLED);
        lv_label_set_text_fmt(baseline_details_text,
            "编号  %s\n有效窗口  %u\nRMS 中位数  %.4f g\nRMS 波动 MAD  %.4f g\n适用工况  %s",
            monitor_view.baseline.baseline_id, (unsigned)monitor_view.baseline.valid_windows,
            (double)monitor_view.baseline.median[3], (double)monitor_view.baseline.mad[3],
            monitor_view.baseline.key.operating_profile_id);
        lv_bar_set_value(baseline_bar, monitor_view.baseline_windows > 60U ? 60 : (int32_t)monitor_view.baseline_windows, LV_ANIM_OFF);
        lv_label_set_text_fmt(baseline_state, "运行基线 · %s", monitor_view.baseline_active ? "采集中" :
            monitor_view.baseline_timed_out ? "采集超时" : monitor_view.baseline_mismatch ? "不匹配" : monitor_view.baseline_valid ? "已生效" : "未建立");
        lv_label_set_text_fmt(baseline_info, "版本 B%u · 有效窗口 %u/60", (unsigned)monitor_view.baseline.version,
            (unsigned)monitor_view.baseline_windows);
        if (monitor_view.baseline_valid) {
            lv_label_set_text_fmt(baseline_rms, "%.4f g", (double)monitor_view.baseline.median[3]);
            lv_label_set_text_fmt(baseline_time, "建立时运行 %llu 分钟（该次启动）", (unsigned long long)(monitor_view.baseline.created.mono_us / 60000000ULL));
        } else { lv_label_set_text(baseline_rms, "—"); lv_label_set_text(baseline_time, "建立时间  —"); }
        if (monitor_view.baseline_active && monitor_view.baseline_windows < 60U) {
            lv_obj_add_state(baseline_button, LV_STATE_DISABLED);
            lv_label_set_text(baseline_button_label, "正在采集有效窗口");
        } else {
            lv_obj_remove_state(baseline_button, LV_STATE_DISABLED);
            lv_label_set_text(baseline_button_label, monitor_view.baseline_active ? "保存基线" :
                monitor_view.baseline_valid ? "重新建立" : "建立基线");
        }
    } else {
        lv_obj_add_state(baseline_button, LV_STATE_DISABLED);
        lv_obj_add_state(baseline_view_button, LV_STATE_DISABLED);
    }
    ed_mqtt_get_state(&mqtt);
    ed_mqtt_get_ui_state(&transport);
#if defined(RT_USING_WIFI) && !defined(ED_UI_HOST_PREVIEW)
    lv_label_set_text(wifi_state, rt_wlan_is_ready() ? "已连接 · 网络就绪" :
        rt_wlan_is_connected() ? "已连接 · 等待获取地址" : "未连接 · 本地监测继续");
#endif
    lv_label_set_text_fmt(mqtt_status, "%s\n服务器  %s\n待同步事件更新  %u 条", mqtt.connected ? "在线" : "未连接",
        transport.broker_host[0] ? transport.broker_host : "尚无连接记录", (unsigned)transport.pending_events);
    lv_obj_set_style_text_color(mqtt_status, lv_color_hex(mqtt.connected ? COLOR_GREEN : COLOR_AMBER), 0);
    lv_label_set_text(sync_steps, transport.event_receipts ?
        "本地记录 → 网络发送 → 服务端回执\n已有事件更新收到落库回执" :
        "本地记录 → 网络发送 → 服务端回执\n尚未收到事件落库回执");
    lv_label_set_text_fmt(sync_status, "本次事件回执  %u 条\n总待发送  %u 项（含遥测）",
        (unsigned)transport.event_receipts, (unsigned)mqtt.queued);
}

void edgedoctor_ui_process(void)
{
    uint32_t now;
    if (ui.root == NULL)
    {
        return;
    }
    consume_ui_commands();
    if (requested_tab != 0U)
    {
        uint8_t index = (uint8_t)(requested_tab - 1U);
        lv_tabview_set_active(ui.tabview, index, LV_ANIM_OFF);
        paint_navigation(index);
        requested_tab = 0U;
    }
    else
    {
        uint8_t index = (uint8_t)lv_tabview_get_tab_active(ui.tabview);
        if (index != ui.active_tab)
        {
            paint_navigation(index);
        }
    }
    now = lv_tick_get();
    if ((uint32_t)(now - ui.last_refresh_ms) < UI_REFRESH_MS)
    {
        return;
    }
    ui.last_refresh_ms = now;
    refresh_panels();
    refresh_result();
    refresh_spectrum();
    refresh_profile();
}

int edgedoctor_ui_stress_switch(uint32_t count, uint32_t *elapsed_ms)
{
    uint32_t started;
    uint32_t index;
    if ((ui.root == NULL) || (ui.tabview == NULL) || (count == 0U))
        return -1;
    started = lv_tick_get();
    for (index = 0U; index < count; ++index)
    {
        uint8_t tab = (uint8_t)(2U + (index % 4U));
        lv_tabview_set_active(ui.tabview, tab, LV_ANIM_OFF);
        paint_navigation(tab);
        lv_obj_update_layout(ui.root);
    }
    if (elapsed_ms != NULL)
        *elapsed_ms = (uint32_t)(lv_tick_get() - started);
    return 0;
}

static int edgeui(int argc, char **argv)
{
    if ((argc == 2) && (strcmp(argv[1], "alarm") == 0))
    {
        if (post_ui_command(ED_UI_COMMAND_ALARM, 1U) != 0)
            return -1;
        rt_kprintf("edgeui: critical alarm demonstration enabled\n");
        return 0;
    }
    if ((argc == 2) && (strcmp(argv[1], "ack") == 0))
    {
        if (post_ui_command(ED_UI_COMMAND_ACK, 1U) != 0)
            return -1;
        rt_kprintf("edgeui: alarm acknowledgement requested\n");
        return 0;
    }
    if ((argc == 2) && (strcmp(argv[1], "clear") == 0))
    {
        if (post_ui_command(ED_UI_COMMAND_ALARM, 0U) != 0)
            return -1;
        rt_kprintf("edgeui: alarm demonstration cleared\n");
        return 0;
    }
    if ((argc == 3) && (strcmp(argv[1], "demo") == 0) &&
        ((strcmp(argv[2], "on") == 0) || (strcmp(argv[2], "off") == 0)))
    {
        uint8_t enabled = (uint8_t)(strcmp(argv[2], "on") == 0);
        if (post_ui_command(ED_UI_COMMAND_DEMO, enabled) != 0)
            return -1;
        rt_kprintf("edgeui: demo data %s\n", enabled ? "enabled" : "disabled");
        return 0;
    }
    if ((argc == 3) && (strcmp(argv[1], "tab") == 0) &&
        (argv[2][0] >= '0') && (argv[2][0] <= '5') && (argv[2][1] == '\0'))
    {
        if (post_ui_command(ED_UI_COMMAND_TAB,
                            (uint8_t)(argv[2][0] - '0')) != 0)
            return -1;
        rt_kprintf("edgeui: tab %c requested\n", argv[2][0]);
        return 0;
    }
    if ((argc == 2) && (strcmp(argv[1], "info") == 0))
    {
        ed_snapshot_t snapshot;
        int live = ed_monitor_app_read_snapshot(&snapshot);
        rt_kprintf("edgeui: ready=%u nav=custom tabs=6 machine=440x148 pending_commands=%u live_snapshot=%u quality=%u\n",
                   ui_command_queue_ready ? 1U : 0U,
                   (unsigned)ui_command_queue.entry,
                   live > 0 ? 1U : 0U,
                   live > 0 ? (unsigned)snapshot.quality : 0U);
        return 0;
    }
    rt_kprintf("usage: edgeui info | edgeui tab 0..5 | edgeui demo on|off | edgeui alarm | edgeui ack | edgeui clear\n");
    return -1;
}
MSH_CMD_EXPORT(edgeui, EdgeDoctor industrial HMI control);
