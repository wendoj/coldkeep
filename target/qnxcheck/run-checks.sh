#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# QNX feature checks. Run ON THE TARGET, as root, from the directory that holds
# able_server and able_client:
#
#   sh run-checks.sh                 > results.txt 2>&1   (first boot)
#   sh run-checks.sh persist-check   > results2.txt 2>&1  (after a reboot)
#
# Then copy the results files back to the Mac and paste them into the chat.
# Nothing here changes the system except /dev/ckcheck (while the test runs) and
# small marker files named .ck_* in the directories it probes.

TEST_UID=${TEST_UID:-2000}
MODE=${1:-all}

section() { echo; echo "================ $* ================"; }
try() { echo "\$ $*"; "$@" 2>&1; echo "(exit $?)"; }

section "system"
try uname -a
try id
try date

if [ "$MODE" = "persist-check" ]; then
  section "Q5 persistence (after reboot)"
  for d in /data /data/home /data/var /var /tmp /home "$HOME"; do
    if [ -f "$d/.ck_persist" ]; then echo "$d: marker SURVIVED reboot ($(cat "$d/.ck_persist"))"
    else echo "$d: marker missing"; fi
  done
  exit 0
fi

section "Q1/Q2 custom ability + client ability check"
./able_server &
SERVER=$!
sleep 1
echo "--- 1. client stays root (does root hold a custom ability by default?)"
./able_client
echo "--- 2. client drops to uid $TEST_UID WITHOUT keeping the ability (expect EPERM)"
./able_client -u "$TEST_UID"
echo "--- 3. client drops to uid $TEST_UID and KEEPS the ability (expect accepted)"
./able_client -u "$TEST_UID" -k
echo "--- 4. ordinary shell command as uid $TEST_UID (expect EPERM)"
try on -u "$TEST_UID" sh -c 'echo x > /dev/ckcheck'
echo "--- 5. abilities of a dropped process, as pidin shows them"
./able_client -u "$TEST_UID" -s 4 &
CLIENT=$!
sleep 2
try pidin -p "$CLIENT" abilities
try pidin -p "$CLIENT" -f a
wait "$CLIENT"
kill "$SERVER" 2>/dev/null

section "Q3 security policy"
for t in secpolpush secpolgenerate secpolcompile secpol; do try which "$t"; done
try ls /proc/boot
echo "--- procnto command line (policy support is a procnto option)"
try pidin -p 1 arg
try ls -l /dev/secpol
try ls -l /proc/self/secpol

section "Q4 high availability manager"
try which ham hamctrl
pidin 2>/dev/null | grep -i ham || echo "no ham process running"

section "Q5 writable directories"
for d in /data /data/home /data/var /var /tmp /home "$HOME"; do
  if [ -d "$d" ] && echo "written $(date)" > "$d/.ck_persist" 2>/dev/null; then
    echo "$d: WRITABLE (marker written; reboot and run 'persist-check')"
  else
    echo "$d: not writable or missing"
  fi
done
try df -h

section "Q7 adaptive partitioning"
try which aps
try aps show

section "networking and services"
try ifconfig
try which sshd qconn
pidin 2>/dev/null | grep -E "sshd|qconn" || echo "no sshd/qconn running"

echo
echo "done"
