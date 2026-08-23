# Teeworlds 64 ROM build. Runs inside the toolchain container; use the root
# Makefile from the host.
#
# VARIANT selects what the ROM does:
#
#   game             the playable game (menu, humans, split screen)  -> teeworlds64.z64
#   auto-1p-easy     autoplay DM dm1, 1 player vs 3x hunter13        -> teeworlds64-auto-1p-easy.z64
#   auto-1p-medium   autoplay DM dm1, 1 player vs 3x hunter119       -> teeworlds64-auto-1p-medium.z64
#   auto-1p-hard     autoplay DM dm1, 1 player vs 3x hunter140       -> teeworlds64-auto-1p-hard.z64
#   auto-1p-hard-dm6 autoplay DM dm6 (death-tile hazard map)         -> teeworlds64-auto-1p-hard-dm6.z64
#   auto-2view       autoplay DM dm1, 2-way split screen (halves)    -> teeworlds64-auto-2view.z64
#   auto-3view       autoplay DM dm1, 3-way split + score quadrant   -> teeworlds64-auto-3view.z64
#   auto-4view       autoplay DM dm1, 4-way split screen             -> teeworlds64-auto-4view.z64
#   auto-soak        autoplay DM dm1, short looping matches          -> teeworlds64-auto-soak.z64
#   auto-ctf         autoplay CTF ctf1 2v2, 4 views, hunter147       -> teeworlds64-auto-ctf.z64
#   auto-ctf5        autoplay CTF ctf5 2v2, larger geometry          -> teeworlds64-auto-ctf5.z64
#   auto-tdm         autoplay TDM dm2 2v2, 4 views                   -> teeworlds64-auto-tdm.z64
#   auto-lms         autoplay LMS lms1, round-based survival         -> teeworlds64-auto-lms.z64
#   auto-lts         autoplay LTS dm7 2v2, round-based survival      -> teeworlds64-auto-lts.z64
#   auto-ctf-long    autoplay CTF ctf1, 2 views, 5 min (flag funnel) -> teeworlds64-auto-ctf-long.z64
#   auto-maps        autoplay map-rotation soak, all 16 staged maps  -> teeworlds64-auto-maps.z64
#   s1               deterministic bot-match scenario 1              -> teeworlds64-s1.z64
#   s2               deterministic bot-match scenario 2              -> teeworlds64-s2.z64
#
# The deterministic scenarios are separate ROMs on purpose: botbench runs
# exactly one match per process, and CConsole::Init() keeps function-local
# statics that would bind a second match to the first match's config object.
# The game shell works around the same limitation by reusing one console and
# one CGameContext across matches (OnShutdown()/OnInit(), the server's own map
# reload path).
#
# The whole game family shares one object tree; only tw_variant.c carries the
# autoplay selector, so switching variants is a one-file recompile and a
# relink.

VARIANT ?= game

TW64_SCENARIO =
TW64_AUTOPLAY_MODE = 0

ifeq ($(VARIANT),s1)
  ROM_NAME = teeworlds64-s1
  ROM_TITLE = "Teeworlds 64 Sim1"
  BUILD_DIR = build/s1
  TW64_SCENARIO = 1
else ifeq ($(VARIANT),s2)
  ROM_NAME = teeworlds64-s2
  ROM_TITLE = "Teeworlds 64 Sim2"
  BUILD_DIR = build/s2
  TW64_SCENARIO = 2
else
  BUILD_DIR = build/game
  ROM_TITLE = "Teeworlds 64"
  ifeq ($(VARIANT),game)
    ROM_NAME = teeworlds64
    TW64_AUTOPLAY_MODE = 0
  else ifeq ($(VARIANT),auto-1p-easy)
    ROM_NAME = teeworlds64-auto-1p-easy
    TW64_AUTOPLAY_MODE = 1
  else ifeq ($(VARIANT),auto-1p-medium)
    ROM_NAME = teeworlds64-auto-1p-medium
    TW64_AUTOPLAY_MODE = 2
  else ifeq ($(VARIANT),auto-1p-hard)
    ROM_NAME = teeworlds64-auto-1p-hard
    TW64_AUTOPLAY_MODE = 3
  else ifeq ($(VARIANT),auto-4view)
    ROM_NAME = teeworlds64-auto-4view
    TW64_AUTOPLAY_MODE = 4
  else ifeq ($(VARIANT),auto-soak)
    ROM_NAME = teeworlds64-auto-soak
    TW64_AUTOPLAY_MODE = 5
  else ifeq ($(VARIANT),auto-1p-hard-dm6)
    ROM_NAME = teeworlds64-auto-1p-hard-dm6
    TW64_AUTOPLAY_MODE = 6
  else ifeq ($(VARIANT),auto-2view)
    ROM_NAME = teeworlds64-auto-2view
    TW64_AUTOPLAY_MODE = 7
  else ifeq ($(VARIANT),auto-3view)
    ROM_NAME = teeworlds64-auto-3view
    TW64_AUTOPLAY_MODE = 8
  else ifeq ($(VARIANT),auto-ctf)
    ROM_NAME = teeworlds64-auto-ctf
    TW64_AUTOPLAY_MODE = 9
  else ifeq ($(VARIANT),auto-ctf5)
    ROM_NAME = teeworlds64-auto-ctf5
    TW64_AUTOPLAY_MODE = 10
  else ifeq ($(VARIANT),auto-tdm)
    ROM_NAME = teeworlds64-auto-tdm
    TW64_AUTOPLAY_MODE = 11
  else ifeq ($(VARIANT),auto-lms)
    ROM_NAME = teeworlds64-auto-lms
    TW64_AUTOPLAY_MODE = 12
  else ifeq ($(VARIANT),auto-lts)
    ROM_NAME = teeworlds64-auto-lts
    TW64_AUTOPLAY_MODE = 13
  else ifeq ($(VARIANT),auto-ctf-long)
    ROM_NAME = teeworlds64-auto-ctf-long
    TW64_AUTOPLAY_MODE = 14
  else ifeq ($(VARIANT),auto-maps)
    ROM_NAME = teeworlds64-auto-maps
    TW64_AUTOPLAY_MODE = 15
  else
    $(error unknown VARIANT '$(VARIANT)')
  endif
endif

# This port deliberately uses the pinned preview surface; the gitlink is the
# compatibility boundary, so unlock it without per-call deprecation noise.
LIBDRAGON_PREVIEW = 2
include $(N64_INST)/include/n64.mk

TW_ROOT = build/teeworlds/src
TW_GEN = generated

# The simulation must not be compiled with the -ffast-math that n64.mk puts in
# N64_CFLAGS: the host reference is plain IEEE SSE arithmetic. These flags are
# appended after the defaults so they win. The R4300 has no FMA, so with
# contraction off basic float ops are bit-identical to the host; the remaining
# known divergence source is libm (powf/sinf/cosf/atan2f: newlib vs glibc).
TW_DETERMINISM = -fno-fast-math -ffp-contract=off -fno-unsafe-math-optimizations \
	-fno-associative-math -fno-reciprocal-math -fno-finite-math-only \
	-fsigned-zeros -ftrapping-math

# Shared headers use <generated/protocol.h>, so the staging directory's parent
# goes on the include path, mirroring -Isrc on the host.
TW_INCLUDES = -I$(TW_ROOT) -I. -I$(TW_ROOT)/engine/external/zlib
TW_DEFINES = -DCONF_PLATFORM_N64=1 $(if $(TW64_SCENARIO),-DTW64_SCENARIO=$(TW64_SCENARIO))

# Upstream Teeworlds and the vendored third-party C are not warning clean under
# the libdragon -Wall -Werror profile; silence them rather than patching frozen
# host sources.
# libdragon's dma.c exports io_read()/io_write() as the 32-bit PI bus
# accessors, which collide with the Teeworlds file IO of the same name. Rename
# the Teeworlds side at the preprocessor so both keep working; the two main.cpp
# translation units are the only ones that pull in <dma.h> and they are
# excluded below.
TW_SYMFIX = -Dio_read=tw_io_read -Dio_write=tw_io_write

# Optimisation level for the shared Teeworlds sources. libdragon's default is
# -O2; this hook exists so the level can be A/B'd on the target rather than
# assumed, because the R4300 has a 16 KiB instruction cache and a bigger
# unrolled body can lose. Whatever is set here must keep the s1 simulation ROM
# hash identical -- the determinism flags above are appended after it.
TW_OPT ?=

TW_CXXFLAGS = $(TW_INCLUDES) $(TW_DEFINES) $(TW_SYMFIX) $(TW_OPT) $(TW_DETERMINISM) -w
TW_CFLAGS = $(TW_INCLUDES) $(TW_DEFINES) $(TW_SYMFIX) $(TW_OPT) $(TW_DETERMINISM) -w

# Our own target sources keep warnings on, but cannot be -Werror because they
# include the upstream headers.
TW_SYMFIX_LOCAL = $(TW_SYMFIX)
N64_SRC_CXXFLAGS = $(TW_INCLUDES) $(TW_DEFINES) $(TW_SYMFIX_LOCAL) $(TW_DETERMINISM) -Wno-error
N64_SRC_CFLAGS = $(TW_INCLUDES) $(TW_DEFINES) $(TW_SYMFIX_LOCAL) $(TW_DETERMINISM) -Wno-error

TW_GAME_SHARED = \
	game/collision.cpp \
	game/gamecore.cpp \
	game/layers.cpp

TW_GAME_SERVER = \
	game/server/bot.cpp \
	game/server/entity.cpp \
	game/server/eventhandler.cpp \
	game/server/gamecontext.cpp \
	game/server/gamecontroller.cpp \
	game/server/gameworld.cpp \
	game/server/player.cpp \
	game/server/entities/character.cpp \
	game/server/entities/flag.cpp \
	game/server/entities/laser.cpp \
	game/server/entities/pickup.cpp \
	game/server/entities/projectile.cpp \
	game/server/gamemodes/ctf.cpp \
	game/server/gamemodes/dm.cpp \
	game/server/gamemodes/lms.cpp \
	game/server/gamemodes/lts.cpp \
	game/server/gamemodes/mod.cpp \
	game/server/gamemodes/tdm.cpp

TW_ENGINE_SHARED = \
	engine/shared/compression.cpp \
	engine/shared/config.cpp \
	engine/shared/console.cpp \
	engine/shared/datafile.cpp \
	engine/shared/kernel.cpp \
	engine/shared/linereader.cpp \
	engine/shared/map.cpp \
	engine/shared/memheap.cpp \
	engine/shared/packer.cpp \
	engine/shared/storage.cpp

TW_CPP = $(TW_GAME_SHARED) $(TW_GAME_SERVER) $(TW_ENGINE_SHARED)

TW_C = \
	base/hash.c \
	base/hash_bundled.c \
	base/hash_libtomcrypt.c \
	engine/external/md5/md5.c \
	engine/external/zlib/adler32.c \
	engine/external/zlib/compress.c \
	engine/external/zlib/crc32.c \
	engine/external/zlib/deflate.c \
	engine/external/zlib/infback.c \
	engine/external/zlib/inffast.c \
	engine/external/zlib/inflate.c \
	engine/external/zlib/inftrees.c \
	engine/external/zlib/trees.c \
	engine/external/zlib/uncompr.c \
	engine/external/zlib/zutil.c

# Staged by the host Makefile from the cached botbench build so the ROM uses
# exactly the generated protocol/data tables the host reference was built with.
TW_GEN_CPP = \
	git_revision.cpp \
	protocol.cpp \
	server_data.cpp

ifeq ($(TW64_SCENARIO),)
N64_CPP = \
	src/main_game.cpp \
	src/game/tw_audio.cpp \
	src/game/tw_game.cpp \
	src/game/tw_input.cpp \
	src/game/tw_render.cpp
N64_MAIN_OBJ = $(BUILD_DIR)/n64/src/main_game.o
VARIANT_OBJ = $(BUILD_DIR)/variant/$(VARIANT).o
else
N64_CPP = \
	src/main.cpp \
	src/sim/tw_match.cpp
N64_MAIN_OBJ = $(BUILD_DIR)/n64/src/main.o
VARIANT_OBJ =
endif

N64_C = \
	src/platform/tw_rand.c \
	src/platform/tw_system.c

OBJS = \
	$(TW_CPP:%.cpp=$(BUILD_DIR)/tw/%.o) \
	$(TW_C:%.c=$(BUILD_DIR)/tw/%.o) \
	$(TW_GEN_CPP:%.cpp=$(BUILD_DIR)/gen/%.o) \
	$(N64_CPP:%.cpp=$(BUILD_DIR)/n64/%.o) \
	$(N64_C:%.c=$(BUILD_DIR)/n64/%.o) \
	$(VARIANT_OBJ)

$(BUILD_DIR)/tw/%.o: $(TW_ROOT)/%.cpp rom.mk
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(N64_CXX) -c $(N64_CXXFLAGS) $(TW_CXXFLAGS) -MMD -o $@ $<

$(BUILD_DIR)/tw/%.o: $(TW_ROOT)/%.c rom.mk
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(N64_CC) -c $(N64_CFLAGS) $(TW_CFLAGS) -MMD -o $@ $<

$(BUILD_DIR)/gen/%.o: $(TW_GEN)/%.cpp rom.mk
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(N64_CXX) -c $(N64_CXXFLAGS) $(TW_CXXFLAGS) -MMD -o $@ $<

$(BUILD_DIR)/n64/%.o: %.cpp rom.mk
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(N64_CXX) -c $(N64_CXXFLAGS) $(N64_SRC_CXXFLAGS) -MMD -o $@ $<

$(BUILD_DIR)/n64/%.o: %.c rom.mk
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(N64_CC) -c $(N64_CFLAGS) $(N64_SRC_CFLAGS) -MMD -o $@ $<

# Only tw_variant.c sees TW64_AUTOPLAY_MODE, so the object tree above is shared
# by every game-family ROM and switching variants relinks instead of rebuilds.
$(BUILD_DIR)/variant/%.o: src/game/tw_variant.c rom.mk
	@mkdir -p $(dir $@)
	@echo "    [CC] $< ($*)"
	$(N64_CC) -c $(N64_CFLAGS) $(N64_SRC_CFLAGS) \
		-DTW64_AUTOPLAY_MODE=$(TW64_AUTOPLAY_MODE) -o $@ $<

# main.cpp/main_game.cpp are the only target sources that include
# <libdragon.h>, hence the only ones that must keep libdragon's
# io_read()/io_write().
$(N64_MAIN_OBJ): TW_SYMFIX_LOCAL =

all: $(ROM_NAME).z64

$(BUILD_DIR)/$(ROM_NAME).dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/$(ROM_NAME).elf: $(OBJS)

$(ROM_NAME).z64: N64_ROM_TITLE = $(ROM_TITLE)
$(ROM_NAME).z64: $(BUILD_DIR)/$(ROM_NAME).dfs

clean:
	rm -rf build teeworlds64.z64 teeworlds64-s[12].z64 teeworlds64-auto-*.z64

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

.PHONY: all clean
