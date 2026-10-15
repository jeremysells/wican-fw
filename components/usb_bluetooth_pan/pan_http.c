// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_bluetooth_pan.h"
#include "pan_private.h"
#include "http_server_manager.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>

static esp_err_t send_json(httpd_req_t *request, cJSON *json)
{
    if (!json) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return result;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    pan_status_t *status = malloc(sizeof(*status));
    if (!status) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    pan_status_read(status);
    uint64_t now = esp_timer_get_time() / 1000;
    cJSON *json = cJSON_CreateObject();
    if (!json) { free(status); return send_json(request, NULL); }
#define BOOL(field) cJSON_AddBoolToObject(json, #field, status->field)
#define STR(field) cJSON_AddStringToObject(json, #field, status->field)
#define NUM(field) cJSON_AddNumberToObject(json, #field, status->field)
    BOOL(compiled); BOOL(enabled); BOOL(usb_host_enabled); BOOL(dongle_present);
    BOOL(bonded); BOOL(network_up); BOOL(numeric_pending); BOOL(manual_disconnect);
    STR(state); STR(error); STR(adapter_address); STR(peer_address); STR(adapter_product);
    STR(ip); STR(gateway); STR(dns);
    NUM(vid); NUM(pid); NUM(revision); NUM(manufacturer); NUM(hci_version);
    NUM(hci_revision); NUM(lmp_subversion); NUM(attempts); NUM(reconnects);
    NUM(tx_frames); NUM(rx_frames); NUM(tx_dropped); NUM(rx_dropped);
    if (status->numeric_pending) NUM(passkey);
    cJSON_AddNumberToObject(json, "retry_in_s", status->retry_at_ms > now
        ? (status->retry_at_ms - now + 999) / 1000 : 0);
    cJSON_AddNumberToObject(json, "pairing_remaining_s", status->pairing_until_ms > now
        ? (status->pairing_until_ms - now + 999) / 1000 : 0);
    cJSON *devices = cJSON_AddArrayToObject(json, "devices");
    for (unsigned i = 0; i < status->scan_count; ++i) {
        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "address", status->scan[i].address);
        cJSON_AddStringToObject(device, "name", status->scan[i].name);
        cJSON_AddNumberToObject(device, "rssi", status->scan[i].rssi);
        cJSON_AddItemToArray(devices, device);
    }
    free(status);
    return send_json(request, json);
}

static esp_err_t command_handler(httpd_req_t *request)
{
    char body[193];
    if (!request->content_len || request->content_len >= sizeof(body))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Command body must be 1..192 bytes");
    size_t offset = 0;
    unsigned timeouts = 0;
    while (offset < request->content_len) {
        int count = httpd_req_recv(request, body + offset, request->content_len - offset);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (count <= 0) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Incomplete body");
        offset += count;
    }
    body[offset] = 0;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(body, offset + 1, &end, true);
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "value");
    if (!cJSON_IsString(action) || (value && (!cJSON_IsNumber(value)
        || value->valuedouble < 0 || value->valuedouble > 999999
        || value->valuedouble != value->valueint))) {
        cJSON_Delete(json);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid action or confirmation code");
    }
    if (!strcmp(action->valuestring, "confirm") && !value) {
        cJSON_Delete(json);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Confirmation code required");
    }
    static const char *const actions[] = { "scan", "pair", "connect", "disconnect", "confirm", "reject", "forget" };
    bool known = false;
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        if (!strcmp(action->valuestring, actions[i])) known = true;
    if (!known) {
        cJSON_Delete(json);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Unknown PAN action");
    }
    esp_err_t result = pan_command(action->valuestring, value ? value->valueint : 0);
    cJSON_Delete(json);
    json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", result == ESP_OK);
    if (result != ESP_OK) {
        httpd_resp_set_status(request, result == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict");
        cJSON_AddStringToObject(json, "error", esp_err_to_name(result));
    } else httpd_resp_set_status(request, "202 Accepted");
    return send_json(request, json);
}

esp_err_t usb_bluetooth_pan_register_http(void)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/usb/pan", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/usb/pan/action", .method = HTTP_POST, .handler = command_handler },
    };
    esp_err_t error = http_server_manager_register_handlers(routes, sizeof(routes) / sizeof(routes[0]));
    if (error != ESP_OK) return error;
#if CONFIG_WICAN_WEBUI_V2
    extern const uint8_t start[] asm("_binary_pan_index_html_gz_start");
    extern const uint8_t end[] asm("_binary_pan_index_html_gz_end");
    static const http_asset_t assets[] = {
        { .uri = "/", .content_type = "text/html", .data_start = start,
          .data_end = end, .content_encoding = "gzip" },
        { .uri = "/index.html", .content_type = "text/html", .data_start = start,
          .data_end = end, .content_encoding = "gzip" },
        { 0 },
    };
    error = http_server_manager_register_assets(assets);
#endif
    return error;
}
