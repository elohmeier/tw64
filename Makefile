# Standalone Teeworlds 64 build. Host generators run from the pinned Teeworlds
# submodule; target compilation runs in the pinned libdragon toolchain image.

DOCKER ?= podman
TOOLCHAIN_IMG = tw64-toolchain:local
PROJECT_ROOT := $(abspath .)
TEEWORLDS_DIR ?= $(PROJECT_ROOT)/teeworlds
LIBDRAGON_DIR ?= $(PROJECT_ROOT)/libdragon
PATCHED_TEEWORLDS_DIR ?= $(PROJECT_ROOT)/build/teeworlds
HOST_BUILD ?= $(PROJECT_ROOT)/build/host
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
PYTHON ?= python3
CMAKE ?= cmake
VARIANT ?= game

# The generated protocol/data tables come from the pinned host source so the
# ROM and its deterministic reference executable cannot drift apart.
GENERATED_SRC = $(HOST_BUILD)/src/generated
GENERATED_FILES = git_revision.cpp nethash.cpp protocol.cpp protocol.h server_data.cpp server_data.h

# Every map the game can select, staged into the DFS image. The list must stay
# in sync with s_aMaps[] in n64/src/game/tw_game.cpp: the menu offers exactly
# these names, filtered by mode (ctf* carry flag stands, the rest do not).
ROM_MAPS = dm1 dm2 dm3 dm6 dm7 dm8 dm9 lms1 \
	ctf1 ctf2 ctf3 ctf4 ctf5 ctf6 ctf7 ctf8

# Every ROM flavour rom.mk knows about.
SIM_VARIANTS = s1 s2
AUTOPLAY_VARIANTS = \
	auto-1p-easy auto-1p-medium auto-1p-hard auto-4view auto-soak \
	auto-1p-hard-dm6 auto-2view auto-3view auto-ctf auto-ctf5 \
	auto-ctf-long auto-tdm auto-lms auto-lts auto-maps
GAME_VARIANTS = game $(AUTOPLAY_VARIANTS)

.PHONY: image host prepare generated rom rom-all rom-sim rom-game rom-autoplay stage \
	verify-host verify-rom package ci clean

# Build libdragon from the pinned upstream preview submodule. The Dockerfile's
# base image digest pins the compiler layer independently from this source pin.
image:
	$(DOCKER) build \
		--build-arg LIBDRAGON_COMMIT=$(shell git -C $(LIBDRAGON_DIR) rev-parse HEAD) \
		-f docker/toolchain/Dockerfile -t $(TOOLCHAIN_IMG) $(LIBDRAGON_DIR)

# Configure and build the host workbench. Besides verifying the bot seam, this
# is the authoritative producer of generated protocol and server-data tables.
host:
	cd $(TEEWORLDS_DIR) && $(CMAKE) -S . -B $(HOST_BUILD) \
		-DCMAKE_BUILD_TYPE=Release -DCLIENT=OFF
	$(CMAKE) --build $(HOST_BUILD) --target botbench --parallel $(JOBS)

# Apply the narrow target integration patch to a generated source copy. Never
# modify the Teeworlds submodule: an upstream conflict must fail preparation.
prepare:
	$(PYTHON) scripts/prepare_teeworlds.py \
		--teeworlds $(TEEWORLDS_DIR) \
		--patch patches/teeworlds-n64.patch \
		--output $(PATCHED_TEEWORLDS_DIR)

# Every source the graphics converter reads. `stage` reruns it on any change;
# the converter itself also fingerprints these and exits early when nothing
# moved, so a no-op stage costs one Python start and no container start.
ASSET_SOURCES = \
	$(PROJECT_ROOT)/scripts/convert_n64_assets.py \
	$(TEEWORLDS_DIR)/datasrc/content.py \
	$(TEEWORLDS_DIR)/datasrc/game.png \
	$(TEEWORLDS_DIR)/datasrc/skins/body/standard.png \
	$(TEEWORLDS_DIR)/datasrc/skins/feet/standard.png \
	$(TEEWORLDS_DIR)/datasrc/skins/eyes/standard.png \
	$(TEEWORLDS_DIR)/datasrc/ui/gui_logo.png \
	$(wildcard $(TEEWORLDS_DIR)/datasrc/mapres/*.png) \
	$(foreach m,$(ROM_MAPS),$(TEEWORLDS_DIR)/datasrc/maps/$(m).map)

# Convert the desktop art into libdragon sprites. Outputs are gitignored build
# products; only the converter is committed.
filesystem/gfx/assets.json: $(ASSET_SOURCES)
	$(PYTHON) scripts/convert_n64_assets.py \
		--project-root $(PROJECT_ROOT) --teeworlds-root $(TEEWORLDS_DIR) \
		--docker $(DOCKER) --image $(TOOLCHAIN_IMG)

# The desktop client's own sound effects, as libdragon .wav64 waveforms. Same
# shape as the graphics converter: gitignored outputs, committed converter,
# fingerprint stamp. The converter also cross-checks datasrc/content.py's
# sound order against the staged generated/protocol.h before converting, so a
# shifted SOUND_* enum is a build failure rather than a wrong sample.
AUDIO_SOURCES = \
	$(PROJECT_ROOT)/scripts/convert_n64_audio.py \
	$(TEEWORLDS_DIR)/datasrc/content.py \
	$(wildcard $(TEEWORLDS_DIR)/datasrc/audio/*.wv)

filesystem/sfx/audio.json: generated $(AUDIO_SOURCES)
	$(PYTHON) scripts/convert_n64_audio.py \
		--project-root $(PROJECT_ROOT) --teeworlds-root $(TEEWORLDS_DIR) \
		--docker $(DOCKER) --image $(TOOLCHAIN_IMG)

.PHONY: assets audio
assets: filesystem/gfx/assets.json filesystem/sfx/audio.json
audio: filesystem/sfx/audio.json

# The in-match HUD font. mkfont converts libdragon's own CC0 pixel font
# (monogram, already inside the toolchain image) into the font64 format the RDP
# text engine reads, so text becomes part of the display list instead of a CPU
# pass that has to drain the RDP first. Monochrome and ASCII-only on purpose:
# the whole atlas is 32x22 pixels at 1bpp, which keeps the per-paragraph TMEM
# upload negligible. The HUD is the one text surface with a frame budget, so it
# stays on this font.
TW64_FONT_TTF = /opt/libdragon/examples/fontgallery/assets/monogram.ttf

filesystem/ui/monogram.font64: Makefile
	@mkdir -p filesystem/ui
	$(DOCKER) run --rm -v $(PROJECT_ROOT):/workdir:z -w /workdir \
		$(TOOLCHAIN_IMG) mkfont --monochrome --no-kerning --compress 1 \
		--range 20-7F -o filesystem/ui $(TW64_FONT_TTF)

# The menu/end-screen fonts: the desktop client's own UI typeface. Two sizes,
# both anti-aliased with a one-pixel outline, which is how the desktop UI draws
# text and which is what keeps a label readable over the animated backdrop
# instead of needing an opaque panel behind it. An outlined aliased font is an
# IA8 atlas where the intensity picks between fill and outline colour, so a
# style change is still just a primitive/environment colour.
#
# mkfont names its output after the input file, so each size is produced in a
# scratch directory and renamed; the two sizes share one TTF.
TW64_UI_TTF = teeworlds/datasrc/fonts/DejaVuSans.ttf
TW64_MKFONT_UI = mkfont --no-kerning --compress 1 --range 20-7F \
	--display 320x240 --outline 1

filesystem/ui/menu.font64: Makefile $(TEEWORLDS_DIR)/datasrc/fonts/DejaVuSans.ttf
	@mkdir -p filesystem/ui build/font
	$(DOCKER) run --rm -v $(PROJECT_ROOT):/workdir:z -w /workdir \
		$(TOOLCHAIN_IMG) sh -c '$(TW64_MKFONT_UI) --size 12 \
		-o build/font $(TW64_UI_TTF) && \
		mv build/font/DejaVuSans.font64 filesystem/ui/menu.font64'

filesystem/ui/menu_small.font64: Makefile $(TEEWORLDS_DIR)/datasrc/fonts/DejaVuSans.ttf
	@mkdir -p filesystem/ui build/font
	$(DOCKER) run --rm -v $(PROJECT_ROOT):/workdir:z -w /workdir \
		$(TOOLCHAIN_IMG) sh -c '$(TW64_MKFONT_UI) --size 9 \
		-o build/font $(TW64_UI_TTF) && \
		mv build/font/DejaVuSans.font64 filesystem/ui/menu_small.font64'

TW64_FONTS = filesystem/ui/monogram.font64 filesystem/ui/menu.font64 \
	filesystem/ui/menu_small.font64

.PHONY: font
font: $(TW64_FONTS)

# Stage generated sources and the ROM filesystem payload. Both trees are build
# outputs and are gitignored. Audio depends on this target so its enum check is
# enforced even on a completely fresh checkout.
generated: host
	@mkdir -p generated filesystem/maps
	@for f in $(GENERATED_FILES); do \
		if [ ! -f "$(GENERATED_SRC)/$$f" ]; then \
			echo "missing generated source: $(GENERATED_SRC)/$$f" >&2; \
			echo "build the host botbench first (cmake --build $(BOTBENCH_BUILD) --target botbench)" >&2; \
			exit 1; \
		fi; \
		cmp -s "$(GENERATED_SRC)/$$f" "generated/$$f" || cp -f "$(GENERATED_SRC)/$$f" generated/; \
	done

stage: prepare generated filesystem/gfx/assets.json filesystem/sfx/audio.json $(TW64_FONTS)
	@mkdir -p filesystem/maps
	@for m in $(ROM_MAPS); do \
		cmp -s "$(TEEWORLDS_DIR)/datasrc/maps/$$m.map" "filesystem/maps/$$m.map" \
			|| cp -f "$(TEEWORLDS_DIR)/datasrc/maps/$$m.map" filesystem/maps/; \
	done

# TW_OPT is the shared-source optimisation hook; see rom.mk.
TW_OPT ?=
BUILD_ROM = $(DOCKER) run --rm -v $(PROJECT_ROOT):/workdir:z -w /workdir \
	$(TOOLCHAIN_IMG) make -f rom.mk -j$(JOBS) TW_OPT="$(TW_OPT)"

rom: stage
	$(BUILD_ROM) VARIANT=$(VARIANT)

# The playable ROM only.
rom-game: stage
	$(BUILD_ROM) VARIANT=game

# The unattended capture ROMs. They share one object tree, so this is one full
# build plus three relinks.
rom-autoplay: stage
	@for v in $(AUTOPLAY_VARIANTS); do \
		$(BUILD_ROM) VARIANT=$$v || exit 1; \
	done

# The two deterministic simulation ROMs.
rom-sim: stage
	@for v in $(SIM_VARIANTS); do \
		$(BUILD_ROM) VARIANT=$$v || exit 1; \
	done

rom-all: rom-game rom-autoplay rom-sim

verify-host: host
	$(PYTHON) scripts/verify_host.py $(HOST_BUILD)/botbench

verify-rom: rom-game rom-sim
	$(PYTHON) scripts/verify_roms.py teeworlds64.z64 teeworlds64-s1.z64 teeworlds64-s2.z64

package: verify-rom
	@mkdir -p dist
	cp -f teeworlds64.z64 teeworlds64-s1.z64 teeworlds64-s2.z64 dist/
	cd dist && sha256sum teeworlds64.z64 teeworlds64-s1.z64 teeworlds64-s2.z64 > SHA256SUMS

ci: verify-host package

clean:
	rm -rf build generated dist filesystem/maps filesystem/gfx filesystem/sfx filesystem/ui
	rm -f teeworlds64.z64 teeworlds64-s1.z64 teeworlds64-s2.z64 teeworlds64-auto-*.z64
