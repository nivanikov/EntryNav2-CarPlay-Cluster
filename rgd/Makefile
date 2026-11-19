# Copyright (c) 2026 nivanikov
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Host-side unit tests for the iAP2 route-guidance parser (iap2_rgd.c).
# These run on your PC; no car and no cross-toolchain needed:
#     make test
#
# The LD_PRELOAD shim itself (rgd_shim.c -> carplay-shim.so) is built with the head
# unit's own armhf toolchain and loaded into the CARPLAY process on the car; it is not
# built here. See ../README.md and ../persist/README-persist.md.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11

test: test_iap2_rgd
	./test_iap2_rgd

test_iap2_rgd: test_iap2_rgd.c iap2_rgd.c iap2_rgd.h
	$(CC) $(CFLAGS) -o $@ test_iap2_rgd.c iap2_rgd.c

clean:
	rm -f test_iap2_rgd

.PHONY: test clean
