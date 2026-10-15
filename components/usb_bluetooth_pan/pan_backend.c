// SPDX-License-Identifier: GPL-3.0-or-later
#include "pan_private.h"
#include "pan_usb.h"
#include "bluetooth.h"
#include "bluetooth_psm.h"
#include "bluetooth_sdp.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "classic/bnep.h"
#include "classic/sdp_client.h"
#include "btstack_run_loop_embedded.h"
#include "classic/btstack_link_key_db.h"
#include "dev_status_manager.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "usb_bluetooth_pan";
/* usb_eth_host listens to every standard ETH lease event. Private IDs on
 * IP_EVENT keep PAN's netif and route policy under this component's ownership. */
enum { PAN_GOT_IP = 0x50414e, PAN_LOST_IP = 0x50414f };
typedef struct { uint16_t size; uint8_t data[PAN_FRAME_MAX]; } ethernet_frame_t;
typedef enum { CMD_SCAN, CMD_PAIR, CMD_CONNECT, CMD_DISCONNECT, CMD_CONFIRM, CMD_REJECT, CMD_FORGET } command_kind_t;
typedef struct { command_kind_t kind; uint32_t value, generation; } command_t;
static QueueHandle_t commands, frames;
static StaticQueue_t frame_queue_control;
static uint8_t *frame_queue_storage;
static TaskHandle_t task;
static SemaphoreHandle_t stopped;
static volatile bool stop_requested;
static volatile bool link_active, lease_lost;
static bool host_initialized, ready, connecting, sdp_seen, bnep_pending;
static bool scanning, one_shot, bond_loaded;
static uint16_t bnep_channel, acl_handle = HCI_CON_HANDLE_INVALID;
static uint32_t observed_generation;
static uint64_t connect_deadline, dhcp_deadline;
static unsigned failures;
static bd_addr_t adapter;
static btstack_packet_callback_registration_t hci_registration;
static esp_netif_t *netif;
static esp_event_handler_instance_t ip_handler;
static esp_netif_driver_base_t net_driver;
static ethernet_frame_t *outgoing;

uint32_t hal_time_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
/* All host operations execute on one task; IRQs only enqueue USB chunks. */
void hal_cpu_disable_irqs(void) { }
void hal_cpu_enable_irqs(void) { }
void hal_cpu_enable_irqs_and_sleep(void) { vTaskDelay(pdMS_TO_TICKS(1)); }
void btstack_assert_failed(const char *file, const uint16_t line)
{
    ESP_LOGE(TAG, "Bluetooth host assertion: %s:%u", file, line);
    abort();
}
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static void state(const char *name, const char *error)
{
    pan_status_lock();
    snprintf(pan_status.state, sizeof(pan_status.state), "%s", name);
    if (error) snprintf(pan_status.error, sizeof(pan_status.error), "%s", error);
    pan_status_unlock();
}

static bool pairing_allowed(const uint8_t peer[6])
{
    pan_status_lock();
    bool allowed = pan_pairing_allowed(pan_config.peer, peer, now_ms(), pan_status.pairing_until_ms);
    pan_status_unlock();
    return allowed;
}

/* Store one phone bond per adapter. USB dongles can be exchanged without
 * accidentally reusing a link key belonging to a different local address. */
static char bond_key[13];
static void db_open(void) { }
static void db_close(void) { }
static void db_set_address(bd_addr_t address)
{
    memcpy(adapter, address, 6);
    snprintf(bond_key, sizeof(bond_key), "%02x%02x%02x%02x%02x%02x",
             address[0], address[1], address[2], address[3], address[4], address[5]);
}

static int db_get(bd_addr_t address, link_key_t key, link_key_type_t *type)
{
    if (!bond_key[0] || !pan_peer_allowed(pan_config.peer, address)) return 0;
    nvs_handle_t handle;
    if (nvs_open("usb_pan_keys", NVS_READONLY, &handle) != ESP_OK) return 0;
    uint8_t record[24];
    size_t size = sizeof(record);
    esp_err_t error = nvs_get_blob(handle, bond_key, record, &size);
    nvs_close(handle);
    if (error != ESP_OK || size != sizeof(record) || record[0] != 1
        || memcmp(record + 1, address, 6)) return 0;
    memcpy(key, record + 7, 16);
    *type = (link_key_type_t)record[23];
    return 1;
}

static void db_put(bd_addr_t address, link_key_t key, link_key_type_t type)
{
    if (!pairing_allowed(address) || !bond_key[0]) return;
    uint8_t record[24] = { 1 };
    memcpy(record + 1, address, 6);
    memcpy(record + 7, key, 16);
    record[23] = type;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("usb_pan_keys", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, bond_key, record, sizeof(record));
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    pan_status_lock();
    pan_status.bonded = error == ESP_OK;
    if (error != ESP_OK) snprintf(pan_status.error, sizeof(pan_status.error), "Could not persist phone bond: %s", esp_err_to_name(error));
    pan_status_unlock();
}

static void db_delete(bd_addr_t address)
{
    if (!bond_key[0] || !pan_peer_allowed(pan_config.peer, address)) return;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("usb_pan_keys", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_erase_key(handle, bond_key);
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    pan_status_lock();
    if (error == ESP_OK) pan_status.bonded = false;
    else snprintf(pan_status.error, sizeof(pan_status.error), "Could not erase phone bond: %s", esp_err_to_name(error));
    pan_status_unlock();
}
static int db_iterator_init(btstack_link_key_iterator_t *iterator) { (void)iterator; return 0; }
static int db_iterator_next(btstack_link_key_iterator_t *iterator, bd_addr_t address, link_key_t key, link_key_type_t *type)
{ (void)iterator; (void)address; (void)key; (void)type; return 0; }
static void db_iterator_done(btstack_link_key_iterator_t *iterator) { (void)iterator; }
static const btstack_link_key_db_t link_keys = {
    .open = db_open, .close = db_close, .set_local_bd_addr = db_set_address,
    .get_link_key = db_get, .put_link_key = db_put, .delete_link_key = db_delete,
    .iterator_init = db_iterator_init, .iterator_get_next = db_iterator_next, .iterator_done = db_iterator_done,
};

static void network_down(void)
{
    link_active = false;
    if (netif) {
        esp_netif_action_disconnected(netif, NULL, 0, NULL);
        esp_netif_action_stop(netif, NULL, 0, NULL);
    }
    pan_status_lock();
    bool had_ip = pan_status.network_up;
    pan_status.network_up = false;
    pan_status.ip[0] = pan_status.gateway[0] = pan_status.dns[0] = 0;
    pan_status_unlock();
    if (had_ip) dev_status_manager_clear(DEV_STATUS_BIT_ETH_CONNECTED);
    dhcp_deadline = 0;
    if (frames) xQueueReset(frames);
    if (outgoing) outgoing->size = 0;
}

static void ip_event(void *context, esp_event_base_t base, int32_t event, void *data)
{
    (void)context; (void)base;
    if (event == PAN_LOST_IP) { if (link_active) lease_lost = true; return; }
    if (event != PAN_GOT_IP || !data || !link_active) return;
    ip_event_got_ip_t *got = data;
    if (got->esp_netif != netif) return;
    esp_netif_ip_info_t current;
    if (esp_netif_get_ip_info(netif, &current) != ESP_OK
        || !current.ip.addr || current.ip.addr != got->ip_info.ip.addr) return;
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    pan_status_lock();
    if (!link_active) { pan_status_unlock(); return; }
    pan_status.network_up = true;
    snprintf(pan_status.ip, sizeof(pan_status.ip), IPSTR, IP2STR(&got->ip_info.ip));
    snprintf(pan_status.gateway, sizeof(pan_status.gateway), IPSTR, IP2STR(&got->ip_info.gw));
    snprintf(pan_status.dns, sizeof(pan_status.dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    strcpy(pan_status.state, "connected");
    pan_status.error[0] = 0;
    dev_status_manager_set(DEV_STATUS_BIT_ETH_CONNECTED);
    pan_status_unlock();
}

static esp_err_t transmit(void *driver, void *data, size_t size)
{
    (void)driver;
    if (!link_active) return ESP_ERR_INVALID_STATE;
    if (size < 14 || size > PAN_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    /* Keep packet-sized temporaries off the 3 KB lwIP task stack. */
    ethernet_frame_t *frame = heap_caps_malloc(sizeof(*frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame) {
        frame->size = size;
        memcpy(frame->data, data, size);
        bool queued = xQueueSend(frames, frame, 0) == pdTRUE;
        free(frame);
        if (queued) return ESP_OK;
    }
    pan_status_lock(); pan_status.tx_dropped++; pan_status_unlock();
    return ESP_ERR_NO_MEM;
}
static void free_rx(void *driver, void *data) { (void)driver; free(data); }
static esp_err_t attach(esp_netif_t *interface, void *driver)
{
    ((esp_netif_driver_base_t *)driver)->netif = interface;
    esp_netif_driver_ifconfig_t config = {
        .handle = driver, .transmit = transmit, .driver_free_rx_buffer = free_rx,
    };
    return esp_netif_set_driver_config(interface, &config);
}

static esp_err_t network_init(void)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USB_BT_PAN";
    base.if_desc = "Bluetooth PAN";
    base.get_ip_event = PAN_GOT_IP;
    base.lost_ip_event = PAN_LOST_IP;
    base.route_prio = pan_config.prefer_pan ? 150 : 40;
    esp_netif_config_t config = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    netif = esp_netif_new(&config);
    if (!netif) return ESP_ERR_NO_MEM;
    net_driver.post_attach = attach;
    esp_err_t error = esp_netif_attach(netif, &net_driver);
    if (error != ESP_OK) return error;
    return esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL, &ip_handler);
}

static void retry(const char *error)
{
    connecting = false;
    one_shot = false;
    connect_deadline = 0;
    network_down();
    pan_status_lock();
    uint32_t delay = pan_retry_delay(pan_config.reconnect_min_s, pan_config.reconnect_max_s, failures++);
    pan_status.retry_at_ms = now_ms() + (uint64_t)delay * 1000;
    bool manual = pan_status.manual_disconnect;
    pan_status_unlock();
    state(manual ? "disconnected" : "retry_wait", error);
}

static void disconnect_link(void)
{
    bool had_bnep = bnep_channel || bnep_pending;
    bnep_channel = 0; bnep_pending = false;
    network_down();
    if (had_bnep) bnep_disconnect(pan_config.peer);
    if (acl_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(acl_handle);
    acl_handle = HCI_CON_HANDLE_INVALID;
    connecting = false;
    connect_deadline = 0;
}

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);
static void sdp_result(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)type; (void)channel; (void)size;
    if (!connecting) return;
    if (hci_event_packet_get_type(packet) == SDP_EVENT_QUERY_ATTRIBUTE_VALUE)
        sdp_seen = true;
    if (hci_event_packet_get_type(packet) == SDP_EVENT_QUERY_COMPLETE) {
        if (sdp_event_query_complete_get_status(packet) || !sdp_seen) {
            disconnect_link();
            retry("Phone did not advertise a NAP service; enable Bluetooth tethering");
        } else {
            state("connecting", "");
            bnep_pending = true;
            if (bnep_connect(packet_handler, pan_config.peer, BLUETOOTH_PSM_BNEP,
                             BLUETOOTH_SERVICE_CLASS_PANU, BLUETOOTH_SERVICE_CLASS_NAP)) {
                bnep_pending = false;
                retry("Could not start PAN connection");
            }
        }
    }
}

static void connect_phone(void)
{
    if (!ready || connecting || bnep_channel || scanning) return;
    static const uint8_t empty[6];
    if (!memcmp(pan_config.peer, empty, 6)) {
        state("no_phone", "Select a phone and submit settings");
        pan_status_lock();
        pan_status.retry_at_ms = now_ms() + 60000;
        pan_status_unlock();
        return;
    }
    link_key_t key;
    link_key_type_t key_type;
    bond_loaded = db_get(pan_config.peer, key, &key_type) != 0;
    pan_status_lock();
    pan_status.bonded = bond_loaded;
    bool allowed = pan_status.pairing_until_ms > now_ms();
    pan_status.attempts++;
    if (pan_status.attempts > 1) pan_status.reconnects++;
    pan_status_unlock();
    if (!bond_loaded && !allowed) {
        state("unpaired", "Pair the configured phone first");
        pan_status_lock(); pan_status.retry_at_ms = now_ms() + 60000; pan_status_unlock();
        one_shot = false;
        return;
    }
    connecting = true;
    sdp_seen = false;
    connect_deadline = now_ms() + (uint64_t)pan_config.connect_timeout_s * 1000;
    state("discovering_service", "");
    uint8_t result = sdp_client_query_uuid16(sdp_result, pan_config.peer, BLUETOOTH_SERVICE_CLASS_NAP);
    if (result) retry("Service discovery could not start");
}

static void record_scan(uint8_t *packet)
{
    bd_addr_t address;
    gap_event_inquiry_result_get_bd_addr(packet, address);
    char text[18]; pan_address_format(address, text);
    pan_status_lock();
    unsigned index = 0;
    while (index < pan_status.scan_count && strcmp(pan_status.scan[index].address, text)) index++;
    if (index < PAN_SCAN_MAX) {
        if (index == pan_status.scan_count) pan_status.scan_count++;
        strcpy(pan_status.scan[index].address, text);
        pan_status.scan[index].rssi = gap_event_inquiry_result_get_rssi_available(packet)
            ? gap_event_inquiry_result_get_rssi(packet) : 0;
        pan_status.scan[index].class_of_device = gap_event_inquiry_result_get_class_of_device(packet);
        if (gap_event_inquiry_result_get_name_available(packet)) {
            size_t length = gap_event_inquiry_result_get_name_len(packet);
            if (length >= sizeof(pan_status.scan[index].name)) length = sizeof(pan_status.scan[index].name) - 1;
            memcpy(pan_status.scan[index].name, gap_event_inquiry_result_get_name(packet), length);
            pan_status.scan[index].name[length] = 0;
        }
    }
    pan_status_unlock();
}

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (type == BNEP_DATA_PACKET) {
        if (channel != bnep_channel || size < 14 || size > PAN_FRAME_MAX) return;
        uint8_t *frame = malloc(size);
        if (!frame) { pan_status_lock(); pan_status.rx_dropped++; pan_status_unlock(); return; }
        memcpy(frame, packet, size);
        esp_err_t result = esp_netif_receive(netif, frame, size, NULL);
        /* ethernetif_input owns the buffer on success AND failure. */
        pan_status_lock();
        if (result == ESP_OK) pan_status.rx_frames++; else pan_status.rx_dropped++;
        pan_status_unlock();
        return;
    }
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t peer;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            gap_local_bd_addr(adapter);
            db_set_address(adapter);
            ready = true;
            pan_status_lock(); pan_address_format(adapter, pan_status.adapter_address); pan_status_unlock();
            state("ready", "");
        }
        break;
    case HCI_EVENT_COMMAND_COMPLETE:
        if (size >= 14 && little_endian_read_16(packet, 3) == 0x1001 && packet[5] == 0) {
            pan_status_lock();
            pan_status.hci_version = packet[6];
            pan_status.hci_revision = little_endian_read_16(packet, 7);
            pan_status.manufacturer = little_endian_read_16(packet, 10);
            pan_status.lmp_subversion = little_endian_read_16(packet, 12);
            pan_status_unlock();
        }
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        if (!hci_event_connection_complete_get_status(packet)) {
            hci_event_connection_complete_get_bd_addr(packet, peer);
            uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
            if (!pan_peer_allowed(pan_config.peer, peer)) gap_disconnect(handle);
            else acl_handle = handle;
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, peer);
        if (pan_config.allow_legacy_pairing && pairing_allowed(peer)) gap_pin_code_response(peer, pan_config.legacy_pin);
        else gap_pin_code_negative(peer);
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, peer);
        if (!pairing_allowed(peer)) gap_ssp_confirmation_negative(peer);
        else {
            pan_status_lock();
            pan_status.numeric_pending = true;
            pan_status.passkey = hci_event_user_confirmation_request_get_numeric_value(packet);
            connect_deadline = pan_status.pairing_until_ms;
            strcpy(pan_status.state, "confirm_pairing");
            pan_status_unlock();
        }
        break;
    case HCI_EVENT_USER_PASSKEY_REQUEST:
        hci_event_user_passkey_request_get_bd_addr(packet, peer);
        gap_ssp_passkey_negative(peer);
        state("pairing_failed", "Phone requested a pairing method this controller does not support");
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        if (hci_event_authentication_complete_get_connection_handle(packet) != acl_handle) break;
        pan_status_lock(); pan_status.numeric_pending = false; pan_status_unlock();
        if (hci_event_authentication_complete_get_status(packet)) {
            disconnect_link(); retry("Authentication failed; forget the bond on both devices and pair again");
        }
        break;
    case GAP_EVENT_INQUIRY_RESULT: record_scan(packet); break;
    case GAP_EVENT_INQUIRY_COMPLETE: scanning = false; state("ready", ""); break;
    case BNEP_EVENT_CHANNEL_OPENED:
        bnep_pending = false;
        if (bnep_event_channel_opened_get_status(packet)) {
            retry("PAN connection rejected by phone");
        } else {
            bnep_event_channel_opened_get_remote_address(packet, peer);
            bnep_channel = bnep_event_channel_opened_get_bnep_cid(packet);
            if (!pan_peer_allowed(pan_config.peer, peer)
                || acl_handle == HCI_CON_HANDLE_INVALID || gap_security_level(acl_handle)
                    < (pan_config.allow_legacy_pairing ? LEVEL_2 : LEVEL_3)) {
                disconnect_link(); retry("PAN link did not satisfy authentication and encryption requirements"); break;
            }
            connecting = false; connect_deadline = 0;
            link_active = true;
            esp_netif_set_mac(netif, adapter);
            esp_netif_action_start(netif, NULL, 0, NULL);
            esp_netif_action_connected(netif, NULL, 0, NULL);
            dhcp_deadline = now_ms() + (uint64_t)pan_config.dhcp_timeout_s * 1000;
            state("dhcp", "");
        }
        break;
    case BNEP_EVENT_CHANNEL_CLOSED:
        bnep_channel = 0; bnep_pending = false;
        retry("Bluetooth PAN link disconnected");
        break;
    case BNEP_EVENT_CHANNEL_TIMEOUT:
        disconnect_link(); retry("Bluetooth PAN connection timed out"); break;
    case BNEP_EVENT_CAN_SEND_NOW:
        if (outgoing->size && bnep_channel) {
            int result = bnep_send(bnep_channel, outgoing->data, outgoing->size);
            pan_status_lock();
            if (!result) pan_status.tx_frames++; else pan_status.tx_dropped++;
            pan_status_unlock();
            outgoing->size = 0;
        }
        break;
    default: break;
    }
}

static void process_command(command_t command)
{
    if (command.generation != pan_usb_generation()) return;
    switch (command.kind) {
    case CMD_SCAN:
        if (!ready || connecting || bnep_channel) { state("busy", "Disconnect PAN before scanning"); break; }
        pan_status_lock(); pan_status.scan_count = 0; pan_status_unlock();
        scanning = gap_inquiry_start(8) == 0;
        state(scanning ? "scanning" : "ready", scanning ? "" : "Scan could not start");
        break;
    case CMD_PAIR:
        pan_status_lock();
        pan_status.pairing_until_ms = now_ms() + (uint64_t)pan_config.pairing_timeout_s * 1000;
        pan_status.manual_disconnect = false;
        pan_status_unlock();
        one_shot = true;
        connect_phone();
        break;
    case CMD_CONNECT:
        pan_status_lock(); pan_status.manual_disconnect = false; pan_status.retry_at_ms = 0; pan_status_unlock();
        one_shot = true;
        connect_phone();
        break;
    case CMD_DISCONNECT:
    case CMD_FORGET:
        pan_status_lock();
        pan_status.manual_disconnect = true;
        pan_status.pairing_until_ms = 0;
        bool reject_pending = pan_status.numeric_pending;
        pan_status.numeric_pending = false;
        pan_status_unlock();
        if (reject_pending) gap_ssp_confirmation_negative(pan_config.peer);
        one_shot = false;
        disconnect_link();
        if (command.kind == CMD_FORGET) gap_drop_link_key_for_bd_addr(pan_config.peer);
        state(command.kind == CMD_FORGET ? "unpaired" : "disconnected", "");
        break;
    case CMD_CONFIRM:
    case CMD_REJECT:
        pan_status_lock();
        bool pending = pan_status.numeric_pending;
        bool accept = pending && command.kind == CMD_CONFIRM && command.value == pan_status.passkey
                      && pan_status.pairing_until_ms > now_ms();
        pan_status.numeric_pending = false;
        pan_status_unlock();
        if (pending) {
            if (accept) gap_ssp_confirmation_response(pan_config.peer);
            else gap_ssp_confirmation_negative(pan_config.peer);
        }
        if (!accept) state("pairing_failed", "Pairing confirmation rejected");
        break;
    }
}

static void host_close(void)
{
    network_down();
    pan_usb_close();
    ready = connecting = false;
    acl_handle = HCI_CON_HANDLE_INVALID;
    if (host_initialized) {
        /* hci_close frees live connections and the host itself. Calling
         * hci_deinit afterwards would dereference the released host. */
        hci_close();
        sdp_client_deinit();
        bnep_deinit();
        l2cap_deinit();
        btstack_run_loop_deinit();
    }
    host_initialized = ready = connecting = scanning = bnep_pending = false;
    bnep_channel = 0;
    acl_handle = HCI_CON_HANDLE_INVALID;
    connect_deadline = dhcp_deadline = 0;
    pan_status_lock(); pan_status.numeric_pending = false; pan_status_unlock();
}

static void restore_dns(void *argument)
{
    (void)argument;
    pan_status_lock();
    if (netif && esp_netif_get_default_netif() == netif) {
        esp_netif_dns_info_t server;
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &server) == ESP_OK
            && server.ip.type == ESP_IPADDR_TYPE_V4 && server.ip.u_addr.ip4.addr) {
            ip_addr_t address;
            ip_addr_set_ip4_u32_val(address, server.ip.u_addr.ip4.addr);
            dns_setserver(0, &address);
        }
    }
    pan_status_unlock();
}

static void worker(void *argument)
{
    (void)argument;
    uint64_t last_dns = 0, controller_deadline = 0;
    while (!stop_requested) {
        bool present = pan_usb_present();
        uint32_t generation = pan_usb_generation();
        if (host_initialized && (!present || generation != observed_generation)) {
            host_close(); state("waiting_for_dongle", "");
        }
        if (present && !host_initialized) {
            observed_generation = generation;
            btstack_memory_init();
            btstack_run_loop_init(btstack_run_loop_embedded_get_instance());
            hci_init(pan_usb_transport(), NULL);
            hci_set_link_key_db(&link_keys);
            l2cap_init();
            gap_set_security_level(pan_config.allow_legacy_pairing ? LEVEL_2 : LEVEL_3);
            gap_set_minimal_service_security_level(pan_config.allow_legacy_pairing ? LEVEL_2 : LEVEL_3);
            bnep_init();
            gap_set_local_name(pan_config.device_name);
            gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
            gap_ssp_set_auto_accept(0);
            hci_registration.callback = packet_handler;
            hci_add_event_handler(&hci_registration);
            host_initialized = true;
            controller_deadline = now_ms() + 12000;
            state("initializing", "");
            hci_power_control(HCI_POWER_ON);
        }
        if (!host_initialized) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (!pan_usb_poll()) {
            host_close(); retry("USB HCI transport failed or overflowed");
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        btstack_run_loop_embedded_execute_once();
        command_t command;
        while (xQueueReceive(commands, &command, 0) == pdTRUE) process_command(command);
        uint64_t now = now_ms();
        pan_status_lock();
        bool has_ip = pan_status.network_up;
        bool manual = pan_status.manual_disconnect;
        bool pair_expired = pan_status.pairing_until_ms && pan_status.pairing_until_ms <= now;
        uint64_t retry_at = pan_status.retry_at_ms;
        bool reject_expired = pair_expired && pan_status.numeric_pending;
        if (pair_expired) {
            pan_status.pairing_until_ms = 0;
            pan_status.numeric_pending = false;
        }
        pan_status_unlock();
        if (reject_expired) gap_ssp_confirmation_negative(pan_config.peer);
        if (!ready && now > controller_deadline) {
            host_close(); retry("USB Bluetooth controller initialization timed out");
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        if (connect_deadline && now > connect_deadline) {
            disconnect_link(); retry("Phone connection timed out");
        }
        if (dhcp_deadline && now > dhcp_deadline && !has_ip) {
            disconnect_link(); retry("Phone did not provide a DHCP lease");
        }
        if (lease_lost) {
            lease_lost = false;
            disconnect_link(); retry("Phone DHCP lease was lost");
            has_ip = false;
        }
        if (has_ip) {
            failures = 0;
            if (now - last_dns > 1000) { tcpip_callback(restore_dns, NULL); last_dns = now; }
            if (!outgoing->size && xQueueReceive(frames, outgoing, 0) == pdTRUE)
                bnep_request_can_send_now_event(bnep_channel);
        } else if (bnep_channel && !outgoing->size && xQueueReceive(frames, outgoing, 0) == pdTRUE) {
            /* DHCP/ARP frames must flow before network_up is set. */
            bnep_request_can_send_now_event(bnep_channel);
        }
        if (ready && !manual && !connecting && !bnep_channel && !scanning && now >= retry_at
            && (pan_config.auto_connect || one_shot)) connect_phone();
    }
    host_close();
    if (ip_handler) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler);
    pan_status_lock();
    esp_netif_t *old = netif;
    netif = NULL;
    pan_status_unlock();
    if (old) esp_netif_destroy(old);
    ip_handler = NULL;
    state("off", "");
    task = NULL;
    xSemaphoreGive(stopped);
    vTaskDelete(NULL);
}

esp_err_t pan_backend_start(void)
{
    if (task) return ESP_OK;
    if (stopped) {
        esp_err_t error = pan_backend_stop();
        if (error != ESP_OK) return error;
    }
    esp_err_t error = pan_usb_init();
    if (error != ESP_OK) return error;
    commands = xQueueCreate(8, sizeof(command_t));
    frame_queue_storage = heap_caps_malloc(4 * sizeof(ethernet_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    outgoing = heap_caps_calloc(1, sizeof(*outgoing), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    stopped = xSemaphoreCreateBinary();
    if (!commands || !frame_queue_storage || !outgoing || !stopped) goto allocation_failed;
    frames = xQueueCreateStatic(4, sizeof(ethernet_frame_t), frame_queue_storage, &frame_queue_control);
    error = network_init();
    if (error != ESP_OK) goto setup_failed;
    lease_lost = false;
    stop_requested = false;
    state("waiting_for_dongle", "");
    if (xTaskCreate(worker, "usb_pan", 6144, NULL, 5, &task) != pdPASS) goto allocation_failed;
    pan_usb_accept_connections(true);
    return ESP_OK;
allocation_failed:
    error = ESP_ERR_NO_MEM;
setup_failed:
    if (ip_handler) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler);
    ip_handler = NULL;
    if (netif) { esp_netif_destroy(netif); netif = NULL; }
    if (frames) vQueueDelete(frames);
    if (commands) vQueueDelete(commands);
    if (stopped) vSemaphoreDelete(stopped);
    free(frame_queue_storage); free(outgoing);
    frames = commands = NULL; stopped = NULL;
    frame_queue_storage = NULL; outgoing = NULL;
    return error;
}

esp_err_t pan_backend_stop(void)
{
    if (!stopped) return ESP_OK;
    pan_usb_accept_connections(false);
    pan_status_lock();
    stop_requested = true;
    pan_status_unlock();
    if (xSemaphoreTake(stopped, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    vQueueDelete(frames); frames = NULL;
    vQueueDelete(commands); commands = NULL;
    vSemaphoreDelete(stopped); stopped = NULL;
    free(frame_queue_storage); frame_queue_storage = NULL;
    free(outgoing); outgoing = NULL;
    return ESP_OK;
}

esp_err_t pan_command(const char *action, uint32_t value)
{
    static const char *const names[] = { "scan", "pair", "connect", "disconnect", "confirm", "reject", "forget" };
    unsigned kind = 0;
    while (kind < sizeof(names) / sizeof(names[0]) && strcmp(action, names[kind])) kind++;
    if (kind == sizeof(names) / sizeof(names[0])) return ESP_ERR_INVALID_ARG;
    pan_status_lock();
    if (!task || stop_requested || !pan_config.enabled) {
        pan_status_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    bool pending = pan_status.numeric_pending;
    bool present = pan_status.dongle_present;
    uint32_t expected = pan_status.passkey;
    esp_err_t error = present ? ESP_OK : ESP_ERR_INVALID_STATE;
    static const uint8_t empty[6];
    if ((kind == CMD_PAIR || kind == CMD_CONNECT || kind == CMD_FORGET)
        && !memcmp(pan_config.peer, empty, 6)) error = ESP_ERR_INVALID_STATE;
    if (kind == CMD_CONFIRM && (!pending || value != expected)) error = ESP_ERR_INVALID_ARG;
    if (kind == CMD_REJECT && !pending) error = ESP_ERR_INVALID_STATE;
    command_t command = { .kind = kind, .value = value, .generation = pan_usb_generation() };
    if (error == ESP_OK && xQueueSend(commands, &command, 0) != pdTRUE) error = ESP_ERR_TIMEOUT;
    pan_status_unlock();
    return error;
}
