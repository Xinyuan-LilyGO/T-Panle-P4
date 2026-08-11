#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "T_Panle_P4_board_config.h"
#include "axp517.h"

static const char *TAG = "axp517_example";
static volatile bool s_axp517_irq_pending = false;

static void IRAM_ATTR axp517_irq_isr(void *arg)
{
    (void)arg;
    s_axp517_irq_pending = true;
}

static const char *charge_status_name(axp517_chg_status_t status)
{
    static const char *names[] = {
        "Trickle",
        "Pre",
        "CC",
        "CV",
        "Done",
        "Not charging",
    };

    return status < (sizeof(names) / sizeof(names[0])) ? names[status] : "Reserved";
}

static const char *battery_current_dir_name(axp517_bat_current_dir_t dir)
{
    switch (dir)
    {
    case AXP517_BAT_CURRENT_STANDBY:
        return "standby";
    case AXP517_BAT_CURRENT_CHARGE:
        return "charge";
    case AXP517_BAT_CURRENT_DISCHARGE:
        return "discharge";
    default:
        return "reserved";
    }
}

static const char *bc_detect_name(axp517_bc_detect_t type)
{
    switch (type)
    {
    case AXP517_BC_SDP:
        return "SDP";
    case AXP517_BC_CDP:
        return "CDP";
    case AXP517_BC_DCP:
        return "DCP";
    default:
        return "Unknown";
    }
}

static const char *ts_fault_name(axp517_ts_fault_t fault)
{
    switch (fault)
    {
    case AXP517_TS_FAULT_NORMAL:
        return "normal";
    case AXP517_TS_FAULT_CHG_COLD:
        return "charge cold";
    case AXP517_TS_FAULT_CHG_HOT:
        return "charge hot";
    case AXP517_TS_FAULT_WORK_COLD:
        return "work cold";
    case AXP517_TS_FAULT_WORK_HOT:
        return "work hot";
    default:
        return "reserved";
    }
}

static void log_ret(const char *name, esp_err_t ret)
{
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "%s failed: %s", name, esp_err_to_name(ret));
    }
}

static esp_err_t axp517_irq_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << AXP517_IRQ_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        return ret;
    }

    ret = gpio_isr_handler_remove((gpio_num_t)AXP517_IRQ_PIN);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGD(TAG, "remove old irq handler: %s", esp_err_to_name(ret));
    }

    ret = gpio_isr_handler_add((gpio_num_t)AXP517_IRQ_PIN, axp517_irq_isr, NULL);
    if (ret != ESP_OK)
    {
        return ret;
    }

    s_axp517_irq_pending = gpio_get_level((gpio_num_t)AXP517_IRQ_PIN) == 0;
    ESP_LOGI(TAG, "AXP517 IRQ GPIO%d ready, level=%d", AXP517_IRQ_PIN, gpio_get_level((gpio_num_t)AXP517_IRQ_PIN));
    return ESP_OK;
}

static void axp517_handle_irq(axp517_handle_t *axp)
{
    if (!s_axp517_irq_pending && gpio_get_level((gpio_num_t)AXP517_IRQ_PIN) != 0)
    {
        return;
    }

    s_axp517_irq_pending = false;

    axp517_irq_status_t irq = {0};
    esp_err_t ret = axp517_irq_read(axp, &irq);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "read irq failed: %s", esp_err_to_name(ret));
        return;
    }

    if (irq.bits == 0)
    {
        ESP_LOGI(TAG, "irq pin active but no BMU irq bits");
        return;
    }

    ESP_LOGI(TAG, "irq: bits=0x%08" PRIX32 " raw=%02X %02X %02X %02X",
             irq.bits, irq.raw[0], irq.raw[1], irq.raw[2], irq.raw[3]);

    ret = axp517_irq_clear(axp, irq.bits);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "clear irq failed: %s", esp_err_to_name(ret));
    }

    if (gpio_get_level((gpio_num_t)AXP517_IRQ_PIN) == 0)
    {
        s_axp517_irq_pending = true;
    }
}

static void axp517_config_common(axp517_handle_t *axp)
{
    log_ret("enable adc", axp517_enable_adc_channels(axp, AXP517_ADC_ALL_EN));

    log_ret("enable fuel gauge", axp517_enable_fuel_gauge(axp, true));
    log_ret("low battery threshold", axp517_set_low_battery_threshold(axp, 10, 3));

    log_ret("enable bc1.2", axp517_enable_bc12_detect(axp, true));
    log_ret("enable type-c", axp517_enable_typec_detect(axp, true));
    log_ret("auto dpdm", axp517_bc12_enable_auto_dpdm(axp, true));

    log_ret("input voltage limit", axp517_set_input_voltage_limit(axp, 4600));
    log_ret("input current limit", axp517_set_input_current_limit(axp, 1500));
    log_ret("vsys min", axp517_set_vsys_min(axp, 3500));
    log_ret("vbus ov threshold", axp517_set_vbus_ov_threshold(axp, 16500));

    log_ret("charge voltage", axp517_set_charge_voltage(axp, 4200));
    log_ret("charge current", axp517_set_charge_current(axp, 1024));
    log_ret("precharge current", axp517_set_precharge_current(axp, 128));
    log_ret("trickle current", axp517_set_trickle_current(axp, 96));
    log_ret("termination current", axp517_set_termination_current(axp, 320));
    log_ret("charge termination", axp517_enable_charge_termination(axp, true));
    log_ret("battery detection", axp517_enable_battery_detection(axp, true));
    log_ret("charger timer", axp517_config_charge_timer(axp, true, 2, true, 2, true));
    log_ret("thermal regulation", axp517_set_thermal_regulation_threshold(axp, AXP517_THERMAL_REG_100C));
    log_ret("die protection", axp517_config_die_temperature_protection(axp, true, AXP517_DIE_OT_125C));
    log_ret("enable charger", axp517_enable_charger(axp, true));

    log_ret("batfet delay", axp517_set_batfet_close_delay(axp, AXP517_BATFET_DELAY_8MS));
    log_ret("batfet ocp close", axp517_enable_batfet_ocp_close(axp, true));

    log_ret("boost voltage", axp517_set_boost_voltage(axp, 5062));
    log_ret("boost disable threshold", axp517_set_boost_disable_threshold(axp, AXP517_BOOST_DISABLE_3V0));
    log_ret("boost current limit", axp517_set_boost_current_limit(axp, AXP517_BOOST_LIMIT_900MA));
    /* Only enable boost when your hardware needs OTG/VMID output. */
    log_ret("disable boost", axp517_enable_boost(axp, false));

    log_ret("ts pin", axp517_config_ts_pin(axp,
                                           AXP517_TS_PIN_BATTERY_NTC,
                                           AXP517_TS_CURRENT_ADC_ON_1,
                                           AXP517_TS_CURRENT_50UA));
    log_ret("jeita enable", axp517_enable_jeita(axp, true));

    log_ret("chgled enable", axp517_enable_chgled(axp, true));
    log_ret("chgled config", axp517_config_chgled(axp, false, AXP517_CHGLED_TYPE_A, AXP517_CHGLED_REG_LOW, false));

    log_ret("watchdog config", axp517_watchdog_config(axp,
                                                      AXP517_WDT_TIMEOUT_64S,
                                                      AXP517_WDT_ACTION_IRQ_ONLY));
    log_ret("watchdog enable", axp517_enable_watchdog(axp, true));

    log_ret("irq clear all", axp517_irq_clear_all(axp));
    log_ret("irq enable", axp517_irq_enable(axp, 0xFFFFFFFFU));
}

static void axp517_log_runtime(axp517_handle_t *axp)
{
    axp517_watchdog_feed(axp);

    axp517_status_t status = {0};
    if (axp517_get_status(axp, &status) == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "status: vbus_present=%d good=%d bat=%d batfet=%d sys=%d dir=%s chg=%s vindpm=%d thermal=%d cur_limit=%d",
                 status.vbus_present,
                 status.vbus_good,
                 status.bat_present,
                 status.batfet_on,
                 status.system_on,
                 battery_current_dir_name(status.bat_current_dir),
                 charge_status_name(status.charge_status),
                 status.vindpm,
                 status.thermal_regulation,
                 status.current_limit);
    }

    axp517_fault_t fault = {0};
    if (axp517_get_fault(axp, &fault) == ESP_OK)
    {
        char fault_text[64];
        axp517_get_fault_char(axp, fault_text, sizeof(fault_text));
        ESP_LOGI(TAG,
                 "fault: fault0=0x%02X fault1=0x%02X ts=%s vsys_ov=%d vbat_uvlo=%d decoded=%s",
                 fault.fault0,
                 fault.fault1,
                 ts_fault_name(fault.ts_fault),
                 fault.vsys_ov_5v,
                 fault.vbat_uvlo,
                 fault_text);
        if (fault.fault1)
        {
            axp517_clear_fault(axp, fault.fault1);
        }
    }

    uint8_t percent = 0;
    uint8_t soh = 0;
    uint8_t bat_temp_raw = 0;
    uint16_t vbat = 0;
    uint16_t vbus = 0;
    uint16_t vsys = 0;
    uint16_t ibus = 0;
    uint16_t ichg = 0;
    uint16_t idis = 0;
    uint16_t ts_mv = 0;
    float die_c = 0.0f;

    axp517_read_battery_percent(axp, &percent);
    axp517_read_battery_soh(axp, &soh);
    axp517_read_battery_temperature_reg(axp, &bat_temp_raw);
    axp517_read_vbat(axp, &vbat);
    axp517_read_vbus(axp, &vbus);
    axp517_read_vsys(axp, &vsys);
    axp517_read_ibus(axp, &ibus);
    axp517_read_ibat_charge(axp, &ichg);
    axp517_read_ibat_discharge(axp, &idis);
    axp517_read_ts_voltage(axp, &ts_mv);
    axp517_read_die_temperature(axp, &die_c);

    ESP_LOGI(TAG,
             "adc: bat=%u%% soh=%u%% vbat=%umV vbus=%umV vsys=%umV ibus=%umA ichg=%umA idis=%umA ts=%umV die=%.1fC bat_temp_raw=0x%02X",
             percent,
             soh,
             vbat,
             vbus,
             vsys,
             ibus,
             ichg,
             idis,
             ts_mv,
             die_c,
             bat_temp_raw);

    axp517_bc_detect_t bc = AXP517_BC_UNKNOWN;
    if (axp517_get_bc_detect(axp, &bc) == ESP_OK)
    {
        ESP_LOGI(TAG, "bc1.2: %s", bc_detect_name(bc));
    }

    bool mppt_enabled = false;
    if (axp517_get_mppt_state(axp, &mppt_enabled) == ESP_OK)
    {
        ESP_LOGI(TAG, "mppt: %s", mppt_enabled ? "enabled" : "disabled");
    }

    axp517_pd_version_t pd = {0};
    if (axp517_pd_read_version(axp, &pd) == ESP_OK)
    {
        uint8_t cc_status = 0;
        uint8_t power_status = 0;
        uint16_t alert = 0;
        axp517_pd_read_cc_status(axp, &cc_status);
        axp517_pd_read_power_status(axp, &power_status);
        axp517_pd_read_alert(axp, &alert);
        ESP_LOGI(TAG,
                 "pd: typec=0x%02X pd_ver=0x%02X pd_rev=0x%02X if=%02X/%02X cc=0x%02X power=0x%02X alert=0x%04X",
                 pd.typec_rev,
                 pd.usbpd_ver,
                 pd.usbpd_rev,
                 pd.pd_if_ver,
                 pd.pd_if_rev,
                 cc_status,
                 power_status,
                 alert);
    }
}

void app_main(void)
{
    i2c_master_bus_handle_t bus_handle = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    axp517_handle_t axp;
    esp_err_t ret = axp517_init(&axp, bus_handle, AXP517_I2C_ADDR);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "AXP517 init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "AXP517 ready");

    axp517_config_common(&axp);
    log_ret("irq gpio init", axp517_irq_gpio_init());

    uint8_t data_buf0 = 0;
    axp517_data_buffer_read(&axp, 0, &data_buf0);
    ESP_LOGI(TAG, "data buffer0 before write: 0x%02X", data_buf0);
    axp517_data_buffer_write(&axp, 0, 0xA5);

    int runtime_log_ticks = 0;
    while (1)
    {
        axp517_handle_irq(&axp);

        if (runtime_log_ticks <= 0)
        {
            axp517_log_runtime(&axp);
            runtime_log_ticks = 30;
        }
        runtime_log_ticks--;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
