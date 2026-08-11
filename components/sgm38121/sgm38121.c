#include "sgm38121.h"
#include "esp_log.h"

#define TAG "SGM38121"

/* DVDD 电压公式: V = (504 + 8 * d) mV, 有效范围 d = 0x03~0x7D */
#define DVDD_BASE_MV    504
#define DVDD_STEP_MV    8
#define DVDD_MIN_MV     528
#define DVDD_MAX_MV     1504
#define DVDD_REG_MIN    0x03
#define DVDD_REG_MAX    0x7D

/* AVDD 电压公式: V = (1384 + 8 * d) mV, 有效范围 d = 0x0F~0xFF */
#define AVDD_BASE_MV    1384
#define AVDD_STEP_MV    8
#define AVDD_MIN_MV     1504
#define AVDD_MAX_MV     3424
#define AVDD_REG_MIN    0x0F
#define AVDD_REG_MAX    0xFF

/* ======================== I2C 底层操作 ======================== */

esp_err_t sgm38121_write_reg(sgm38121_handle_t *handle, uint8_t reg, uint8_t val)
{
    uint8_t data[2] = { reg, val };
    return i2c_master_transmit(handle->dev_handle, data, sizeof(data), -1);
}

esp_err_t sgm38121_read_reg(sgm38121_handle_t *handle, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(handle->dev_handle, &reg, 1, val, 1, -1);
}

/* ======================== 初始化与销毁 ======================== */

esp_err_t sgm38121_init(sgm38121_handle_t *handle, i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!handle || !bus) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &handle->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SGM38121 device");
        return ret;
    }

    uint8_t rev;
    ret = sgm38121_read_reg(handle, SGM38121_REG_CHIP_REV, &rev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SGM38121 not responding");
        i2c_master_bus_rm_device(handle->dev_handle);
        handle->dev_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "SGM38121 initialized (chip_rev=0x%02X)", rev);
    return ESP_OK;
}

esp_err_t sgm38121_deinit(sgm38121_handle_t *handle)
{
    if (!handle || !handle->dev_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = i2c_master_bus_rm_device(handle->dev_handle);
    handle->dev_handle = NULL;
    return ret;
}

/* ======================== 通道使能控制 ======================== */

esp_err_t sgm38121_enable_channel(sgm38121_handle_t *handle, sgm38121_channel_t ch, bool enable)
{
    if (ch >= SGM38121_CH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, SGM38121_REG_ENABLE, &val);
    if (ret != ESP_OK) return ret;

    uint8_t bit = (1 << ch);
    if (enable) {
        val |= bit;
    } else {
        val &= ~bit;
    }
    return sgm38121_write_reg(handle, SGM38121_REG_ENABLE, val);
}

esp_err_t sgm38121_enable_all(sgm38121_handle_t *handle, bool enable)
{
    uint8_t val = enable ? 0x0F : 0x00;
    return sgm38121_write_reg(handle, SGM38121_REG_ENABLE, val);
}

/* ======================== 输出电压设置 ======================== */

esp_err_t sgm38121_set_dvdd1_voltage(sgm38121_handle_t *handle, uint16_t mv)
{
    if (mv < DVDD_MIN_MV || mv > DVDD_MAX_MV) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_val = (uint8_t)((mv - DVDD_BASE_MV) / DVDD_STEP_MV);
    if (reg_val < DVDD_REG_MIN) reg_val = DVDD_REG_MIN;
    if (reg_val > DVDD_REG_MAX) reg_val = DVDD_REG_MAX;
    return sgm38121_write_reg(handle, SGM38121_REG_DVDD1_VOUT, reg_val);
}

esp_err_t sgm38121_set_dvdd2_voltage(sgm38121_handle_t *handle, uint16_t mv)
{
    if (mv < DVDD_MIN_MV || mv > DVDD_MAX_MV) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_val = (uint8_t)((mv - DVDD_BASE_MV) / DVDD_STEP_MV);
    if (reg_val < DVDD_REG_MIN) reg_val = DVDD_REG_MIN;
    if (reg_val > DVDD_REG_MAX) reg_val = DVDD_REG_MAX;
    return sgm38121_write_reg(handle, SGM38121_REG_DVDD2_VOUT, reg_val);
}

esp_err_t sgm38121_set_avdd1_voltage(sgm38121_handle_t *handle, uint16_t mv)
{
    if (mv < AVDD_MIN_MV || mv > AVDD_MAX_MV) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_val = (uint8_t)((mv - AVDD_BASE_MV) / AVDD_STEP_MV);
    if (reg_val < AVDD_REG_MIN) reg_val = AVDD_REG_MIN;
    if (reg_val > AVDD_REG_MAX) reg_val = AVDD_REG_MAX;
    return sgm38121_write_reg(handle, SGM38121_REG_AVDD1_VOUT, reg_val);
}

esp_err_t sgm38121_set_avdd2_voltage(sgm38121_handle_t *handle, uint16_t mv)
{
    if (mv < AVDD_MIN_MV || mv > AVDD_MAX_MV) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_val = (uint8_t)((mv - AVDD_BASE_MV) / AVDD_STEP_MV);
    if (reg_val < AVDD_REG_MIN) reg_val = AVDD_REG_MIN;
    if (reg_val > AVDD_REG_MAX) reg_val = AVDD_REG_MAX;
    return sgm38121_write_reg(handle, SGM38121_REG_AVDD2_VOUT, reg_val);
}

/* ======================== 上电时序配置 ======================== */

esp_err_t sgm38121_set_sequence(sgm38121_handle_t *handle, sgm38121_channel_t ch, sgm38121_seq_slot_t slot)
{
    if (ch >= SGM38121_CH_MAX || slot > SGM38121_SEQ_SLOT_7) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr;
    uint8_t shift;

    switch (ch) {
        case SGM38121_CH_DVDD1:
            reg_addr = SGM38121_REG_SEQ_DVDD;
            shift = 0;
            break;
        case SGM38121_CH_DVDD2:
            reg_addr = SGM38121_REG_SEQ_DVDD;
            shift = 4;
            break;
        case SGM38121_CH_AVDD1:
            reg_addr = SGM38121_REG_SEQ_AVDD;
            shift = 0;
            break;
        case SGM38121_CH_AVDD2:
            reg_addr = SGM38121_REG_SEQ_AVDD;
            shift = 4;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, reg_addr, &val);
    if (ret != ESP_OK) return ret;

    val &= ~(0x0F << shift);
    val |= ((uint8_t)slot & 0x07) << shift;
    return sgm38121_write_reg(handle, reg_addr, val);
}

esp_err_t sgm38121_set_seq_speed(sgm38121_handle_t *handle, uint8_t speed)
{
    if (speed > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, SGM38121_REG_SEQ_CTRL, &val);
    if (ret != ESP_OK) return ret;

    val &= ~SGM38121_SEQ_SPEED_MASK;
    val |= (speed << 6);
    return sgm38121_write_reg(handle, SGM38121_REG_SEQ_CTRL, val);
}

esp_err_t sgm38121_seq_powerup(sgm38121_handle_t *handle)
{
    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, SGM38121_REG_SEQ_CTRL, &val);
    if (ret != ESP_OK) return ret;

    val &= ~SGM38121_SEQ_CTRL_MASK;
    val |= SGM38121_SEQ_CTRL_POWERUP;
    return sgm38121_write_reg(handle, SGM38121_REG_SEQ_CTRL, val);
}

esp_err_t sgm38121_seq_shutdown(sgm38121_handle_t *handle)
{
    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, SGM38121_REG_SEQ_CTRL, &val);
    if (ret != ESP_OK) return ret;

    val &= ~SGM38121_SEQ_CTRL_MASK;
    val |= SGM38121_SEQ_CTRL_SHUTDOWN;
    return sgm38121_write_reg(handle, SGM38121_REG_SEQ_CTRL, val);
}

/* ======================== 放电控制 ======================== */

esp_err_t sgm38121_set_discharge_mode(sgm38121_handle_t *handle, bool manual)
{
    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, SGM38121_REG_DISCHARGE, &val);
    if (ret != ESP_OK) return ret;

    if (manual) {
        val |= SGM38121_MANUAL_DISCH;
    } else {
        val &= ~SGM38121_MANUAL_DISCH;
    }
    return sgm38121_write_reg(handle, SGM38121_REG_DISCHARGE, val);
}

esp_err_t sgm38121_set_discharge_channel(sgm38121_handle_t *handle, sgm38121_channel_t ch, bool enable)
{
    if (ch >= SGM38121_CH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, SGM38121_REG_DISCHARGE, &val);
    if (ret != ESP_OK) return ret;

    uint8_t bit = (1 << ch);
    if (enable) {
        val |= bit;
    } else {
        val &= ~bit;
    }
    return sgm38121_write_reg(handle, SGM38121_REG_DISCHARGE, val);
}

/* ======================== 唤醒功能 ======================== */

esp_err_t sgm38121_set_wakeup(sgm38121_handle_t *handle, bool enable)
{
    uint8_t val;
    esp_err_t ret = sgm38121_read_reg(handle, SGM38121_REG_FUNCTION, &val);
    if (ret != ESP_OK) return ret;

    if (enable) {
        val |= SGM38121_WAKE_UP_EN;
    } else {
        val &= ~SGM38121_WAKE_UP_EN;
    }
    return sgm38121_write_reg(handle, SGM38121_REG_FUNCTION, val);
}

/* ======================== 芯片版本读取 ======================== */

esp_err_t sgm38121_get_chip_rev(sgm38121_handle_t *handle, uint8_t *rev)
{
    return sgm38121_read_reg(handle, SGM38121_REG_CHIP_REV, rev);
}
