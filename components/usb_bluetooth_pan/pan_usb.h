// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "hci_transport.h"

esp_err_t pan_usb_init(void);
void pan_usb_accept_connections(bool accept);
bool pan_usb_present(void);
uint32_t pan_usb_generation(void);
bool pan_usb_poll(void);
void pan_usb_close(void);
const hci_transport_t *pan_usb_transport(void);
