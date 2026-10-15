// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "pan_policy.h"

typedef struct {
    bool enabled, auto_connect, prefer_pan, allow_legacy_pairing, cli;
    uint8_t peer[6];
    char device_name[33], legacy_pin[17];
    uint32_t reconnect_min_s, reconnect_max_s, connect_timeout_s;
    uint32_t dhcp_timeout_s, pairing_timeout_s;
} pan_config_t;

typedef struct {
    bool compiled, enabled, usb_host_enabled, dongle_present, bonded;
    bool numeric_pending, network_up, manual_disconnect;
    char state[24], error[128], adapter_address[18], peer_address[18];
    char adapter_product[48], ip[16], gateway[16], dns[16];
    uint16_t vid, pid, revision, manufacturer, hci_revision, lmp_subversion;
    uint8_t hci_version;
    uint32_t passkey, attempts, reconnects, tx_frames, rx_frames;
    uint32_t tx_dropped, rx_dropped;
    uint64_t retry_at_ms, pairing_until_ms;
    unsigned scan_count;
    struct { char address[18], name[64]; uint32_t class_of_device; int rssi; } scan[PAN_SCAN_MAX];
} pan_status_t;

extern pan_config_t pan_config;
esp_err_t pan_settings_register(void);
esp_err_t pan_cli_register(void);
void pan_status_read(pan_status_t *out);
void pan_status_lock(void);
void pan_status_unlock(void);
extern pan_status_t pan_status;
esp_err_t pan_command(const char *action, uint32_t value);
esp_err_t pan_backend_start(void);
esp_err_t pan_backend_stop(void);
