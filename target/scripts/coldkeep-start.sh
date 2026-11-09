#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Start one COLDKEEP store on the QNX target (run as root).
#
#   coldkeep-start.sh legacy|protected [seed] [speed]
#   coldkeep-start.sh stop
#
# Env: CASES (default chilled:4,frozen:2,coldroom:2), KEEP=1 keeps the previous
# journal and governor state (default: a fresh run).
MODE=${1:?usage: coldkeep-start.sh legacy|protected|stop [seed] [speed]}
SEED=${2:-7}
SPEED=${3:-30}
CASES=${CASES:-chilled:4,frozen:2,coldroom:2}
BIN=/data/coldkeep/bin
DATA=/data/coldkeep
LOG=$DATA/log

slay -f sup >/dev/null 2>&1
slay -f bridge mgmtd brokerd governord casectl auditd plantsim >/dev/null 2>&1
sleep 1
[ "$MODE" = stop ] && { echo "stopped"; exit 0; }
case "$MODE" in legacy|protected) ;; *) echo "mode must be legacy or protected"; exit 2 ;; esac

mkdir -p "$LOG"
if [ "${KEEP:-0}" != 1 ]; then
  rm -f "$DATA/gov/state"
  [ -f "$DATA/audit.bin" ] && mv "$DATA/audit.bin" "$DATA/audit.prev.bin"
fi

N=0
OLDIFS=$IFS; IFS=,
for part in $CASES; do N=$((N + ${part#*:})); done
IFS=$OLDIFS

"$BIN/plantsim" -s "$SEED" -x "$SPEED" -c "$CASES" > "$LOG/plantsim.log" 2>&1 &
sleep 1
"$BIN/auditd" > "$LOG/auditd.log" 2>&1 &
if [ "$MODE" = protected ]; then
  # sup starts (and restarts) the trusted processes in order, each once the
  # previous one's resource manager is attached
  rm -f "$LOG"/casectl.*.log "$LOG"/governord.log "$LOG"/brokerd.log
  set --
  i=1
  while [ $i -le $N ]; do
    set -- "$@" -c "casectl.$i /dev/coldkeep/case/$i $BIN/casectl -n $i"
    i=$((i + 1))
  done
  "$BIN/sup" -l "$LOG" "$@" \
    -c "governord /dev/coldkeep/gov $BIN/governord" \
    -c "brokerd /dev/coldkeep/broker $BIN/brokerd" > "$LOG/sup.log" 2>&1 &
  i=0
  while [ ! -e /dev/coldkeep/broker ] && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
  "$BIN/mgmtd" > "$LOG/mgmtd.log" 2>&1 &
  "$BIN/bridge" > "$LOG/bridge.log" 2>&1 &
else
  i=1
  while [ $i -le $N ]; do
    "$BIN/casectl" -n $i -L > "$LOG/casectl.$i.log" 2>&1 &
    i=$((i + 1))
  done
  sleep 1
  "$BIN/mgmtd" -L > "$LOG/mgmtd.log" 2>&1 &
  "$BIN/bridge" -L > "$LOG/bridge.log" 2>&1 &
fi
sleep 2
echo "COLDKEEP $MODE store: $N cases, seed $SEED, speed ${SPEED}x"
cat "$LOG"/*.log
