# Data structures that we want to test
DS = rbtree_omap_drop dlist_carumap

# HYBRID libraries to evaluate: algorithm and orec policy
HYBRID_ALG  = lazy wb_c1 wb_c2
HYBRID_OREC = po ps

# Get the default build config
include ../config.mk

# NB: The intention is to build a binary for each combination of $(DS),
# $(HYBRID_ALG), and $(HYBRID_OREC)
EXEFILES = $(foreach a, $(HYBRID_ALG), $(foreach o, $(HYBRID_OREC), $(foreach d, $(DS), $(ODIR)/$d.${a}_$o.exe)))
DFILES   = $(patsubst %.exe, %.d, $(EXEFILES))
