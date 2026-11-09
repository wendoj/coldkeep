#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Copy built files to the QNX target over scp.
#
#   scripts/qnx-deploy.sh <target-ip> target/qnxcheck able_server able_client run-checks.sh
#
# Env: QNX_USER (default qnxuser), QNX_PORT (default 22), QNX_DEST (default ckcheck,
# relative to the user's home on the target).
set -euo pipefail

IP=${1:?usage: qnx-deploy.sh <ip> <dir> <files...>}
DIR=${2:?missing dir}
shift 2
USER_T=${QNX_USER:-qnxuser}
PORT=${QNX_PORT:-22}
DEST=${QNX_DEST:-ckcheck}

cd "$(dirname "$0")/../$DIR"
ssh -p "$PORT" "$USER_T@$IP" "mkdir -p $DEST"
scp -P "$PORT" "$@" "$USER_T@$IP:$DEST/"
echo "copied to $USER_T@$IP:~/$DEST"
