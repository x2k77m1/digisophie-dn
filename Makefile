CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
CROSS ?= m68k-elf-
CROSS_CFLAGS = -mcpu=54455 -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie -fomit-frame-pointer -Wall -Wextra -Werror

.PHONY: test cross-check dn-host-tests
out:
	mkdir -p out
out/test_sophie: sophie.c sophie.h tests/test_sophie.c | out
	$(CC) $(CFLAGS) -I. sophie.c tests/test_sophie.c -o $@
test: out/test_sophie
	./out/test_sophie
# ColdFire check for the shared DSP core (needs no firmware headers).
# The full mod (digisophie-dn/engine.c, overlay.c) is checked by
# scripts/build-digisophie-dn.sh against tools/elekloader headers.
cross-check: | out
	mkdir -p out/cross
	$(CROSS)gcc $(CROSS_CFLAGS) -I. -c sophie.c -o out/cross/sophie.o
dn-host-tests:
	python3 tools/sophie_dn_host_test.py
	python3 tools/sophie_engine_host_test.py
