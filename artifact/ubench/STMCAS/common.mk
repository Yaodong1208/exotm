# Data structures for the STMCAS work
DS = dlist_omap                     dlist_opt_omap                   \
     dlist_caumap                   dlist_opt_caumap                 \
     slist_omap                                                      \
     slist_opt_caumap                                                \
     dlist_carumap                                                   \
     ibst_omap                                                       \
     rbtree_omap                                                     \
     skiplist_cached_opt_omap         
                                    

# STMCAS libraries to evaluate: algorithm and orec policy
STMCAS_ALG = stmcas
STMCAS_OREC = po ps

# Get the default build config
include ../config.mk

# NB: The intention is to build a binary for each combination of $(DS),
# $(STMCAS_ALG), and $(STMCAS_OREC).
EXEFILES = $(foreach a, $(STMCAS_ALG), $(foreach o, $(STMCAS_OREC), $(foreach d, $(DS), $(ODIR)/$d.${a}_$o.exe)))
DFILES   = $(patsubst %.exe, %.d, $(EXEFILES))
