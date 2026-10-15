// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_bluetooth_pan.h"
#include "pan_private.h"
#include "cmdline_manager.h"
#include "log_manager.h"
#include "settings_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

pan_status_t pan_status EXT_RAM_BSS_ATTR;
static SemaphoreHandle_t status_mutex;

void pan_status_lock(void) { if (status_mutex) xSemaphoreTake(status_mutex, portMAX_DELAY); }
void pan_status_unlock(void) { if (status_mutex) xSemaphoreGive(status_mutex); }
void pan_status_read(pan_status_t *out)
{
    pan_status_lock();
    *out = pan_status;
    pan_status_unlock();
}

#ifndef CONFIG_WICAN_USB_BLUETOOTH_PAN
esp_err_t pan_backend_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t pan_backend_stop(void) { return ESP_OK; }
esp_err_t pan_command(const char *action, uint32_t value)
{
    (void)action; (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static int cli(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        pan_status_t *status = malloc(sizeof(*status));
        if (!status) return 1;
        pan_status_read(status);
        cmdline_printf("PAN %s; adapter %s; peer %s; IP %s; error %s\n",
                       status->state, status->adapter_address, status->peer_address,
                       status->ip, status->error);
        if (status->numeric_pending) cmdline_printf("Pairing code: %06lu\n", (unsigned long)status->passkey);
        for (unsigned i = 0; i < status->scan_count; ++i)
            cmdline_printf("%s %s (%d dBm)\n", status->scan[i].address, status->scan[i].name, status->scan[i].rssi);
        free(status);
        return 0;
    }
    uint32_t value = 0;
    if (argc == 3 && !strcmp(argv[1], "confirm")) {
        char *end;
        unsigned long parsed = strtoul(argv[2], &end, 10);
        if (!argv[2][0] || *end || parsed > 999999) return 1;
        value = (uint32_t)parsed;
    } else if (argc != 2 || !strcmp(argv[1], "confirm")) return 1;
    esp_err_t error = pan_command(argv[1], value);
    cmdline_printf("%s\n", esp_err_to_name(error));
    return error != ESP_OK;
}

esp_err_t pan_cli_register(void)
{
    static const esp_console_cmd_t command = {
        .command = "pan", .help = "USB Bluetooth PAN status, scan, pair, connect, disconnect, confirm CODE, reject, forget",
        .func = cli,
    };
    return cmdline_manager_register(&command);
}

esp_err_t usb_bluetooth_pan_init(void)
{
    status_mutex = xSemaphoreCreateMutex();
    if (!status_mutex) return ESP_ERR_NO_MEM;
    strcpy(pan_status.state, "off");
#ifdef CONFIG_WICAN_USB_BLUETOOTH_PAN
    pan_status.compiled = true;
#else
    strcpy(pan_status.state, "unavailable");
#endif
    static const log_descriptor_t log = { "usb_bluetooth_pan", ESP_LOG_INFO };
    log_manager_register(&log);
    return pan_settings_register();
}

esp_err_t usb_bluetooth_pan_start(void)
{
    cJSON *usb = NULL;
    esp_err_t error = settings_manager_get("usb_host_manager", &usb);
    pan_status.usb_host_enabled = error == ESP_OK
        && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(usb, "enabled"))
        && (!cJSON_GetObjectItemCaseSensitive(usb, "role")
            || !strcmp(cJSON_GetObjectItemCaseSensitive(usb, "role")->valuestring, "host"));
    cJSON_Delete(usb);
    if (!pan_config.enabled) return ESP_OK;
    if (!pan_status.compiled) {
        strcpy(pan_status.error, "PAN backend was not included in this firmware build");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!pan_status.usb_host_enabled) {
        strcpy(pan_status.state, "usb_disabled");
        strcpy(pan_status.error, "Enable USB host mode and reboot");
        return ESP_ERR_INVALID_STATE;
    }
    error = pan_backend_start();
    if (error != ESP_OK) {
        strcpy(pan_status.state, "error");
        snprintf(pan_status.error, sizeof(pan_status.error), "PAN startup failed: %s", esp_err_to_name(error));
    }
    return error;
}

esp_err_t usb_bluetooth_pan_stop(void) { return pan_backend_stop(); }
