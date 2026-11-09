#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Boot a QNX image built by mkqnximage (--type=qemu --arch=x86_64) with the host's
# QEMU (e.g. Homebrew on macOS). Same machine/disk/kernel arguments as QNX's own
# runimage script, but with QEMU user-mode networking and port forwards instead of
# a Linux bridge, so it works on macOS.
#
#   scripts/qemu-run.sh <image-dir> [--slot N] [--headless]
#   scripts/qemu-run.sh <image-dir> --stop
#
# Slot N (default 0) picks the forwarded host ports, so two stores can run at once:
#   ssh  127.0.0.1:(2222+N)   -> target 22
#   mgmt 127.0.0.1:(18080+N)  -> target 8080
#   tele 127.0.0.1:(17070+N)  -> target 7070
#   serial console (headless only) on 127.0.0.1:(4440+N), also logged to output/qemu.out
set -euo pipefail

DIR=${1:?usage: qemu-run.sh <image-dir> [--slot N] [--headless|--stop]}
shift
SLOT=0 HEADLESS=0 STOP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --slot) SLOT=$2; shift 2 ;;
    --headless) HEADLESS=1; shift ;;
    --stop) STOP=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

cd "$DIR"
PIDFILE=output/qemu.pid
if [ "$STOP" = 1 ]; then
  if [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null; then echo "stopped"; else echo "not running"; fi
  rm -f "$PIDFILE"
  exit 0
fi

opt() { grep "^OPT_$1=" output/options | head -1 | cut -d"'" -f2; }
CPU=$(opt CPU); RAM=$(opt RAM); MAC=$(opt MACADDR)
SSH=$((2222 + SLOT)); MGMT=$((18080 + SLOT)); TELE=$((17070 + SLOT)); CONS=$((4440 + SLOT))

ARGS=(
  -cpu max -smp "$CPU" -m "$RAM"   # no hardware accel on macOS: same as QNX runimage without KVM
  -drive file=output/disk-qemu.vmdk,if=ide,id=drv0
  -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH-:22,hostfwd=tcp:127.0.0.1:$MGMT-:8080,hostfwd=tcp:127.0.0.1:$TELE-:7070"
  -device "virtio-net-pci,netdev=net0,mac=$MAC"
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0
  -kernel output/ifs.bin
  -pidfile "$PIDFILE"
)

echo "ssh: ssh -p $SSH root@127.0.0.1   mgmt: 127.0.0.1:$MGMT   telemetry: 127.0.0.1:$TELE"
if [ "$HEADLESS" = 1 ]; then
  qemu-system-x86_64 "${ARGS[@]}" -display none -monitor none -daemonize \
    -chardev "socket,id=ser0,host=127.0.0.1,port=$CONS,server=on,wait=off,logfile=output/qemu.out" \
    -serial chardev:ser0
  echo "running in background; console: nc 127.0.0.1 $CONS (log: $DIR/output/qemu.out); stop with --stop"
else
  echo "serial console below; quit QEMU with Ctrl+A then X"
  exec qemu-system-x86_64 "${ARGS[@]}" -nographic -serial mon:stdio
fi
