# Shared NDS build rules for all games.
#
# A game Makefile sets NAME, GAME_TITLE, GAME_SUBTITLE and optionally
# DEFINES / GEN_ASSETS (files generated from the ROM), then includes this file.
# Game sources live in src/, shared runtime sources in common/nds/src/ and are
# compiled into every game directly, so per-game defines (FIX_xxx) apply to both.

BLOCKSDS		?= /opt/blocksds/core
WONDERFUL_TOOLCHAIN	?= /opt/wonderful
ARM_NONE_EABI_PATH	?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

.DEFAULT_GOAL	:= all

FC2DS_ROOT	:= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
COMMON_NDS	:= $(FC2DS_ROOT)/common/nds
TOOLS		:= $(FC2DS_ROOT)/common/tools
ROMDIR		:= $(FC2DS_ROOT)/roms
PYTHON		?= python3

GAME_SUBTITLE	?=
GAME_AUTHOR	?= fc2ds
GAME_ICON	?= $(BLOCKSDS)/sys/icon.gif

BUILDDIR	:= build
GENDIR		:= $(BUILDDIR)/gen
OBJDIR		:= $(BUILDDIR)/obj
ELF		:= $(BUILDDIR)/$(NAME).elf
MAP		:= $(BUILDDIR)/$(NAME).map
ROM		:= $(BUILDDIR)/$(NAME).nds

ARM7ELF		?= $(BLOCKSDS)/sys/arm7/main_core/arm7_maxmod.elf

PREFIX		:= $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC		:= $(PREFIX)gcc
LD		:= $(PREFIX)gcc
NDSTOOL		:= $(BLOCKSDS)/tools/ndstool/ndstool

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

# Sources
# -------

# A game with ported code in port/ also gets common/vnes (the virtual NES
# the port runs on) and port/ram.h, generated into $(HOSTDIR) (see below).
VNES		:= $(FC2DS_ROOT)/common/vnes
HOSTDIR		:= $(BUILDDIR)/host
GAME_SOURCES	:= $(shell find src -name "*.c" -o -name "*.s")
COMMON_SOURCES	:= $(shell find $(COMMON_NDS)/src -name "*.c" -o -name "*.s")
PORT_SOURCES	:= $(wildcard port/*.c)
VNES_SOURCES	:= $(if $(PORT_SOURCES),$(VNES)/vnes.c $(VNES)/vraster.c $(VNES)/vapu.c)

OBJS		:= $(addprefix $(OBJDIR)/game/,$(addsuffix .o,$(GAME_SOURCES:src/%=%))) \
		   $(addprefix $(OBJDIR)/common/,$(addsuffix .o,$(COMMON_SOURCES:$(COMMON_NDS)/src/%=%))) \
		   $(addprefix $(OBJDIR)/port/,$(addsuffix .o,$(PORT_SOURCES:port/%=%))) \
		   $(addprefix $(OBJDIR)/vnes/,$(addsuffix .o,$(VNES_SOURCES:$(VNES)/%=%)))
DEPS		:= $(OBJS:.o=.d)

# Flags
# -----

DEFINES		+= -D__NDS__ -D__BLOCKSDS__ -DARM9
ARCH		:= -mthumb -mcpu=arm946e-s+nofp
SPECS		:= $(BLOCKSDS)/sys/crts/ds_arm9.specs
INCLUDES	:= -Isrc -I$(COMMON_NDS)/include -I$(GENDIR) -I$(VNES) -Iport -I$(HOSTDIR) \
		   -I$(BLOCKSDS)/libs/libnds/include -I$(BLOCKSDS)/libs/maxmod/include

CFLAGS		:= -Wall -Wextra -std=gnu17 -O2 -ffunction-sections -fdata-sections \
		   $(ARCH) $(INCLUDES) $(DEFINES) -specs=$(SPECS) $(CFLAGS)
ASFLAGS		:= -x assembler-with-cpp $(ARCH) $(INCLUDES) $(DEFINES) -specs=$(SPECS)
LDFLAGS		:= $(ARCH) -L$(BLOCKSDS)/libs/libnds/lib -L$(BLOCKSDS)/libs/maxmod/lib -Wl,-Map,$(MAP) -Wl,--gc-sections \
		   -Wl,--start-group -lmm9 -lnds9 -lc -Wl,--end-group -specs=$(SPECS)

# Text and fonts
# --------------
# A game with translations sets TEXT_LANGS (e.g. ja en zh) and TEXT_BASE, the
# YAML dumped from the ROM that gives positions and fallback text. Each
# language becomes $(GENDIR)/text_<lang>.bin; Japanese gets font_ja.bin, the
# other languages share font_zh.bin. Fonts come from common/fonts/fetch.sh.
# TEXT_EXTRA lists more characters font_zh.bin needs (UI strings such as the
# subtitle language names).

ifneq ($(strip $(TEXT_LANGS)),)

FONTDIR		:= $(FC2DS_ROOT)/common/fonts/fusion-pixel
FONT_BDF_ja	:= $(FONTDIR)/fusion-pixel-12px-proportional-ja.bdf
FONT_BDF_zh	:= $(FONTDIR)/fusion-pixel-12px-proportional-zh_hans.bdf
TEXT_OTHER	:= $(filter-out ja,$(TEXT_LANGS))
TEXT_ASSETS	:= $(foreach l,$(TEXT_LANGS),$(GENDIR)/text_$(l).bin) \
		   $(GENDIR)/font_ja.bin $(if $(TEXT_OTHER),$(GENDIR)/font_zh.bin)
GEN_ASSETS	+= $(TEXT_ASSETS)

$(GENDIR)/text_ja.bin: $(TEXT_BASE) $(TOOLS)/mktext.py
	@echo "  MKTEXT  $@"
	$(V)$(PYTHON) $(TOOLS)/mktext.py $< $@

$(GENDIR)/text_%.bin: text/%.yaml $(TEXT_BASE) $(TOOLS)/mktext.py
	@echo "  MKTEXT  $@"
	$(V)$(PYTHON) $(TOOLS)/mktext.py $< $@ --base $(TEXT_BASE)

$(GENDIR)/font_ja.bin: $(FONT_BDF_ja) $(GENDIR)/text_ja.bin $(TOOLS)/mkfont.py
	@echo "  MKFONT  $@"
	$(V)$(PYTHON) $(TOOLS)/mkfont.py $< $@ $(GENDIR)/text_ja.bin.chars >/dev/null

$(GENDIR)/text_extra.chars: Makefile
	$(V)mkdir -p $(@D)
	$(V)printf '%s' '$(TEXT_EXTRA)' > $@

$(GENDIR)/font_zh.bin: $(FONT_BDF_zh) $(foreach l,$(TEXT_OTHER),$(GENDIR)/text_$(l).bin) $(GENDIR)/text_extra.chars \
		$(TOOLS)/mkfont.py
	@echo "  MKFONT  $@"
	$(V)$(PYTHON) $(TOOLS)/mkfont.py $< $@ $(foreach l,$(TEXT_OTHER),$(GENDIR)/text_$(l).bin.chars) \
		$(GENDIR)/text_extra.chars >/dev/null

$(FONTDIR)/%.bdf:
	@echo "  $@ is missing, run common/fonts/fetch.sh"
	@exit 1

endif

# Targets
# -------

.PHONY: all clean assets

all: $(ROM)

assets: $(GEN_ASSETS)

ifeq ($(strip $(GAME_SUBTITLE)),)
    GAME_FULL_TITLE := $(GAME_TITLE);$(GAME_AUTHOR)
else
    GAME_FULL_TITLE := $(GAME_TITLE);$(GAME_SUBTITLE);$(GAME_AUTHOR)
endif

$(ROM): $(ELF)
	@echo "  NDSTOOL $@"
	$(V)$(NDSTOOL) -c $@ -7 $(ARM7ELF) -9 $(ELF) -b $(GAME_ICON) "$(GAME_FULL_TITLE)"

$(ELF): $(OBJS)
	@echo "  LD      $@"
	$(V)$(LD) -o $@ $(OBJS) $(LDFLAGS)

clean:
	@echo "  CLEAN"
	$(V)rm -rf $(BUILDDIR)

# Generated assets must exist before anything is compiled.
$(OBJS): | $(GEN_ASSETS)

$(OBJDIR)/game/%.c.o: src/%.c
	@echo "  CC      $<"
	@mkdir -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/game/%.s.o: src/%.s
	@echo "  AS      $<"
	@mkdir -p $(@D)
	$(V)$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/port/%.c.o: port/%.c $(HOSTDIR)/ram.h
	@echo "  CC      $<"
	@mkdir -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/vnes/%.c.o: $(VNES)/%.c
	@echo "  CC      common/vnes/$*.c"
	@mkdir -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/common/%.c.o: $(COMMON_NDS)/src/%.c
	@echo "  CC      common/nds/src/$*.c"
	@mkdir -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/common/%.s.o: $(COMMON_NDS)/src/%.s
	@echo "  AS      common/nds/src/$*.s"
	@mkdir -p $(@D)
	$(V)$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

# Verification
# ------------
# `make verify` builds the reference runner (common/tools/nesref) on the host
# with the ported code in port/ hooked in (PORT_HOOK blocks under NESREF in
# each port file), and runs every input script in
# verify/: the original and the port run side by side and their RAM must
# match after every tick. Ported routines whose original code no script
# reaches are listed, since they are not verified yet. The game Makefile sets TICK_PC (where a tick ends),
# RAM_NAMES (Markdown files naming RAM, for port/ram.h) and VERIFY_FLAGS
# (e.g. --ignore for the stack), and optionally VERIFY_DEPS / VERIFY_GEN_SCRIPTS
# for generated input scripts.

HOST_CC		?= gcc
NESREF		:= $(TOOLS)/nesref
VERIFY_BIN	:= $(HOSTDIR)/verify

.PHONY: verify

$(HOSTDIR)/ram.h: $(RAM_NAMES) $(TOOLS)/mkramh.py $(TOOLS)/trace6502.py
	@echo "  RAMH    $@"
	$(V)$(PYTHON) $(TOOLS)/mkramh.py $@ $(RAM_NAMES)

$(VERIFY_BIN): $(PORT_SOURCES) $(wildcard port/*.h) $(HOSTDIR)/ram.h $(wildcard $(NESREF)/*.[ch]) $(wildcard $(VNES)/*.[ch])
	@echo "  HOSTCC  $@"
	$(V)$(HOST_CC) -O2 -g -Wall -Wextra -DNESREF -Iport -I$(HOSTDIR) -I$(NESREF) -I$(VNES) -I$(COMMON_NDS)/include -o $@ \
		$(NESREF)/nesref.c $(NESREF)/cpu6502.c $(VNES)/vnes.c $(VNES)/vraster.c $(VNES)/vrender.c $(VNES)/vapu.c \
		$(COMMON_NDS)/src/ndsframe.c $(COMMON_NDS)/src/nes_chr.c $(PORT_SOURCES)

# --native runs the port as it runs on the NDS (no 6502 at all); with
# VERIFY_MODE=--compare the port machine runs the original code with the
# ported routines hooked in, for games that are only partly ported.
VERIFY_MODE ?= --native

verify: $(VERIFY_BIN) $(ROM_FILE) $(VERIFY_DEPS)
	$(V)rm -f $(HOSTDIR)/coverage.txt
	$(V)for f in verify/*.txt $(VERIFY_GEN_SCRIPTS); do \
		echo "  VERIFY  $$f"; \
		$(VERIFY_BIN) $(ROM_FILE) --tick-pc $(TICK_PC) $(VERIFY_MODE) --speed 32 --input $$f \
			--coverage $(HOSTDIR)/coverage.txt $(VERIFY_FLAGS) || exit 1; \
	done
	$(V)awk '{ n[$$1] += $$2 } END { for (k in n) { t++; if (!n[k]) { u++; printf "  not reached: %s\n", k } } \
		printf "  %d ported routines, %d reached by the scripts\n", t, t - u }' $(HOSTDIR)/coverage.txt

# Disassembly
# -----------
# `make disasm` traces the code (disasm/layout.toml), writes da65 info files and
# listings to build/disasm/, then reassembles every listing with ca65/ld65 and
# checks that it matches the ROM byte for byte.

DISASMDIR	:= $(BUILDDIR)/disasm

.PHONY: disasm

disasm: $(ROM_FILE) disasm/layout.toml $(TOOLS)/trace6502.py
	@echo "  TRACE   disasm/layout.toml"
	$(V)$(PYTHON) $(TOOLS)/trace6502.py disasm/layout.toml $(ROM_FILE) $(DISASMDIR)
	$(V)cd $(DISASMDIR) && for info in *.info; do \
		seg=$${info%.info}; \
		base=$$(sed -n 's/.*STARTADDR \$$\([0-9A-F]*\);.*/\1/p' $$info); \
		echo "  DA65    $$seg.s"; \
		da65 -i $$info || exit 1; \
		ca65 -o $$seg.o $$seg.s && ld65 -t none --start-addr 0x$$base -o $$seg.out $$seg.o || exit 1; \
		cmp -s $$seg.out $$seg.bin || { echo "  $$seg.s does not reassemble to the original bytes"; exit 1; }; \
	done
	@echo "  OK      all listings reassemble to the original ROM"

-include $(DEPS)
