# Shared NDS build rules for all games.
#
# A game Makefile sets NAME, GAME_TITLE, GAME_SUBTITLE and optionally
# DEFINES / GEN_ASSETS (files generated from the ROM), then includes this file.
# Game sources live in src/, shared runtime sources in common/nds/src/ and are
# compiled into every game directly, so per-game defines (FIX_xxx) apply to both.

BLOCKSDS		?= /opt/blocksds/core
WONDERFUL_TOOLCHAIN	?= /opt/wonderful
ARM_NONE_EABI_PATH	?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

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

GAME_SOURCES	:= $(shell find src -name "*.c" -o -name "*.s")
COMMON_SOURCES	:= $(shell find $(COMMON_NDS)/src -name "*.c" -o -name "*.s")

OBJS		:= $(addprefix $(OBJDIR)/game/,$(addsuffix .o,$(GAME_SOURCES:src/%=%))) \
		   $(addprefix $(OBJDIR)/common/,$(addsuffix .o,$(COMMON_SOURCES:$(COMMON_NDS)/src/%=%)))
DEPS		:= $(OBJS:.o=.d)

# Flags
# -----

DEFINES		+= -D__NDS__ -D__BLOCKSDS__ -DARM9
ARCH		:= -mthumb -mcpu=arm946e-s+nofp
SPECS		:= $(BLOCKSDS)/sys/crts/ds_arm9.specs
INCLUDES	:= -Isrc -I$(COMMON_NDS)/include -I$(GENDIR) -I$(BLOCKSDS)/libs/libnds/include

CFLAGS		:= -Wall -Wextra -std=gnu17 -O2 -ffunction-sections -fdata-sections \
		   $(ARCH) $(INCLUDES) $(DEFINES) -specs=$(SPECS) $(CFLAGS)
ASFLAGS		:= -x assembler-with-cpp $(ARCH) $(INCLUDES) $(DEFINES) -specs=$(SPECS)
LDFLAGS		:= $(ARCH) -L$(BLOCKSDS)/libs/libnds/lib -Wl,-Map,$(MAP) -Wl,--gc-sections \
		   -Wl,--start-group -lnds9 -lc -Wl,--end-group -specs=$(SPECS)

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

$(OBJDIR)/common/%.c.o: $(COMMON_NDS)/src/%.c
	@echo "  CC      common/nds/src/$*.c"
	@mkdir -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/common/%.s.o: $(COMMON_NDS)/src/%.s
	@echo "  AS      common/nds/src/$*.s"
	@mkdir -p $(@D)
	$(V)$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

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
