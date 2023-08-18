# We have a collection of data structures and a collection of xSTM algorithms.
# For xSTM, the way to get a data structure to use a xSTM algorithm is to link
# the .o of the data structure with the .o of the xSTM algorithm.  This would
# be easy if `make` supported multiple wildcards, but it doesn't
#
# Our solution is to build in two steps.  The first step (this file) creates a
# temporary Makefile (rules.mk) in the output directory.  It then recursively
# calls `make` using the `build.mk` Makefile.  That, in turn, includes rules.mk.
#
# The net effect is that we have a nasty Makefile that has an explicit build
# rule for each of the <data structure, xSTM algorithm> combinations (no
# wildcards!), but we don't have to maintain it.

# rules and definitions common to this file and `build.mk`
include common.mk

# The default target just recurses, but it ensures the temporary Makefile is
# built first.
.DEFAULT_GOAL = all
.PHONY: all clean
all: $(ODIR)/rules.mk
	$(MAKE) -f build.mk

# clean by clobbering the build folder
clean:
	@echo Cleaning up...
	@rm -rf $(ODIR)

# Build the rules Makefile as a whole bunch of simple link rules
$(ODIR)/rules.mk:
	$(shell \
		for d in $(DS); \
		do \
			for t in $(TM_LIB_NAMES); \
			do \
				echo $(ODIR)/$$d.$$t.exe: $(ODIR)/$$d.opt.bc $(TM_ROOT)/libs/$(ODIR)/$$t.bc >> $@; \
				echo '	@echo "[LD]" $$@' >> $@; \
				echo '	@$$(LD) $$^ -o $$@ $$(LDFLAGS)' >> $@; \
			done; \
        done)