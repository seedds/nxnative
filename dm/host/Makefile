# Game profile (dm/games/<id>/): the host is built for it (make GAME=base game title ID, required: there is no default game). game_profile.h is generated from game.tsv into gen/<id>/; the chain version header dmsh_chain.h (one for every game) lives in dm/gfx/nvn
# without GAME only clean passes; anything else (a bare make too, whose default goal is the host) stops and lists the profiles
ifeq ($(strip $(GAME)),)
ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),dmhost)),)
$(error GAME is not set: make GAME=<base game title ID>: $(shell python3 ../games/game.py list | awk -F'\t' '{printf "%s (%s)  ", $$1, $$3}'))
endif
endif
# A profile outside the repository (DM_GAMES_DIR, e.g. the NSP script's provisional profile) is reached through the symlink gen/<id>/profile: make cannot take paths with spaces.
# DM_GAMES_FIRST=1 (the NSP script's --provisional) takes it even when dm/games has the game
ifneq ($(strip $(GAME)),)
GAME_DIR := $(if $(and $(wildcard ../games/$(GAME)/game.tsv),$(if $(DM_GAMES_FIRST),,1)),../games/$(GAME),$(shell python3 ../games/game.py link $(GAME) gen/$(GAME)/profile >&2 && echo gen/$(GAME)/profile))
ifeq ($(GAME_DIR),)
$(error no game profile $(GAME) (neither in dm/games nor in DM_GAMES_DIR))
endif
endif
GAME_H = gen/$(GAME)/game_profile.h
include ../mac/min_os.mk
# -O2 as the other native code here (libvpx, dav1d, the launcher; the iOS host builds at Xcode Release's -Os). Not -O3 (no measured gain over -O2) and not -flto, which moves
# hle_bigstack.c's inline-assembly entry apart from its return path (SIGBUS in dm_bigstack_enter on Breath of the Wild, PR #556)
CFLAGS=-O2 -g -Wall -Wno-unused-function -mmacosx-version-min=$(MACOS_MIN)
# One job per core unless the command line sets -j: the host and the libraries below build one object per source (GNU make 3.81, the system's, takes -j from MAKEFLAGS set here)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif
# Service-layer takeover hle_*.c files are included by wildcard: adding a file does not touch this line (avoids merge conflicts)
SRC=main.c loader.c hle.c crash.c probe.c thread.c save_sync.c memstat.m haptics_apple.m pad_switch2_apple.m $(sort $(wildcard hle_*.c)) front_mac.m web_apple.m entry.S
# NVN null device + nn::vi + the Vulkan layer (dm/gfx/)
# Sources under dm/gfx, dm/gfx/nvn, dm/gfx/tex, dm/gfx/vk and dm/gfx/gl are included by wildcard (tests/ and metal/ do not go into the host): adding a file does not touch this line
GFX=$(sort $(wildcard ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m ../gfx/vk/*.m ../gfx/gl/*.m))
# Third party: libvpx ships the VP8 and VP9 decoder sources (movie decoding, hle_movie.c), with its configuration headers in config/ (arm64, shared by Mac and iOS). Built separately into a static library, file by file, so host changes do not recompile it
VPX=../third_party/libvpx
VPX_SRC=$(sort $(shell find $(VPX) -name '*.c'))
VPX_OBJ=$(patsubst $(VPX)/%.c,gen/vpx/%.o,$(VPX_SRC))
VPX_LIB=gen/vpx/libvpx.a
gen/vpx/%.o: $(VPX)/%.c $(wildcard $(VPX)/config/*.h) ../mac/min_os.mk
	@mkdir -p $(dir $@)
	clang -O2 -g -mmacosx-version-min=$(MACOS_MIN) -I$(VPX) -I$(VPX)/config -c $< -o $@
$(VPX_LIB): $(VPX_OBJ)
	rm -f $@; ar rcs $@ $^
# Third party: dav1d, the AV1 decoder where VideoToolbox has none (hle_movie_av1.c), with its arm64 NEON assembly and its configuration in config/. Its *_tmpl.c files are built
# through the per-bit-depth wrappers in config/tmpl, and the assembly files other assembly files include (*_tmpl.S, util.S) are not built on their own: the list meson builds
DAV1D=../third_party/dav1d
DAV1D_SRC=$(sort $(filter-out %_tmpl.c,$(wildcard $(DAV1D)/src/*.c)) $(DAV1D)/src/arm/cpu.c $(wildcard $(DAV1D)/config/tmpl/*.c) \
          $(filter-out %_tmpl.S %/util.S,$(wildcard $(DAV1D)/src/arm/64/*.S)))
DAV1D_OBJ=$(patsubst $(DAV1D)/%,gen/dav1d/%.o,$(DAV1D_SRC))
DAV1D_LIB=gen/dav1d/libdav1d.a
DAV1D_H=$(wildcard $(DAV1D)/config/*.h $(DAV1D)/src/*.h $(DAV1D)/src/*_tmpl.c $(DAV1D)/src/arm/*.h $(DAV1D)/src/arm/asm.S $(DAV1D)/src/arm/64/*_tmpl.S $(DAV1D)/src/arm/64/util.S $(DAV1D)/include/*/*.h)
gen/dav1d/%.o: $(DAV1D)/% $(DAV1D_H) ../mac/min_os.mk
	@mkdir -p $(dir $@)
	clang -O2 -g -mmacosx-version-min=$(MACOS_MIN) -fvisibility=hidden -fomit-frame-pointer -fno-align-functions -I$(DAV1D) -I$(DAV1D)/include -I$(DAV1D)/config -c $< -o $@
$(DAV1D_LIB): $(DAV1D_OBJ)
	rm -f $@; ar rcs $@ $^
# The host's own Metal shaders (dm/gfx/msl/*.metal) compiled for MSL_SDK into gen/msl-<sdk>/host_msl_gen.h (dm/gfx/tools/host_msl.py; dm/gfx/nvn/host_msl.m loads them), so the host
# compiles no MSL at run time. The iOS build (dm/ios/tools/gen_build.py) makes it with MSL_SDK=iphoneos, for the app's deployment target (MSL_MIN_iphoneos, as project.yml)
MSL_SDK ?= macosx
MSL_MIN_macosx = $(MACOS_MIN)
MSL_MIN_iphoneos = 26.0
MSL_SRC = $(sort $(wildcard ../gfx/msl/*.metal))
MSL_H = gen/msl-$(MSL_SDK)/host_msl_gen.h
# Recheck the toolchain and target on every build; the generator keeps the header unchanged when its inputs match.
$(MSL_H): $(MSL_SRC) ../gfx/tools/host_msl.py FORCE
	python3 ../gfx/tools/host_msl.py --sdk $(MSL_SDK) $(if $(MSL_MIN_$(MSL_SDK)),--min $(MSL_MIN_$(MSL_SDK))) $@ $(MSL_SRC)
.PHONY: host_msl
host_msl: $(MSL_H)
.DEFAULT_GOAL := dmhost   # The header generation rules are written first below, so the default goal must be stated explicitly (otherwise make only generates the headers and does not build the host)
.PHONY: game_profile
game_profile: $(GAME_H)
# Regenerated on every make (FORCE): the same GAME can come from another profile folder (DM_GAMES_FIRST), whose game.tsv may be older than the header; game.py writes it only when it changes
$(GAME_H): $(GAME_DIR)/game.tsv ../games/game.py FORCE
	@python3 ../games/game.py header $(GAME) $@
.PHONY: FORCE
FORCE:
# The host: one object per source in gen/<id>/obj (dm.h includes the profile header, so every game has its own), compiled on every core, then linked into gen/<id>/dmhost with
# the same flags (as clang's one-step compile and link gives each of its jobs the whole line) and its dSYM made from the objects, as clang does after its own link. Each object
# lists the headers its source read (-MMD; -MP: a removed header is no error), so an edit rebuilds the objects that include it and the link; the Makefile (CFLAGS, these lines)
# rebuilds them all. The generated headers come first (order-only: the dependency files name them for the sources that read them). The NSP script builds gen/<id>/dmhost and takes it
HOST_CFLAGS = $(CFLAGS) -I../gfx/nvn/include -I$(dir $(MSL_H)) -I../third_party/vulkan-headers/include -I$(dir $(GAME_H)) -I$(VPX) -I$(DAV1D)/include -I../third_party/stb_vorbis -fobjc-arc
HOST_FRAMEWORKS = -framework Cocoa -framework WebKit -framework UserNotifications -framework QuartzCore -framework Metal -framework MetalFX -framework GameController -framework CoreBluetooth -framework IOBluetooth -framework CoreHaptics -framework IOKit -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework ImageIO -framework UniformTypeIdentifiers
HOST_SRC = $(SRC) $(GFX)
ifneq ($(filter ../%,$(filter-out ../gfx/%,$(HOST_SRC))),)
$(error host sources outside dm/host and dm/gfx need an object rule: $(filter ../%,$(filter-out ../gfx/%,$(HOST_SRC))))
endif
OBJ_DIR = gen/$(GAME)/obj
HOST_OBJ = $(foreach s,$(HOST_SRC),$(if $(filter ../gfx/%,$(s)),$(OBJ_DIR)/gfx/$(s:../gfx/%=%).o,$(OBJ_DIR)/host/$(s).o))   # the line's order: the link lays code out in it
HOST_BIN = gen/$(GAME)/dmhost
$(OBJ_DIR)/host/%.o: % Makefile ../mac/min_os.mk | $(GAME_H) $(MSL_H)
	@mkdir -p $(dir $@)
	clang $(HOST_CFLAGS) -MMD -MP -c $< -o $@
$(OBJ_DIR)/gfx/%.o: ../gfx/% Makefile ../mac/min_os.mk | $(GAME_H) $(MSL_H)
	@mkdir -p $(dir $@)
	clang $(HOST_CFLAGS) -MMD -MP -c $< -o $@
$(HOST_BIN): $(HOST_OBJ) $(VPX_LIB) $(DAV1D_LIB) Makefile ../mac/min_os.mk
	clang $(HOST_CFLAGS) -o $@ $(HOST_OBJ) $(VPX_LIB) $(DAV1D_LIB) $(HOST_FRAMEWORKS)
	dsymutil $@
ifneq ($(strip $(GAME)),)
-include $(HOST_OBJ:.o=.d)
endif
# dm/host/dmhost (what the run commands, regress.py and CI start) is a copy of this game's gen/<id>/dmhost with its dSYM, made again whenever the two differ: another game's or
# an older build's (make's whole-second times could not tell two games' builds in one second apart)
dmhost: $(HOST_BIN) FORCE
	@cmp -s $(HOST_BIN) $@ || { rm -rf $@ $@.dSYM && cp $(HOST_BIN) $@ && cp -R $(HOST_BIN).dSYM $@.dSYM; }
clean:   # the host and everything make generates for it (gen/: the profile headers, each game's objects and host, and the libvpx and dav1d builds; dmhost_* and *.dSYM beside them, as in .gitignore)
	rm -rf gen dmhost dmhost_* *.dSYM
.PHONY: clean
