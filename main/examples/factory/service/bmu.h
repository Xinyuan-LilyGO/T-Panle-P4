#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define BMU_FAULT_TEXT_LEN 64

typedef struct
{
    bool ready;
    bool charger_enabled;
    bool charger_hw_enabled;
    bool battery_detection_enabled;
    bool fuel_gauge_enabled;
    bool bc12_enabled;
    bool bat_present;
    int bat_current_dir;
    bool vindpm;
    bool thermal_regulation;
    bool current_limit;
    int battery_percent;
    int battery_soh;
    int charge_status;
    int bc12_type;
    int vbat_mv;
    int vbus_mv;
    int vsys_mv;
    int ibus_ma;
    int ichg_ma;
    int idis_ma;
    int charge_voltage_mv;
    int charge_current_limit_ma;
    int input_current_limit_ma;
    int ts_mv;
    int die_temp_c_x10;
    uint8_t fault0;
    uint8_t fault1;
    char fault_text[BMU_FAULT_TEXT_LEN];
} bmu_info_t;

esp_err_t bmu_init(i2c_master_bus_handle_t i2c_bus);
bool bmu_is_ready(void);
bool bmu_charger_enable_set(bool enable);
bool bmu_charge_current_set(int ma);
bool bmu_input_current_limit_set(int ma);
bool bmu_low_battery_warn_set(int warn_percent);
bool bmu_status_info_get(bmu_info_t *status);
bool bmu_battery_summary_get(int *battery_percent, bool *battery_charging);
bool bmu_die_temperature_get(int *temp_c_x10);
bool bmu_enter_ship_mode(void);
bool bmu_enter_low_power(void);
bool bmu_exit_low_power(void);
