#include "axp517.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define TAG "AXP517"

static esp_err_t axp517_check_handle(axp517_handle_t *handle)
{
    return (handle && handle->dev_handle) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t axp517_read_u14(axp517_handle_t *handle, uint8_t reg_h, uint16_t *raw)
{
    if (!raw) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[2] = {0};
    esp_err_t ret = axp517_read_block(handle, reg_h, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    *raw = (uint16_t)(((buf[0] & 0x3F) << 8) | buf[1]);
    return ESP_OK;
}

static esp_err_t axp517_read_u16(axp517_handle_t *handle, uint8_t reg_h, uint16_t *raw)
{
    if (!raw) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[2] = {0};
    esp_err_t ret = axp517_read_block(handle, reg_h, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    *raw = (uint16_t)((buf[0] << 8) | buf[1]);
    return ESP_OK;
}

esp_err_t axp517_write_byte(axp517_handle_t *handle, uint8_t reg, uint8_t val)
{
    esp_err_t ret = axp517_check_handle(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(handle->dev_handle, data, sizeof(data), -1);
}

esp_err_t axp517_read_byte(axp517_handle_t *handle, uint8_t reg, uint8_t *val)
{
    esp_err_t ret = axp517_check_handle(handle);
    if (ret != ESP_OK || !val) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(handle->dev_handle, &reg, 1, val, 1, -1);
}

esp_err_t axp517_read_block(axp517_handle_t *handle, uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t ret = axp517_check_handle(handle);
    if (ret != ESP_OK || (len > 0 && !buf)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    return i2c_master_transmit_receive(handle->dev_handle, &reg, 1, buf, len, -1);
}

esp_err_t axp517_update_bits(axp517_handle_t *handle, uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t old_val = 0;
    esp_err_t ret = axp517_read_byte(handle, reg, &old_val);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t new_val = (old_val & (uint8_t)~mask) | (val & mask);
    if (new_val == old_val) {
        return ESP_OK;
    }

    return axp517_write_byte(handle, reg, new_val);
}

esp_err_t axp517_init(axp517_handle_t *handle, i2c_master_bus_handle_t bus_handle, uint8_t addr)
{
    if (!handle || !bus_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(handle, 0, sizeof(*handle));
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &handle->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AXP517 device");
        return ret;
    }

    uint8_t tmp = 0;
    ret = axp517_read_byte(handle, AXP517_REG_STATUS0, &tmp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP517 not responding");
        i2c_master_bus_rm_device(handle->dev_handle);
        handle->dev_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "AXP517 initialized (status0=0x%02X)", tmp);
    return ESP_OK;
}

esp_err_t axp517_deinit(axp517_handle_t *handle)
{
    esp_err_t ret = axp517_check_handle(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_master_bus_rm_device(handle->dev_handle);
    if (ret == ESP_OK) {
        handle->dev_handle = NULL;
    }
    return ret;
}

esp_err_t axp517_enable_charger(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN1, AXP517_CHG_EN, enable ? AXP517_CHG_EN : 0);
}

esp_err_t axp517_enable_boost(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN1, AXP517_BOOST_EN, enable ? AXP517_BOOST_EN : 0);
}

esp_err_t axp517_enable_buck(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN1, AXP517_BUCK_EN, enable ? AXP517_BUCK_EN : 0);
}

esp_err_t axp517_enable_chgled(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN1, AXP517_CHGLED_EN, enable ? AXP517_CHGLED_EN : 0);
}

esp_err_t axp517_enable_bc12_detect(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN0, AXP517_MOD0_BC12_EN,
                              enable ? AXP517_MOD0_BC12_EN : 0);
}

esp_err_t axp517_enable_typec_detect(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN0, AXP517_MOD0_TYPEC_EN,
                              enable ? AXP517_MOD0_TYPEC_EN : 0);
}

esp_err_t axp517_enable_vbus_detect(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_COMMON_CFG, 0x01, enable ? 0 : 0x01);
}

esp_err_t axp517_enable_vbus_force_discharge(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_COMMON_CFG, (1U << 6), enable ? (1U << 6) : 0);
}

esp_err_t axp517_enable_vbus_uvlo_discharge(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_COMMON_CFG, (1U << 5), enable ? (1U << 5) : 0);
}

esp_err_t axp517_enable_vsys_off_discharge(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_COMMON_CFG, (1U << 4), enable ? (1U << 4) : 0);
}

esp_err_t axp517_set_vbus_discharge_current(axp517_handle_t *handle, uint8_t current_code)
{
    if (current_code > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_COMMON_CFG, 0x0C, (uint8_t)(current_code << 2));
}

esp_err_t axp517_set_charge_voltage(axp517_handle_t *handle, uint16_t mv)
{
    uint8_t code;
    switch (mv) {
    case 4000: code = 0x00; break;
    case 4100: code = 0x01; break;
    case 4200: code = 0x02; break;
    case 4350: code = 0x03; break;
    case 4400: code = 0x04; break;
    case 3800: code = 0x05; break;
    case 3600: code = 0x06; break;
    case 5000: code = 0x07; break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return axp517_update_bits(handle, AXP517_REG_CV_VOLT, 0x07, code);
}

esp_err_t axp517_set_charge_current(axp517_handle_t *handle, uint16_t ma)
{
    if (ma > 5120) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t code = ma / 64;
    if (code > 80) {
        code = 80;
    }
    return axp517_update_bits(handle, AXP517_REG_ICC, 0x7F, (uint8_t)code);
}

esp_err_t axp517_set_input_current_limit(axp517_handle_t *handle, uint16_t ma)
{
    if (ma < 100 || ma > 3250) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t code = (ma - 100) / 50;
    if (code > 63) {
        code = 63;
    }
    return axp517_update_bits(handle, AXP517_REG_IINLIM, 0xFC, (uint8_t)(code << 2));
}

esp_err_t axp517_set_input_voltage_limit(axp517_handle_t *handle, uint16_t mv)
{
    if (mv < 3600 || mv > 16200) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t code = (mv <= 3600) ? 0 : (uint16_t)(((mv - 3600) / 100) + 1);
    if (code > 127) {
        code = 127;
    }
    return axp517_update_bits(handle, AXP517_REG_VINDPM, 0x7F, (uint8_t)code);
}

esp_err_t axp517_set_vsys_min(axp517_handle_t *handle, uint16_t mv)
{
    if (mv < 1000 || mv > 3800) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t code = (mv - 1000) / 100;
    if (code > 28) {
        code = 28;
    }
    return axp517_update_bits(handle, AXP517_REG_VSYS_MIN, 0x1F, (uint8_t)code);
}

esp_err_t axp517_set_vbus_ov_threshold(axp517_handle_t *handle, uint16_t mv)
{
    uint8_t code;
    switch (mv) {
    case 16500: code = 0; break;
    case 14200: code = 1; break;
    case 11000: code = 2; break;
    case 6500: code = 3; break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return axp517_update_bits(handle, AXP517_REG_VBUS_OV_CFG, 0xC0, (uint8_t)(code << 6));
}

esp_err_t axp517_get_status(axp517_handle_t *handle, axp517_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t st0 = 0;
    uint8_t st1 = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_STATUS0, &st0);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = axp517_read_byte(handle, AXP517_REG_STATUS1, &st1);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(status, 0, sizeof(*status));
    status->status0 = st0;
    status->status1 = st1;
    status->vbus_present = (st0 & AXP517_VBUS_PRESENT) != 0;
    status->vbus_good = (st0 & AXP517_VBUS_GOOD) != 0;
    status->bat_present = (st0 & AXP517_BAT_PRESENT) != 0;
    status->batfet_on = (st0 & AXP517_BATFET_ON) != 0;
    status->thermal_regulation = (st0 & AXP517_THERMAL_REGULATION) != 0;
    status->current_limit = (st0 & AXP517_CURRENT_LIMIT) != 0;
    status->system_on = (st1 & (1U << 4)) != 0;
    status->vindpm = (st1 & (1U << 3)) != 0;
    status->bat_current_dir = (axp517_bat_current_dir_t)((st1 >> 5) & 0x03);
    status->charge_status = (axp517_chg_status_t)(st1 & 0x07);
    return ESP_OK;
}

esp_err_t axp517_get_fault(axp517_handle_t *handle, axp517_fault_t *fault)
{
    if (!fault) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t f0 = 0;
    uint8_t f1 = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_FAULT0, &f0);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = axp517_read_byte(handle, AXP517_REG_FAULT1, &f1);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(fault, 0, sizeof(*fault));
    fault->fault0 = f0;
    fault->fault1 = f1;
    fault->ts_fault = (axp517_ts_fault_t)((f0 >> 4) & 0x07);
    fault->vsys_ov_5v = (f1 & (1U << 3)) != 0;
    fault->vbat_uvlo = (f1 & (1U << 2)) != 0;
    return ESP_OK;
}

static const char *axp517_ts_fault_text(axp517_ts_fault_t fault)
{
    switch (fault) {
    case AXP517_TS_FAULT_NORMAL:
        return NULL;
    case AXP517_TS_FAULT_CHG_COLD:
        return "TS charge cold";
    case AXP517_TS_FAULT_CHG_HOT:
        return "TS charge hot";
    case AXP517_TS_FAULT_WORK_COLD:
        return "TS work cold";
    case AXP517_TS_FAULT_WORK_HOT:
        return "TS work hot";
    default:
        return "TS unknown";
    }
}

static bool axp517_fault_text_append(char *buf, size_t buf_size, const char *text, bool has_text)
{
    if (!buf || buf_size == 0 || !text || text[0] == '\0') {
        return has_text;
    }

    size_t used = strnlen(buf, buf_size);
    if (used >= buf_size - 1) {
        return true;
    }

    int written = snprintf(buf + used, buf_size - used, "%s%s", has_text ? " / " : "", text);
    return has_text || written > 0;
}

const char *axp517_get_fault_char(axp517_handle_t *handle, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return "";
    }

    buf[0] = '\0';

    axp517_fault_t fault = {0};
    esp_err_t ret = axp517_get_fault(handle, &fault);
    if (ret != ESP_OK) {
        snprintf(buf, buf_size, "%s", "Read failed");
        return buf;
    }

    bool has_fault = false;
    has_fault = axp517_fault_text_append(buf, buf_size, axp517_ts_fault_text(fault.ts_fault), has_fault);
    has_fault = axp517_fault_text_append(buf, buf_size, fault.vsys_ov_5v ? "VSYS over 5V" : NULL, has_fault);
    has_fault = axp517_fault_text_append(buf, buf_size, fault.vbat_uvlo ? "VBAT UVLO" : NULL, has_fault);

    if (!has_fault) {
        snprintf(buf, buf_size, "%s", "Clear");
    }

    return buf;
}

esp_err_t axp517_clear_fault(axp517_handle_t *handle, uint8_t fault1_mask)
{
    return axp517_write_byte(handle, AXP517_REG_FAULT1, fault1_mask & 0x0C);
}

esp_err_t axp517_get_vbus_status(axp517_handle_t *handle, bool *present, bool *good)
{
    uint8_t val = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_STATUS0, &val);
    if (ret != ESP_OK) {
        return ret;
    }

    if (present) {
        *present = (val & AXP517_VBUS_PRESENT) != 0;
    }
    if (good) {
        *good = (val & AXP517_VBUS_GOOD) != 0;
    }
    return ESP_OK;
}

esp_err_t axp517_get_bat_present(axp517_handle_t *handle, bool *present)
{
    if (!present) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_STATUS0, &val);
    if (ret == ESP_OK) {
        *present = (val & AXP517_BAT_PRESENT) != 0;
    }
    return ret;
}

esp_err_t axp517_get_batfet_state(axp517_handle_t *handle, bool *on)
{
    if (!on) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_STATUS0, &val);
    if (ret == ESP_OK) {
        *on = (val & AXP517_BATFET_ON) != 0;
    }
    return ret;
}

esp_err_t axp517_get_charging_status(axp517_handle_t *handle, axp517_chg_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_STATUS1, &val);
    if (ret == ESP_OK) {
        uint8_t code = val & 0x07;
        *status = (code <= AXP517_CHG_NOT) ? (axp517_chg_status_t)code : AXP517_CHG_NOT;
    }
    return ret;
}

esp_err_t axp517_get_bc_detect(axp517_handle_t *handle, axp517_bc_detect_t *type)
{
    if (!type) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_BC_DETECT, &val);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t code = (val >> 5) & 0x07;
    *type = (code <= AXP517_BC_DCP) ? (axp517_bc_detect_t)code : AXP517_BC_UNKNOWN;
    return ESP_OK;
}

esp_err_t axp517_bc12_force_detect(axp517_handle_t *handle)
{
    return axp517_update_bits(handle, AXP517_REG_BC12_CTRL3, (1U << 7), (1U << 7));
}

esp_err_t axp517_bc12_enable_auto_dpdm(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_BC12_CTRL3, (1U << 6), enable ? (1U << 6) : 0);
}

esp_err_t axp517_irq_set_enabled(axp517_handle_t *handle, uint32_t mask)
{
    for (int i = 0; i < 4; i++) {
        esp_err_t ret = axp517_write_byte(handle, (uint8_t)(AXP517_REG_IRQ_EN0 + i), (uint8_t)((mask >> (i * 8)) & 0xFF));
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t axp517_irq_enable(axp517_handle_t *handle, uint32_t mask)
{
    for (int i = 0; i < 4; i++) {
        uint8_t reg = (uint8_t)(AXP517_REG_IRQ_EN0 + i);
        uint8_t byte_mask = (uint8_t)((mask >> (i * 8)) & 0xFF);
        if (byte_mask == 0) {
            continue;
        }
        esp_err_t ret = axp517_update_bits(handle, reg, byte_mask, byte_mask);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t axp517_irq_disable(axp517_handle_t *handle, uint32_t mask)
{
    for (int i = 0; i < 4; i++) {
        uint8_t reg = (uint8_t)(AXP517_REG_IRQ_EN0 + i);
        uint8_t byte_mask = (uint8_t)((mask >> (i * 8)) & 0xFF);
        if (byte_mask == 0) {
            continue;
        }
        esp_err_t ret = axp517_update_bits(handle, reg, byte_mask, 0);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t axp517_irq_read(axp517_handle_t *handle, axp517_irq_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    for (int i = 0; i < 4; i++) {
        esp_err_t ret = axp517_read_byte(handle, (uint8_t)(AXP517_REG_IRQ_STAT0 + i), &status->raw[i]);
        if (ret != ESP_OK) {
            return ret;
        }
        status->bits |= ((uint32_t)status->raw[i]) << (i * 8);
    }
    return ESP_OK;
}

esp_err_t axp517_irq_clear(axp517_handle_t *handle, uint32_t mask)
{
    for (int i = 0; i < 4; i++) {
        uint8_t byte_mask = (uint8_t)((mask >> (i * 8)) & 0xFF);
        if (byte_mask == 0) {
            continue;
        }
        esp_err_t ret = axp517_write_byte(handle, (uint8_t)(AXP517_REG_IRQ_STAT0 + i), byte_mask);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t axp517_irq_clear_all(axp517_handle_t *handle)
{
    return axp517_irq_clear(handle, 0xFFFFFFFFU);
}

esp_err_t axp517_set_precharge_current(axp517_handle_t *handle, uint16_t ma)
{
    if (ma > 960) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_IPRECHG_ITRICHG, 0x0F, (uint8_t)(ma / 64));
}

esp_err_t axp517_set_trickle_current(axp517_handle_t *handle, uint16_t ma)
{
    if (ma == 0) {
        return axp517_update_bits(handle, AXP517_REG_IPRECHG_ITRICHG, 0x70, 0);
    }
    if (ma < 32 || ma > 224) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_IPRECHG_ITRICHG, 0x70, (uint8_t)((ma / 32) << 4));
}

esp_err_t axp517_set_termination_current(axp517_handle_t *handle, uint16_t ma)
{
    if (ma > 960) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_ITERM_CTRL, 0x0F, (uint8_t)(ma / 64));
}

esp_err_t axp517_enable_charge_termination(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_ITERM_CTRL, (1U << 4), enable ? (1U << 4) : 0);
}

esp_err_t axp517_enable_dpm_charge_termination(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_ITERM_CTRL, (1U << 5), enable ? 0 : (1U << 5));
}

esp_err_t axp517_set_thermal_regulation_threshold(axp517_handle_t *handle, axp517_thermal_reg_threshold_t threshold)
{
    if (threshold > AXP517_THERMAL_REG_120C) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_THERMAL_CFG, 0x03, (uint8_t)threshold);
}

esp_err_t axp517_config_die_temperature_protection(axp517_handle_t *handle, bool enable,
                                                   axp517_die_ot_threshold_t threshold)
{
    if (threshold > AXP517_DIE_OT_135C) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = (uint8_t)(((uint8_t)threshold << 1) | (enable ? 0x01 : 0));
    return axp517_update_bits(handle, AXP517_REG_DIE_TEMP_CFG, 0x07, val);
}

esp_err_t axp517_config_charge_timer(axp517_handle_t *handle, bool pre_enable, uint8_t pre_timer_code,
                                     bool fast_enable, uint8_t fast_timer_code, bool slow_in_dpm)
{
    if (pre_timer_code > 3 || fast_timer_code > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    if (slow_in_dpm) {
        val |= (1U << 7);
    }
    if (fast_enable) {
        val |= (1U << 6);
    }
    val |= (uint8_t)((fast_timer_code & 0x03) << 4);
    if (pre_enable) {
        val |= (1U << 2);
    }
    val |= (pre_timer_code & 0x03);
    return axp517_update_bits(handle, AXP517_REG_CHG_TIMER_CFG, 0xF7, val);
}

esp_err_t axp517_enable_battery_detection(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_BAT_DET_CTRL, 0x01, enable ? 0x01 : 0);
}

esp_err_t axp517_force_batfet(axp517_handle_t *handle, bool on)
{
    if (on) {
        esp_err_t ret = axp517_set_batfet_force_disable(handle, false);
        if (ret != ESP_OK) {
            return ret;
        }
        return axp517_update_bits(handle, AXP517_REG_BATFET_CTRL, 0x01, 0x01);
    }
    return axp517_set_batfet_force_disable(handle, true);
}

esp_err_t axp517_set_batfet_force_disable(axp517_handle_t *handle, bool disable)
{
    return axp517_update_bits(handle, AXP517_REG_BATFET_CTRL, (1U << 2), disable ? (1U << 2) : 0);
}

esp_err_t axp517_enter_ship_mode(axp517_handle_t *handle)
{
    return axp517_update_bits(handle, AXP517_REG_BATFET_CTRL, (1U << 3), (1U << 3));
}

esp_err_t axp517_enable_batfet_ocp_close(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_BATFET_CTRL, (1U << 1), enable ? (1U << 1) : 0);
}

esp_err_t axp517_set_batfet_close_delay(axp517_handle_t *handle, axp517_batfet_delay_t delay)
{
    if (delay > AXP517_BATFET_DELAY_32MS) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_BATFET_CTRL, 0x30, (uint8_t)(delay << 4));
}

esp_err_t axp517_force_rbfet_enable(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_RBFET_CTRL, 0x01, enable ? 0x01 : 0);
}

esp_err_t axp517_set_boost_voltage(axp517_handle_t *handle, uint16_t mv)
{
    if (mv < 4550 || mv > 5510) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t code = (uint16_t)((mv - 4550 + 32) / 64);
    if (code > 15) {
        code = 15;
    }
    return axp517_update_bits(handle, AXP517_REG_BOOST_CFG, 0xF0, (uint8_t)(code << 4));
}

esp_err_t axp517_set_boost_disable_threshold(axp517_handle_t *handle, axp517_boost_disable_threshold_t threshold)
{
    if (threshold > AXP517_BOOST_DISABLE_2V6) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_BOOST_CFG, 0x0C, (uint8_t)(threshold << 2));
}

esp_err_t axp517_set_boost_current_limit(axp517_handle_t *handle, axp517_boost_current_limit_t limit)
{
    if (limit > AXP517_BOOST_LIMIT_2000MA) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_update_bits(handle, AXP517_REG_BOOST_CFG, 0x03, (uint8_t)limit);
}

esp_err_t axp517_enable_mppt(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MPPT_CFG, 0x01, enable ? 0x01 : 0);
}

esp_err_t axp517_get_mppt_state(axp517_handle_t *handle, bool *enabled)
{
    if (!enabled) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_MPPT_CFG, &val);
    if (ret == ESP_OK) {
        *enabled = (val & (1U << 1)) != 0;
    }
    return ret;
}

esp_err_t axp517_enable_watchdog(axp517_handle_t *handle, bool enable)
{
    esp_err_t ret = axp517_update_bits(handle, AXP517_REG_MOD_EN0, AXP517_MOD0_WD_CLK_EN,
                                       enable ? AXP517_MOD0_WD_CLK_EN : 0);
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_update_bits(handle, AXP517_REG_MOD_EN1, AXP517_MOD1_WD_EN,
                              enable ? AXP517_MOD1_WD_EN : 0);
}

esp_err_t axp517_watchdog_feed(axp517_handle_t *handle)
{
    return axp517_update_bits(handle, AXP517_REG_WDT_CTRL, (1U << 3), (1U << 3));
}

esp_err_t axp517_watchdog_config(axp517_handle_t *handle, axp517_watchdog_timeout_t timeout,
                                 axp517_watchdog_action_t action)
{
    if (timeout > AXP517_WDT_TIMEOUT_128S || action > AXP517_WDT_ACTION_POR) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = (uint8_t)(((uint8_t)action << 4) | ((uint8_t)timeout & 0x07));
    return axp517_update_bits(handle, AXP517_REG_WDT_CTRL, 0x37, val);
}

esp_err_t axp517_reset_gauge(axp517_handle_t *handle)
{
    return axp517_update_bits(handle, AXP517_REG_RESET_CFG, (1U << 2), (1U << 2));
}

esp_err_t axp517_software_por(axp517_handle_t *handle)
{
    return axp517_update_bits(handle, AXP517_REG_RESET_CFG, 0x01, 0x01);
}

esp_err_t axp517_enable_pwron_16s_por(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_RESET_CFG, (1U << 1), enable ? (1U << 1) : 0);
}

esp_err_t axp517_config_pwron(axp517_handle_t *handle, bool offlevel_close_batfet, bool irq_open_batfet,
                              uint8_t irq_level_code, uint8_t off_level_code, uint8_t on_level_code)
{
    if (irq_level_code > 3 || off_level_code > 3 || on_level_code > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    if (offlevel_close_batfet) {
        val |= (1U << 7);
    }
    if (irq_open_batfet) {
        val |= (1U << 6);
    }
    val |= (uint8_t)((irq_level_code & 0x03) << 4);
    val |= (uint8_t)((off_level_code & 0x03) << 2);
    val |= (on_level_code & 0x03);
    return axp517_write_byte(handle, AXP517_REG_PWRON_CFG, val);
}

esp_err_t axp517_enable_fuel_gauge(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN0, AXP517_MOD0_GAUGE_EN,
                              enable ? AXP517_MOD0_GAUGE_EN : 0);
}

esp_err_t axp517_set_fuel_gauge_low_freq(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_MOD_EN1, AXP517_MOD1_GAUGE_LOW_FREQ,
                              enable ? AXP517_MOD1_GAUGE_LOW_FREQ : 0);
}

esp_err_t axp517_set_low_battery_threshold(axp517_handle_t *handle, uint8_t warn_percent, uint8_t shutdown_percent)
{
    if (warn_percent < 5 || warn_percent > 20 || shutdown_percent > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = (uint8_t)(((warn_percent - 5) << 4) | (shutdown_percent & 0x0F));
    return axp517_write_byte(handle, AXP517_REG_LOW_BAT_WARN, val);
}

esp_err_t axp517_read_battery_temperature_reg(axp517_handle_t *handle, uint8_t *raw_temp)
{
    return axp517_read_byte(handle, AXP517_REG_BAT_TEMP, raw_temp);
}

esp_err_t axp517_read_battery_soh(axp517_handle_t *handle, uint8_t *soh)
{
    return axp517_read_byte(handle, AXP517_REG_BAT_SOH, soh);
}

esp_err_t axp517_read_battery_percent(axp517_handle_t *handle, uint8_t *percent)
{
    return axp517_read_byte(handle, AXP517_REG_BAT_PERCENT, percent);
}

esp_err_t axp517_fuel_gauge_select_sram(axp517_handle_t *handle, bool sram)
{
    return axp517_update_bits(handle, AXP517_REG_FG_CTRL, (1U << 4), sram ? (1U << 4) : 0);
}

esp_err_t axp517_fuel_gauge_enable_brom_write(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_FG_CTRL, 0x01, enable ? 0x01 : 0);
}

esp_err_t axp517_write_battery_param(axp517_handle_t *handle, uint8_t value)
{
    return axp517_write_byte(handle, AXP517_REG_BAT_PARAM, value);
}

esp_err_t axp517_read_battery_param(axp517_handle_t *handle, uint8_t *value)
{
    return axp517_read_byte(handle, AXP517_REG_BAT_PARAM, value);
}

esp_err_t axp517_enable_adc_channels(axp517_handle_t *handle, uint8_t mask)
{
    return axp517_write_byte(handle, AXP517_REG_ADC_EN, mask);
}

esp_err_t axp517_read_vbat(axp517_handle_t *handle, uint16_t *mv)
{
    return axp517_read_u14(handle, AXP517_REG_VBAT_H, mv);
}

esp_err_t axp517_read_vbus(axp517_handle_t *handle, uint16_t *mv)
{
    if (!mv) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw = 0;
    esp_err_t ret = axp517_read_u14(handle, AXP517_REG_VBUS_H, &raw);
    if (ret == ESP_OK) {
        *mv = (uint16_t)(raw * 2);
    }
    return ret;
}

esp_err_t axp517_read_vsys(axp517_handle_t *handle, uint16_t *mv)
{
    return axp517_read_adc_selected(handle, 0x01, mv);
}

esp_err_t axp517_read_ibat_charge(axp517_handle_t *handle, uint16_t *ma)
{
    if (!ma) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_u16 = 0;
    esp_err_t ret = axp517_read_u16(handle, AXP517_REG_IBAT_H, &raw_u16);
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t raw = (int16_t)raw_u16;
    *ma = (raw > 0) ? (uint16_t)(((uint32_t)raw * 25U) / 100U) : 0;
    return ESP_OK;
}

esp_err_t axp517_read_ibat_discharge(axp517_handle_t *handle, uint16_t *ma)
{
    if (!ma) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_u16 = 0;
    esp_err_t ret = axp517_read_u16(handle, AXP517_REG_IBAT_H, &raw_u16);
    if (ret != ESP_OK) {
        return ret;
    }

    int16_t raw = (int16_t)raw_u16;
    *ma = (raw < 0) ? (uint16_t)(((uint32_t)(-raw) * 25U) / 100U) : 0;
    return ESP_OK;
}

esp_err_t axp517_read_ibus(axp517_handle_t *handle, uint16_t *ma)
{
    return axp517_read_u14(handle, AXP517_REG_IBUS_H, ma);
}

esp_err_t axp517_read_ts_voltage(axp517_handle_t *handle, uint16_t *mv)
{
    if (!mv) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw = 0;
    esp_err_t ret = axp517_read_u14(handle, AXP517_REG_TS_H, &raw);
    if (ret == ESP_OK) {
        *mv = (uint16_t)(raw / 2);
    }
    return ret;
}

esp_err_t axp517_read_adc_selected(axp517_handle_t *handle, uint8_t source, uint16_t *raw)
{
    if (source > 0x0F || !raw) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = axp517_update_bits(handle, AXP517_REG_ADC_SEL, 0x0F, source);
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_read_u14(handle, AXP517_REG_ADC_DATA_H, raw);
}

esp_err_t axp517_read_die_temperature(axp517_handle_t *handle, float *deg_c)
{
    if (!deg_c) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw = 0;
    esp_err_t ret = axp517_read_adc_selected(handle, 0x00, &raw);
    if (ret == ESP_OK) {
        *deg_c = (3552.0f - (float)raw) / 1.79f + 25.0f;
    }
    return ret;
}

esp_err_t axp517_read_battery_temperature(axp517_handle_t *handle, float *deg_c)
{
    if (!deg_c) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t mv = 0;
    esp_err_t ret = axp517_read_ts_voltage(handle, &mv);
    if (ret == ESP_OK) {
        *deg_c = (float)mv;
    }
    return ret;
}

esp_err_t axp517_config_ts_pin(axp517_handle_t *handle, axp517_ts_pin_mode_t mode,
                               axp517_ts_current_mode_t current_mode, axp517_ts_current_t current)
{
    if (mode > AXP517_TS_PIN_FIXED_INPUT ||
        current_mode > AXP517_TS_CURRENT_ALWAYS_ON ||
        current > AXP517_TS_CURRENT_60UA) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = (uint8_t)(((uint8_t)mode << 4) | ((uint8_t)current_mode << 2) | (uint8_t)current);
    return axp517_update_bits(handle, AXP517_REG_TS_CFG, 0x1F, val);
}

esp_err_t axp517_set_ts_hysteresis(axp517_handle_t *handle, uint16_t low_to_normal_mv, uint16_t high_to_normal_mv)
{
    uint16_t low_code = low_to_normal_mv / 16;
    uint16_t high_code = high_to_normal_mv / 4;
    if (low_code > 255 || high_code > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_TS_HYSL2H, (uint8_t)low_code);
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_write_byte(handle, AXP517_REG_TS_HYSH2L, (uint8_t)high_code);
}

esp_err_t axp517_set_ts_charge_thresholds(axp517_handle_t *handle, uint16_t cold_mv, uint16_t hot_mv)
{
    uint16_t cold_code = cold_mv / 32;
    uint16_t hot_code = hot_mv / 2;
    if (cold_code > 255 || hot_code > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_VLTF_CHG, (uint8_t)cold_code);
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_write_byte(handle, AXP517_REG_VHTF_CHG, (uint8_t)hot_code);
}

esp_err_t axp517_set_ts_work_thresholds(axp517_handle_t *handle, uint16_t cold_mv, uint16_t hot_mv)
{
    uint16_t cold_code = cold_mv / 32;
    uint16_t hot_code = hot_mv / 2;
    if (cold_code > 255 || hot_code > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_VLTF_WORK, (uint8_t)cold_code);
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_write_byte(handle, AXP517_REG_VHTF_WORK, (uint8_t)hot_code);
}

esp_err_t axp517_enable_jeita(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_JEITA_EN, 0x01, enable ? 0x01 : 0);
}

esp_err_t axp517_config_jeita(axp517_handle_t *handle, uint8_t warm_current_fall, uint8_t cool_current_fall,
                              uint8_t warm_voltage_fall, uint8_t cool_voltage_fall)
{
    if (warm_current_fall > 2 || cool_current_fall > 2 || warm_voltage_fall > 2 || cool_voltage_fall > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = (uint8_t)((warm_current_fall << 6) | (cool_current_fall << 4) |
                            (warm_voltage_fall << 2) | cool_voltage_fall);
    return axp517_write_byte(handle, AXP517_REG_JEITA_CFG, val);
}

esp_err_t axp517_set_jeita_cool_warm(axp517_handle_t *handle, uint16_t cool_mv, uint16_t warm_mv)
{
    uint16_t cool_code = cool_mv / 16;
    uint16_t warm_code = warm_mv / 8;
    if (cool_code > 255 || warm_code > 255) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_JEITA_COOL, (uint8_t)cool_code);
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_write_byte(handle, AXP517_REG_JEITA_WARM, (uint8_t)warm_code);
}

esp_err_t axp517_set_ts_fixed_voltage(axp517_handle_t *handle, uint16_t mv)
{
    uint16_t raw = mv * 2;
    if (raw > 0x3FFF) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_TS_DATA_H, (uint8_t)((raw >> 8) & 0x3F));
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_write_byte(handle, AXP517_REG_TS_DATA_L, (uint8_t)(raw & 0xFF));
}

esp_err_t axp517_select_ts_fixed_voltage(axp517_handle_t *handle, bool fixed)
{
    return axp517_update_bits(handle, AXP517_REG_TS_SRC_SEL, (1U << 3), fixed ? (1U << 3) : 0);
}

esp_err_t axp517_gpio_set_output(axp517_handle_t *handle, axp517_gpio_level_t level)
{
    if (level > AXP517_GPIO_HIGH) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = (uint8_t)((1U << 4) | ((uint8_t)level & 0x03));
    return axp517_update_bits(handle, AXP517_REG_GPIO_CFG, 0x1F, val);
}

esp_err_t axp517_gpio_get_input(axp517_handle_t *handle, bool *high)
{
    if (!high) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_GPIO_CFG, &val);
    if (ret == ESP_OK) {
        *high = (val & (1U << 1)) != 0;
    }
    return ret;
}

esp_err_t axp517_gpio_set_pd_irq_source(axp517_handle_t *handle, bool enable)
{
    return axp517_update_bits(handle, AXP517_REG_GPIO_CFG, 0x0C, enable ? 0x04 : 0);
}

esp_err_t axp517_od_set_low(axp517_handle_t *handle, bool low)
{
    uint8_t val = (1U << 7) | (low ? (1U << 6) : 0);
    return axp517_update_bits(handle, AXP517_REG_GPIO_CFG, 0xC0, val);
}

esp_err_t axp517_config_chgled(axp517_handle_t *handle, bool push_pull, axp517_chgled_mode_t mode,
                               axp517_chgled_output_t output, bool breath_enable)
{
    if (mode > AXP517_CHGLED_BY_REG || output > AXP517_CHGLED_REG_HIGH) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    if (push_pull) {
        val |= (1U << 7);
    }
    if (breath_enable) {
        val |= (1U << 6);
    }
    val |= (uint8_t)(((uint8_t)output & 0x07) << 3);
    val |= ((uint8_t)mode & 0x07);
    return axp517_write_byte(handle, AXP517_REG_CHGLED_CFG, val);
}

esp_err_t axp517_write_breath_led_raw(axp517_handle_t *handle, const uint8_t values[6])
{
    if (!values) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t regs[6] = {
        AXP517_REG_BREATH_CTRL0,
        AXP517_REG_BREATH_CTRL1,
        AXP517_REG_BREATH_CTRL2,
        AXP517_REG_BREATH_CTRL3,
        AXP517_REG_BREATH_CTRL4,
        AXP517_REG_BREATH_CTRL5,
    };

    for (int i = 0; i < 6; i++) {
        esp_err_t ret = axp517_write_byte(handle, regs[i], values[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t axp517_data_buffer_write(axp517_handle_t *handle, uint8_t index, uint8_t value)
{
    const uint8_t regs[3] = {AXP517_REG_DATA_BUF0, AXP517_REG_DATA_BUF1, AXP517_REG_DATA_BUF2};
    if (index >= 3) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_write_byte(handle, regs[index], value);
}

esp_err_t axp517_data_buffer_read(axp517_handle_t *handle, uint8_t index, uint8_t *value)
{
    const uint8_t regs[3] = {AXP517_REG_DATA_BUF0, AXP517_REG_DATA_BUF1, AXP517_REG_DATA_BUF2};
    if (index >= 3) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp517_read_byte(handle, regs[index], value);
}

esp_err_t axp517_pd_read_version(axp517_handle_t *handle, axp517_pd_version_t *version)
{
    if (!version) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(version, 0, sizeof(*version));
    esp_err_t ret = axp517_read_byte(handle, AXP517_REG_USBTYPEC_REV_L, &version->typec_rev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = axp517_read_byte(handle, AXP517_REG_USBPD_VER, &version->usbpd_ver);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = axp517_read_byte(handle, AXP517_REG_USBPD_REV, &version->usbpd_rev);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = axp517_read_byte(handle, AXP517_REG_PD_IF_VER, &version->pd_if_ver);
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_read_byte(handle, AXP517_REG_PD_IF_REV, &version->pd_if_rev);
}

esp_err_t axp517_pd_read_alert(axp517_handle_t *handle, uint16_t *alert)
{
    if (!alert) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[2] = {0};
    esp_err_t ret = axp517_read_block(handle, AXP517_REG_PD_ALERT_L, buf, sizeof(buf));
    if (ret == ESP_OK) {
        *alert = (uint16_t)(buf[0] | (buf[1] << 8));
    }
    return ret;
}

esp_err_t axp517_pd_clear_alert(axp517_handle_t *handle, uint16_t alert)
{
    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_PD_ALERT_L, (uint8_t)(alert & 0xFF));
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_write_byte(handle, AXP517_REG_PD_ALERT_H, (uint8_t)(alert >> 8));
}

esp_err_t axp517_pd_set_alert_mask(axp517_handle_t *handle, uint16_t mask)
{
    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_PD_ALERT_MASK_L, (uint8_t)(mask & 0xFF));
    if (ret != ESP_OK) {
        return ret;
    }
    return axp517_write_byte(handle, AXP517_REG_PD_ALERT_MASK_H, (uint8_t)(mask >> 8));
}

esp_err_t axp517_pd_command(axp517_handle_t *handle, uint8_t command)
{
    return axp517_write_byte(handle, AXP517_REG_COMMAND, command);
}

esp_err_t axp517_pd_read_cc_status(axp517_handle_t *handle, uint8_t *status)
{
    return axp517_read_byte(handle, AXP517_REG_CC_STATUS, status);
}

esp_err_t axp517_pd_read_power_status(axp517_handle_t *handle, uint8_t *status)
{
    return axp517_read_byte(handle, AXP517_REG_POWER_STATUS, status);
}

esp_err_t axp517_pd_read_fault_status(axp517_handle_t *handle, uint8_t *status)
{
    return axp517_read_byte(handle, AXP517_REG_FAULT_STATUS, status);
}

esp_err_t axp517_pd_clear_fault_status(axp517_handle_t *handle, uint8_t fault)
{
    return axp517_write_byte(handle, AXP517_REG_FAULT_STATUS, fault);
}

esp_err_t axp517_pd_set_receive_detect(axp517_handle_t *handle, uint8_t mask)
{
    return axp517_write_byte(handle, AXP517_REG_RECEIVE_DETECT, mask);
}

esp_err_t axp517_pd_read_rx_buffer(axp517_handle_t *handle, uint8_t *buf, size_t len)
{
    return axp517_read_block(handle, AXP517_REG_RX_BUFFER, buf, len);
}

esp_err_t axp517_pd_write_tx_buffer(axp517_handle_t *handle, uint8_t frame_type, const uint8_t *buf, size_t len)
{
    if (len > 0 && !buf) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = axp517_write_byte(handle, AXP517_REG_TX_BUF_FRAME_TYPE, frame_type);
    if (ret != ESP_OK || len == 0) {
        return ret;
    }

    for (size_t i = 0; i < len; i++) {
        ret = axp517_write_byte(handle, AXP517_REG_TX_BUFFER, buf[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}
