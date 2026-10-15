// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAN_ADDRESS_SIZE 18
#define PAN_FRAME_MAX 1514
#define PAN_SCAN_MAX 12

bool pan_address_parse(const char *text, uint8_t out[6]);
void pan_address_format(const uint8_t address[6], char out[PAN_ADDRESS_SIZE]);
bool pan_peer_allowed(const uint8_t configured[6], const uint8_t peer[6]);
bool pan_pairing_allowed(const uint8_t configured[6], const uint8_t peer[6],
                         uint64_t now_ms, uint64_t pairing_until_ms);
uint32_t pan_retry_delay(uint32_t minimum_s, uint32_t maximum_s, unsigned failures);

typedef struct {
    uint8_t data[2048];
    size_t used;
} pan_hci_reassembly_t;

typedef void (*pan_hci_packet_fn)(uint8_t type, const uint8_t *packet,
                                 size_t size, void *context);
bool pan_hci_feed(pan_hci_reassembly_t *rx, uint8_t type,
                  const uint8_t *data, size_t size,
                  pan_hci_packet_fn deliver, void *context);
