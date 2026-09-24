#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "bsp_private.h"

static const char *TAG = "t_panel_p4_bsp";

#define BSP_MIPI_PHY_LDO_CHANNEL 3
#define BSP_MIPI_PHY_LDO_VOLTAGE_MV 2500

const char *t_panel_p4_bsp_get_board_name(void)
{
    return CONFIG_T_PANEL_P4_BOARD_NAME;
}

esp_err_t t_panel_p4_bsp_init(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");
    memset(bsp, 0, sizeof(*bsp));

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bsp->i2c_bus), TAG,
                        "create I2C bus failed");

    esp_err_t ret = t_panel_p4_bsp_board_init(bsp);
    if (ret != ESP_OK) {
        i2c_del_master_bus(bsp->i2c_bus);
        memset(bsp, 0, sizeof(*bsp));
        return ret;
    }

    ESP_LOGI(TAG, "%s initialized", t_panel_p4_bsp_get_board_name());
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_deinit(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");

    esp_err_t ret = ESP_OK;
    if (bsp->camera_flash_pwm_initialized) {
        ret = t_panel_p4_bsp_camera_flash_pwm_deinit(bsp);
    }
    esp_err_t resource_ret = t_panel_p4_bsp_speaker_set_enabled(bsp, false);
    if (ret == ESP_OK) {
        ret = resource_ret;
    }
    resource_ret = t_panel_p4_bsp_i2s_deinit(bsp);
    if (ret == ESP_OK) {
        ret = resource_ret;
    }
    resource_ret = t_panel_p4_bsp_spi_deinit(bsp);
    if (ret == ESP_OK) {
        ret = resource_ret;
    }
    resource_ret = t_panel_p4_bsp_mipi_phy_deinit(bsp);
    if (ret == ESP_OK) {
        ret = resource_ret;
    }
    resource_ret = t_panel_p4_bsp_board_deinit(bsp);
    if (ret == ESP_OK) {
        ret = resource_ret;
    }
    if (bsp->i2c_bus) {
        const esp_err_t bus_ret = i2c_del_master_bus(bsp->i2c_bus);
        if (ret == ESP_OK) {
            ret = bus_ret;
        }
    }
    memset(bsp, 0, sizeof(*bsp));
    return ret;
}

i2c_master_bus_handle_t t_panel_p4_bsp_get_i2c_bus(const t_panel_p4_bsp_t *bsp)
{
    return bsp ? bsp->i2c_bus : NULL;
}

esp_io_expander_handle_t t_panel_p4_bsp_get_io_expander(const t_panel_p4_bsp_t *bsp)
{
    return bsp ? bsp->io_expander : NULL;
}

esp_err_t t_panel_p4_bsp_spi_init(t_panel_p4_bsp_t *bsp,
                                  spi_host_device_t host,
                                  int max_transfer_size)
{
    ESP_RETURN_ON_FALSE(bsp && !bsp->spi_initialized && max_transfer_size >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid SPI init arguments");
#if CONFIG_T_PANEL_P4_BOARD_RECT
    (void)host;
    ESP_LOGW(TAG, "Rect board has no dedicated general-purpose SPI bus pins");
    return ESP_ERR_NOT_SUPPORTED;
#else
    const spi_bus_config_t config = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = max_transfer_size,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(host, &config, SPI_DMA_CH_AUTO),
                        TAG, "initialize SPI bus failed");
    bsp->spi_host = host;
    bsp->spi_initialized = true;
    ESP_LOGI(TAG, "SPI bus initialized: host=%d, SCLK=%d, MISO=%d, MOSI=%d",
             host, SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    return ESP_OK;
#endif
}

esp_err_t t_panel_p4_bsp_spi_deinit(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");
    if (!bsp->spi_initialized) {
        return ESP_OK;
    }
    esp_err_t ret = spi_bus_free(bsp->spi_host);
    if (ret == ESP_OK) {
        bsp->spi_initialized = false;
    }
    return ret;
}

esp_err_t t_panel_p4_bsp_i2s_init(t_panel_p4_bsp_t *bsp,
                                  const t_panel_p4_bsp_i2s_config_t *config)
{
    ESP_RETURN_ON_FALSE(bsp && config && !bsp->i2s_tx && !bsp->i2s_rx &&
                            config->sample_rate_hz > 0 &&
                            config->dma_desc_num > 0 &&
                            config->dma_frame_num > 0 &&
                            (config->enable_tx || config->enable_rx),
                        ESP_ERR_INVALID_ARG, TAG, "invalid I2S init arguments");

    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, config->role);
    channel_config.auto_clear = true;
    channel_config.dma_desc_num = config->dma_desc_num;
    channel_config.dma_frame_num = config->dma_frame_num;
    esp_err_t ret = i2s_new_channel(&channel_config,
                                    config->enable_tx ? &tx : NULL,
                                    config->enable_rx ? &rx : NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(config->data_bit_width,
                                                        config->slot_mode),
        .gpio_cfg = {
            .mclk = I2S_MCK_PIN,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DO_PIN,
            .din = I2S_DI_PIN,
        },
    };
    standard_config.clk_cfg.mclk_multiple = config->mclk_multiple;
    if (tx) {
        ret = i2s_channel_init_std_mode(tx, &standard_config);
    }
    if (ret == ESP_OK && rx) {
        ret = i2s_channel_init_std_mode(rx, &standard_config);
    }
    if (ret != ESP_OK) {
        if (tx) {
            i2s_del_channel(tx);
        }
        if (rx) {
            i2s_del_channel(rx);
        }
        return ret;
    }

    bsp->i2s_tx = tx;
    bsp->i2s_rx = rx;
    ESP_LOGI(TAG, "I2S initialized: %u Hz, MCLK=%d, BCLK=%d, WS=%d, DOUT=%d, DIN=%d",
             (unsigned)config->sample_rate_hz, I2S_MCK_PIN, I2S_BCK_PIN,
             I2S_WS_PIN, I2S_DO_PIN, I2S_DI_PIN);
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_i2s_deinit(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");
    esp_err_t ret = ESP_OK;
    if (bsp->i2s_tx) {
        ret = i2s_del_channel(bsp->i2s_tx);
        bsp->i2s_tx = NULL;
    }
    if (bsp->i2s_rx) {
        esp_err_t rx_ret = i2s_del_channel(bsp->i2s_rx);
        bsp->i2s_rx = NULL;
        if (ret == ESP_OK) {
            ret = rx_ret;
        }
    }
    return ret;
}

esp_err_t t_panel_p4_bsp_speaker_set_enabled(t_panel_p4_bsp_t *bsp, bool enabled)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");
    if (bsp->speaker_enabled == enabled) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(bsp->i2c_bus, ESP_ERR_INVALID_STATE, TAG,
                        "BSP is not initialized");
    ESP_RETURN_ON_ERROR(t_panel_p4_bsp_board_speaker_set(bsp, enabled), TAG,
                        "set speaker amplifier failed");
    bsp->speaker_enabled = enabled;
    return ESP_OK;
}

bool t_panel_p4_bsp_speaker_is_enabled(const t_panel_p4_bsp_t *bsp)
{
    return bsp && bsp->speaker_enabled;
}

esp_err_t t_panel_p4_bsp_mipi_phy_init(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");
    if (bsp->mipi_phy_ldo) {
        return ESP_OK;
    }
    const esp_ldo_channel_config_t config = {
        .chan_id = BSP_MIPI_PHY_LDO_CHANNEL,
        .voltage_mv = BSP_MIPI_PHY_LDO_VOLTAGE_MV,
    };
    esp_err_t ret = esp_ldo_acquire_channel(&config, &bsp->mipi_phy_ldo);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* LCD may already own the shared channel; keep it powered. */
        ESP_LOGW(TAG, "MIPI PHY LDO%d is already acquired, continue",
                 BSP_MIPI_PHY_LDO_CHANNEL);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "acquire MIPI PHY LDO failed");
    ESP_LOGI(TAG, "MIPI PHY LDO%d enabled at %dmV",
             BSP_MIPI_PHY_LDO_CHANNEL, BSP_MIPI_PHY_LDO_VOLTAGE_MV);
    return ESP_OK;
}

esp_err_t t_panel_p4_bsp_mipi_phy_deinit(t_panel_p4_bsp_t *bsp)
{
    ESP_RETURN_ON_FALSE(bsp, ESP_ERR_INVALID_ARG, TAG, "BSP handle is NULL");
    if (!bsp->mipi_phy_ldo) {
        return ESP_OK;
    }
    esp_err_t ret = esp_ldo_release_channel(bsp->mipi_phy_ldo);
    if (ret == ESP_OK) {
        bsp->mipi_phy_ldo = NULL;
    }
    return ret;
}
