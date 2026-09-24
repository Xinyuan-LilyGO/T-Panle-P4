// axp517.h
#ifndef __AXP517_H__
#define __AXP517_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AXP517_I2C_ADDR         0x34
#define AXP517_IRQ_GPIO_UNUSED  (-1)
#define AXP517_DEFAULT_RUNTIME_LOG_INTERVAL_MS 3000U

/* BMU registers */
#define AXP517_REG_STATUS0      0x00
#define AXP517_REG_STATUS1      0x01
#define AXP517_REG_DATA_BUF0    0x04
#define AXP517_REG_BC_DETECT    0x05
#define AXP517_REG_FAULT0       0x06
#define AXP517_REG_FAULT1       0x08
#define AXP517_REG_MOD_EN0      0x0B
#define AXP517_REG_DATA_BUF1    0x0C
#define AXP517_REG_DATA_BUF2    0x0D
#define AXP517_REG_COMMON_CFG   0x10
#define AXP517_REG_GPIO_CFG     0x11
#define AXP517_REG_BATFET_CTRL  0x12
#define AXP517_REG_RBFET_CTRL   0x13
#define AXP517_REG_DIE_TEMP_CFG 0x14
#define AXP517_REG_VSYS_MIN     0x15
#define AXP517_REG_VINDPM       0x16
#define AXP517_REG_IINLIM       0x17
#define AXP517_REG_RESET_CFG    0x18
#define AXP517_REG_MOD_EN1      0x19
#define AXP517_REG_WDT_CTRL     0x1A
#define AXP517_REG_LOW_BAT_WARN 0x1B
#define AXP517_REG_PWRON_CFG    0x1C
#define AXP517_REG_VBUS_OV_CFG  0x1D
#define AXP517_REG_BOOST_CFG    0x1E
#define AXP517_REG_MPPT_CFG     0x22
#define AXP517_REG_BC12_CTRL0   0x28
#define AXP517_REG_BC12_CTRL1   0x29
#define AXP517_REG_BC12_CTRL2   0x2A
#define AXP517_REG_BC12_CTRL3   0x2B
#define AXP517_REG_CHGLED_CFG   0x30
#define AXP517_REG_BREATH_CTRL0 0x32
#define AXP517_REG_BREATH_CTRL1 0x33
#define AXP517_REG_BREATH_CTRL2 0x34
#define AXP517_REG_BREATH_CTRL3 0x36
#define AXP517_REG_BREATH_CTRL4 0x37
#define AXP517_REG_BREATH_CTRL5 0x38
#define AXP517_REG_IRQ_EN0      0x40
#define AXP517_REG_IRQ_EN1      0x41
#define AXP517_REG_IRQ_EN2      0x42
#define AXP517_REG_IRQ_EN3      0x43
#define AXP517_REG_IRQ_STAT0    0x48
#define AXP517_REG_IRQ_STAT1    0x49
#define AXP517_REG_IRQ_STAT2    0x4A
#define AXP517_REG_IRQ_STAT3    0x4B
#define AXP517_REG_TS_CFG       0x50
#define AXP517_REG_TS_HYSL2H    0x52
#define AXP517_REG_TS_HYSH2L    0x53
#define AXP517_REG_VLTF_CHG     0x54
#define AXP517_REG_VHTF_CHG     0x55
#define AXP517_REG_VLTF_WORK    0x56
#define AXP517_REG_VHTF_WORK    0x57
#define AXP517_REG_JEITA_EN     0x58
#define AXP517_REG_JEITA_CFG    0x59
#define AXP517_REG_JEITA_COOL   0x5A
#define AXP517_REG_JEITA_WARM   0x5B
#define AXP517_REG_TS_DATA_H    0x5C
#define AXP517_REG_TS_DATA_L    0x5D
#define AXP517_REG_IPRECHG_ITRICHG 0x61
#define AXP517_REG_ICC          0x62
#define AXP517_REG_ITERM_CTRL   0x63
#define AXP517_REG_CV_VOLT      0x64
#define AXP517_REG_THERMAL_CFG  0x65
#define AXP517_REG_CHG_TIMER_CFG 0x67
#define AXP517_REG_BAT_DET_CTRL 0x68
#define AXP517_REG_BAT_PARAM    0x70
#define AXP517_REG_FG_CTRL      0x71
#define AXP517_REG_BAT_TEMP     0x72
#define AXP517_REG_BAT_SOH      0x73
#define AXP517_REG_BAT_PERCENT  0x74
#define AXP517_REG_TS_SRC_SEL   0x82
#define AXP517_REG_ADC_EN       0x90
#define AXP517_REG_VBAT_H       0x91
#define AXP517_REG_VBAT_L       0x92
#define AXP517_REG_IBAT_H       0x93
#define AXP517_REG_IBAT_L       0x94
#define AXP517_REG_TS_H         0x95
#define AXP517_REG_TS_L         0x96
#define AXP517_REG_IBUS_H       0x97
#define AXP517_REG_IBUS_L       0x98
#define AXP517_REG_VBUS_H       0x99
#define AXP517_REG_VBUS_L       0x9A
#define AXP517_REG_ADC_SEL      0x9B
#define AXP517_REG_ADC_DATA_H   0x9C
#define AXP517_REG_ADC_DATA_L   0x9D

/* USB Type-C / PD TCPC registers */
#define AXP517_REG_USBTYPEC_REV_L 0xA6
#define AXP517_REG_USBTYPEC_REV_H 0xA7
#define AXP517_REG_USBTYPEC_REV   AXP517_REG_USBTYPEC_REV_L
#define AXP517_REG_USBPD_VER      0xA8
#define AXP517_REG_USBPD_REV      0xA9
#define AXP517_REG_PD_IF_VER      0xAA
#define AXP517_REG_PD_IF_REV      0xAB
#define AXP517_REG_PD_ALERT_L     0xB0
#define AXP517_REG_PD_ALERT_H     0xB1
#define AXP517_REG_PD_ALERT_MASK_L 0xB2
#define AXP517_REG_PD_ALERT_MASK_H 0xB3
#define AXP517_REG_POWER_STATUS_MASK 0xB4
#define AXP517_REG_FAULT_STATUS_MASK 0xB5
#define AXP517_REG_TCPC_CONTROL   0xB9
#define AXP517_REG_ROLE_CONTROL   0xBA
#define AXP517_REG_FAULT_CONTROL  0xBB
#define AXP517_REG_POWER_CONTROL  0xBC
#define AXP517_REG_CC_STATUS      0xBD
#define AXP517_REG_POWER_STATUS   0xBE
#define AXP517_REG_FAULT_STATUS   0xBF
#define AXP517_REG_COMMAND        0xC3
#define AXP517_REG_DEVICE_CAP1_L  0xC4
#define AXP517_REG_DEVICE_CAP1_H  0xC5
#define AXP517_REG_DEVICE_CAP2_L  0xC6
#define AXP517_REG_DEVICE_CAP2_H  0xC7
#define AXP517_REG_MSG_HEADER_INFO 0xCE
#define AXP517_REG_RECEIVE_DETECT 0xCF
#define AXP517_REG_VBUS_VOLTAGE_L 0xD0
#define AXP517_REG_VBUS_VOLTAGE_H 0xD1
#define AXP517_REG_VBUS_SNK_DISCONNECT_THLD_L 0xD2
#define AXP517_REG_VBUS_SNK_DISCONNECT_THLD_H 0xD3
#define AXP517_REG_VBUS_STOP_DISCHARGE_THLD_L 0xD4
#define AXP517_REG_VBUS_STOP_DISCHARGE_THLD_H 0xD5
#define AXP517_REG_VBUS_VOLT_ALM_HI_CFG_L 0xD6
#define AXP517_REG_VBUS_VOLT_ALM_HI_CFG_H 0xD7
#define AXP517_REG_VBUS_VOLT_ALM_LO_CFG_L 0xD8
#define AXP517_REG_VBUS_VOLT_ALM_LO_CFG_H 0xD9
#define AXP517_REG_RX_BUFFER      0xDA
#define AXP517_REG_TX_BUF_FRAME_TYPE 0xDB
#define AXP517_REG_TX_BUFFER      0xDC

/* Bit definitions */
#define AXP517_VBUS_GOOD          (1U << 5)
#define AXP517_BATFET_ON          (1U << 4)
#define AXP517_BAT_PRESENT        (1U << 3)
#define AXP517_THERMAL_REGULATION (1U << 1)
#define AXP517_CURRENT_LIMIT      (1U << 0)

#define AXP517_PD_VBUS_PRESENT                (1U << 2)
#define AXP517_PD_DEBUG_ACCESSORY             (1U << 7)
#define AXP517_PD_COMMAND_ENABLE_VBUS_DETECT 0x33

#define AXP517_MOD0_BC12_EN       (1U << 4)
#define AXP517_MOD0_TYPEC_EN      (1U << 3)
#define AXP517_MOD0_GAUGE_EN      (1U << 2)
#define AXP517_MOD0_WD_CLK_EN     (1U << 0)

#define AXP517_MOD1_GAUGE_LOW_FREQ (1U << 6)
#define AXP517_BOOST_EN           (1U << 4)
#define AXP517_BUCK_EN            (1U << 3)
#define AXP517_CHGLED_EN          (1U << 2)
#define AXP517_CHG_EN             (1U << 1)
#define AXP517_MOD1_WD_EN         (1U << 0)

#define AXP517_ADC_VBUS_CURRENT_EN    (1U << 7)
#define AXP517_ADC_BAT_DISCHARGE_EN   (1U << 6)
#define AXP517_ADC_BAT_CHARGE_EN      (1U << 5)
#define AXP517_ADC_DIE_TEMP_EN        (1U << 4)
#define AXP517_ADC_VSYS_VOLTAGE_EN    (1U << 3)
#define AXP517_ADC_VBUS_VOLTAGE_EN    (1U << 2)
#define AXP517_ADC_TS_VOLTAGE_EN      (1U << 1)
#define AXP517_ADC_BAT_VOLTAGE_EN     (1U << 0)
#define AXP517_ADC_ALL_EN             0xFF

typedef enum {
    AXP517_CHG_TRICKLE = 0,
    AXP517_CHG_PRE     = 1,
    AXP517_CHG_CC      = 2,
    AXP517_CHG_CV      = 3,
    AXP517_CHG_DONE    = 4,
    AXP517_CHG_NOT     = 5,
} axp517_chg_status_t;

typedef enum {
    AXP517_BAT_CURRENT_STANDBY = 0,
    AXP517_BAT_CURRENT_CHARGE = 1,
    AXP517_BAT_CURRENT_DISCHARGE = 2,
} axp517_bat_current_dir_t;

typedef enum {
    AXP517_BC_UNKNOWN = 0,
    AXP517_BC_SDP = 1,
    AXP517_BC_CDP = 2,
    AXP517_BC_DCP = 3,
} axp517_bc_detect_t;

typedef enum {
    AXP517_TS_FAULT_NORMAL = 0,
    AXP517_TS_FAULT_CHG_COLD = 1,
    AXP517_TS_FAULT_CHG_HOT = 2,
    AXP517_TS_FAULT_WORK_COLD = 5,
    AXP517_TS_FAULT_WORK_HOT = 6,
} axp517_ts_fault_t;

typedef enum {
    AXP517_WDT_TIMEOUT_1S = 0,
    AXP517_WDT_TIMEOUT_2S,
    AXP517_WDT_TIMEOUT_4S,
    AXP517_WDT_TIMEOUT_8S,
    AXP517_WDT_TIMEOUT_16S,
    AXP517_WDT_TIMEOUT_32S,
    AXP517_WDT_TIMEOUT_64S,
    AXP517_WDT_TIMEOUT_128S,
} axp517_watchdog_timeout_t;

typedef enum {
    AXP517_WDT_ACTION_IRQ_ONLY = 0,
    AXP517_WDT_ACTION_IRQ_AND_SYSTEM_RESET = 1,
    AXP517_WDT_ACTION_SYSTEM_RESET_BUCK_BATFET_RESTART = 2,
    AXP517_WDT_ACTION_POR = 3,
} axp517_watchdog_action_t;

typedef enum {
    AXP517_BATFET_DELAY_0MS = 0,
    AXP517_BATFET_DELAY_8MS = 1,
    AXP517_BATFET_DELAY_16MS = 2,
    AXP517_BATFET_DELAY_32MS = 3,
} axp517_batfet_delay_t;

typedef enum {
    AXP517_BOOST_DISABLE_3V2 = 0,
    AXP517_BOOST_DISABLE_3V0 = 1,
    AXP517_BOOST_DISABLE_2V8 = 2,
    AXP517_BOOST_DISABLE_2V6 = 3,
} axp517_boost_disable_threshold_t;

typedef enum {
    AXP517_BOOST_LIMIT_500MA = 0,
    AXP517_BOOST_LIMIT_900MA = 1,
    AXP517_BOOST_LIMIT_1500MA = 2,
    AXP517_BOOST_LIMIT_2000MA = 3,
} axp517_boost_current_limit_t;

typedef enum {
    AXP517_THERMAL_REG_60C = 0,
    AXP517_THERMAL_REG_80C = 1,
    AXP517_THERMAL_REG_100C = 2,
    AXP517_THERMAL_REG_120C = 3,
} axp517_thermal_reg_threshold_t;

typedef enum {
    AXP517_DIE_OT_115C = 0,
    AXP517_DIE_OT_125C = 1,
    AXP517_DIE_OT_135C = 2,
} axp517_die_ot_threshold_t;

typedef enum {
    AXP517_CHGLED_TYPE_A = 0,
    AXP517_CHGLED_TYPE_B = 1,
    AXP517_CHGLED_BREATH_BY_CHARGER = 2,
    AXP517_CHGLED_BREATH_BY_REG = 3,
    AXP517_CHGLED_BY_REG = 6,
} axp517_chgled_mode_t;

typedef enum {
    AXP517_CHGLED_REG_HIZ = 0,
    AXP517_CHGLED_REG_BLINK_1HZ = 1,
    AXP517_CHGLED_REG_BLINK_4HZ = 2,
    AXP517_CHGLED_REG_LOW = 3,
    AXP517_CHGLED_REG_HIGH = 4,
} axp517_chgled_output_t;

typedef enum {
    AXP517_GPIO_HIZ = 0,
    AXP517_GPIO_LOW = 1,
    AXP517_GPIO_HIGH = 2,
} axp517_gpio_level_t;

typedef enum {
    AXP517_TS_PIN_BATTERY_NTC = 0,
    AXP517_TS_PIN_FIXED_INPUT = 1,
} axp517_ts_pin_mode_t;

typedef enum {
    AXP517_TS_CURRENT_OFF = 0,
    AXP517_TS_CURRENT_ADC_ON_1 = 1,
    AXP517_TS_CURRENT_ADC_ON_2 = 2,
    AXP517_TS_CURRENT_ALWAYS_ON = 3,
} axp517_ts_current_mode_t;

typedef enum {
    AXP517_TS_CURRENT_20UA = 0,
    AXP517_TS_CURRENT_40UA = 1,
    AXP517_TS_CURRENT_50UA = 2,
    AXP517_TS_CURRENT_60UA = 3,
} axp517_ts_current_t;

typedef struct {
    i2c_master_dev_handle_t dev_handle;
    int irq_gpio;
    volatile bool irq_pending;
    bool irq_handler_registered;
    bool runtime_log_enabled;
    uint32_t runtime_log_interval_ms;
    int64_t last_runtime_log_us;
} axp517_handle_t;

typedef struct {
    uint8_t status0;
    uint8_t status1;
    bool vbus_present;
    bool vbus_good;
    bool bat_present;
    bool batfet_on;
    bool system_on;
    bool vindpm;
    bool thermal_regulation;
    bool current_limit;
    axp517_bat_current_dir_t bat_current_dir;
    axp517_chg_status_t charge_status;
} axp517_status_t;

typedef struct {
    uint8_t fault0;
    uint8_t fault1;
    axp517_ts_fault_t ts_fault;
    bool vsys_ov_5v;
    bool vbat_uvlo;
} axp517_fault_t;

typedef struct {
    uint8_t raw[4];
    uint32_t bits;
} axp517_irq_status_t;

typedef struct {
    uint8_t typec_rev;
    uint8_t usbpd_ver;
    uint8_t usbpd_rev;
    uint8_t pd_if_ver;
    uint8_t pd_if_rev;
} axp517_pd_version_t;

esp_err_t axp517_init(axp517_handle_t *handle, i2c_master_bus_handle_t bus_handle, uint8_t addr);
esp_err_t axp517_deinit(axp517_handle_t *handle);

/* Board-facing default setup and periodic service. Pass AXP517_IRQ_GPIO_UNUSED
 * when the IRQ pin is not connected; the service then polls IRQ registers. */
esp_err_t axp517_init_default(axp517_handle_t *handle,
                              i2c_master_bus_handle_t bus_handle,
                              int irq_gpio);
esp_err_t axp517_set_runtime_log(axp517_handle_t *handle,
                                 bool enable,
                                 uint32_t interval_ms);
esp_err_t axp517_process(axp517_handle_t *handle);

esp_err_t axp517_write_byte(axp517_handle_t *handle, uint8_t reg, uint8_t val);
esp_err_t axp517_read_byte(axp517_handle_t *handle, uint8_t reg, uint8_t *val);
esp_err_t axp517_read_block(axp517_handle_t *handle, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t axp517_update_bits(axp517_handle_t *handle, uint8_t reg, uint8_t mask, uint8_t val);

/* Basic control */
esp_err_t axp517_enable_charger(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_boost(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_buck(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_chgled(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_bc12_detect(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_typec_detect(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_vbus_detect(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_vbus_force_discharge(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_vbus_uvlo_discharge(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_vsys_off_discharge(axp517_handle_t *handle, bool enable);
esp_err_t axp517_set_vbus_discharge_current(axp517_handle_t *handle, uint8_t current_code);
esp_err_t axp517_set_charge_voltage(axp517_handle_t *handle, uint16_t mv);
esp_err_t axp517_set_charge_current(axp517_handle_t *handle, uint16_t ma);
esp_err_t axp517_set_input_current_limit(axp517_handle_t *handle, uint16_t ma);
esp_err_t axp517_set_input_voltage_limit(axp517_handle_t *handle, uint16_t mv);
esp_err_t axp517_set_vsys_min(axp517_handle_t *handle, uint16_t mv);
esp_err_t axp517_set_vbus_ov_threshold(axp517_handle_t *handle, uint16_t mv);

/* Status and fault */
esp_err_t axp517_get_status(axp517_handle_t *handle, axp517_status_t *status);
esp_err_t axp517_get_fault(axp517_handle_t *handle, axp517_fault_t *fault);
const char *axp517_get_fault_char(axp517_handle_t *handle, char *buf, size_t buf_size);
esp_err_t axp517_clear_fault(axp517_handle_t *handle, uint8_t fault1_mask);
esp_err_t axp517_get_vbus_status(axp517_handle_t *handle, bool *present, bool *good);
esp_err_t axp517_get_bat_present(axp517_handle_t *handle, bool *present);
esp_err_t axp517_get_batfet_state(axp517_handle_t *handle, bool *on);
esp_err_t axp517_get_charging_status(axp517_handle_t *handle, axp517_chg_status_t *status);
esp_err_t axp517_get_bc_detect(axp517_handle_t *handle, axp517_bc_detect_t *type);
esp_err_t axp517_bc12_force_detect(axp517_handle_t *handle);
esp_err_t axp517_bc12_enable_auto_dpdm(axp517_handle_t *handle, bool enable);

/* IRQ */
esp_err_t axp517_irq_enable(axp517_handle_t *handle, uint32_t mask);
esp_err_t axp517_irq_disable(axp517_handle_t *handle, uint32_t mask);
esp_err_t axp517_irq_set_enabled(axp517_handle_t *handle, uint32_t mask);
esp_err_t axp517_irq_read(axp517_handle_t *handle, axp517_irq_status_t *status);
esp_err_t axp517_irq_clear(axp517_handle_t *handle, uint32_t mask);
esp_err_t axp517_irq_clear_all(axp517_handle_t *handle);

/* Charger details */
esp_err_t axp517_set_precharge_current(axp517_handle_t *handle, uint16_t ma);
esp_err_t axp517_set_trickle_current(axp517_handle_t *handle, uint16_t ma);
esp_err_t axp517_set_termination_current(axp517_handle_t *handle, uint16_t ma);
esp_err_t axp517_enable_charge_termination(axp517_handle_t *handle, bool enable);
esp_err_t axp517_enable_dpm_charge_termination(axp517_handle_t *handle, bool enable);
esp_err_t axp517_set_thermal_regulation_threshold(axp517_handle_t *handle, axp517_thermal_reg_threshold_t threshold);
esp_err_t axp517_config_die_temperature_protection(axp517_handle_t *handle, bool enable,
                                                   axp517_die_ot_threshold_t threshold);
esp_err_t axp517_config_charge_timer(axp517_handle_t *handle, bool pre_enable, uint8_t pre_timer_code,
                                     bool fast_enable, uint8_t fast_timer_code, bool slow_in_dpm);
esp_err_t axp517_enable_battery_detection(axp517_handle_t *handle, bool enable);

/* BATFET/RBFET */
esp_err_t axp517_force_batfet(axp517_handle_t *handle, bool on);
esp_err_t axp517_set_batfet_force_disable(axp517_handle_t *handle, bool disable);
esp_err_t axp517_enter_ship_mode(axp517_handle_t *handle);
esp_err_t axp517_enable_batfet_ocp_close(axp517_handle_t *handle, bool enable);
esp_err_t axp517_set_batfet_close_delay(axp517_handle_t *handle, axp517_batfet_delay_t delay);
esp_err_t axp517_force_rbfet_enable(axp517_handle_t *handle, bool enable);

/* Boost/MPPT */
esp_err_t axp517_set_boost_voltage(axp517_handle_t *handle, uint16_t mv);
esp_err_t axp517_set_boost_disable_threshold(axp517_handle_t *handle, axp517_boost_disable_threshold_t threshold);
esp_err_t axp517_set_boost_current_limit(axp517_handle_t *handle, axp517_boost_current_limit_t limit);
esp_err_t axp517_enable_mppt(axp517_handle_t *handle, bool enable);
esp_err_t axp517_get_mppt_state(axp517_handle_t *handle, bool *enabled);

/* Watchdog/reset/power key */
esp_err_t axp517_enable_watchdog(axp517_handle_t *handle, bool enable);
esp_err_t axp517_watchdog_feed(axp517_handle_t *handle);
esp_err_t axp517_watchdog_config(axp517_handle_t *handle, axp517_watchdog_timeout_t timeout,
                                 axp517_watchdog_action_t action);
esp_err_t axp517_reset_gauge(axp517_handle_t *handle);
esp_err_t axp517_software_por(axp517_handle_t *handle);
esp_err_t axp517_enable_pwron_16s_por(axp517_handle_t *handle, bool enable);
esp_err_t axp517_config_pwron(axp517_handle_t *handle, bool offlevel_close_batfet, bool irq_open_batfet,
                              uint8_t irq_level_code, uint8_t off_level_code, uint8_t on_level_code);

/* Fuel gauge */
esp_err_t axp517_enable_fuel_gauge(axp517_handle_t *handle, bool enable);
esp_err_t axp517_set_fuel_gauge_low_freq(axp517_handle_t *handle, bool enable);
esp_err_t axp517_set_low_battery_threshold(axp517_handle_t *handle, uint8_t warn_percent, uint8_t shutdown_percent);
esp_err_t axp517_read_battery_temperature_reg(axp517_handle_t *handle, uint8_t *raw_temp);
esp_err_t axp517_read_battery_soh(axp517_handle_t *handle, uint8_t *soh);
esp_err_t axp517_read_battery_percent(axp517_handle_t *handle, uint8_t *percent);
esp_err_t axp517_fuel_gauge_select_sram(axp517_handle_t *handle, bool sram);
esp_err_t axp517_fuel_gauge_enable_brom_write(axp517_handle_t *handle, bool enable);
esp_err_t axp517_write_battery_param(axp517_handle_t *handle, uint8_t value);
esp_err_t axp517_read_battery_param(axp517_handle_t *handle, uint8_t *value);

/* ADC */
esp_err_t axp517_enable_adc_channels(axp517_handle_t *handle, uint8_t mask);
esp_err_t axp517_read_vbat(axp517_handle_t *handle, uint16_t *mv);
esp_err_t axp517_read_vbus(axp517_handle_t *handle, uint16_t *mv);
esp_err_t axp517_read_vsys(axp517_handle_t *handle, uint16_t *mv);
esp_err_t axp517_read_ibat_charge(axp517_handle_t *handle, uint16_t *ma);
esp_err_t axp517_read_ibat_discharge(axp517_handle_t *handle, uint16_t *ma);
esp_err_t axp517_read_ibus(axp517_handle_t *handle, uint16_t *ma);
esp_err_t axp517_read_ts_voltage(axp517_handle_t *handle, uint16_t *mv);
esp_err_t axp517_read_die_temperature(axp517_handle_t *handle, float *deg_c);
esp_err_t axp517_read_battery_temperature(axp517_handle_t *handle, float *deg_c);
esp_err_t axp517_read_adc_selected(axp517_handle_t *handle, uint8_t source, uint16_t *raw);

/* TS/JEITA */
esp_err_t axp517_config_ts_pin(axp517_handle_t *handle, axp517_ts_pin_mode_t mode,
                               axp517_ts_current_mode_t current_mode, axp517_ts_current_t current);
esp_err_t axp517_set_ts_hysteresis(axp517_handle_t *handle, uint16_t low_to_normal_mv, uint16_t high_to_normal_mv);
esp_err_t axp517_set_ts_charge_thresholds(axp517_handle_t *handle, uint16_t cold_mv, uint16_t hot_mv);
esp_err_t axp517_set_ts_work_thresholds(axp517_handle_t *handle, uint16_t cold_mv, uint16_t hot_mv);
esp_err_t axp517_enable_jeita(axp517_handle_t *handle, bool enable);
esp_err_t axp517_config_jeita(axp517_handle_t *handle, uint8_t warm_current_fall, uint8_t cool_current_fall,
                              uint8_t warm_voltage_fall, uint8_t cool_voltage_fall);
esp_err_t axp517_set_jeita_cool_warm(axp517_handle_t *handle, uint16_t cool_mv, uint16_t warm_mv);
esp_err_t axp517_set_ts_fixed_voltage(axp517_handle_t *handle, uint16_t mv);
esp_err_t axp517_select_ts_fixed_voltage(axp517_handle_t *handle, bool fixed);

/* GPIO/OD/CHGLED/Data buffer */
esp_err_t axp517_gpio_set_output(axp517_handle_t *handle, axp517_gpio_level_t level);
esp_err_t axp517_gpio_get_input(axp517_handle_t *handle, bool *high);
esp_err_t axp517_gpio_set_pd_irq_source(axp517_handle_t *handle, bool enable);
esp_err_t axp517_od_set_low(axp517_handle_t *handle, bool low);
esp_err_t axp517_config_chgled(axp517_handle_t *handle, bool push_pull, axp517_chgled_mode_t mode,
                               axp517_chgled_output_t output, bool breath_enable);
esp_err_t axp517_write_breath_led_raw(axp517_handle_t *handle, const uint8_t values[6]);
esp_err_t axp517_data_buffer_write(axp517_handle_t *handle, uint8_t index, uint8_t value);
esp_err_t axp517_data_buffer_read(axp517_handle_t *handle, uint8_t index, uint8_t *value);

/* PD/TCPC low-level helpers */
esp_err_t axp517_pd_read_version(axp517_handle_t *handle, axp517_pd_version_t *version);
esp_err_t axp517_pd_read_alert(axp517_handle_t *handle, uint16_t *alert);
esp_err_t axp517_pd_clear_alert(axp517_handle_t *handle, uint16_t alert);
esp_err_t axp517_pd_set_alert_mask(axp517_handle_t *handle, uint16_t mask);
esp_err_t axp517_pd_command(axp517_handle_t *handle, uint8_t command);
esp_err_t axp517_pd_read_cc_status(axp517_handle_t *handle, uint8_t *status);
esp_err_t axp517_pd_read_power_status(axp517_handle_t *handle, uint8_t *status);
esp_err_t axp517_pd_read_fault_status(axp517_handle_t *handle, uint8_t *status);
esp_err_t axp517_pd_clear_fault_status(axp517_handle_t *handle, uint8_t fault);
esp_err_t axp517_pd_set_receive_detect(axp517_handle_t *handle, uint8_t mask);
esp_err_t axp517_pd_read_rx_buffer(axp517_handle_t *handle, uint8_t *buf, size_t len);
esp_err_t axp517_pd_write_tx_buffer(axp517_handle_t *handle, uint8_t frame_type, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
