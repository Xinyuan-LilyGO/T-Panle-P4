#ifndef __SGM38121_H__
#define __SGM38121_H__

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C 地址 */
#define SGM38121_I2C_ADDR           0x28

/* 寄存器地址 */
#define SGM38121_REG_CHIP_REV       0x00
#define SGM38121_REG_DISCHARGE      0x02
#define SGM38121_REG_DVDD1_VOUT     0x03
#define SGM38121_REG_DVDD2_VOUT     0x04
#define SGM38121_REG_AVDD1_VOUT     0x05
#define SGM38121_REG_AVDD2_VOUT     0x06
#define SGM38121_REG_FUNCTION       0x07
#define SGM38121_REG_SEQ_DVDD       0x0A
#define SGM38121_REG_SEQ_AVDD       0x0B
#define SGM38121_REG_ENABLE         0x0E
#define SGM38121_REG_SEQ_CTRL       0x0F

/* REG0x02 位定义 */
#define SGM38121_MANUAL_DISCH       (1 << 7)
#define SGM38121_AVDD2_DISCH_EN     (1 << 3)
#define SGM38121_AVDD1_DISCH_EN     (1 << 2)
#define SGM38121_DVDD2_DISCH_EN     (1 << 1)
#define SGM38121_DVDD1_DISCH_EN     (1 << 0)

/* REG0x07 位定义 */
#define SGM38121_WAKE_UP_EN         (1 << 2)

/* REG0x0E 位定义 */
#define SGM38121_AVDD2_EN           (1 << 3)
#define SGM38121_AVDD1_EN           (1 << 2)
#define SGM38121_DVDD2_EN           (1 << 1)
#define SGM38121_DVDD1_EN           (1 << 0)

/* REG0x0F 位定义 */
#define SGM38121_SEQ_SPEED_MASK     0xC0
#define SGM38121_SEQ_SPEED_2MS      (0x00 << 6)
#define SGM38121_SEQ_SPEED_1MS      (0x01 << 6)
#define SGM38121_SEQ_SPEED_0_5MS    (0x02 << 6)
#define SGM38121_SEQ_SPEED_0_25MS   (0x03 << 6)
#define SGM38121_SEQ_CTRL_MASK      0x30
#define SGM38121_SEQ_CTRL_STANDBY   (0x00 << 4)
#define SGM38121_SEQ_CTRL_POWERUP   (0x01 << 4)
#define SGM38121_SEQ_CTRL_SHUTDOWN  (0x02 << 4)

/* LDO 通道枚举 */
typedef enum {
    SGM38121_CH_DVDD1 = 0,
    SGM38121_CH_DVDD2,
    SGM38121_CH_AVDD1,
    SGM38121_CH_AVDD2,
    SGM38121_CH_MAX,
} sgm38121_channel_t;

/* 上电时序 slot 枚举 */
typedef enum {
    SGM38121_SEQ_SLOT_NONE = 0,
    SGM38121_SEQ_SLOT_1 = 1,
    SGM38121_SEQ_SLOT_2 = 2,
    SGM38121_SEQ_SLOT_3 = 3,
    SGM38121_SEQ_SLOT_4 = 4,
    SGM38121_SEQ_SLOT_5 = 5,
    SGM38121_SEQ_SLOT_6 = 6,
    SGM38121_SEQ_SLOT_7 = 7,
} sgm38121_seq_slot_t;

/* 设备句柄 */
typedef struct {
    i2c_master_dev_handle_t dev_handle;
} sgm38121_handle_t;

/* 初始化与销毁 */
esp_err_t sgm38121_init(sgm38121_handle_t *handle, i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t sgm38121_deinit(sgm38121_handle_t *handle);

/* 底层寄存器操作 */
esp_err_t sgm38121_write_reg(sgm38121_handle_t *handle, uint8_t reg, uint8_t val);
esp_err_t sgm38121_read_reg(sgm38121_handle_t *handle, uint8_t reg, uint8_t *val);

/* 通道使能/禁用 */
esp_err_t sgm38121_enable_channel(sgm38121_handle_t *handle, sgm38121_channel_t ch, bool enable);
esp_err_t sgm38121_enable_all(sgm38121_handle_t *handle, bool enable);

/* 输出电压设置 (单位: mV) */
esp_err_t sgm38121_set_dvdd1_voltage(sgm38121_handle_t *handle, uint16_t mv);
esp_err_t sgm38121_set_dvdd2_voltage(sgm38121_handle_t *handle, uint16_t mv);
esp_err_t sgm38121_set_avdd1_voltage(sgm38121_handle_t *handle, uint16_t mv);
esp_err_t sgm38121_set_avdd2_voltage(sgm38121_handle_t *handle, uint16_t mv);

/* 上电时序配置 */
esp_err_t sgm38121_set_sequence(sgm38121_handle_t *handle, sgm38121_channel_t ch, sgm38121_seq_slot_t slot);
esp_err_t sgm38121_set_seq_speed(sgm38121_handle_t *handle, uint8_t speed);
esp_err_t sgm38121_seq_powerup(sgm38121_handle_t *handle);
esp_err_t sgm38121_seq_shutdown(sgm38121_handle_t *handle);

/* 放电控制 */
esp_err_t sgm38121_set_discharge_mode(sgm38121_handle_t *handle, bool manual);
esp_err_t sgm38121_set_discharge_channel(sgm38121_handle_t *handle, sgm38121_channel_t ch, bool enable);

/* 唤醒功能 */
esp_err_t sgm38121_set_wakeup(sgm38121_handle_t *handle, bool enable);

/* 读取芯片版本 */
esp_err_t sgm38121_get_chip_rev(sgm38121_handle_t *handle, uint8_t *rev);

#ifdef __cplusplus
}
#endif

#endif
