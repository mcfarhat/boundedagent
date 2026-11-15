# BoundedAgent: build with the QNX SDP 8.0 toolchain.  `source ~/qnx800/qnxsdp-env.sh` first.
#   make            -> x86_64 (QEMU)      make ARCH=aarch64le -> Raspberry Pi 5
ARCH   ?= x86_64
CC      = qcc -Vgcc_nto$(ARCH)
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDLIBS  = -lm
OUT     = build/$(ARCH)
BINS    = ctrld guardd sensord loadgen agentd_stub

SRC_ctrld       = ctrld/ctrld.c
SRC_guardd      = guardd/guardd.c
SRC_sensord     = sensord/sensord.c
SRC_loadgen     = loadgen/loadgen.c
SRC_agentd_stub = agentd/agentd_stub.c

all: $(addprefix $(OUT)/,$(BINS))

.SECONDEXPANSION:
$(OUT)/%: $$(SRC_$$*) common/ba.h common/rt.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT):
	mkdir -p $@

clean:
	rm -r build

.PHONY: all clean
