#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Copy the built COLDKEEP processes to a QEMU target and (optionally) start a store.
#   scripts/deploy.sh <slot> [legacy|protected] [seed] [speed]
set -euo pipefail
SLOT=${1:?usage: deploy.sh <slot> [legacy|protected] [seed] [speed]}
MODE=${2:-}
ARCH=${ARCH:-x86_64}
cd "$(dirname "$0")/.."
B=target/build/$ARCH
for p in plantsim casectl auditd governord brokerd mgmtd; do
  [ -x "$B/$p" ] || { echo "missing $B/$p: run scripts/qnx-build.sh target" >&2; exit 1; }
done
scripts/qnx-ssh.sh "$SLOT" 'mkdir -p /data/coldkeep/bin; [ -f /data/coldkeep/bin/coldkeep-start.sh ] && sh /data/coldkeep/bin/coldkeep-start.sh stop >/dev/null; true'
scripts/qnx-scp.sh "$SLOT" /data/coldkeep/bin "$B/plantsim" "$B/casectl" "$B/auditd" "$B/governord" \
  "$B/brokerd" "$B/mgmtd" target/scripts/coldkeep-start.sh
echo "deployed to slot $SLOT"
if [ -n "$MODE" ]; then
  out=$(scripts/qnx-ssh.sh "$SLOT" "sh /data/coldkeep/bin/coldkeep-start.sh $MODE ${3:-7} ${4:-30}")
  echo "${out%%$'\n'*}"
fi
