#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copy files to a QEMU target started by qemu-run.sh.
#   scripts/qnx-scp.sh <slot> <dest-dir-on-target> <files...>
set -euo pipefail
SLOT=${1:?usage: qnx-scp.sh <slot> <dest> <files...>}; DEST=${2:?missing dest}; shift 2
OPTS=(-o UserKnownHostsFile="$HOME/coldkeep-qemu/known_hosts" -o StrictHostKeyChecking=accept-new -o BatchMode=yes)
ssh "${OPTS[@]}" -p $((2222 + SLOT)) root@127.0.0.1 "mkdir -p '$DEST'"
scp -q "${OPTS[@]}" -P $((2222 + SLOT)) "$@" "root@127.0.0.1:$DEST/"
