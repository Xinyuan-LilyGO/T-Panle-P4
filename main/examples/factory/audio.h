#ifndef __AUDIO_H__
#define __AUDIO_H__

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"

esp_err_t factory_audio_init(i2c_master_bus_handle_t i2c_bus,
                             esp_io_expander_handle_t io_expander);
void factory_audio_stop_music(void);

#endif // __AUDIO_H__
