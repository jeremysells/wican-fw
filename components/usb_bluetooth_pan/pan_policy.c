// SPDX-License-Identifier: GPL-3.0-or-later
#include "pan_policy.h"

#include <stdio.h>
#include <string.h>

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool pan_address_parse(const char *text, uint8_t out[6])
{
    uint8_t address[6];
    bool any = false, all_ff = true;
    if (!text || strlen(text) != 17 || !out) return false;
    for (unsigned i = 0; i < 6; ++i) {
        int high = hex_digit(text[i * 3]), low = hex_digit(text[i * 3 + 1]);
        if (high < 0 || low < 0 || (i < 5 && text[i * 3 + 2] != ':')) return false;
        address[i] = (uint8_t)((high << 4) | low);
        any |= address[i] != 0;
        all_ff &= address[i] == 255;
    }
    if (!any || all_ff) return false;
    memcpy(out, address, sizeof(address));
    return true;
}

void pan_address_format(const uint8_t address[6], char out[PAN_ADDRESS_SIZE])
{
    snprintf(out, PAN_ADDRESS_SIZE, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[0], address[1], address[2], address[3], address[4], address[5]);
}

bool pan_peer_allowed(const uint8_t configured[6], const uint8_t peer[6])
{
    static const uint8_t empty[6];
    return configured && peer && memcmp(configured, empty, 6) != 0
           && memcmp(configured, peer, 6) == 0;
}

bool pan_pairing_allowed(const uint8_t configured[6], const uint8_t peer[6],
                         uint64_t now_ms, uint64_t pairing_until_ms)
{
    return pairing_until_ms > now_ms && pan_peer_allowed(configured, peer);
}

uint32_t pan_retry_delay(uint32_t minimum_s, uint32_t maximum_s, unsigned failures)
{
    uint32_t delay = minimum_s;
    if (maximum_s < minimum_s) return maximum_s;
    while (failures-- && delay < maximum_s) {
        if (delay > maximum_s / 2) return maximum_s;
        delay *= 2;
    }
    return delay;
}

bool pan_hci_feed(pan_hci_reassembly_t *rx, uint8_t type,
                  const uint8_t *data, size_t size,
                  pan_hci_packet_fn deliver, void *context)
{
    size_t header = type == 4 ? 2 : type == 2 ? 4 : 0;
    if (!rx || !header || (!data && size) || !deliver) return false;
    while (size) {
        size_t room = sizeof(rx->data) - rx->used;
        size_t take = size < room ? size : room;
        if (!take) { rx->used = 0; return false; }
        memcpy(rx->data + rx->used, data, take);
        rx->used += take;
        data += take;
        size -= take;
        while (rx->used >= header) {
            size_t length = header + (type == 4 ? rx->data[1]
                          : ((size_t)rx->data[2] | ((size_t)rx->data[3] << 8)));
            if (length > sizeof(rx->data)) { rx->used = 0; return false; }
            if (rx->used < length) break;
            deliver(type, rx->data, length, context);
            rx->used -= length;
            memmove(rx->data, rx->data + length, rx->used);
        }
    }
    return true;
}
