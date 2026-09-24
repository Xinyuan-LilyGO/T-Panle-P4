#include "ui_internal.h"

#define ROUND_LORA_CONTENT_W UI_ROUND_MAIN_CONT_W
#define ROUND_LORA_CHAT_H 320
#define ROUND_LORA_MAX_MESSAGES 24

typedef struct
{
    lv_obj_t *chat_list;
    lv_obj_t *chat_placeholder;
    lv_obj_t *parameter_summary;
    lv_obj_t *composer;
    lv_obj_t *keyboard_layer;
    lv_obj_t *keyboard_input;
    lv_obj_t *keyboard;
    lv_obj_t *parameter_layer;
    lv_obj_t *rx_btn;
    lv_obj_t *rx_btn_label;
    lv_obj_t *periodic_btn;
    lv_obj_t *periodic_btn_label;
    uint16_t message_count;
    int last_control_mode;
    bool resume_rx_after_tx;
} round_lora_page_ui_t;

static round_lora_page_ui_t s_round_lora_ui;

static void lora_page_create(Page *page);
static void round_lora_tx_packet_append(const char *text, int state);
static void round_lora_rx_packet_append(const char *text, float rssi, float snr);
static void round_lora_chat_clear(void);
static void round_lora_params_changed(void);
static void round_lora_page_on_enter(bool init_failed);
static void round_lora_page_on_leave(void);
static void round_lora_page_on_destroy(void);
static void round_lora_page_on_timer(void);
static void round_lora_tx_finished(void);

#include "lora_page.h"

static const char *TAG = "[UI][lora_page]";

#define LORA_SUB_GHZ_MAX_POWER_DBM 22
#define LORA_2_4_GHZ_MAX_POWER_DBM 5

static lora_page_ui_t s_lora_ui;
static int s_lora_status = LORA_CONTROL_STOP;
static int s_lora_sf = LORA_SF_DEFAULT;
static int s_lora_cr = LORA_CR_DEFAULT;
static int s_lora_power_dbm = LORA_SUB_GHZ_MAX_POWER_DBM;
static int s_lora_tx_interval_s = LORA_TX_INTERVAL_MIN_S;
static float s_lora_bw = 125.0f;
static bool s_lora_mode_switch_pending = false;
static volatile bool s_lora_page_active = false;
static float s_lora_freq = LORA_FREQ_DEFAULT_X10;
static const float s_lora_freq_choices[] = {433.0, 868.0, 915.0, 923.0, 2400.0, 2450.0};
static const float s_lota_bw_choices[] = {62.5, 125.0, 250.0, 406.0, 500.0, 812.0, 1000.0};

#define LORA_HF_MIN_FREQUENCY_MHZ 1900.0f
#define LORA_HF_DEFAULT_BANDWIDTH_KHZ 406.0f

static int lora_page_max_power_for_frequency(float freq_mhz)
{
    int hardware_max = factory_lora_get_max_output_power(freq_mhz);
    int band_max = freq_mhz >= LORA_HF_MIN_FREQUENCY_MHZ
                       ? LORA_2_4_GHZ_MAX_POWER_DBM
                       : LORA_SUB_GHZ_MAX_POWER_DBM;
    return hardware_max < band_max ? hardware_max : band_max;
}

/*********lora Page************/
static void lora_param_event_cb(lv_event_t *e);
static void lora_freq_event_cb(lv_event_t *e);
static void lora_bw_event_cb(lv_event_t *e);
static void lora_page_create(Page *page);

static void lora_page_update_param_labels(void)
{
    if (s_lora_ui.sf_value_label)
    {
        lv_label_set_text_fmt(s_lora_ui.sf_value_label, "SF%d", s_lora_sf);
    }
    if (s_lora_ui.cr_value_label)
    {
        lv_label_set_text_fmt(s_lora_ui.cr_value_label, "4/%d", s_lora_cr);
    }
    if (s_lora_ui.power_value_label)
    {
        lv_label_set_text_fmt(s_lora_ui.power_value_label, "%d dBm", s_lora_power_dbm);
    }
    if (s_lora_ui.tx_interval_value_label)
    {
        lv_label_set_text_fmt(s_lora_ui.tx_interval_value_label, "%d s", s_lora_tx_interval_s);
    }
    round_lora_params_changed();
}

static uint32_t lora_choice_index_from_value(const float *choices, size_t count, float value, uint32_t fallback)
{

    if (choices == NULL || count == 0)
    {
        return fallback;
    }

    uint32_t best = fallback < count ? fallback : 0;
    float best_diff = fabsf(choices[best] - value);
    for (size_t i = 0; i < count; i++)
    {
        float diff = fabsf(choices[i] - value);
        if (diff < best_diff)
        {
            best = (uint32_t)i;
            best_diff = diff;
        }
    }
    return best;
}

static void lora_page_timestamp(char *buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year >= 120)
    {
        snprintf(buf, len, "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        return;
    }

    int64_t uptime_ms = esp_timer_get_time() / 1000;
    snprintf(buf, len, "%" PRId64 "ms", uptime_ms);
}

static void lora_page_ui_update(const char *mode, uint32_t mode_color,
                                const char *status, uint32_t status_color,
                                const char *log_text)
{
    if (!s_lora_page_active)
    {
        return;
    }

    ui_lock();
    if (mode && s_lora_ui.mode_label)
    {
        lv_label_set_text(s_lora_ui.mode_label, mode);
        lv_obj_set_style_text_color(s_lora_ui.mode_label, lv_color_hex(mode_color), 0);
    }
    if (status && s_lora_ui.status_label)
    {
        lv_label_set_text(s_lora_ui.status_label, status);
        lv_obj_set_style_text_color(s_lora_ui.status_label, lv_color_hex(status_color), 0);
    }
    if (log_text && s_lora_ui.lora_log)
    {
        lv_label_set_text(s_lora_ui.lora_log, log_text);
    }
    ui_unlock();
}

static lv_obj_t *lora_param_slider_create(lv_obj_t *parent, const char *name, int min, int max, int value, lora_param_t param)
{
    lv_obj_t *row = ui_flex_container_create(parent,
                                             LV_PCT(100),
                                             48,
                                             LV_FLEX_FLOW_COLUMN,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_CENTER,
                                             LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(row, 4, 0);

    lv_obj_t *head = ui_flex_container_create(row,
                                              LV_PCT(90),
                                              18,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_label = ui_label_create(head, name, &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_width(name_label, 110);

    ui_flex_spacer_create(head);

    lv_obj_t *value_label = ui_label_create(head, "", &lv_font_montserrat_14, UI_TEXT);
    lv_obj_set_width(value_label, 110);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, LV_PCT(90), 12);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(param == LORA_PARAM_POWER ? UI_SECONDARY : UI_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_90, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_WARN), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(slider, lv_color_hex(UI_WARN), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, lora_param_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)param);

    switch (param)
    {
    case LORA_PARAM_SF:
        s_lora_ui.sf_slider = slider;
        s_lora_ui.sf_value_label = value_label;
        break;
    case LORA_PARAM_CR:
        s_lora_ui.cr_slider = slider;
        s_lora_ui.cr_value_label = value_label;
        break;
    case LORA_PARAM_POWER:
        s_lora_ui.power_slider = slider;
        s_lora_ui.power_value_label = value_label;
        break;
    case LORA_PARAM_TX_INTERVAL:
        s_lora_ui.tx_interval_slider = slider;
        s_lora_ui.tx_interval_value_label = value_label;
        break;
    }

    return slider;
}

static lv_obj_t *lora_textarea_create(lv_obj_t *parent, int32_t w, int32_t h, const char *text, bool one_line)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, h);
    lv_textarea_set_text(ta, text);
    lv_textarea_set_max_length(ta, one_line ? LORA_TEXT_MAX_LEN - 1 : 1800);
    lv_textarea_set_one_line(ta, one_line);
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_60, 0);
    lv_obj_set_style_radius(ta, 26, 0);
    lv_obj_set_style_shadow_width(ta, 12, 0);
    lv_obj_set_style_shadow_color(ta, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_shadow_opa(ta, LV_OPA_20, 0);
    lv_obj_set_style_pad_left(ta, 18, 0);
    lv_obj_set_style_pad_right(ta, 18, 0);
    lv_obj_set_style_pad_top(ta, 8, 0);
    lv_obj_set_style_pad_bottom(ta, 8, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    return ta;
}

const char *lora_page_chip_name(lora_app_chip_t chip)
{
    switch (chip)
    {
    case LORA_APP_CHIP_SX1262:
        return "SX1262";
    case LORA_APP_CHIP_SX1276:
        return "SX1276";
    case LORA_APP_CHIP_LR1121:
        return "LR1121";
    case LORA_APP_CHIP_LR2021:
        return "LR2021";
    default:
        return "UNKNOWN";
    }
}

static void lora_param_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *slider = lv_event_get_target_obj(e);
    lora_param_t param = (lora_param_t)(intptr_t)lv_event_get_user_data(e);
    int value = (int)lv_slider_get_value(slider);

    switch (param)
    {
    case LORA_PARAM_SF:
        s_lora_sf = value;
        factory_lora_set_spreading_factor((uint8_t)s_lora_sf);
        break;
    case LORA_PARAM_CR:
        s_lora_cr = value;
        factory_lora_set_coding_rate((uint8_t)s_lora_cr);
        break;
    case LORA_PARAM_POWER:
        s_lora_power_dbm = value;
        factory_lora_set_output_power((int8_t)s_lora_power_dbm);
        break;
    case LORA_PARAM_TX_INTERVAL:
        s_lora_tx_interval_s = value;
        factory_lora_set_tx_interval(s_lora_tx_interval_s);
        break;
    }
    lora_page_update_param_labels();
    ESP_LOGI(TAG, "LORA param changed: SF:%d CR:%d POWER:%d,Time:%ds", s_lora_sf, s_lora_cr, s_lora_power_dbm, s_lora_tx_interval_s);
}

static void lora_freq_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    uint32_t selected = s_lora_ui.freq_dropdown ? lv_dropdown_get_selected(s_lora_ui.freq_dropdown) : LORA_FREQ_DEFAULT_INDEX;
    if (selected >= (uint32_t)(sizeof(s_lora_freq_choices) / sizeof(s_lora_freq_choices[0])))
    {
        selected = LORA_FREQ_DEFAULT_INDEX;
    }

    s_lora_freq = s_lora_freq_choices[selected];
    if (s_lora_freq >= LORA_HF_MIN_FREQUENCY_MHZ &&
        s_lora_bw < LORA_HF_DEFAULT_BANDWIDTH_KHZ)
    {
        s_lora_bw = LORA_HF_DEFAULT_BANDWIDTH_KHZ;
        if (s_lora_ui.bw_dropdown)
        {
            lv_dropdown_set_selected(
                s_lora_ui.bw_dropdown,
                lora_choice_index_from_value(s_lota_bw_choices,
                                             sizeof(s_lota_bw_choices) / sizeof(s_lota_bw_choices[0]),
                                             s_lora_bw, LORA_BW_DEFAULT_INDEX));
        }
    }
    int max_power = lora_page_max_power_for_frequency(s_lora_freq);
    s_lora_power_dbm = max_power;
    if (s_lora_ui.power_slider)
    {
        lv_slider_set_range(s_lora_ui.power_slider, LORA_POWER_MIN_DBM, max_power);
        lv_slider_set_value(s_lora_ui.power_slider, s_lora_power_dbm, LV_ANIM_OFF);
    }
    lora_page_update_param_labels();

    int state = factory_lora_set_frequency(s_lora_freq);
    if (state == 0)
    {
        factory_lora_set_output_power((int8_t)s_lora_power_dbm);
    }
    ESP_LOGI(TAG, "LORA frequency selected:%u freq:%.1fMHz max_power:%ddBm state:%d",
             (unsigned)selected, (double)s_lora_freq, max_power, state);
}

static void lora_bw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    uint32_t selected = s_lora_ui.bw_dropdown ? lv_dropdown_get_selected(s_lora_ui.bw_dropdown) : LORA_BW_DEFAULT_INDEX;
    if (selected >= (uint32_t)(sizeof(s_lota_bw_choices) / sizeof(s_lota_bw_choices[0])))
    {
        selected = LORA_BW_DEFAULT_INDEX;
    }

    s_lora_bw = s_lota_bw_choices[selected];
    ESP_LOGI(TAG, "LORA bandwidth changed to %.1f kHz", (double)s_lora_bw);
    factory_lora_set_bandwidth(s_lora_bw);
}

static factory_lora_mode_t lora_page_to_service_mode(lora_control_action_t action)
{
    switch (action)
    {
    case LORA_CONTROL_TX:
        return FACTORY_LORA_MODE_TX;
    case LORA_CONTROL_RX:
        return FACTORY_LORA_MODE_RX;
    case LORA_CONTROL_TX_ONCE:
        return FACTORY_LORA_MODE_TX_ONCE;
    default:
        return FACTORY_LORA_MODE_STOP;
    }
}

static int lora_page_from_service_mode(factory_lora_mode_t mode)
{
    switch (mode)
    {
    case FACTORY_LORA_MODE_TX:
        return LORA_CONTROL_TX;
    case FACTORY_LORA_MODE_RX:
        return LORA_CONTROL_RX;
    case FACTORY_LORA_MODE_TX_ONCE:
        return LORA_CONTROL_TX_ONCE;
    default:
        return LORA_CONTROL_STOP;
    }
}

static void lora_page_service_status(factory_lora_mode_t mode, int state,
                                     const char *message, void *user_data)
{
    (void)user_data;
    s_lora_status = lora_page_from_service_mode(mode);
    const char *mode_text = mode == FACTORY_LORA_MODE_RX ? "RX" :
                            mode == FACTORY_LORA_MODE_TX ||
                            mode == FACTORY_LORA_MODE_TX_ONCE ? "TX" : "STBY";
    uint32_t color = state == 0 ?
                     (mode == FACTORY_LORA_MODE_RX ? UI_OK :
                      mode == FACTORY_LORA_MODE_STOP ? UI_MUTED : UI_PRIMARY) :
                     UI_ERROR;
    lora_page_ui_update(mode_text, color, NULL, color, message);
}

static void lora_page_service_tx_done(const char *text, int state, void *user_data)
{
    (void)user_data;
    char log_text[48];
    snprintf(log_text, sizeof(log_text), "transmit state:%d", state);
    lora_page_ui_update("TX", state == 0 ? UI_PRIMARY : UI_ERROR,
                        NULL, state == 0 ? UI_PRIMARY : UI_ERROR,
                        log_text);
    round_lora_tx_packet_append(text, state);
}

static void lora_page_service_rx_done(const char *text, float rssi, float snr,
                                      int state, void *user_data)
{
    (void)user_data;
    char log_text[64];
    snprintf(log_text, sizeof(log_text), "RX readData state:%d", state);
    lora_page_ui_update("RX", state == 0 ? UI_OK : UI_ERROR,
                        NULL, state == 0 ? UI_PRIMARY : UI_ERROR,
                        log_text);
    if (state != 0)
    {
        return;
    }

    round_lora_rx_packet_append(text, rssi, snr);
    ui_lock();
    if (s_lora_ui.lora_rssi_snr_label)
    {
        lv_label_set_text_fmt(s_lora_ui.lora_rssi_snr_label,
                              "RSSI %.1f / SNR %.1f",
                              (double)rssi, (double)snr);
        lv_obj_set_style_text_color(s_lora_ui.lora_rssi_snr_label,
                                    lv_color_hex(UI_TEXT), 0);
    }
    ui_unlock();
}

static void lora_page_service_tx_finished(void *user_data)
{
    (void)user_data;
    round_lora_tx_finished();
}

static void lora_page_bind_service_callbacks(void)
{
    const factory_lora_callbacks_t callbacks = {
        .status = lora_page_service_status,
        .tx_done = lora_page_service_tx_done,
        .rx_done = lora_page_service_rx_done,
        .tx_finished = lora_page_service_tx_finished,
        .user_data = NULL,
    };
    factory_lora_set_callbacks(&callbacks);
}

static void lora_page_request_mode(lora_control_action_t action)
{
    factory_lora_request_mode(lora_page_to_service_mode(action));
    s_lora_status = LORA_CONTROL_STOP;
    s_lora_mode_switch_pending = true;
}

static void lora_page_process_mode_switch(void)
{
    factory_lora_process();
    s_lora_status = lora_page_from_service_mode(factory_lora_get_mode());
    s_lora_mode_switch_pending = factory_lora_is_switch_pending();
}

static bool lora_page_send_once_text(const char *text)
{
    return factory_lora_send_once(text);
}

static void lora_page_enter(Page *page)
{
    (void)page;
    lora_page_bind_service_callbacks();
    factory_lora_set_tx_interval(s_lora_tx_interval_s);
    char buf[64];
    bool lora_init_failed = (s_init_error & INIT_LORA_ERROR) != 0;
    snprintf(buf, sizeof(buf), "%s", lora_init_failed ? "INIT ERROR" : "INIT OK");
    lora_page_ui_update("STBY", UI_MUTED, buf, lora_init_failed ? UI_ERROR : UI_OK, NULL);
    round_lora_page_on_enter(lora_init_failed);
}

static void lora_page_timer(Page *page)
{
    (void)page;
    lora_page_process_mode_switch();
    round_lora_page_on_timer();
}

static void lora_page_leave(Page *page)
{
    s_lora_status = LORA_CONTROL_STOP;
    s_lora_mode_switch_pending = false;
    s_lora_page_active = false;
    round_lora_page_on_leave();
    factory_lora_stop();
    factory_lora_set_callbacks(NULL);
    if (page)
    {
        lv_async_call(app_page_destroy_async_cb, (void *)(uintptr_t)page->id);
    }
}

static void lora_page_destroy(Page *page)
{
    (void)page;
    s_lora_page_active = false;
    round_lora_page_on_destroy();
    memset(&s_lora_ui, 0, sizeof(s_lora_ui));
}

void lora_page_register(void)
{
    Page page = {
        .id = PAGE_LORA,
        .name = "lora",
        .on_create = lora_page_create,
        .on_enter = lora_page_enter,
        .on_leave = lora_page_leave,
        .on_destroy = lora_page_destroy,
        .on_gesture = app_page_back_gesture,
        .page_timer = lora_page_timer,
        .timer_interval_ms = LORA_RX_POLL_MS,
    };
    ui_page_register(&page);
}

static void round_lora_panel_style(lv_obj_t *obj, uint32_t bg_color, int radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_20, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_shadow_width(obj, 16, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_20, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *round_lora_button_create(lv_obj_t *parent, int width,
                                          const char *symbol, const char *text,
                                          uint32_t color, lv_event_cb_t event_cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, width, 48);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 12, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text_fmt(label, "%s  %s", symbol, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *round_lora_icon_button_create(lv_obj_t *parent, const char *symbol,
                                               uint32_t color, lv_event_cb_t event_cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 42, 42);
    lv_obj_set_style_radius(btn, 21, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = ui_label_create(btn, symbol, &lv_font_montserrat_20, color);
    lv_obj_center(label);
    return btn;
}

static void round_lora_chat_placeholder_create(void)
{
    if (s_round_lora_ui.chat_list == NULL)
    {
        return;
    }

    s_round_lora_ui.chat_placeholder = lv_label_create(s_round_lora_ui.chat_list);
    lv_obj_set_width(s_round_lora_ui.chat_placeholder, LV_PCT(100));
    lv_label_set_text(s_round_lora_ui.chat_placeholder, "Waiting for LoRa packets");
    lv_obj_set_style_text_font(s_round_lora_ui.chat_placeholder, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_round_lora_ui.chat_placeholder, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_round_lora_ui.chat_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s_round_lora_ui.chat_placeholder, 88, 0);
}

static void round_lora_chat_append_locked(bool outgoing, const char *text,
                                          bool has_radio_meta, float rssi, float snr,
                                          bool success)
{
    if (s_round_lora_ui.chat_list == NULL || text == NULL || text[0] == '\0')
    {
        return;
    }

    if (s_round_lora_ui.chat_placeholder)
    {
        lv_obj_delete(s_round_lora_ui.chat_placeholder);
        s_round_lora_ui.chat_placeholder = NULL;
    }

    while (s_round_lora_ui.message_count >= ROUND_LORA_MAX_MESSAGES)
    {
        lv_obj_t *oldest = lv_obj_get_child(s_round_lora_ui.chat_list, 0);
        if (oldest == NULL)
        {
            s_round_lora_ui.message_count = 0;
            break;
        }
        lv_obj_delete(oldest);
        s_round_lora_ui.message_count--;
    }

    lv_obj_t *row = ui_flex_container_create(s_round_lora_ui.chat_list,
                                             LV_PCT(100), LV_SIZE_CONTENT,
                                             LV_FLEX_FLOW_ROW,
                                             outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_START,
                                             LV_FLEX_ALIGN_START);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, 382, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(outgoing ? 0x12383E : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_border_color(bubble,
                                  lv_color_hex(outgoing ? UI_PRIMARY : UI_LINE), 0);
    lv_obj_set_style_border_opa(bubble, LV_OPA_40, 0);
    lv_obj_set_style_radius(bubble, 18, 0);
    lv_obj_set_style_shadow_width(bubble, 10, 0);
    lv_obj_set_style_shadow_color(bubble,
                                  lv_color_hex(outgoing ? UI_PRIMARY : UI_SECONDARY), 0);
    lv_obj_set_style_shadow_opa(bubble, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_row(bubble, 5, 0);
    ui_obj_set_flex(bubble, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    char timestamp[24] = {0};
    lora_page_timestamp(timestamp, sizeof(timestamp));
    lv_obj_t *meta = lv_label_create(bubble);
    lv_obj_set_width(meta, LV_PCT(100));
    if (has_radio_meta)
    {
        lv_label_set_text_fmt(meta, "RX  %s   RSSI %.1f   SNR %.1f",
                              timestamp, (double)rssi, (double)snr);
    }
    else
    {
        lv_label_set_text_fmt(meta, "TX  %s   %s", timestamp, success ? "SENT" : "FAILED");
    }
    lv_obj_set_style_text_font(meta, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(meta,
                                lv_color_hex(success ? (outgoing ? UI_PRIMARY : UI_OK) : UI_ERROR), 0);

    lv_obj_t *message = lv_label_create(bubble);
    lv_obj_set_width(message, 360);
    lv_label_set_text(message, text);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(message, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(message, lv_color_hex(UI_TEXT), 0);

    s_round_lora_ui.message_count++;
    lv_obj_update_layout(s_round_lora_ui.chat_list);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

static void round_lora_chat_append_from_task(bool outgoing, const char *text,
                                             bool has_radio_meta, float rssi, float snr,
                                             bool success)
{
    if (!s_lora_page_active)
    {
        return;
    }

    ui_lock();
    if (s_lora_page_active)
    {
        round_lora_chat_append_locked(outgoing, text, has_radio_meta, rssi, snr, success);
    }
    ui_unlock();
}

static void round_lora_tx_packet_append(const char *text, int state)
{
    round_lora_chat_append_from_task(true, text, false, 0.0f, 0.0f, state == 0);
}

static void round_lora_rx_packet_append(const char *text, float rssi, float snr)
{
    round_lora_chat_append_from_task(false, text, true, rssi, snr, true);
}

static void round_lora_chat_clear(void)
{
    if (s_round_lora_ui.chat_list == NULL)
    {
        return;
    }
    lv_obj_clean(s_round_lora_ui.chat_list);
    s_round_lora_ui.message_count = 0;
    s_round_lora_ui.chat_placeholder = NULL;
    round_lora_chat_placeholder_create();
}

static void round_lora_params_changed(void)
{
    if (s_round_lora_ui.parameter_summary == NULL)
    {
        return;
    }

    lv_label_set_text_fmt(s_round_lora_ui.parameter_summary,
                          "%.1f MHz  BW %.1f  SF%d  4/%d  %d dBm",
                          (double)s_lora_freq, (double)s_lora_bw,
                          s_lora_sf, s_lora_cr, s_lora_power_dbm);
}

static void round_lora_keyboard_hide(void)
{
    if (s_round_lora_ui.keyboard)
    {
        lv_keyboard_set_textarea(s_round_lora_ui.keyboard, NULL);
    }
    if (s_round_lora_ui.keyboard_layer)
    {
        lv_obj_add_flag(s_round_lora_ui.keyboard_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void round_lora_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)
    {
        if (s_round_lora_ui.composer && s_round_lora_ui.keyboard_input)
        {
            lv_textarea_set_text(s_round_lora_ui.composer,
                                 lv_textarea_get_text(s_round_lora_ui.keyboard_input));
        }
        round_lora_keyboard_hide();
    }
    else if (code == LV_EVENT_CANCEL)
    {
        round_lora_keyboard_hide();
    }
}

static void round_lora_composer_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_round_lora_ui.keyboard_layer == NULL)
    {
        return;
    }

    lv_textarea_set_text(s_round_lora_ui.keyboard_input,
                         lv_textarea_get_text(s_round_lora_ui.composer));
    lv_keyboard_set_textarea(s_round_lora_ui.keyboard, s_round_lora_ui.keyboard_input);
    lv_obj_clear_flag(s_round_lora_ui.keyboard_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_round_lora_ui.keyboard_layer);
}

static void round_lora_send_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_round_lora_ui.composer == NULL)
    {
        return;
    }

    const char *text = lv_textarea_get_text(s_round_lora_ui.composer);
    if (text == NULL || text[0] == '\0')
    {
        if (s_lora_ui.lora_log)
        {
            lv_label_set_text(s_lora_ui.lora_log, "Message is empty");
            lv_obj_set_style_text_color(s_lora_ui.lora_log, lv_color_hex(UI_WARN), 0);
        }
        return;
    }

    s_round_lora_ui.resume_rx_after_tx =
        (s_lora_status == LORA_CONTROL_RX) ||
        (s_lora_mode_switch_pending &&
         factory_lora_get_pending_mode() == FACTORY_LORA_MODE_RX);
    if (lora_page_send_once_text(text))
    {
        lv_textarea_set_text(s_round_lora_ui.composer, "");
    }
}

static void round_lora_rx_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    lora_page_request_mode(s_lora_status == LORA_CONTROL_RX ? LORA_CONTROL_STOP : LORA_CONTROL_RX);
}

static void round_lora_periodic_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    s_round_lora_ui.resume_rx_after_tx = false;
    lora_page_request_mode(s_lora_status == LORA_CONTROL_TX ? LORA_CONTROL_STOP : LORA_CONTROL_TX);
}

static void round_lora_stop_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        s_round_lora_ui.resume_rx_after_tx = false;
        lora_page_request_mode(LORA_CONTROL_STOP);
    }
}

static void round_lora_parameter_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && s_round_lora_ui.parameter_layer)
    {
        lv_obj_add_flag(s_round_lora_ui.parameter_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void round_lora_parameter_open_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && s_round_lora_ui.parameter_layer)
    {
        lv_obj_clear_flag(s_round_lora_ui.parameter_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_round_lora_ui.parameter_layer);
    }
}

static void round_lora_clear_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        round_lora_chat_clear();
    }
}

static void round_lora_keyboard_create(lv_obj_t *root)
{
    s_round_lora_ui.keyboard_layer = lv_obj_create(root);
    lv_obj_set_size(s_round_lora_ui.keyboard_layer, 720, 680);
    lv_obj_align(s_round_lora_ui.keyboard_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_round_lora_ui.keyboard_layer, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_round_lora_ui.keyboard_layer, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_round_lora_ui.keyboard_layer, 1, 0);
    lv_obj_set_style_border_color(s_round_lora_ui.keyboard_layer, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_round_lora_ui.keyboard_layer, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_round_lora_ui.keyboard_layer, 28, 0);
    lv_obj_set_style_shadow_width(s_round_lora_ui.keyboard_layer, 24, 0);
    lv_obj_set_style_shadow_color(s_round_lora_ui.keyboard_layer, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(s_round_lora_ui.keyboard_layer, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(s_round_lora_ui.keyboard_layer, 14, 0);
    lv_obj_remove_flag(s_round_lora_ui.keyboard_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label_create(s_round_lora_ui.keyboard_layer,
                                      "COMPOSE MESSAGE", &lv_font_montserrat_20, UI_PRIMARY);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -190);

    s_round_lora_ui.keyboard_input = lora_textarea_create(s_round_lora_ui.keyboard_layer,
                                                          500, 52, "", true);
    lv_textarea_set_placeholder_text(s_round_lora_ui.keyboard_input, "Enter LoRa message");
    lv_obj_align(s_round_lora_ui.keyboard_input, LV_ALIGN_CENTER, 0, -142);

    s_round_lora_ui.keyboard = lv_keyboard_create(s_round_lora_ui.keyboard_layer);
    lv_obj_set_size(s_round_lora_ui.keyboard, 520, 270);
    lv_obj_align(s_round_lora_ui.keyboard, LV_ALIGN_CENTER, 0, 52);
    lv_keyboard_set_mode(s_round_lora_ui.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_round_lora_ui.keyboard, s_round_lora_ui.keyboard_input);
    lv_obj_set_style_bg_color(s_round_lora_ui.keyboard, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s_round_lora_ui.keyboard, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_round_lora_ui.keyboard, 1, 0);
    lv_obj_set_style_border_color(s_round_lora_ui.keyboard, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_round_lora_ui.keyboard, LV_OPA_30, 0);
    lv_obj_set_style_radius(s_round_lora_ui.keyboard, 22, 0);
    lv_obj_set_style_shadow_width(s_round_lora_ui.keyboard, 14, 0);
    lv_obj_set_style_shadow_color(s_round_lora_ui.keyboard, lv_color_hex(UI_SECONDARY), 0);
    lv_obj_set_style_shadow_opa(s_round_lora_ui.keyboard, LV_OPA_20, 0);
    lv_obj_add_event_cb(s_round_lora_ui.keyboard, round_lora_keyboard_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_add_flag(s_round_lora_ui.keyboard_layer, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *round_lora_dropdown_create(lv_obj_t *parent, const char *options,
                                            uint32_t selected, lv_event_cb_t event_cb)
{
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_obj_set_size(dropdown, 222, 44);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_60, 0);
    lv_obj_set_style_border_width(dropdown, 1, 0);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(dropdown, LV_OPA_30, 0);
    lv_obj_set_style_radius(dropdown, 20, 0);
    lv_obj_set_style_shadow_width(dropdown, 8, 0);
    lv_obj_set_style_shadow_color(dropdown, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(dropdown, LV_OPA_20, 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(UI_TEXT), 0);
    lv_obj_add_event_cb(dropdown, event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dropdown;
}

static void round_lora_parameter_dialog_create(lv_obj_t *root)
{
    s_round_lora_ui.parameter_layer = lv_obj_create(root);
    lv_obj_set_size(s_round_lora_ui.parameter_layer, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(s_round_lora_ui.parameter_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_round_lora_ui.parameter_layer, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_round_lora_ui.parameter_layer, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_round_lora_ui.parameter_layer, 1, 0);
    lv_obj_set_style_border_color(s_round_lora_ui.parameter_layer, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_border_opa(s_round_lora_ui.parameter_layer, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(s_round_lora_ui.parameter_layer, 10, 0);
    lv_obj_remove_flag(s_round_lora_ui.parameter_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(s_round_lora_ui.parameter_layer);
    lv_obj_set_size(panel, UI_ROUND_POPUP_SIZE, UI_ROUND_POPUP_SIZE);
    lv_obj_set_pos(panel, UI_ROUND_POPUP_X, UI_ROUND_POPUP_Y);
    round_lora_panel_style(panel, UI_PANEL, 12);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    ui_obj_set_flex(panel, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = ui_flex_container_create(panel, LV_PCT(100), 42,
                                              LV_FLEX_FLOW_ROW,
                                              LV_FLEX_ALIGN_START,
                                              LV_FLEX_ALIGN_CENTER,
                                              LV_FLEX_ALIGN_CENTER);
    ui_label_create(head, "RADIO PARAMETERS", &lv_font_montserrat_20, UI_PRIMARY);
    ui_flex_spacer_create(head);
    round_lora_icon_button_create(head, LV_SYMBOL_CLOSE, UI_MUTED,
                                  round_lora_parameter_close_event_cb);

    lv_obj_t *dropdown_row = ui_flex_container_create(panel, LV_PCT(100), 48,
                                                      LV_FLEX_FLOW_ROW,
                                                      LV_FLEX_ALIGN_SPACE_BETWEEN,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER);
    s_lora_ui.freq_dropdown = round_lora_dropdown_create(
        dropdown_row,
        "433 MHz\n868 MHz\n915 MHz\n923 MHz\n2400 MHz\n2450 MHz",
        lora_choice_index_from_value(s_lora_freq_choices,
                                     sizeof(s_lora_freq_choices) / sizeof(s_lora_freq_choices[0]),
                                     s_lora_freq, LORA_FREQ_DEFAULT_INDEX),
        lora_freq_event_cb);
    s_lora_ui.bw_dropdown = round_lora_dropdown_create(
        dropdown_row,
        "62.5 kHz\n125 kHz\n250 kHz\n406 kHz\n500 kHz\n812 kHz\n1000 kHz",
        lora_choice_index_from_value(s_lota_bw_choices,
                                     sizeof(s_lota_bw_choices) / sizeof(s_lota_bw_choices[0]),
                                     s_lora_bw, LORA_BW_DEFAULT_INDEX),
        lora_bw_event_cb);

    lora_param_slider_create(panel, "SF", LORA_SF_MIN, LORA_SF_MAX,
                             s_lora_sf, LORA_PARAM_SF);
    lora_param_slider_create(panel, "CR", LORA_CR_MIN, LORA_CR_MAX,
                             s_lora_cr, LORA_PARAM_CR);
    lora_param_slider_create(panel, "POWER", LORA_POWER_MIN_DBM,
                             lora_page_max_power_for_frequency(s_lora_freq),
                             s_lora_power_dbm, LORA_PARAM_POWER);
    lora_param_slider_create(panel, "TX INTERVAL", LORA_TX_INTERVAL_MIN_S,
                             LORA_TX_INTERVAL_MAX_S, s_lora_tx_interval_s,
                             LORA_PARAM_TX_INTERVAL);
    lora_page_update_param_labels();

    lv_obj_t *hint = ui_label_create(panel,
                                     "Changes apply immediately",
                                     &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_add_flag(s_round_lora_ui.parameter_layer, LV_OBJ_FLAG_HIDDEN);
}

static void lora_page_create(Page *page)
{
    memset(&s_round_lora_ui, 0, sizeof(s_round_lora_ui));
    s_round_lora_ui.last_control_mode = -1;
    s_lora_ui.root = page->root;
    s_lora_page_active = true;

    ui_background_create(page->root);

    lv_obj_set_style_bg_color(s_lora_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_scrollbar_mode(s_lora_ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(s_lora_ui.root, 0, 0);
    lv_obj_add_flag(s_lora_ui.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_lora_ui.root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_cont = lv_obj_create(s_lora_ui.root);
    ui_main_cont_style_init(main_cont);
    lv_obj_set_style_pad_row(main_cont, 7, 0);

    lv_obj_t *status_strip = ui_flex_container_create(main_cont, ROUND_LORA_CONTENT_W, 48,
                                                      LV_FLEX_FLOW_ROW,
                                                      LV_FLEX_ALIGN_START,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER);
    round_lora_panel_style(status_strip, UI_PANEL, 8);
    lv_obj_set_style_pad_left(status_strip, 12, 0);
    lv_obj_set_style_pad_right(status_strip, 12, 0);
    lv_obj_set_style_pad_column(status_strip, 10, 0);

    s_lora_ui.mode_label = ui_label_create(status_strip, "STBY", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(s_lora_ui.mode_label, 42);
    s_lora_ui.lora_log = ui_label_create(status_strip, "READY", &lv_font_montserrat_12, UI_MUTED);
    lv_obj_set_width(s_lora_ui.lora_log, 96);
    lv_label_set_long_mode(s_lora_ui.lora_log, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(s_lora_ui.mode_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lora_ui.lora_log, LV_OBJ_FLAG_HIDDEN);
    s_round_lora_ui.parameter_summary = ui_label_create(status_strip, "", &lv_font_montserrat_12, UI_TEXT);
    lv_obj_set_width(s_round_lora_ui.parameter_summary, 390);
    lv_label_set_long_mode(s_round_lora_ui.parameter_summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_round_lora_ui.parameter_summary, LV_TEXT_ALIGN_LEFT, 0);
    ui_flex_spacer_create(status_strip);
    round_lora_icon_button_create(status_strip, LV_SYMBOL_SETTINGS, UI_SECONDARY,
                                  round_lora_parameter_open_event_cb);

    lv_obj_t *chat_panel = lv_obj_create(main_cont);
    lv_obj_set_size(chat_panel, ROUND_LORA_CONTENT_W, ROUND_LORA_CHAT_H);
    round_lora_panel_style(chat_panel, UI_PANEL, 12);
    lv_obj_set_style_pad_all(chat_panel, 10, 0);
    lv_obj_set_style_pad_row(chat_panel, 6, 0);
    ui_obj_set_flex(chat_panel, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START,
                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *chat_head = ui_flex_container_create(chat_panel, LV_PCT(100), 42,
                                                   LV_FLEX_FLOW_ROW,
                                                   LV_FLEX_ALIGN_START,
                                                   LV_FLEX_ALIGN_CENTER,
                                                   LV_FLEX_ALIGN_CENTER);
    lv_obj_t *messages_title = ui_label_create(chat_head, "MESSAGES", &lv_font_montserrat_12, UI_SECONDARY);
    lv_obj_set_width(messages_title, 90);
    s_lora_ui.lora_rssi_snr_label = ui_label_create(chat_head, "RSSI -- / SNR --",
                                                    &lv_font_montserrat_12, UI_MUTED);
    ui_flex_spacer_create(chat_head);
    round_lora_icon_button_create(chat_head, LV_SYMBOL_TRASH, UI_MUTED,
                                  round_lora_clear_event_cb);

    s_round_lora_ui.chat_list = lv_obj_create(chat_panel);
    lv_obj_set_size(s_round_lora_ui.chat_list, LV_PCT(100), 242);
    lv_obj_set_style_bg_opa(s_round_lora_ui.chat_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_round_lora_ui.chat_list, 0, 0);
    lv_obj_set_style_pad_all(s_round_lora_ui.chat_list, 4, 0);
    lv_obj_set_style_pad_row(s_round_lora_ui.chat_list, 8, 0);
    lv_obj_set_flex_flow(s_round_lora_ui.chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_round_lora_ui.chat_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_round_lora_ui.chat_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_round_lora_ui.chat_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_round_lora_ui.chat_list, LV_OBJ_FLAG_SCROLLABLE);
    round_lora_chat_placeholder_create();

    lv_obj_t *composer_row = ui_flex_container_create(main_cont, ROUND_LORA_CONTENT_W, 56,
                                                      LV_FLEX_FLOW_ROW,
                                                      LV_FLEX_ALIGN_START,
                                                      LV_FLEX_ALIGN_CENTER,
                                                      LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(composer_row, 8, 0);
    s_round_lora_ui.composer = lora_textarea_create(composer_row, 460, 52, "Hello LoRa", true);
    s_lora_ui.tx_input = s_round_lora_ui.composer;
    lv_textarea_set_placeholder_text(s_round_lora_ui.composer, "Write a LoRa message");
    lv_obj_add_event_cb(s_round_lora_ui.composer, round_lora_composer_event_cb, LV_EVENT_CLICKED, NULL);
    round_lora_icon_button_create(composer_row, LV_SYMBOL_RIGHT, UI_PRIMARY,
                                  round_lora_send_event_cb);

    lv_obj_t *actions = ui_flex_container_create(main_cont, ROUND_LORA_CONTENT_W, 52,
                                                 LV_FLEX_FLOW_ROW,
                                                 LV_FLEX_ALIGN_SPACE_BETWEEN,
                                                 LV_FLEX_ALIGN_CENTER,
                                                 LV_FLEX_ALIGN_CENTER);
    s_round_lora_ui.rx_btn = round_lora_button_create(actions, 150, LV_SYMBOL_DOWNLOAD,
                                                      "RECEIVE", UI_OK, round_lora_rx_event_cb);
    s_round_lora_ui.rx_btn_label = lv_obj_get_child(s_round_lora_ui.rx_btn, 0);
    s_round_lora_ui.periodic_btn = round_lora_button_create(actions, 190, LV_SYMBOL_REFRESH,
                                                            "AUTO", UI_PRIMARY,
                                                            round_lora_periodic_event_cb);
    s_round_lora_ui.periodic_btn_label = lv_obj_get_child(s_round_lora_ui.periodic_btn, 0);
    round_lora_button_create(actions, 150, LV_SYMBOL_STOP, "STOP", UI_MUTED,
                             round_lora_stop_event_cb);

    round_lora_keyboard_create(s_lora_ui.root);
    round_lora_parameter_dialog_create(s_lora_ui.root);
    round_lora_params_changed();
    round_lora_page_on_timer();
}

static void round_lora_page_on_enter(bool init_failed)
{
    (void)init_failed;
}

static void round_lora_page_on_leave(void)
{
    s_round_lora_ui.resume_rx_after_tx = false;
}

static void round_lora_page_on_destroy(void)
{
    memset(&s_round_lora_ui, 0, sizeof(s_round_lora_ui));
}

static void round_lora_page_on_timer(void)
{
    if (s_round_lora_ui.rx_btn == NULL || s_round_lora_ui.periodic_btn == NULL)
    {
        return;
    }

    int control_mode = s_lora_mode_switch_pending ? -2 : s_lora_status;
    if (control_mode == s_round_lora_ui.last_control_mode)
    {
        if (s_round_lora_ui.periodic_btn_label)
        {
            lv_label_set_text_fmt(s_round_lora_ui.periodic_btn_label,
                                  "%s  AUTO %ds", LV_SYMBOL_REFRESH, s_lora_tx_interval_s);
        }
        return;
    }
    s_round_lora_ui.last_control_mode = control_mode;

    bool rx_active = control_mode == LORA_CONTROL_RX;
    bool periodic_active = control_mode == LORA_CONTROL_TX;
    lv_obj_set_style_bg_color(s_round_lora_ui.rx_btn,
                              lv_color_hex(rx_active ? UI_OK : UI_PANEL_HL), 0);
    lv_obj_set_style_bg_color(s_round_lora_ui.periodic_btn,
                              lv_color_hex(periodic_active ? UI_PRIMARY : UI_PANEL_HL), 0);
    lv_obj_set_style_text_color(s_round_lora_ui.rx_btn_label,
                                lv_color_hex(rx_active ? UI_BG : UI_OK), 0);
    lv_obj_set_style_text_color(s_round_lora_ui.periodic_btn_label,
                                lv_color_hex(periodic_active ? UI_BG : UI_PRIMARY), 0);
    lv_label_set_text_fmt(s_round_lora_ui.rx_btn_label, "%s  %s",
                          LV_SYMBOL_DOWNLOAD, rx_active ? "RECEIVING" : "RECEIVE");
    lv_label_set_text_fmt(s_round_lora_ui.periodic_btn_label, "%s  AUTO %ds",
                          LV_SYMBOL_REFRESH, s_lora_tx_interval_s);
}

static void round_lora_tx_finished(void)
{
    if (s_round_lora_ui.resume_rx_after_tx && s_lora_page_active)
    {
        s_round_lora_ui.resume_rx_after_tx = false;
        lora_page_request_mode(LORA_CONTROL_RX);
    }
}
