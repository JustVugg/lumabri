CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
ENGINE  ?= ../moe-stream/c

all: tracker maintainer liblumabri.so test_shim lumabri

lumabri: lumabri.c lumabri_proto.h
	$(CC) $(CFLAGS) -pthread lumabri.c -o $@

# ---- phase 2: peers execute experts ------------------------------------
# Both sides are built from the engine's own source so the expert math cannot
# drift between local and remote: expert_node.c includes olmoe.c, and the
# chatter is olmoe.c itself with -DLUMABRI_P2P.
P2P_CFLAGS = -O2 -fopenmp -Wall -I. -I$(ENGINE) \
             -Wno-unused-function -Wno-unused-parameter

phase2: expert_node olmoe_p2p
phase2-glm: expert_node_glm

# One expert-node binary per engine. The body is the same file; everything
# engine-shaped lives in expert_engines/<name>.h, which pulls in the engine's
# own source so remote and local run the same kernels on the same weights.
EXPERT_DEPS = expert_node.c lumabri_proto.h

expert_node: $(EXPERT_DEPS) expert_engines/olmoe.h $(ENGINE)/olmoe.c
	$(CC) $(P2P_CFLAGS) expert_node.c -o $@ -lm -lpthread

expert_node_glm: $(EXPERT_DEPS) expert_engines/colibri.h $(ENGINE)/colibri.c
	$(CC) $(P2P_CFLAGS) -DLMBE_ENGINE_HEADER='"expert_engines/colibri.h"' \
	      expert_node.c -o $@ -lm -lpthread

expert_node_inkling: $(EXPERT_DEPS) expert_engines/inkling.h $(ENGINE)/inkling.c
	$(CC) $(P2P_CFLAGS) -DLMBE_ENGINE_HEADER='"expert_engines/inkling.h"' \
	      expert_node.c -o $@ -lm -lpthread

expert_node_kimi: $(EXPERT_DEPS) expert_engines/kimi_k3.h $(ENGINE)/kimi_k3.c
	$(CC) $(P2P_CFLAGS) -DLMBE_ENGINE_HEADER='"expert_engines/kimi_k3.h"' \
	      expert_node.c -o $@ -lm -lpthread

# Regenerate the engine patches from a colibri checkout (never modifies it)
patches:
	python3 engine_patches/make_patches.py --engine-dir $(ENGINE)

patches-check:
	python3 engine_patches/make_patches.py --engine-dir $(ENGINE) --check

# -DLUMIBRI_P2P: the engine patch predates the rename and tests that macro;
# the engine is never modified, so both spellings are defined here.
olmoe_p2p: $(ENGINE)/olmoe.c lumabri_client.h lumibri_client.h lumabri_proto.h
	$(CC) $(P2P_CFLAGS) -DLUMABRI_P2P -DLUMIBRI_P2P $(ENGINE)/olmoe.c -o $@ -lm -lpthread

tiny_olmoe/config.json: make_tiny_olmoe.py
	python3 make_tiny_olmoe.py tiny_olmoe

fixture: tiny_olmoe/config.json

test-phase2: phase2 fixture
	./phase2_test.sh

test-phase2-glm: phase2-glm
	./phase2_glm_test.sh

tracker: tracker.c lumabri_proto.h
	$(CC) $(CFLAGS) -pthread tracker.c -o $@

maintainer: maintainer.c lumabri_proto.h
	$(CC) $(CFLAGS) -pthread maintainer.c -o $@

# The shim interposes libc symbols, so it must not itself be interposable
# state: -fPIC shared object, resolved via RTLD_NEXT at load time.
liblumabri.so: lumashim.c lumabri_proto.h
	$(CC) $(CFLAGS) -shared -fPIC -pthread lumashim.c -o $@ -ldl

test_shim: test_shim.c
	$(CC) $(CFLAGS) test_shim.c -o $@

test: all
	./selftest.sh

# ---- deploy -------------------------------------------------------------
# make install                    → /usr/local (needs sudo)
# make install PREFIX=$$HOME/.local  → per-user, no root
# Every binary lands in PREFIX/bin (they find each other via /proc/self/exe)
# and the shim in PREFIX/lib/lumabri. Phase-2 binaries are installed when
# they have been built (make phase2 ENGINE=...).
PREFIX ?= /usr/local

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/lumabri
	install -m 755 lumabri tracker maintainer $(DESTDIR)$(PREFIX)/bin/
	install -m 644 liblumabri.so $(DESTDIR)$(PREFIX)/lib/lumabri/
	@for b in expert_node expert_node_glm expert_node_inkling expert_node_kimi olmoe_p2p; do \
	    [ -f $$b ] && install -m 755 $$b $(DESTDIR)$(PREFIX)/bin/ || true; done
	@echo "installed under $(DESTDIR)$(PREFIX)"

clean:
	rm -f tracker maintainer liblumabri.so test_shim lumabri olmoe_p2p \
	      expert_node expert_node_glm expert_node_inkling expert_node_kimi

.PHONY: all test clean install phase2 phase2-glm fixture test-phase2 \
        test-phase2-glm patches patches-check
