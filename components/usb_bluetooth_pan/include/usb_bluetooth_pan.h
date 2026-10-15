// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esp_err.h"

esp_err_t usb_bluetooth_pan_init(void);
esp_err_t usb_bluetooth_pan_start(void);
esp_err_t usb_bluetooth_pan_stop(void);
esp_err_t usb_bluetooth_pan_register_http(void);
