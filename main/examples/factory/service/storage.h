#pragma once

#include "esp_err.h"
#include "esp_io_expander.h"
#include "factory_service_types.h"

esp_err_t storage_init(esp_io_expander_handle_t io_expander);
bool usb_otg_msc_set(bool usb_mode);
bool usb_otg_msc_is_usb_mode(void);
bool storage_info_get(factory_storage_info_t *info);
bool storage_app_access_begin(void);
void storage_app_access_end(void);
