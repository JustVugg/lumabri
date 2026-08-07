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

# every engine's peer in one go
engines: expert_node expert_node_glm expert_node_inkling expert_node_kimi \
         expert_node_deepseek

# everything phase 2 needs, both sides
phase2-all: engines chatters

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

# DeepSeek V4 needs two extra things at build time.
#
# Its own -D set (upstream's expert-store sizing) has to match, or the store
# is laid out differently than the engine expects.
#
# And deepseek.c does `#undef main` halfway through — it renames a legacy
# entry point of its own — which kills the usual `#define main` trick every
# other glue relies on. So the CLI entry point is renamed textually into a
# build copy instead. One deterministic substitution, and if upstream ever
# changes that signature the copy still has a second main() and the compile
# says so rather than silently building the wrong program.
DS_CFLAGS = -DCOLI_V4_MAX_PIN_SLOTS_PER_LAYER=16 -DCOLI_V4_PIN_RAMP_REQUESTS=24 \
            -DCOLI_V4_EXPERIMENTAL_BLOCK_OTHER_PROFILE \
            -DCOLI_V4_EXPERIMENTAL_DUAL_EXPERT_LOADER

build/deepseek_noentry.c: $(ENGINE)/deepseek.c
	@mkdir -p build
	@sed 's/^int main(int argc, char \*\*argv) {/int deepseek_cli_main_unused(int argc, char **argv) {/' \
	     $< > $@
	@grep -q deepseek_cli_main_unused $@ || \
	  { echo "deepseek.c: main() signature changed — update the rule"; exit 1; }

expert_node_deepseek: $(EXPERT_DEPS) expert_engines/deepseek.h build/deepseek_noentry.c
	$(CC) $(P2P_CFLAGS) $(DS_CFLAGS) -Ibuild \
	      -DLMBE_ENGINE_HEADER='"expert_engines/deepseek.h"' \
	      expert_node.c -o $@ -lm -lpthread

# Regenerate the engine patches from a colibri checkout (never modifies it)
patches:
	python3 engine_patches/make_patches.py --engine-dir $(ENGINE)

patches-check:
	python3 engine_patches/make_patches.py --engine-dir $(ENGINE) --check

# ---- the chatter side: one patched engine binary per engine -------------
#
# `lumabri chat` prefers <engine>_p2p over <engine> in --engines-dir, and
# without one it runs the stock engine — which works, but phase 2 never
# engages and the chatter quietly downloads expert weights instead of asking
# peers to run them. So every engine needs its build here, not just olmoe.
#
# The engine is never modified: the patch is applied to a COPY under build/.
# A working copy that already has the patch applied is taken as-is, so an
# engine tree someone patched by hand still builds.
#
# -DLUMIBRI_P2P: the patches predate the rename and test that macro; both
# spellings are defined so either vintage compiles.
build/%_p2p.c: $(ENGINE)/%.c engine_patches/%-p2p.diff
	@mkdir -p build
	@if grep -q LUMIBRI_P2P $<; then cp $< $@; \
	 else patch -s -p2 -o $@ $< engine_patches/$*-p2p.diff; fi

ENGINE_P2P_DEPS = lumabri_client.h lumibri_client.h lumabri_proto.h

olmoe_p2p: build/olmoe_p2p.c $(ENGINE_P2P_DEPS)
	$(CC) $(P2P_CFLAGS) -DLUMABRI_P2P -DLUMIBRI_P2P $< -o $@ -lm -lpthread

colibri_p2p: build/colibri_p2p.c $(ENGINE_P2P_DEPS)
	$(CC) $(P2P_CFLAGS) -DLUMABRI_P2P -DLUMIBRI_P2P $< -o $@ -lm -lpthread

inkling_p2p: build/inkling_p2p.c $(ENGINE_P2P_DEPS)
	$(CC) $(P2P_CFLAGS) -DLUMABRI_P2P -DLUMIBRI_P2P $< -o $@ -lm -lpthread

kimi_k3_p2p: build/kimi_k3_p2p.c $(ENGINE_P2P_DEPS)
	$(CC) $(P2P_CFLAGS) -DLUMABRI_P2P -DLUMIBRI_P2P $< -o $@ -lm -lpthread

deepseek_p2p: build/deepseek_p2p.c $(ENGINE_P2P_DEPS)
	$(CC) $(P2P_CFLAGS) $(DS_CFLAGS) -DLUMABRI_P2P -DLUMIBRI_P2P $< -o $@ -lm -lpthread

# what a CHATTER needs; `engines` is what a compute DONOR needs
chatters: olmoe_p2p colibri_p2p inkling_p2p kimi_k3_p2p deepseek_p2p

tiny_olmoe/config.json: make_tiny_olmoe.py
	python3 make_tiny_olmoe.py tiny_olmoe

fixture: tiny_olmoe/config.json

test-phase2: phase2 fixture
	./phase2_test.sh

test-phase2-glm: phase2-glm
	./phase2_glm_test.sh

test-phase2-inkling: expert_node_inkling
	./phase2_inkling_test.sh

test-phase2-kimi: expert_node_kimi
	./phase2_kimi_test.sh

# needs a real DeepSeek V4 model: MODEL=<dir> (no synthetic fixture, see the
# script's header for why)
test-phase2-deepseek: expert_node_deepseek
	./phase2_deepseek_test.sh

# Every engine's byte-identity proof, one after the other. DeepSeek V4 has no
# synthetic fixture (see phase2_deepseek_test.sh), so it runs only when a real
# model is there — and says so when it is not, rather than quietly passing on
# four engines while claiming five.
test-engines: test-phase2 test-phase2-glm test-phase2-inkling test-phase2-kimi
	@if [ -f "$${MODEL:-$$HOME/deepseek_v4}/config.json" ]; then \
	    $(MAKE) --no-print-directory test-phase2-deepseek; \
	else \
	    echo; echo "── DeepSeek V4 SKIPPED: no model at $${MODEL:-$$HOME/deepseek_v4}"; \
	    echo "   it has no synthetic fixture — MODEL=<dir> make test-engines"; \
	fi

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
	@for b in expert_node expert_node_glm expert_node_inkling expert_node_kimi \
	         expert_node_deepseek \
	         olmoe_p2p colibri_p2p inkling_p2p kimi_k3_p2p deepseek_p2p; do \
	    [ -f $$b ] && install -m 755 $$b $(DESTDIR)$(PREFIX)/bin/ || true; done
	@echo "installed under $(DESTDIR)$(PREFIX)"

clean:
	rm -f tracker maintainer liblumabri.so test_shim lumabri \
	      olmoe_p2p colibri_p2p inkling_p2p kimi_k3_p2p deepseek_p2p \
	      expert_node expert_node_glm expert_node_inkling expert_node_kimi \
	      expert_node_deepseek
	rm -rf build

.PHONY: all test clean install phase2 phase2-glm engines chatters phase2-all \
        fixture test-phase2 \
        test-phase2-glm test-phase2-inkling test-phase2-kimi \
        test-phase2-deepseek test-engines \
        patches patches-check
