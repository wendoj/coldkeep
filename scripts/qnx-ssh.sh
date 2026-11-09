#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Run a command as root on a QEMU target started by qemu-run.sh.
#   scripts/qnx-ssh.sh <slot> [command...]
set -euo pipefail
SLOT=${1:?usage: qnx-ssh.sh <slot> [command...]}; shift
exec ssh -p $((2222 + SLOT)) -o UserKnownHostsFile="$HOME/coldkeep-qemu/known_hosts" \
  -o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=20 root@127.0.0.1 "$@"
