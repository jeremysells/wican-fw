// SPDX-License-Identifier: GPL-3.0-or-later
#include "pan_usb.h"
#include "pan_private.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usbh_core.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PAN_USB_TX_CAPACITY 1728

/* ISR callbacks copy small USB chunks only. Reassembly and the Bluetooth
 * stack run exclusively on the PAN worker's internal-RAM stack. */
typedef struct {
    uint32_t generation;
    uint8_t type, length;
    uint8_t data[64];
} usb_message_t;

static struct {
    struct usbh_hubport *port;
    struct usb_endpoint_descriptor *events, *in, *out;
    struct usbh_urb event_urb, in_urb, out_urb;
    uint8_t *event_buffer, *in_buffer, *tx_buffer;
    volatile bool active, failed, accepting;
    volatile uint32_t generation;
    SemaphoreHandle_t mutex;
    QueueHandle_t messages;
    void (*packet_handler)(uint8_t, uint8_t *, uint16_t);
} usb;
static pan_hci_reassembly_t *event_rx, *acl_rx;

static void received(void *argument, int bytes)
{
    struct usbh_urb *urb = argument;
    if (!usb.active) return;
    if (bytes < 0 || bytes > 64) { usb.failed = true; return; }
    if (bytes) {
        usb_message_t message = { .generation = usb.generation,
            .type = urb == &usb.event_urb ? 4 : 2, .length = bytes };
        memcpy(message.data, urb->transfer_buffer, bytes);
        BaseType_t wake = pdFALSE;
        BaseType_t sent = xPortInIsrContext()
            ? xQueueSendFromISR(usb.messages, &message, &wake)
            : xQueueSend(usb.messages, &message, 0);
        if (sent != pdTRUE) usb.failed = true;
        if (xPortInIsrContext() && wake) portYIELD_FROM_ISR();
    }
    if (usb.active && usbh_submit_urb(urb) < 0) usb.failed = true;
}

static void cancel_receivers(void)
{
    usb.active = false;
    usbh_kill_urb(&usb.event_urb);
    usbh_kill_urb(&usb.in_urb);
}

static int class_connect(struct usbh_hubport *port, uint8_t interface)
{
    if (interface != 0) return 0; /* SCO/audio interface is not used by PAN. */
    if (!pan_config.enabled || !usb.mutex || !usb.accepting) return -USB_ERR_NOTSUPP;
    xSemaphoreTake(usb.mutex, portMAX_DELAY);
    if (usb.port) { xSemaphoreGive(usb.mutex); return -USB_ERR_BUSY; }
    struct usb_endpoint_descriptor *events = NULL, *in = NULL, *out = NULL;
    struct usbh_interface_altsetting *alternate = &port->config.intf[interface].altsetting[0];
    if (alternate->intf_desc.bNumEndpoints > CONFIG_USBHOST_MAX_ENDPOINTS) {
        xSemaphoreGive(usb.mutex);
        return -USB_ERR_NOTSUPP;
    }
    for (unsigned i = 0; i < alternate->intf_desc.bNumEndpoints; ++i) {
        struct usb_endpoint_descriptor *ep = &alternate->ep[i].ep_desc;
        if (USB_GET_ENDPOINT_TYPE(ep->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT
            && (ep->bEndpointAddress & 0x80)) events = ep;
        if (USB_GET_ENDPOINT_TYPE(ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
            if (ep->bEndpointAddress & 0x80) in = ep;
            else out = ep;
        }
    }
    if (!events || !in || !out || port->speed != USB_SPEED_FULL
        || USB_GET_MAXPACKETSIZE(events->wMaxPacketSize) > 64
        || USB_GET_MAXPACKETSIZE(in->wMaxPacketSize) > 64
        || USB_GET_MAXPACKETSIZE(out->wMaxPacketSize) > 64
        || !USB_GET_MAXPACKETSIZE(events->wMaxPacketSize)
        || !USB_GET_MAXPACKETSIZE(in->wMaxPacketSize)
        || !USB_GET_MAXPACKETSIZE(out->wMaxPacketSize)) {
        xSemaphoreGive(usb.mutex);
        return -USB_ERR_NOTSUPP;
    }
    usb.event_buffer = heap_caps_aligned_alloc(64, 64, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    usb.in_buffer = heap_caps_aligned_alloc(64, 64, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    usb.tx_buffer = heap_caps_aligned_alloc(64, PAN_USB_TX_CAPACITY, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!usb.event_buffer || !usb.in_buffer || !usb.tx_buffer) {
        free(usb.event_buffer); free(usb.in_buffer); free(usb.tx_buffer);
        usb.event_buffer = usb.in_buffer = usb.tx_buffer = NULL;
        xSemaphoreGive(usb.mutex);
        return -USB_ERR_NOMEM;
    }
    usb.port = port;
    usb.events = events; usb.in = in; usb.out = out;
    usb.failed = false;
    usb.generation++;
    port->config.intf[interface].priv = &usb;
    pan_status_lock();
    pan_status.dongle_present = true;
    pan_status.vid = port->device_desc.idVendor;
    pan_status.pid = port->device_desc.idProduct;
    pan_status.revision = port->device_desc.bcdDevice;
    snprintf(pan_status.adapter_product, sizeof(pan_status.adapter_product), "%s",
             port->iProduct ? port->iProduct : "CSR-compatible USB Bluetooth");
    pan_status_unlock();
    xSemaphoreGive(usb.mutex);
    return 0;
}

static int class_disconnect(struct usbh_hubport *port, uint8_t interface)
{
    if (interface != 0 || !usb.mutex) return 0;
    xSemaphoreTake(usb.mutex, portMAX_DELAY);
    if (usb.port == port) {
        cancel_receivers();
        usb.port = NULL;
        usb.generation++;
        free(usb.event_buffer); free(usb.in_buffer); free(usb.tx_buffer);
        usb.event_buffer = usb.in_buffer = usb.tx_buffer = NULL;
        pan_status_lock();
        pan_status.dongle_present = false;
        pan_status.adapter_address[0] = 0;
        pan_status_unlock();
    }
    xSemaphoreGive(usb.mutex);
    return 0;
}

static const struct usbh_class_driver class_driver = {
    .driver_name = "usb_bluetooth_pan", .connect = class_connect, .disconnect = class_disconnect,
};
static const uint16_t csr_devices[][2] = { { 0x0a12, 0x0001 }, { 0, 0 } };
CLASS_INFO_DEFINE const struct usbh_class_info usb_pan_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS
                 | USB_CLASS_MATCH_INTF_PROTOCOL | USB_CLASS_MATCH_VID_PID,
    .bInterfaceClass = 0xe0, .bInterfaceSubClass = 1, .bInterfaceProtocol = 1,
    .id_table = csr_devices, .class_driver = &class_driver,
};

esp_err_t pan_usb_init(void)
{
    if (usb.messages && usb.mutex && event_rx && acl_rx) return ESP_OK;
    usb.mutex = xSemaphoreCreateMutex();
    usb.messages = xQueueCreate(24, sizeof(usb_message_t));
    event_rx = calloc(1, sizeof(*event_rx));
    acl_rx = calloc(1, sizeof(*acl_rx));
    if (usb.mutex && usb.messages && event_rx && acl_rx) return ESP_OK;
    if (usb.mutex) vSemaphoreDelete(usb.mutex);
    if (usb.messages) vQueueDelete(usb.messages);
    free(event_rx); free(acl_rx);
    usb.mutex = NULL; usb.messages = NULL; event_rx = acl_rx = NULL;
    return ESP_ERR_NO_MEM;
}

void pan_usb_accept_connections(bool accept) { usb.accepting = accept; }

bool pan_usb_present(void)
{
    xSemaphoreTake(usb.mutex, portMAX_DELAY);
    bool present = usb.port != NULL;
    xSemaphoreGive(usb.mutex);
    return present;
}

uint32_t pan_usb_generation(void) { return usb.generation; }

static int transport_open(void)
{
    xSemaphoreTake(usb.mutex, portMAX_DELAY);
    if (!usb.port) { xSemaphoreGive(usb.mutex); return -1; }
    xQueueReset(usb.messages);
    event_rx->used = acl_rx->used = 0;
    usb.failed = false;
    usb.active = true;
    memset(&usb.event_urb, 0, sizeof(usb.event_urb));
    memset(&usb.in_urb, 0, sizeof(usb.in_urb));
    usbh_int_urb_fill(&usb.event_urb, usb.port, usb.events, usb.event_buffer,
                    USB_GET_MAXPACKETSIZE(usb.events->wMaxPacketSize), 0, received, &usb.event_urb);
    usbh_bulk_urb_fill(&usb.in_urb, usb.port, usb.in, usb.in_buffer, 64, 0, received, &usb.in_urb);
    int error = usbh_submit_urb(&usb.event_urb);
    if (!error) error = usbh_submit_urb(&usb.in_urb);
    if (error < 0) cancel_receivers();
    xSemaphoreGive(usb.mutex);
    return error;
}

void pan_usb_close(void)
{
    if (!usb.mutex) return;
    xSemaphoreTake(usb.mutex, portMAX_DELAY);
    if (usb.active) cancel_receivers();
    xSemaphoreGive(usb.mutex);
}

static int transport_close(void) { pan_usb_close(); return 0; }
static void register_handler(void (*handler)(uint8_t, uint8_t *, uint16_t)) { usb.packet_handler = handler; }

static int transport_send(uint8_t type, uint8_t *packet, int size)
{
    if (size < 0 || size > PAN_USB_TX_CAPACITY || (type != 1 && type != 2)) return -1;
    xSemaphoreTake(usb.mutex, portMAX_DELAY);
    int result = -1;
    if (usb.active && usb.port) {
        memcpy(usb.tx_buffer, packet, size);
        if (type == 1) {
            struct usb_setup_packet *setup = usb.port->setup;
            setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_DEVICE;
            setup->bRequest = 0; setup->wValue = 0; setup->wIndex = 0; setup->wLength = size;
            result = usbh_control_transfer(usb.port, setup, usb.tx_buffer);
        } else {
            memset(&usb.out_urb, 0, sizeof(usb.out_urb));
            usbh_bulk_urb_fill(&usb.out_urb, usb.port, usb.out, usb.tx_buffer,
                              size, 500, NULL, NULL);
            result = usbh_submit_urb(&usb.out_urb);
            if (!result && usb.out_urb.actual_length != (uint32_t)size) result = -1;
        }
        if (result < 0) usb.failed = true;
    }
    xSemaphoreGive(usb.mutex);
    return result < 0 ? result : 0;
}

static void deliver(uint8_t type, const uint8_t *packet, size_t size, void *context)
{
    (void)context;
    if (usb.packet_handler) usb.packet_handler(type, (uint8_t *)packet, size);
}

bool pan_usb_poll(void)
{
    usb_message_t message;
    while (xQueueReceive(usb.messages, &message, 0) == pdTRUE) {
        if (message.generation != usb.generation || !usb.active) continue;
        if (!pan_hci_feed(message.type == 4 ? event_rx : acl_rx,
                          message.type, message.data, message.length, deliver, NULL)) usb.failed = true;
    }
    return !usb.failed;
}

const hci_transport_t *pan_usb_transport(void)
{
    static const hci_transport_t transport = {
        .name = "CherryUSB CSR HCI", .open = transport_open, .close = transport_close,
        .register_packet_handler = register_handler, .send_packet = transport_send,
    };
    return &transport;
}
