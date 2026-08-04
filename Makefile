CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
ENGINE  ?= ../moe-stream/c

all: tracker maintainer liblumibri.so test_shim lumibri

lumibri: lumibri.c lumibri_proto.h
	$(CC) $(CFLAGS) -pthread lumibri.c -o $@

# ---- phase 2: peers execute experts ------------------------------------
# Both sides are built from the engine's own source so the expert math cannot
# drift between local and remote: expert_node.c includes olmoe.c, and the
# chatter is olmoe.c itself with -DLUMIBRI_P2P.
P2P_CFLAGS = -O2 -fopenmp -Wall -I. -I$(ENGINE) \
             -Wno-unused-function -Wno-unused-parameter

phase2: expert_node olmoe_p2p

expert_node: expert_node.c lumibri_proto.h $(ENGINE)/olmoe.c
	$(CC) $(P2P_CFLAGS) expert_node.c -o $@ -lm -lpthread

olmoe_p2p: $(ENGINE)/olmoe.c lumibri_client.h lumibri_proto.h
	$(CC) $(P2P_CFLAGS) -DLUMIBRI_P2P $(ENGINE)/olmoe.c -o $@ -lm -lpthread

tiny_olmoe/config.json: make_tiny_olmoe.py
	python3 make_tiny_olmoe.py tiny_olmoe

fixture: tiny_olmoe/config.json

test-phase2: phase2 fixture
	./phase2_test.sh

tracker: tracker.c lumibri_proto.h
	$(CC) $(CFLAGS) -pthread tracker.c -o $@

maintainer: maintainer.c lumibri_proto.h
	$(CC) $(CFLAGS) -pthread maintainer.c -o $@

# The shim interposes libc symbols, so it must not itself be interposable
# state: -fPIC shared object, resolved via RTLD_NEXT at load time.
liblumibri.so: lumishim.c lumibri_proto.h
	$(CC) $(CFLAGS) -shared -fPIC -pthread lumishim.c -o $@ -ldl

test_shim: test_shim.c
	$(CC) $(CFLAGS) test_shim.c -o $@

test: all
	./selftest.sh

clean:
	rm -f tracker maintainer liblumibri.so test_shim

.PHONY: all test clean
