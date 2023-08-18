# rules and definitions common to this file and `Makefile`
include common.mk 

# dependencies for the .o files built from .cc files in this folder
-include $(DFILES) 

# explicit link rules for each of the data structure/xSTM algorithm
# combinations (i.e., $(EXEFILES))
include $(ODIR)/rules.mk

# The default target tries to link each combination.  The rules for each link
# task are in the temporary file `rules.mk`.
.DEFAULT_GOAL = all
.PHONY: all
.PRECIOUS: $(OFILES) $(EXEFILES)
all: $(EXEFILES)

# The link steps each require a .o file built from a .cc in this folder.  This
# rule builds them
$(ODIR)/%.bc: %.cc
	@echo "[CXX] $< --> $@"
	@$(CXX) $< -o $@ -c $(CXXFLAGS)

$(ODIR)/%.opt.bc: $(ODIR)/%.bc
	@echo "[OPT] $< --> $@"
	@$(OPT) $(OPTFLAGS) $< -o $@