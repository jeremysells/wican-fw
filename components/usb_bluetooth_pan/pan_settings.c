// SPDX-License-Identifier: GPL-3.0-or-later
#include "pan_private.h"
#include "settings_manager.h"

#include <stdio.h>
#include <string.h>

pan_config_t pan_config;

static const settings_field_t FIELDS[] = {
    SETTINGS_BOOL("enabled", false),
    SETTINGS_STR("peer_address", 17, ""),
    SETTINGS_STR_LEN("device_name", 1, 32, "WiCAN PRO PAN"),
    SETTINGS_BOOL("auto_connect", true),
    SETTINGS_BOOL("prefer_pan", false),
    SETTINGS_INT("reconnect_min_s", 1, 60, 5),
    SETTINGS_INT("reconnect_max_s", 5, 600, 60),
    SETTINGS_INT("connect_timeout_s", 10, 120, 30),
    SETTINGS_INT("dhcp_timeout_s", 10, 120, 30),
    SETTINGS_INT("pairing_timeout_s", 30, 300, 120),
    SETTINGS_BOOL("allow_legacy_pairing", false),
    SETTINGS_STR("legacy_pin_password", 16, ""),
    SETTINGS_BOOL("cli", true),
};

static esp_err_t validate(const cJSON *settings, char *error, size_t length)
{
    uint8_t address[6];
    const cJSON *peer = cJSON_GetObjectItemCaseSensitive(settings, "peer_address");
    const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(settings, "reconnect_min_s");
    const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(settings, "reconnect_max_s");
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(settings, "legacy_pin_password");
    const char *reason = NULL;
    if (!cJSON_IsString(peer) || (peer->valuestring[0] && !pan_address_parse(peer->valuestring, address)))
        reason = "peer_address must be a Bluetooth address such as AA:BB:CC:DD:EE:FF";
    else if (minimum && maximum && minimum->valueint > maximum->valueint)
        reason = "reconnect_max_s must be at least reconnect_min_s";
    else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "allow_legacy_pairing"))
             && (!cJSON_IsString(pin) || !pin->valuestring[0]))
        reason = "Legacy pairing requires a PIN";
    if (reason) {
        snprintf(error, length, "%s", reason);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t apply(const cJSON *settings)
{
    memset(&pan_config, 0, sizeof(pan_config));
    pan_config.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    pan_config.auto_connect = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "auto_connect"));
    pan_config.prefer_pan = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "prefer_pan"));
    pan_config.allow_legacy_pairing = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "allow_legacy_pairing"));
    pan_config.cli = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "cli"));
    const cJSON *peer = cJSON_GetObjectItemCaseSensitive(settings, "peer_address");
    if (peer && peer->valuestring[0]) (void)pan_address_parse(peer->valuestring, pan_config.peer);
    snprintf(pan_config.device_name, sizeof(pan_config.device_name), "%s",
             cJSON_GetObjectItemCaseSensitive(settings, "device_name")->valuestring);
    snprintf(pan_config.legacy_pin, sizeof(pan_config.legacy_pin), "%s",
             cJSON_GetObjectItemCaseSensitive(settings, "legacy_pin_password")->valuestring);
    pan_config.reconnect_min_s = cJSON_GetObjectItemCaseSensitive(settings, "reconnect_min_s")->valueint;
    pan_config.reconnect_max_s = cJSON_GetObjectItemCaseSensitive(settings, "reconnect_max_s")->valueint;
    pan_config.connect_timeout_s = cJSON_GetObjectItemCaseSensitive(settings, "connect_timeout_s")->valueint;
    pan_config.dhcp_timeout_s = cJSON_GetObjectItemCaseSensitive(settings, "dhcp_timeout_s")->valueint;
    pan_config.pairing_timeout_s = cJSON_GetObjectItemCaseSensitive(settings, "pairing_timeout_s")->valueint;
    pan_status.enabled = pan_config.enabled;
    if (peer && peer->valuestring[0]) pan_address_format(pan_config.peer, pan_status.peer_address);
    if (pan_config.cli) return pan_cli_register();
    return ESP_OK;
}

esp_err_t pan_settings_register(void)
{
    static const settings_descriptor_t descriptor = {
        .name = "usb_bluetooth_pan", .version = 1,
        .fields = FIELDS, .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = apply, .on_validate = validate,
    };
    return settings_manager_register(&descriptor);
}
