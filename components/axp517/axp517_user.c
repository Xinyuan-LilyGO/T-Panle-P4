#include "axp517_user.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "axp517_user";

static esp_err_t initialize_default_driver(axp517_handle_t *handle,
                                           i2c_master_bus_handle_t bus_handle,
                                           int irq_gpio);

struct axp517_device
{
    axp517_handle_t driver;
    bool initialized;
    uint16_t normal_input_current_ma;
    uint16_t low_input_current_ma;
};

static const axp517_device_config_t s_device_default_config = {
    .irq_gpio = AXP517_IRQ_GPIO_UNUSED,
    .charge_voltage_mv = 4200,
    .charge_current_ma = 512,
    .input_current_limit_ma = 1500,
    .input_voltage_limit_mv = 4600,
    .vsys_min_mv = 3500,
    .charger_enabled = true,
    .fuel_gauge_enabled = true,
    .runtime_log_enabled = false,
    .runtime_log_interval_ms = AXP517_DEFAULT_RUNTIME_LOG_INTERVAL_MS,
};

static esp_err_t device_check(const axp517_device_t *device)
{
    return device != NULL && device->initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void axp517_device_config_init(axp517_device_config_t *config)
{
    if (config != NULL)
    {
        *config = s_device_default_config;
    }
}

esp_err_t axp517_device_create(i2c_master_bus_handle_t bus_handle,
                               const axp517_device_config_t *config,
                               axp517_device_t **out_device)
{
    if (bus_handle == NULL || out_device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_device = NULL;

    axp517_device_t *device = calloc(1, sizeof(*device));
    if (device == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    axp517_device_config_t effective = config != NULL ? *config : s_device_default_config;
    if (effective.charge_voltage_mv == 0)
    {
        effective.charge_voltage_mv = s_device_default_config.charge_voltage_mv;
    }
    if (effective.charge_current_ma == 0)
    {
        effective.charge_current_ma = s_device_default_config.charge_current_ma;
    }
    if (effective.input_current_limit_ma == 0)
    {
        effective.input_current_limit_ma = s_device_default_config.input_current_limit_ma;
    }
    if (effective.input_voltage_limit_mv == 0)
    {
        effective.input_voltage_limit_mv = s_device_default_config.input_voltage_limit_mv;
    }
    if (effective.vsys_min_mv == 0)
    {
        effective.vsys_min_mv = s_device_default_config.vsys_min_mv;
    }
    if (effective.runtime_log_interval_ms == 0)
    {
        effective.runtime_log_interval_ms = s_device_default_config.runtime_log_interval_ms;
    }

    esp_err_t ret = initialize_default_driver(&device->driver, bus_handle, effective.irq_gpio);
    if (ret != ESP_OK)
    {
        free(device);
        return ret;
    }
    device->initialized = true;
    device->normal_input_current_ma = effective.input_current_limit_ma;
    device->low_input_current_ma = effective.input_current_limit_ma < 512 ?
                                   effective.input_current_limit_ma : 512;

    ret = axp517_device_charge_configure(device,
                                         effective.charge_voltage_mv,
                                         effective.charge_current_ma,
                                         effective.input_current_limit_ma);
    if (ret == ESP_OK)
    {
        ret = axp517_set_input_voltage_limit(&device->driver, effective.input_voltage_limit_mv);
    }
    if (ret == ESP_OK)
    {
        ret = axp517_set_vsys_min(&device->driver, effective.vsys_min_mv);
    }
    if (ret == ESP_OK)
    {
        ret = axp517_enable_fuel_gauge(&device->driver, effective.fuel_gauge_enabled);
    }
    if (ret == ESP_OK)
    {
        ret = axp517_enable_charger(&device->driver, effective.charger_enabled);
    }
    if (ret == ESP_OK && effective.runtime_log_enabled)
    {
        ret = axp517_set_runtime_log(&device->driver, true, effective.runtime_log_interval_ms);
    }
    if (ret != ESP_OK)
    {
        axp517_deinit(&device->driver);
        free(device);
        return ret;
    }

    *out_device = device;
    return ESP_OK;
}

esp_err_t axp517_device_destroy(axp517_device_t *device)
{
    if (device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = device_check(device);
    if (ret == ESP_OK)
    {
        ret = axp517_deinit(&device->driver);
    }
    if (ret == ESP_OK)
    {
        device->initialized = false;
        free(device);
    }
    return ret;
}

esp_err_t axp517_device_process(axp517_device_t *device)
{
    esp_err_t ret = device_check(device);
    return ret == ESP_OK ? axp517_process(&device->driver) : ret;
}

esp_err_t axp517_device_charge_configure(axp517_device_t *device,
                                         uint16_t voltage_mv,
                                         uint16_t current_ma,
                                         uint16_t input_limit_ma)
{
    esp_err_t ret = device_check(device);
    if (ret != ESP_OK)
    {
        return ret;
    }
    ret = axp517_set_charge_voltage(&device->driver, voltage_mv);
    if (ret == ESP_OK)
    {
        ret = axp517_set_charge_current(&device->driver, current_ma);
    }
    if (ret == ESP_OK)
    {
        ret = axp517_set_input_current_limit(&device->driver, input_limit_ma);
    }
    return ret;
}

esp_err_t axp517_device_charger_set_enabled(axp517_device_t *device, bool enable)
{
    esp_err_t ret = device_check(device);
    return ret == ESP_OK ? axp517_enable_charger(&device->driver, enable) : ret;
}

esp_err_t axp517_device_boost_set_enabled(axp517_device_t *device, bool enable)
{
    esp_err_t ret = device_check(device);
    return ret == ESP_OK ? axp517_enable_boost(&device->driver, enable) : ret;
}

esp_err_t axp517_device_set_power_mode(axp517_device_t *device,
                                       axp517_device_power_mode_t mode)
{
    esp_err_t ret = device_check(device);
    if (ret != ESP_OK)
    {
        return ret;
    }
    switch (mode)
    {
    case AXP517_DEVICE_POWER_NORMAL:
        ret = axp517_set_fuel_gauge_low_freq(&device->driver, false);
        if (ret == ESP_OK) ret = axp517_enable_chgled(&device->driver, true);
        if (ret == ESP_OK) ret = axp517_set_input_current_limit(&device->driver,
                                                                 device->normal_input_current_ma);
        break;
    case AXP517_DEVICE_POWER_LOW:
        ret = axp517_set_input_current_limit(&device->driver, device->low_input_current_ma);
        if (ret == ESP_OK) ret = axp517_enable_chgled(&device->driver, false);
        if (ret == ESP_OK) ret = axp517_set_fuel_gauge_low_freq(&device->driver, true);
        break;
    case AXP517_DEVICE_POWER_SHIP:
        ret = axp517_enter_ship_mode(&device->driver);
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

esp_err_t axp517_device_battery_read(axp517_device_t *device,
                                     axp517_device_battery_t *battery)
{
    if (battery == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = device_check(device);
    if (ret != ESP_OK)
    {
        return ret;
    }

    memset(battery, 0, sizeof(*battery));
    axp517_status_t status = {0};
    ret = axp517_get_status(&device->driver, &status);
    if (ret != ESP_OK)
    {
        return ret;
    }
    battery->present = status.bat_present;
    uint8_t mod1 = 0;
    if (axp517_read_byte(&device->driver, AXP517_REG_MOD_EN1, &mod1) == ESP_OK)
    {
        battery->charger_enabled = (mod1 & AXP517_CHG_EN) != 0;
    }
    battery->charging = status.vbus_good &&
                        status.bat_current_dir == AXP517_BAT_CURRENT_CHARGE &&
                        status.charge_status <= AXP517_CHG_CV;
    axp517_read_battery_percent(&device->driver, &battery->percent);
    axp517_read_battery_soh(&device->driver, &battery->health_percent);
    axp517_read_vbat(&device->driver, &battery->voltage_mv);
    axp517_read_ibat_charge(&device->driver, &battery->charge_current_ma);
    axp517_read_ibat_discharge(&device->driver, &battery->discharge_current_ma);
    axp517_read_battery_temperature(&device->driver, &battery->temperature_c);
    return ESP_OK;
}

esp_err_t axp517_device_power_read(axp517_device_t *device,
                                   axp517_device_power_t *power)
{
    if (power == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = device_check(device);
    if (ret != ESP_OK)
    {
        return ret;
    }
    memset(power, 0, sizeof(*power));
    ret = axp517_get_vbus_status(&device->driver, &power->vbus_present, &power->vbus_good);
    if (ret != ESP_OK) return ret;
    axp517_read_vbus(&device->driver, &power->vbus_mv);
    axp517_read_vsys(&device->driver, &power->vsys_mv);
    axp517_read_ibus(&device->driver, &power->input_current_ma);
    axp517_read_die_temperature(&device->driver, &power->die_temperature_c);
    return ESP_OK;
}

esp_err_t axp517_device_health_read(axp517_device_t *device,
                                    axp517_device_health_t *health)
{
    if (health == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = device_check(device);
    if (ret != ESP_OK)
    {
        return ret;
    }
    memset(health, 0, sizeof(*health));
    ret = axp517_get_fault(&device->driver, &health->fault);
    if (ret == ESP_OK)
    {
        axp517_get_fault_char(&device->driver, health->description,
                              sizeof(health->description));
    }
    return ret;
}

axp517_handle_t *axp517_device_driver_handle(axp517_device_t *device)
{
    return device_check(device) == ESP_OK ? &device->driver : NULL;
}

static void record_result(const char *name, esp_err_t result, esp_err_t *first_error)
{
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "%s failed: %s", name, esp_err_to_name(result));
        if (*first_error == ESP_OK)
        {
            *first_error = result;
        }
    }
}

static void IRAM_ATTR axp517_irq_isr(void *arg)
{
    axp517_handle_t *handle = (axp517_handle_t *)arg;
    handle->irq_pending = true;
}

static esp_err_t init_irq_gpio(axp517_handle_t *handle, int irq_gpio)
{
    handle->irq_gpio = irq_gpio;
    if (irq_gpio == AXP517_IRQ_GPIO_UNUSED)
    {
        ESP_LOGI(TAG, "AXP517 IRQ is not connected; polling IRQ registers");
        return ESP_OK;
    }
    if (!GPIO_IS_VALID_GPIO(irq_gpio))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << irq_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        return ret;
    }
    ret = gpio_isr_handler_remove((gpio_num_t)irq_gpio);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGD(TAG, "Remove previous IRQ handler failed: %s", esp_err_to_name(ret));
    }
    ret = gpio_isr_handler_add((gpio_num_t)irq_gpio, axp517_irq_isr, handle);
    if (ret != ESP_OK)
    {
        return ret;
    }

    handle->irq_handler_registered = true;
    handle->irq_pending = gpio_get_level((gpio_num_t)irq_gpio) == 0;
    return ESP_OK;
}

static esp_err_t apply_default_config(axp517_handle_t *handle)
{
    esp_err_t first_error = ESP_OK;

    record_result("enable ADC", axp517_enable_adc_channels(handle, AXP517_ADC_ALL_EN), &first_error);
    record_result("enable fuel gauge", axp517_enable_fuel_gauge(handle, true), &first_error);
    record_result("low battery threshold", axp517_set_low_battery_threshold(handle, 10, 3), &first_error);
    record_result("enable BC1.2", axp517_enable_bc12_detect(handle, true), &first_error);
    record_result("enable Type-C", axp517_enable_typec_detect(handle, true), &first_error);
    record_result("enable VBUS detection",
                  axp517_pd_command(handle, AXP517_PD_COMMAND_ENABLE_VBUS_DETECT), &first_error);
    record_result("enable auto DPDM", axp517_bc12_enable_auto_dpdm(handle, true), &first_error);
    record_result("input voltage limit", axp517_set_input_voltage_limit(handle, 4600), &first_error);
    record_result("input current limit", axp517_set_input_current_limit(handle, 1500), &first_error);
    record_result("VSYS minimum", axp517_set_vsys_min(handle, 3500), &first_error);
    record_result("VBUS over-voltage threshold", axp517_set_vbus_ov_threshold(handle, 16500), &first_error);
    record_result("charge voltage", axp517_set_charge_voltage(handle, 4200), &first_error);
    record_result("charge current", axp517_set_charge_current(handle, 512), &first_error);
    record_result("precharge current", axp517_set_precharge_current(handle, 128), &first_error);
    record_result("trickle current", axp517_set_trickle_current(handle, 96), &first_error);
    record_result("termination current", axp517_set_termination_current(handle, 50), &first_error);
    record_result("charge termination", axp517_enable_charge_termination(handle, true), &first_error);
    record_result("battery detection", axp517_enable_battery_detection(handle, true), &first_error);
    record_result("charger timer", axp517_config_charge_timer(handle, true, 2, true, 2, true), &first_error);
    record_result("thermal regulation",
                  axp517_set_thermal_regulation_threshold(handle, AXP517_THERMAL_REG_100C), &first_error);
    record_result("die temperature protection",
                  axp517_config_die_temperature_protection(handle, true, AXP517_DIE_OT_125C), &first_error);
    record_result("enable charger", axp517_enable_charger(handle, true), &first_error);
    record_result("BATFET close delay",
                  axp517_set_batfet_close_delay(handle, AXP517_BATFET_DELAY_8MS), &first_error);
    record_result("BATFET OCP close", axp517_enable_batfet_ocp_close(handle, true), &first_error);
    record_result("boost voltage", axp517_set_boost_voltage(handle, 5062), &first_error);
    record_result("boost disable threshold",
                  axp517_set_boost_disable_threshold(handle, AXP517_BOOST_DISABLE_3V0), &first_error);
    record_result("boost current limit",
                  axp517_set_boost_current_limit(handle, AXP517_BOOST_LIMIT_900MA), &first_error);
    record_result("disable boost", axp517_enable_boost(handle, false), &first_error);
    record_result("TS pin", axp517_config_ts_pin(handle, AXP517_TS_PIN_BATTERY_NTC,
                                                   AXP517_TS_CURRENT_ADC_ON_1,
                                                   AXP517_TS_CURRENT_50UA), &first_error);
    record_result("enable JEITA", axp517_enable_jeita(handle, true), &first_error);
    record_result("enable charge LED", axp517_enable_chgled(handle, true), &first_error);
    record_result("configure charge LED",
                  axp517_config_chgled(handle, false, AXP517_CHGLED_TYPE_A,
                                       AXP517_CHGLED_REG_LOW, false), &first_error);
    record_result("disable watchdog", axp517_enable_watchdog(handle, false), &first_error);
    record_result("clear IRQ", axp517_irq_clear_all(handle), &first_error);
    record_result("enable IRQ", axp517_irq_enable(handle, UINT32_MAX), &first_error);
    return first_error;
}

static esp_err_t process_irq(axp517_handle_t *handle)
{
    if (handle->irq_gpio != AXP517_IRQ_GPIO_UNUSED)
    {
        if (!handle->irq_pending && gpio_get_level((gpio_num_t)handle->irq_gpio) != 0)
        {
            return ESP_OK;
        }
        handle->irq_pending = false;
    }

    axp517_irq_status_t irq = {0};
    esp_err_t ret = axp517_irq_read(handle, &irq);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (irq.bits == 0)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Power IRQ flags=0x%08" PRIX32 " raw=%02X %02X %02X %02X",
             irq.bits, irq.raw[0], irq.raw[1], irq.raw[2], irq.raw[3]);
    ret = axp517_irq_clear(handle, irq.bits);
    if (handle->irq_gpio != AXP517_IRQ_GPIO_UNUSED &&
        gpio_get_level((gpio_num_t)handle->irq_gpio) == 0)
    {
        handle->irq_pending = true;
    }
    return ret;
}

static const char *charge_status_name(axp517_chg_status_t status)
{
    static const char *const names[] = {
        "trickle", "pre-charge", "constant-current", "constant-voltage", "complete", "not-charging",
    };
    return status < (sizeof(names) / sizeof(names[0])) ? names[status] : "unknown";
}

static const char *battery_current_dir_name(axp517_bat_current_dir_t dir)
{
    switch (dir)
    {
    case AXP517_BAT_CURRENT_CHARGE: return "charging";
    case AXP517_BAT_CURRENT_DISCHARGE: return "discharging";
    case AXP517_BAT_CURRENT_STANDBY: return "standby";
    default: return "unknown";
    }
}

static const char *bc_detect_name(axp517_bc_detect_t type)
{
    switch (type)
    {
    case AXP517_BC_SDP: return "SDP";
    case AXP517_BC_CDP: return "CDP";
    case AXP517_BC_DCP: return "DCP";
    default: return "unknown";
    }
}

static void log_runtime(axp517_handle_t *handle)
{
    axp517_status_t status = {0};
    if (axp517_get_status(handle, &status) != ESP_OK)
    {
        ESP_LOGW(TAG, "Unable to read AXP517 power status");
        return;
    }

    ESP_LOGI(TAG,
             "Power: VBUS=%s/%s battery=%s BATFET=%s system=%s charge=%s current=%s",
             status.vbus_present ? "present" : "absent",
             status.vbus_good ? "good" : "invalid",
             status.bat_present ? "present" : "absent",
             status.batfet_on ? "on" : "off",
             status.system_on ? "on" : "off",
             charge_status_name(status.charge_status),
             battery_current_dir_name(status.bat_current_dir));

    axp517_fault_t fault = {0};
    if (axp517_get_fault(handle, &fault) == ESP_OK)
    {
        ESP_LOGI(TAG, "Fault: ts=%d vsys_ov=%s vbat_uvlo=%s raw=%02X/%02X",
                 fault.ts_fault,
                 fault.vsys_ov_5v ? "yes" : "no",
                 fault.vbat_uvlo ? "yes" : "no",
                 fault.fault0, fault.fault1);
        if (fault.fault1 != 0)
        {
            axp517_clear_fault(handle, fault.fault1);
        }
    }

    uint8_t percent = 0;
    uint8_t soh = 0;
    uint16_t vbat = 0;
    uint16_t vbus = 0;
    uint16_t vsys = 0;
    uint16_t ibus = 0;
    float die_temp = 0.0f;
    axp517_read_battery_percent(handle, &percent);
    axp517_read_battery_soh(handle, &soh);
    axp517_read_vbat(handle, &vbat);
    axp517_read_vbus(handle, &vbus);
    axp517_read_vsys(handle, &vsys);
    axp517_read_ibus(handle, &ibus);
    axp517_read_die_temperature(handle, &die_temp);
    ESP_LOGI(TAG, "Measurements: battery=%u%% SOH=%u%% VBAT=%umV VBUS=%umV VSYS=%umV IIN=%umA die=%.1fC",
             percent, soh, vbat, vbus, vsys, ibus, die_temp);

    axp517_bc_detect_t bc = AXP517_BC_UNKNOWN;
    if (axp517_get_bc_detect(handle, &bc) == ESP_OK)
    {
        ESP_LOGI(TAG, "USB charger type: %s", bc_detect_name(bc));
    }
}

static esp_err_t initialize_default_driver(axp517_handle_t *handle,
                                           i2c_master_bus_handle_t bus_handle,
                                           int irq_gpio)
{
    esp_err_t ret = axp517_init(handle, bus_handle, AXP517_I2C_ADDR);
    if (ret != ESP_OK)
    {
        return ret;
    }

    handle->irq_gpio = AXP517_IRQ_GPIO_UNUSED;
    handle->runtime_log_interval_ms = AXP517_DEFAULT_RUNTIME_LOG_INTERVAL_MS;
    ret = apply_default_config(handle);
    if (ret == ESP_OK)
    {
        ret = init_irq_gpio(handle, irq_gpio);
    }
    if (ret != ESP_OK)
    {
        axp517_deinit(handle);
        return ret;
    }

    ESP_LOGI(TAG, "AXP517 default power configuration applied");
    return ESP_OK;
}

esp_err_t axp517_init_default(axp517_handle_t *handle,
                              i2c_master_bus_handle_t bus_handle,
                              int irq_gpio)
{
    return initialize_default_driver(handle, bus_handle, irq_gpio);
}

esp_err_t axp517_set_runtime_log(axp517_handle_t *handle,
                                 bool enable,
                                 uint32_t interval_ms)
{
    if (handle == NULL || handle->dev_handle == NULL || (enable && interval_ms == 0))
    {
        return ESP_ERR_INVALID_ARG;
    }
    handle->runtime_log_enabled = enable;
    if (interval_ms > 0)
    {
        handle->runtime_log_interval_ms = interval_ms;
    }
    handle->last_runtime_log_us = 0;
    return ESP_OK;
}

esp_err_t axp517_process(axp517_handle_t *handle)
{
    if (handle == NULL || handle->dev_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = process_irq(handle);
    if (handle->runtime_log_enabled)
    {
        int64_t now_us = esp_timer_get_time();
        if (handle->last_runtime_log_us == 0 ||
            now_us - handle->last_runtime_log_us >=
                (int64_t)handle->runtime_log_interval_ms * 1000)
        {
            log_runtime(handle);
            handle->last_runtime_log_us = now_us;
        }
    }
    return ret;
}
