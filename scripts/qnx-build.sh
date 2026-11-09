#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build a target/ directory with the QNX toolchain inside the "qnx-build"
# Docker container (QNX's docker-mac workflow mounts your Mac home directory
# at /home/$USER inside the container).
#
#   scripts/qnx-build.sh target/qnxcheck            # x86_64
#   ARCH=aarch64le scripts/qnx-build.sh target/qnxcheck
set -euo pipefail

DIR=${1:?usage: qnx-build.sh <dir under the repo> }
ARCH=${ARCH:-x86_64}
CONTAINER=${QNX_CONTAINER:-qnx-build}
CONTAINER_HOME=${QNX_CONTAINER_HOME:-/home/$USER}

REPO=$(cd "$(dirname "$0")/.." && pwd)
case "$REPO" in
  "$HOME"/*) REL=${REPO#"$HOME"/} ;;
  *) echo "repo must live under \$HOME so the container can see it" >&2; exit 1 ;;
esac

docker exec "$CONTAINER" bash -c "
  set -e
  [ -f /usr/local/qnx/env/bin/activate ] && source /usr/local/qnx/env/bin/activate
  source $CONTAINER_HOME/qnx800/qnxsdp-env.sh >/dev/null
  make -C '$CONTAINER_HOME/$REL/$DIR' ARCH=$ARCH
"
