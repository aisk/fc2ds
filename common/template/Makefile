NAME		:= @NAME@
GAME_TITLE	:= @TITLE@
GAME_SUBTITLE	:= fc2ds native port

ROM_FILE	?= ../../roms/@NAME@.nes
ROM_SHA1	:= @SHA1@

GEN_ASSETS	:= build/gen/chr.bin build/gen/prg.bin

# Verification (make verify), filled in once the main loop is known
# (docs/porting-guide.md, phase 4): the PC where one pass of the main loop
# ends, the stack range and the idle loop.
# TICK_PC	:= XXXX
# RAM_NAMES	:= ram.md
# VERIFY_FLAGS	:= --ignore 180-1FF --idle XXXX:YY

include ../../common/nds/game.mk

$(GENDIR)/chr.bin $(GENDIR)/prg.bin &: $(ROM_FILE) Makefile $(TOOLS)/romextract.py $(TOOLS)/nesrom.py
	@echo "  EXTRACT $<"
	$(V)$(PYTHON) $(TOOLS)/romextract.py $< --sha1 $(ROM_SHA1) --chr $(GENDIR)/chr.bin --prg $(GENDIR)/prg.bin
