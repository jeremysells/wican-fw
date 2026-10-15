#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/../../.." && pwd)
task_component="$task_root/components/usb_bluetooth_pan"
task_build=$(mktemp -d /tmp/wican-pan-policy.XXXXXX)
trap 'rm -f "$task_build/policy"; rmdir "$task_build"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -I "$task_component" "$task_component/pan_policy.c" \
    "$task_component/host_test/test_policy.c" -o "$task_build/policy"
"$task_build/policy"
