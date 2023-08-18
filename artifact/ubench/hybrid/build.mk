# rules and definitions common to this file and `Makefile`
include common.mk

# dependencies for the .o files built from .cc files in this folder
-include $(DFILES) 

# explicit build rules for each of the data structure/hybrid algorithm
# combinations (i.e., $(EXEFILES))
include $(ODIR)/rules.mk

# The default target tries to build each combination.  The rules for each task
# are in the temporary file `rules.mk`
.DEFAULT_GOAL = all
.PHONY = all
.PRECIOUS: $(EXEFILES)
all: $(EXEFILES)
