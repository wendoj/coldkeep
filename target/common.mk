# Toolchain settings shared by the target builds.
ARCH    ?= x86_64
QCC     := qcc -Vgcc_nto$(ARCH)

# The portable core is built exactly as on the host (same determinism flags).
CORE_CFLAGS   := -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
                 -Wno-unused-parameter -ffp-contract=off -I../core/include
TARGET_CFLAGS := -O2 -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
                 -ffp-contract=off -I../core/include -Icommon
LDLIBS        := -lsocket -lm
