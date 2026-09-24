#include "bmu.h"

#include <stdio.h>
#include <string.h>

#include "axp517.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BMU_STARTUP_CHARGE_CURRENT_MA 512
#define BMU_NORMAL_INPUT_CURRENT_MA 1500
#define BMU_CHARGE_VOLTAGE_MV 4200

static const char *TAG = "factory_bmu";
static axp517_handle_t s_axp;
static bool s_ready;
static bool s_charger_enabled;
static bool s_battery_detection_enabled;
static bool s_fuel_gauge_enabled;
static bool s_bc12_enabled;
static int s_charge_current_limit_ma = BMU_STARTUP_CHARGE_CURRENT_MA;
static int s_input_current_limit_ma = BMU_NORMAL_INPUT_CURRENT_MA;

esp_err_t bmu_init(i2c_master_bus_handle_t i2c_bus)
{
    esp_err_t ret = axp517_init(&s_axp, i2c_bus, AXP517_I2C_ADDR);
    if (ret != ESP_OK)
    {
        ESP_LOGD(TAG, "AXP517 init failed");
        return ret;
    }

    s_ready = true;
    axp517_enable_adc_channels(&s_axp, AXP517_ADC_ALL_EN);
    s_fuel_gauge_enabled = (axp517_enable_fuel_gauge(&s_axp, true) == ESP_OK);
    axp517_set_low_battery_threshold(&s_axp, 10, 3);

    s_bc12_enabled = (axp517_enable_bc12_detect(&s_axp, true) == ESP_OK);
    axp517_enable_typec_detect(&s_axp, true);
    axp517_bc12_enable_auto_dpdm(&s_axp, true);

    axp517_set_input_current_limit(&s_axp, s_input_current_limit_ma);
    axp517_set_input_voltage_limit(&s_axp, 4600);
    axp517_set_vsys_min(&s_axp, 3500);
    axp517_set_charge_voltage(&s_axp, BMU_CHARGE_VOLTAGE_MV);
    axp517_set_charge_current(&s_axp, BMU_STARTUP_CHARGE_CURRENT_MA);
    s_charger_enabled = (axp517_enable_charger(&s_axp, true) == ESP_OK);

    s_battery_detection_enabled = (axp517_enable_battery_detection(&s_axp, true) == ESP_OK);
    axp517_irq_clear_all(&s_axp);
    ESP_LOGI(TAG, "AXP517 ready, startup charge current %dmA", BMU_STARTUP_CHARGE_CURRENT_MA);

    return ESP_OK;
}

bool bmu_is_ready(void)
{
    return s_ready;
}

bool bmu_charger_enable_set(bool enable)
{
    if (!s_ready)
    {
        return false;
    }

    uint8_t before = 0;
    uint8_t after = 0;
    axp517_read_byte(&s_axp, AXP517_REG_MOD_EN1, &before);

    esp_err_t ret = axp517_enable_charger(&s_axp, enable);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set charger %s failed: %s", enable ? "ON" : "OFF", esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    bool read_ok = axp517_read_byte(&s_axp, AXP517_REG_MOD_EN1, &after) == ESP_OK;
    bool hw_enabled = read_ok && ((after & AXP517_CHG_EN) != 0);

    ESP_LOGI(TAG,
             "Charger target=%s REG19 before=0x%02X after=%s0x%02X",
             enable ? "ON" : "OFF",
             before,
             read_ok ? "" : "read-failed:",
             after);

    if (!read_ok)
    {
        ESP_LOGW(TAG, "Charger readback failed");
        return false;
    }

    s_charger_enabled = hw_enabled;
    if (hw_enabled != enable)
    {
        ESP_LOGW(TAG,
                 "Charger readback mismatch: target=%s hw=%s",
                 enable ? "ON" : "OFF",
                 hw_enabled ? "ON" : "OFF");
        return false;
    }

    return true;
}

bool bmu_charge_current_set(int ma)
{
    if (!s_ready)
    {
        return false;
    }
    esp_err_t ret = axp517_set_charge_current(&s_axp, (uint16_t)ma);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set charge current %dmA failed: %s", ma, esp_err_to_name(ret));
        return false;
    }
    s_charge_current_limit_ma = (ma / 64) * 64;
    return true;
}

bool bmu_input_current_limit_set(int ma)
{
    if (!s_ready)
    {
        return false;
    }
    esp_err_t ret = axp517_set_input_current_limit(&s_axp, (uint16_t)ma);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set input current limit %dmA failed: %s", ma, esp_err_to_name(ret));
        return false;
    }
    s_input_current_limit_ma = ma;
    return true;
}

bool bmu_low_battery_warn_set(int warn_percent)
{
    if (!s_ready)
    {
        return false;
    }
    esp_err_t ret = axp517_set_low_battery_threshold(&s_axp, (uint8_t)warn_percent, 3);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Set low battery warning %d%% failed: %s", warn_percent, esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool bmu_battery_summary_get(int *battery_percent, bool *battery_charging)
{
    if (battery_percent == NULL || battery_charging == NULL)
    {
        return false;
    }

    *battery_percent = -1;
    *battery_charging = false;
    if (!s_ready)
    {
        return false;
    }

    uint8_t percent = 0;
    if (axp517_read_battery_percent(&s_axp, &percent) == ESP_OK && percent <= 100)
    {
        *battery_percent = (int)percent;
    }

    axp517_status_t power_status = {0};
    if (axp517_get_status(&s_axp, &power_status) == ESP_OK)
    {
        bool charge_state =
            power_status.charge_status == AXP517_CHG_TRICKLE ||
            power_status.charge_status == AXP517_CHG_PRE ||
            power_status.charge_status == AXP517_CHG_CC ||
            power_status.charge_status == AXP517_CHG_CV;
        *battery_charging = s_charger_enabled &&
                            power_status.vbus_good &&
                            power_status.bat_current_dir == AXP517_BAT_CURRENT_CHARGE &&
                            charge_state;
    }
    return true;
}

bool bmu_die_temperature_get(int *temp_c_x10)
{
    if (temp_c_x10 == NULL || !s_ready)
    {
        return false;
    }

    float die_temp = 0.0f;
    if (axp517_read_die_temperature(&s_axp, &die_temp) != ESP_OK)
    {
        return false;
    }
    *temp_c_x10 = (int)(die_temp * 10.0f);
    return true;
}

bool bmu_enter_ship_mode(void)
{
    if (!s_ready)
    {
        return false;
    }
    esp_err_t ret = axp517_enter_ship_mode(&s_axp);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Enter ship mode failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool bmu_enter_low_power(void)
{
    if (!s_ready)
    {
        return true;
    }

    bool ok = true;
    if (axp517_set_input_current_limit(&s_axp, BMU_STARTUP_CHARGE_CURRENT_MA) != ESP_OK)
    {
        ok = false;
    }
    else
    {
        s_input_current_limit_ma = BMU_STARTUP_CHARGE_CURRENT_MA;
    }
    if (axp517_set_charge_current(&s_axp, BMU_STARTUP_CHARGE_CURRENT_MA) != ESP_OK)
    {
        ok = false;
    }
    else
    {
        s_charge_current_limit_ma = BMU_STARTUP_CHARGE_CURRENT_MA;
    }
    if (axp517_enable_chgled(&s_axp, false) != ESP_OK)
    {
        ok = false;
    }
    s_fuel_gauge_enabled = (axp517_set_fuel_gauge_low_freq(&s_axp, true) == ESP_OK);
    return ok && s_fuel_gauge_enabled;
}

bool bmu_exit_low_power(void)
{
    if (!s_ready)
    {
        return true;
    }

    bool ok = true;
    if (axp517_set_fuel_gauge_low_freq(&s_axp, false) != ESP_OK)
    {
        ok = false;
    }
    else
    {
        s_fuel_gauge_enabled = true;
    }
    if (axp517_enable_chgled(&s_axp, true) != ESP_OK)
    {
        ok = false;
    }
    if (axp517_set_input_current_limit(&s_axp, BMU_NORMAL_INPUT_CURRENT_MA) != ESP_OK)
    {
        ok = false;
    }
    else
    {
        s_input_current_limit_ma = BMU_NORMAL_INPUT_CURRENT_MA;
    }
    return ok;
}

bool bmu_status_info_get(bmu_info_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    *status = (bmu_info_t){
        .ready = s_ready,
        .charger_enabled = s_charger_enabled,
        .charger_hw_enabled = s_charger_enabled,
        .battery_detection_enabled = s_battery_detection_enabled,
        .fuel_gauge_enabled = s_fuel_gauge_enabled,
        .bc12_enabled = s_bc12_enabled,
        .bat_current_dir = -1,
        .battery_percent = -1,
        .battery_soh = -1,
        .charge_status = -1,
        .bc12_type = -1,
        .vbat_mv = -1,
        .vbus_mv = -1,
        .vsys_mv = -1,
        .ibus_ma = -1,
        .ichg_ma = -1,
        .idis_ma = -1,
        .charge_voltage_mv = BMU_CHARGE_VOLTAGE_MV,
        .charge_current_limit_ma = s_charge_current_limit_ma,
        .input_current_limit_ma = s_input_current_limit_ma,
        .ts_mv = -1,
        .die_temp_c_x10 = -1,
    };
    snprintf(status->fault_text, sizeof(status->fault_text), "%s", "Clear");

    if (!s_ready)
    {
        return false;
    }

    uint8_t mod1 = 0;
    if (axp517_read_byte(&s_axp, AXP517_REG_MOD_EN1, &mod1) == ESP_OK)
    {
        status->charger_hw_enabled = (mod1 & AXP517_CHG_EN) != 0;
        s_charger_enabled = status->charger_hw_enabled;
        status->charger_enabled = s_charger_enabled;
    }

    axp517_status_t power_status = {0};
    if (axp517_get_status(&s_axp, &power_status) == ESP_OK)
    {
        status->bat_present = power_status.bat_present;
        status->bat_current_dir = (int)power_status.bat_current_dir;
        status->vindpm = power_status.vindpm;
        status->thermal_regulation = power_status.thermal_regulation;
        status->current_limit = power_status.current_limit;
        status->charge_status = (int)power_status.charge_status;
    }

    axp517_fault_t fault = {0};
    if (axp517_get_fault(&s_axp, &fault) == ESP_OK)
    {
        status->fault0 = fault.fault0;
        status->fault1 = fault.fault1;
    }
    axp517_get_fault_char(&s_axp, status->fault_text, sizeof(status->fault_text));

    axp517_bc_detect_t bc12 = AXP517_BC_UNKNOWN;
    if (axp517_get_bc_detect(&s_axp, &bc12) == ESP_OK)
    {
        status->bc12_type = (int)bc12;
    }

    uint8_t percent = 0;
    if (axp517_read_battery_percent(&s_axp, &percent) == ESP_OK && percent <= 100)
    {
        status->battery_percent = (int)percent;
    }
    uint8_t soh = 0;
    if (axp517_read_battery_soh(&s_axp, &soh) == ESP_OK && soh <= 100)
    {
        status->battery_soh = (int)soh;
    }

    uint16_t mv = 0;
    if (axp517_read_vbat(&s_axp, &mv) == ESP_OK)
        status->vbat_mv = (int)mv;
    if (axp517_read_vbus(&s_axp, &mv) == ESP_OK)
        status->vbus_mv = (int)mv;
    if (axp517_read_vsys(&s_axp, &mv) == ESP_OK)
        status->vsys_mv = (int)mv;
    if (axp517_read_ts_voltage(&s_axp, &mv) == ESP_OK)
        status->ts_mv = (int)mv;

    uint16_t ma = 0;
    if (axp517_read_ibus(&s_axp, &ma) == ESP_OK)
        status->ibus_ma = (int)ma;
    if (axp517_read_ibat_charge(&s_axp, &ma) == ESP_OK)
        status->ichg_ma = (int)ma;
    if (axp517_read_ibat_discharge(&s_axp, &ma) == ESP_OK)
        status->idis_ma = (int)ma;

    bmu_die_temperature_get(&status->die_temp_c_x10);
    return true;
}
