#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the "qnx800" image and (re)create the "qnx-build" container with your
# Mac home directory mounted at /home/$USER (where ~/qnx800 and this repo live).
#
#   bash docker/setup.sh
set -euo pipefail
cd "$(dirname "$0")"

docker build -t qnx800 --platform linux/amd64 \
  --build-arg USER_NAME="$(id -un)" \
  --build-arg USER_ID="$(id -u)" \
  --build-arg GROUP_ID="$(id -g)" .

docker rm -f qnx-build >/dev/null 2>&1 || true
docker create -it --name qnx-build --platform linux/amd64 \
  -v "$HOME:/home/$USER" qnx800 >/dev/null
docker start qnx-build >/dev/null
echo "qnx-build is running. Checking that the SDP is visible inside it:"
docker exec qnx-build bash -c "ls /home/$USER/qnx800/qnxsdp-env.sh && echo OK || echo 'qnx800 not found under your home folder'"
