# Executables to build.  We assume each .exe is built from just one .cc file.
TARGETS = ebst_ticket_omap lfskiplist_omap lazylist_omap	\
          lazylist_caumap 									\
		  ibst_pathcas_omap iavl_pathcas_omap

# Get the default build config
include ../config.mk

# Names of all .exe files, .o files, and .d files
EXEFILES  = $(patsubst %, $(ODIR)/%.exe, $(TARGETS))
OFILES    = $(patsubst %, $(ODIR)/%.o, $(TARGETS))
DFILES    = $(patsubst %, $(ODIR)/%.d, $(TARGETS))

# dependencies for the .o files built from .cc files in this folder
-include $(DFILES)

# The default target builds all executables in a two step (compile, link)
# process
.DEFAULT_GOAL = all
.PHONY: all clean
.PRECIOUS: $(OFILES) $(EXEFILES)
all: $(EXEFILES)

# Build a .exe file from a .cc file
$(ODIR)/%.exe: %.cc
	@echo "[CXX] $< --> $@"
	@$(CXX) $< -o $@ $(CXXFLAGS) $(LDFLAGS)

# Link a .o file into a .exe
$(ODIR)/%.exe: $(ODIR)/%.o
	@echo "[LD] $^ --> $@"
	@$(CXX) $^ -o $@ $(LDFLAGS)

# clean by clobbering the build folder
clean:
	@echo Cleaning up...
	@rm -rf $(ODIR)
