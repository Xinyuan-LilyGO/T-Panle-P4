/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_system.h"

#include "T_Panle_P4_board_config.h"
#include "driver/i2c_master.h"
#include "sgm38121.h"

static const char *TAG = "sgm38121_example";
sgm38121_handle_t pmic;

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

    // 初始化
    if (sgm38121_init(&pmic, bus_handle, SGM38121_I2C_ADDR) != ESP_OK)
    {
        ESP_LOGE(TAG, "sgm38121 init failed");
    }

    sgm38121_set_dvdd1_voltage(&pmic, 1500); // DVDD1 = 1.5V
    sgm38121_set_avdd1_voltage(&pmic, 2500); // AVDD1 = 2.5V
    sgm38121_set_avdd2_voltage(&pmic, 3300); // AVDD2 = 3.3V

    sgm38121_set_sequence(&pmic, SGM38121_CH_DVDD1, SGM38121_SEQ_SLOT_1);
    sgm38121_set_sequence(&pmic, SGM38121_CH_AVDD1, SGM38121_SEQ_SLOT_2);
    sgm38121_set_sequence(&pmic, SGM38121_CH_AVDD2, SGM38121_SEQ_SLOT_3);

    sgm38121_seq_powerup(&pmic);

    while (1)
    {
        ESP_LOGI(TAG, "loop!");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
