// SPDX-License-Identifier: GPL-3.0-or-later
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_embedded.h"
#include "hci.h"
#include "l2cap.h"
#include "classic/bnep.h"
#include "classic/sdp_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned opened, closed, sent;
uint32_t hal_time_ms(void) { return 0; }
void hal_cpu_disable_irqs(void) { }
void hal_cpu_enable_irqs(void) { }
void hal_cpu_enable_irqs_and_sleep(void) { }
void btstack_assert_failed(const char *file, const uint16_t line)
{
    fprintf(stderr, "BTstack assertion: %s:%u\n", file, line);
    abort();
}
static int transport_open(void) { opened++; return 0; }
static int transport_close(void) { closed++; return 0; }
static void register_handler(void (*handler)(uint8_t, uint8_t *, uint16_t)) { (void)handler; }
static int send_packet(uint8_t type, uint8_t *packet, int size)
{
    assert(type == HCI_COMMAND_DATA_PACKET && size >= 3 && packet);
    sent++;
    return 0;
}

int main(void)
{
    const hci_transport_t transport = {
        .name = "test USB HCI", .open = transport_open, .close = transport_close,
        .register_packet_handler = register_handler, .send_packet = send_packet,
    };
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        btstack_memory_init();
        btstack_run_loop_init(btstack_run_loop_embedded_get_instance());
        hci_init(&transport, NULL);
        l2cap_init();
        bnep_init();
        hci_power_control(HCI_POWER_ON);
        btstack_run_loop_embedded_execute_once();
        /* Model unplug while the controller has not answered HCI Reset. */
        hci_close();
        sdp_client_deinit();
        bnep_deinit();
        l2cap_deinit();
        btstack_run_loop_deinit();
    }
    assert(opened == 32 && closed == 32 && sent >= 32);
    puts("BTstack repeated initialization/teardown: PASS");
    return 0;
}
