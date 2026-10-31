#include "edgedoctor_ui.h"

#include "edgedoctor_app.h"
#include "ed_monitor_app.h"
#include "edgedoctor_backend.h"
#include "edgedoctor_logger.h"
#include "edgedoctor_machine_image.h"

#include <finsh.h>
#include <lvgl.h>
#include <math.h>
#include <rtthread.h>
#include <stdio.h>
#include <string.h>

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

static void baseline_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        (void)post_ui_command(ED_UI_COMMAND_BASELINE, 0U);
}

static void test_alarm_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        (void)post_ui_command(ED_UI_COMMAND_ALARM, 1U);
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

/* The remaining pages deliberately use the same restrained card language as the
 * existing overview.  Data binding is added incrementally; these pages are
 * useful on a real board even when the corresponding history is empty. */
static void build_events(lv_obj_t *page)
{
    lv_obj_t *card;
    lv_obj_t *legacy;
    build_header(page, 2U);
    make_section_title(page, "事件中心");
    card = make_card(page, LV_SIZE_CONTENT);
    make_label(card, "暂无未处理事件", COLOR_TEXT);
    make_label(card, "采集到异常后，事件和前后证据片段会显示在这里", COLOR_MUTED);
    make_label(card, "筛选   待处理   ·   全部", COLOR_BLUE);
    /* Keep the existing diagnosis widgets alive for the shared refresh path;
     * they are moved into the event detail page in the next UI slice. */
    legacy = lv_obj_create(page);
    lv_obj_set_size(legacy, 1, 1);
    lv_obj_add_flag(legacy, LV_OBJ_FLAG_HIDDEN);
    build_diagnosis(legacy);
}

static void build_trends(lv_obj_t *page)
{
    lv_obj_t *card;
    build_header(page, 3U);
    make_section_title(page, "历史趋势");
    card = make_card(page, 196);
    make_label(card, "RMS / 基线偏离", COLOR_TEXT);
    make_label(card, "暂无历史窗口 · 连接传感器后自动记录", COLOR_MUTED);
    make_label(card, "历史数据源未接通；1小时/24小时暂不可用", COLOR_MUTED);
    make_section_title(page, "维修前后对比");
    card = make_card(page, LV_SIZE_CONTENT);
    make_label(card, "尚未建立维修记录", COLOR_MUTED);
}

static void build_asset(lv_obj_t *page)
{
    lv_obj_t *card;
    lv_obj_t *button;
    lv_obj_t *label;
    build_header(page, 4U);
    make_section_title(page, "设备与基线");
    card = make_card(page, LV_SIZE_CONTENT);
    make_label(card, "设备  未命名设备", COLOR_TEXT);
    make_label(card, "安装位置  未设置\n传感器  板载 IMU · 208 Hz\n当前基线  尚未建立", COLOR_MUTED);
    button = lv_button_create(card);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_add_event_cb(button, baseline_button_event, LV_EVENT_CLICKED, NULL);
    label = make_label(button, "建立基线", COLOR_BLUE);
    lv_obj_center(label);
    make_label(card, "维修记录：后端未接通", COLOR_MUTED);
}

static void build_connection(lv_obj_t *page)
{
    lv_obj_t *card;
    lv_obj_t *button;
    lv_obj_t *label;
    lv_obj_t *legacy;
    build_header(page, 5U);
    make_section_title(page, "连接与系统");
    card = make_card(page, LV_SIZE_CONTENT);
    make_label(card, "Wi-Fi       未连接", COLOR_TEXT);
    make_label(card, "MQTT        未配置\n最近同步    —\n待发 revision  0", COLOR_MUTED);
    button = lv_button_create(card);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_add_event_cb(button, test_alarm_button_event, LV_EVENT_CLICKED, NULL);
    label = make_label(button, "发送测试告警", COLOR_BLUE);
    lv_obj_center(label);
    make_section_title(page, "诊断运行时");
    card = make_card(page, LV_SIZE_CONTENT);
    make_label(card, "CPU / DSP P95    等待真实采样\n存储与日志       查看系统页", COLOR_MUTED);
    legacy = lv_obj_create(page);
    lv_obj_set_size(legacy, 1, 1);
    lv_obj_add_flag(legacy, LV_OBJ_FLAG_HIDDEN);
    build_profiler(legacy);
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
    uint32_t baseline_windows = 0U;
    uint8_t baseline_active = 0U;
    uint8_t baseline_valid = 0U;
    uint8_t baseline_timed_out = 0U;
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
    if (ed_monitor_app_baseline_status(&baseline_windows, &baseline_active,
                                       &baseline_valid, &baseline_timed_out) == 0)
    {
        if (baseline_active)
            lv_label_set_text_fmt(ui.profile_storage, "基线采集中  %u/60 窗口",
                                  (unsigned)baseline_windows);
        else if (baseline_timed_out)
            lv_label_set_text(ui.profile_storage, "基线采集超时，请重新建立");
        else if (baseline_valid)
            lv_label_set_text(ui.profile_storage, "匹配基线已加载");
        else
            lv_label_set_text(ui.profile_storage, "尚未建立匹配基线");
    }
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
        set_header_state(edgedoctor_app_is_replaying() ? "正在回放" : "设备正常",
                         edgedoctor_app_is_replaying() ? COLOR_RED : COLOR_GREEN);
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
    refresh_result();
    refresh_spectrum();
    refresh_profile();
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
