# SPDX-License-Identifier: Apache-2.0
#
# Minimal Linux build environment for the QNX SDP 8.0 host tools (qcc). The SDP
# itself is NOT in the image: it stays in ~/qnx800 on the Mac and is reached
# through the home-directory mount, so no licensed files are ever baked in.
#
# Every download uses verified HTTPS, so this also builds on networks that block
# plain HTTP. The base image (Ubuntu 22.04) already ships ca-certificates.
FROM buildpack-deps:jammy-curl

ARG DEBIAN_FRONTEND=noninteractive

RUN sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g; s|http://security.ubuntu.com|https://security.ubuntu.com|g' /etc/apt/sources.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      make build-essential openssh-client sshpass rsync file less vim-tiny locales \
 && locale-gen en_US.UTF-8 \
 && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

ARG USER_NAME
ARG USER_ID
ARG GROUP_ID
RUN groupadd --gid "${GROUP_ID}" qnxdev 2>/dev/null || true \
 && useradd --uid "${USER_ID}" --gid "${GROUP_ID}" --no-log-init --no-create-home \
      --home-dir "/home/${USER_NAME}" "${USER_NAME}"
USER ${USER_NAME}
WORKDIR /home/${USER_NAME}

CMD ["/bin/bash"]
