# Data structures that we want to test
DS = ibst_omap

# Path to the xSTM plugin and STM libraries
TM_ROOT = ../../policies/xSTM

# TM libraries to evaluate
include $(TM_ROOT)/libs/tm_names.mk # defines TM_LIB_NAMES

# Get the default build config, and the special config for our LLVM plugin
include ../config.mk
include $(TM_ROOT)/common/xSTM.mk # NB: this updates CXXFLAGS!

CXXFLAGS += -emit-llvm -fno-slp-vectorize -fno-vectorize
LD        = $(CXX)
OPT      = opt-15

# NB: The intention is to build a binary for each combination of $(DS) and
#     $(TM_LIB_NAMES).  We get there by building one .o per $(DS), and linking
#     each of those .o files with each entry in $(TM_LIB_NAMES)
EXEFILES = $(foreach l, $(TM_LIB_NAMES), $(foreach t, $(DS), $(ODIR)/$t.$l.exe))
OFILES   = $(patsubst %, $(ODIR)/%.bc, $(DS))
OPTFILES = $(patsubst %, $(ODIR)/%.opt.bc, $(DS))
DFILES   = $(patsubst %, $(ODIR)/%.d, $(DS))