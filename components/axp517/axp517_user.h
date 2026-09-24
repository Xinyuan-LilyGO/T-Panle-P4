#ifndef __AXP517_USER_H__
#define __AXP517_USER_H__

#include "axp517.h"

#ifdef __cplusplus
extern "C" {
#endif

/* High-level object model. One object owns one AXP517 instance. */
typedef struct axp517_device axp517_device_t;

typedef struct {
    int irq_gpio;
    uint16_t charge_voltage_mv;
    uint16_t charge_current_ma;
    uint16_t input_current_limit_ma;
    uint16_t input_voltage_limit_mv;
    uint16_t vsys_min_mv;
    bool charger_enabled;
    bool fuel_gauge_enabled;
    bool runtime_log_enabled;
    uint32_t runtime_log_interval_ms;
} axp517_device_config_t;

typedef struct {
    bool present;
    bool charger_enabled;
    bool charging;
    uint8_t percent;
    uint8_t health_percent;
    uint16_t voltage_mv;
    uint16_t charge_current_ma;
    uint16_t discharge_current_ma;
    float temperature_c;
} axp517_device_battery_t;

typedef struct {
    bool vbus_present;
    bool vbus_good;
    uint16_t vbus_mv;
    uint16_t vsys_mv;
    uint16_t input_current_ma;
    float die_temperature_c;
} axp517_device_power_t;

typedef struct {
    axp517_fault_t fault;
    char description[160];
} axp517_device_health_t;

typedef enum {
    AXP517_DEVICE_POWER_NORMAL = 0,
    AXP517_DEVICE_POWER_LOW,
    AXP517_DEVICE_POWER_SHIP,
} axp517_device_power_mode_t;

void axp517_device_config_init(axp517_device_config_t *config);
esp_err_t axp517_device_create(i2c_master_bus_handle_t bus_handle,
                               const axp517_device_config_t *config,
                               axp517_device_t **out_device);
esp_err_t axp517_device_destroy(axp517_device_t *device);
esp_err_t axp517_device_process(axp517_device_t *device);
esp_err_t axp517_device_set_power_mode(axp517_device_t *device,
                                       axp517_device_power_mode_t mode);
esp_err_t axp517_device_charge_configure(axp517_device_t *device,
                                         uint16_t voltage_mv,
                                         uint16_t current_ma,
                                         uint16_t input_limit_ma);
esp_err_t axp517_device_charger_set_enabled(axp517_device_t *device, bool enable);
esp_err_t axp517_device_boost_set_enabled(axp517_device_t *device, bool enable);
esp_err_t axp517_device_battery_read(axp517_device_t *device,
                                     axp517_device_battery_t *battery);
esp_err_t axp517_device_power_read(axp517_device_t *device,
                                   axp517_device_power_t *power);
esp_err_t axp517_device_health_read(axp517_device_t *device,
                                    axp517_device_health_t *health);
axp517_handle_t *axp517_device_driver_handle(axp517_device_t *device);

#ifdef __cplusplus
}
#endif

#endif
