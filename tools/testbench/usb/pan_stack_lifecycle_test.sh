#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/../../.." && pwd)
task_component="$task_root/components/usb_bluetooth_pan"
: "${WICAN_BTSTACK_PATH:?Set WICAN_BTSTACK_PATH to the pinned BTstack checkout}"
task_build=$(mktemp -d /tmp/wican-pan-stack.XXXXXX)
trap 'rm -f "$task_build/lifecycle"; rmdir "$task_build"' EXIT
sources=()
for source in btstack_linked_list btstack_linked_queue btstack_memory \
    btstack_memory_pool btstack_run_loop btstack_util btstack_crypto \
    btstack_tlv hci hci_cmd hci_dump l2cap l2cap_signaling ad_parser; do
    sources+=("$WICAN_BTSTACK_PATH/src/$source.c")
done
for source in bnep sdp_client sdp_util; do
    sources+=("$WICAN_BTSTACK_PATH/src/classic/$source.c")
done
sources+=("$WICAN_BTSTACK_PATH/platform/embedded/btstack_run_loop_embedded.c")
"${CC:-cc}" -std=c11 -fsanitize=address,undefined -g \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$task_component" -I "$WICAN_BTSTACK_PATH/src" \
    -I "$WICAN_BTSTACK_PATH/platform/embedded" \
    "${sources[@]}" "$task_component/host_test/test_stack_lifecycle.c" \
    -o "$task_build/lifecycle"
"$task_build/lifecycle"
