# We have a collection of data structures and a collection of STMCAS algorithms.
# For STMCAS, the way to get a data structure to use an STMCAS algorithm is to
# instantiate the data structure with that STMCAS's descriptor type.  The only
# way to do this without lots of extra .cc files is to pass include files and
# `-D` flags at compile time.
#
# Our solution is to build in two steps.  The first step (this file) creates a
# temporary Makefile (rules.mk) in the output directory.  It then recursively
# calls `make` using the `build.mk` Makefile.  That, in turn, includes rules.mk.
#
# The net effect is that we have a nasty Makefile that has an explicit build
# rule for each of the <data structure, STMCAS algorithm> combinations (no
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

# Build the rules Makefile as a whole bunch of simple build rules
$(ODIR)/rules.mk:
	$(shell \
		for d in $(DS); \
		do \
			for a in $(STMCAS_ALG); \
			do \
				for o in $(STMCAS_OREC); \
				do \
					echo $(ODIR)/$$d."$$a"_$$o.exe: $$d.cc ../../policies/STMCAS/$$a.h >> $@; \
					echo '	@echo "[CXX]"' "$$d".cc '--\> $$@'  >> $@; \
					echo '	@$$(CXX) $$< -o $$@ $$(CXXFLAGS)' -DSTMCAS_ALG="$$a"_t -DSTMCAS_OREC=orec_"$$o"_t -include ../../policies/STMCAS/$$a.h '$$(LDFLAGS)' >> $@; \
				done; \
			done; \
        done)