// SPDX-License-Identifier: GPL-3.0-or-later
#include "pan_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned delivered;
static uint8_t expected_type;
static const uint8_t *expected_packet;
static size_t expected_size;
static void receive(uint8_t type, const uint8_t *packet, size_t size, void *context)
{
    (void)context;
    assert(type == expected_type);
    assert(size == expected_size);
    assert(!memcmp(packet, expected_packet, size));
    delivered++;
}

int main(void)
{
    uint8_t phone[6], stranger[6], copy[6];
    assert(pan_address_parse("AA:BB:CC:DD:EE:01", phone));
    assert(pan_address_parse("AA:BB:CC:DD:EE:02", stranger));
    const char *invalid[] = { "", "AA:BB:CC:DD:EE", "AA:BB:CC:DD:EE:01x",
        "AG:BB:CC:DD:EE:01", "AA-BB-CC-DD-EE-01", "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!pan_address_parse(invalid[i], copy));
    assert(!pan_address_parse(NULL, copy));
    assert(!pan_address_parse("AA:BB:CC:DD:EE:01", NULL));
    char text[18];
    pan_address_format(phone, text);
    assert(!strcmp(text, "AA:BB:CC:DD:EE:01"));
    assert(pan_peer_allowed(phone, phone));
    assert(!pan_peer_allowed(phone, stranger));
    memset(copy, 0, 6);
    assert(!pan_peer_allowed(copy, copy));
    assert(pan_pairing_allowed(phone, phone, 10, 11));
    assert(!pan_pairing_allowed(phone, phone, 11, 11));
    assert(!pan_pairing_allowed(phone, stranger, 10, 11));
    assert(!pan_pairing_allowed(phone, phone, 0, 0));
    assert(pan_retry_delay(5, 60, 0) == 5);
    assert(pan_retry_delay(5, 60, 3) == 40);
    assert(pan_retry_delay(5, 60, 4) == 60);
    assert(pan_retry_delay(5, 60, 1000) == 60);
    assert(pan_retry_delay(UINT32_MAX / 2, UINT32_MAX, 100) == UINT32_MAX);
    assert(pan_retry_delay(30, 10, 0) == 10);

    /* USB event endpoints can fragment a single HCI event into 16-byte
     * reads; ACL bulk reads can contain both fragments and whole packets. */
    const uint8_t event[] = { 0x0e, 6, 1, 1, 0x10, 0, 0x06, 0x22 };
    const uint8_t acl[] = { 1, 0x20, 7, 0, 3, 0, 15, 0, 1, 2, 3 };
    const struct { uint8_t type; const uint8_t *data; size_t size; } cases[] = {
        { 4, event, sizeof(event) }, { 2, acl, sizeof(acl) }
    };
    for (unsigned c = 0; c < 2; ++c) {
        expected_type = cases[c].type;
        expected_packet = cases[c].data;
        expected_size = cases[c].size;
        for (size_t split = 0; split <= expected_size; ++split) {
            pan_hci_reassembly_t rx = { 0 };
            delivered = 0;
            assert(pan_hci_feed(&rx, expected_type, expected_packet, split, receive, NULL));
            assert(pan_hci_feed(&rx, expected_type, expected_packet + split, expected_size - split, receive, NULL));
            assert(delivered == 1 && rx.used == 0);
        }
        uint8_t combined[32];
        memcpy(combined, expected_packet, expected_size);
        memcpy(combined + expected_size, expected_packet, expected_size);
        pan_hci_reassembly_t rx = { 0 };
        delivered = 0;
        assert(pan_hci_feed(&rx, expected_type, combined, expected_size * 2, receive, NULL));
        assert(delivered == 2 && !rx.used);
    }
    pan_hci_reassembly_t rx = { 0 };
    const uint8_t oversized[] = { 1, 0, 0xff, 0xff };
    assert(!pan_hci_feed(&rx, 2, oversized, sizeof(oversized), receive, NULL));
    assert(!rx.used);
    assert(!pan_hci_feed(&rx, 3, event, sizeof(event), receive, NULL));
    assert(!pan_hci_feed(NULL, 4, event, sizeof(event), receive, NULL));
    assert(!pan_hci_feed(&rx, 4, NULL, 1, receive, NULL));
    assert(!pan_hci_feed(&rx, 4, event, sizeof(event), NULL, NULL));

    uint8_t large_acl[1699] = { 1, 0x20, 0x9f, 0x06 };
    for (size_t i = 4; i < sizeof(large_acl); ++i) large_acl[i] = (uint8_t)i;
    uint8_t large_event[257] = { 0x0e, 255 };
    for (size_t i = 2; i < sizeof(large_event); ++i) large_event[i] = (uint8_t)i;
    const struct { uint8_t type; const uint8_t *data; size_t size; } large_cases[] = {
        { 2, large_acl, sizeof(large_acl) }, { 4, large_event, sizeof(large_event) }
    };
    for (unsigned c = 0; c < 2; ++c) {
        expected_type = large_cases[c].type;
        expected_packet = large_cases[c].data;
        expected_size = large_cases[c].size;
        for (size_t chunk = 1; chunk <= 64; ++chunk) {
            rx.used = 0;
            delivered = 0;
            for (size_t offset = 0; offset < expected_size; offset += chunk) {
                size_t length = expected_size - offset;
                if (length > chunk) length = chunk;
                assert(pan_hci_feed(&rx, expected_type, expected_packet + offset, length, receive, NULL));
            }
            assert(delivered == 1 && !rx.used);
        }
    }
    puts("PAN policy and HCI reassembly: PASS");
    return 0;
}
