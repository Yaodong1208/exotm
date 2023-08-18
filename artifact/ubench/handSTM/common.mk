# Data structures that we want to test
DS = slist_omap skiplist_omap_bigtx       \
     ibst_omap	rbtree_omap dlist_caumap dlist_carumap

# handSTM libraries to evaluate: algorithm and orec policy
HANDSTM_ALG  = eager_c1 eager_c2 lazy wb_c1 wb_c2
HANDSTM_OREC = po ps

# Get the default build config
include ../config.mk

# NB: The intention is to build a binary for each combination of $(DS),
#     $(HANDSTM_ALG), and $(HANDSTM_OREC)
EXEFILES = $(foreach a, $(HANDSTM_ALG), $(foreach o, $(HANDSTM_OREC), $(foreach d, $(DS), $(ODIR)/$d.${a}_$o.exe)))
DFILES   = $(patsubst %.exe, %.d, $(EXEFILES))
