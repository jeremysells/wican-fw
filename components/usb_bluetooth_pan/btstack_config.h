// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define ENABLE_CLASSIC
#define HAVE_MALLOC
#define HAVE_EMBEDDED_TIME_MS
// BNEP requires a 1691-byte L2CAP MTU. HCI fragments this into the
// controller's smaller ACL packets (310 bytes on the supplied adapters).
#define HCI_ACL_PAYLOAD_SIZE 1695
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_L2CAP_SERVICES 2
#define MAX_NR_BNEP_CHANNELS 1
#define MAX_NR_BNEP_SERVICES 1
#define MAX_NR_SDP_CLIENTS 1
