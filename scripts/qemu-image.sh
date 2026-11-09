#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build a minimal QNX 8.0 x86_64 QEMU image with mkqnximage, inside the
# "qnx-build" container (see docker/setup.sh). Small (~150 MB) and built only from
# your installed SDP, unlike the 50 GB prebuilt quick start disk.
#
#   scripts/qemu-image.sh ~/coldkeep-qemu/store-a store-a [~/.ssh/id_ed25519.pub]
#
# Extra mkqnximage options go in QNX_IMAGE_OPTS, e.g. QNX_IMAGE_OPTS="--secpol=develop".
# The directory must be under $HOME (the container sees $HOME at /home/$USER).
# Root can then log in with the given SSH key: ssh -p 2222 root@127.0.0.1
set -euo pipefail

DIR=${1:?usage: qemu-image.sh <dir under \$HOME> <hostname> [ssh-public-key]}
HOST=${2:?missing hostname}
KEY=${3:-$HOME/.ssh/id_ed25519.pub}
CONTAINER=${QNX_CONTAINER:-qnx-build}
CHOME=${QNX_CONTAINER_HOME:-/home/$USER}

mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
case "$DIR" in "$HOME"/*) ;; *) echo "$DIR must be under \$HOME" >&2; exit 1 ;; esac
case "$KEY" in "$HOME"/*) ;; *) echo "$KEY must be under \$HOME" >&2; exit 1 ;; esac
[ -f "$KEY" ] || { echo "no SSH public key at $KEY" >&2; exit 1; }
CDIR=$CHOME/${DIR#"$HOME"/}
CKEY=$CHOME/${KEY#"$HOME"/}

mki() {
  docker exec "$CONTAINER" bash -c "cd '$CDIR' && source $CHOME/qnx800/qnxsdp-env.sh >/dev/null && mkqnximage $* --noprompt" \
    | grep -v '^Warning: Host file' || true
}

# First pass creates local/snippets with the chosen options.
mki --type=qemu --arch=x86_64 --hostname="$HOST" --ssh-ident="$CKEY" ${QNX_IMAGE_OPTS:-} --build >/dev/null

# The SDP's OpenSSH sshd needs its sshd-auth helper, which the stock image omits.
S=$DIR/local/snippets
grep -q 'sshd-auth' "$S/system_files.custom" ||
  printf '\n# OpenSSH sshd needs its sshd-auth helper next to sshd-session\nbin/sshd-auth=usr/libexec/sshd-auth\n' >> "$S/system_files.custom"
# The stock boot-time sshd start exits (it looks for /usr/libexec/sshd-auth), so
# start sshd again at the end of start-up, pointing it at the real helper path.
grep -q 'SshdAuthPath' "$S/post_start.custom" ||
  printf '\n# start sshd with the path of its auth helper\npidin -f a | grep -q "bin/sshd" || /system/bin/sshd -f /system/etc/ssh/sshd_config -o SshdAuthPath=/system/bin/sshd-auth\n' >> "$S/post_start.custom"

mki --build | grep -i -E 'error|fail' || true
ls -l "$DIR/output/disk-qemu" "$DIR/output/ifs.bin"
echo "built $DIR; boot with: scripts/qemu-run.sh $DIR --headless"
