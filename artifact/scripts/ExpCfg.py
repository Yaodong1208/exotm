#
# This file defines the executables for each data structure, the configuration
# of each data structure, and the rules for how to run experiments.
#

import Types

DsCfg = Types.DsCfg
ExeCfg = Types.ExeCfg
ExpCfg = Types.ExpCfg

# Common configuration rules for a data structure (bucket size, chunk size,
# resize threshold, snapshot frequency, max levels, and name)
dsRules = {"list_default": DsCfg(4, 8, 8, 3, 32, "list_default"),
           "list_nosnap": DsCfg(4, 8, 8, 65536, 32, "list_nosnap"),
           "skiplist_default": DsCfg(4, 8, 8, 33, 32, "skiplist_default"),
           "umap_default": DsCfg(262144, 8, 65536, 33, 32, "umap_default"),
           "bst_default": DsCfg(4, 8, 8, 33, 32, "tree_default"),
           "skipvector_default":DsCfg(4,8,8,32,4,"skipvector_default"),
           "rbt_default": DsCfg(4, 8, 8, 33, 32, "rbtree_default")
           }

# Paths to executables, and their printable names
exeNames = {
    # Baseline
    "base_lazylist": ExeCfg("baseline/obj64/lazylist_omap.exe", "base_lazylist"),
    "base_ebst": ExeCfg("baseline/obj64/ebst_ticket_omap.exe", "base_ebst"),
    "base_caumap": ExeCfg("baseline/obj64/lazylist_caumap.exe", "base_caumap"),
    "base_skiplist": ExeCfg("baseline/obj64/lfskiplist_omap.exe", "base_skiplist"),
    "base_ibst": ExeCfg("baseline/obj64/ibst_pathcas_omap.exe", "base_ibst"),
    "base_ibbst": ExeCfg("baseline/obj64/iavl_pathcas_omap.exe", "base_ibbst"),

    # xSTM (NB: there are many more that we don't currently test)
    "xstm_ibst": ExeCfg("xSTM/obj64/ibst_omap.exo_eager_c1_q.exe", "xstm_ibst_ee1q"),
    "xstm_ibst_tiny": ExeCfg("xSTM/obj64/ibst_omap.orec_gv1_eager_c2_q.exe", "xstm_ibst_o1e2"),

    # handSTM (NB: there are many more that we don't currently test)
    "handstm_ibst": ExeCfg("handSTM/obj64/ibst_omap.eager_c1_po.exe", "handstm_ibst_ee1o"),
    "handstm_slist": ExeCfg("handSTM/obj64/slist_omap.eager_c1_po.exe", "handstm_slist_ee1o"),
    "handstm_caumap": ExeCfg("handSTM/obj64/dlist_caumap.eager_c1_po.exe", "handstm_dcaumap_ee1o"),
    "handstm_carumap": ExeCfg("handSTM/obj64/dlist_carumap.eager_c1_po.exe", "handstm_dcarumap_ee1o"),
    "handstm_skiplist": ExeCfg("handSTM/obj64/skiplist_omap_bigtx.eager_c1_po.exe", "handstm_skiplist_bigtx_ee1o"),
    "handstm_irbtree": ExeCfg("handSTM/obj64/rbtree_omap.eager_c1_po.exe", "handstm_rbtree_ee1o"),

    # Hybrid
    "hybrid_irbtree": ExeCfg("hybrid/obj64/rbtree_omap_drop.lazy_po.exe", "hybrid_rbtree_lzpo"),
    "hybrid_carumap": ExeCfg("hybrid/obj64/dlist_carumap.lazy_po.exe", "hybrid_carumap_lzpo"),

    # STMCAS (NB: there are many more that we don't currently test)
    "stmcas_ibst": ExeCfg("STMCAS/obj64/ibst_omap.stmcas_po.exe", "stmcas_ibst"),
    "stmcas_ibst_ps": ExeCfg("STMCAS/obj64/ibst_omap.stmcas_ps.exe", "stmcas_ibst_ps"),
    "stmcas_dlist": ExeCfg("STMCAS/obj64/dlist_opt_omap.stmcas_po.exe", "stmcas_dlist"),
    "stmcas_dlist_noopt": ExeCfg("STMCAS/obj64/dlist_omap.stmcas_po.exe", "stmcas_dlist_noopt"),
    "stmcas_slist": ExeCfg("STMCAS/obj64/slist_omap.stmcas_po.exe", "stmcas_slist"),
    "stmcas_slist_ps": ExeCfg("STMCAS/obj64/slist_omap.stmcas_ps.exe", "stmcas_slist_ps"),
    "stmcas_slist_noopt": ExeCfg("STMCAS/obj64/slist_omap.stmcas_po.exe", "stmcas_slist_noopt"),
    "stmcas_caumap": ExeCfg("STMCAS/obj64/dlist_opt_caumap.stmcas_po.exe", "stmcas_dcaumap"),
    "stmcas_caumap_noopt": ExeCfg("STMCAS/obj64/dlist_caumap.stmcas_po.exe", "stmcas_dcaumap_noopt"),
    "stmcas_caumap_slist": ExeCfg("STMCAS/obj64/slist_opt_caumap.stmcas_po.exe", "stmcas_dcaumap_noopt"),
    "stmcas_carumap": ExeCfg("STMCAS/obj64/dlist_carumap.stmcas_po.exe", "stmcas_dcarumap"),
    "stmcas_skiplist_cached": ExeCfg("STMCAS/obj64/skiplist_cached_opt_omap.stmcas_po.exe", "stmcas_skiplist_cached"),
    "stmcas_irbtree_po":ExeCfg("STMCAS/obj64/rbtree_omap.stmcas_po.exe", "stmcas_irbtree_po"),
}

# Rules for running the trials of an experiment.  We start with a few constants:
threads = [1, 12, 24, 48, 96]  # thread counts to test
seconds = 5  # seconds per experiment
fillThreads = 1  # how many threads should pre-fill the data structure
trials = 10  # number of trials to average
machine = 'mario'  # A mnemonic for the machine where tests are being run

# Configuration settings for experiments.  We primarily parameterize on key
# range and lookup ratio.  However, we need special configurations for the tree
# tests, because unbalanced BST warm-up needs to be in random order, whereas
# everything else is fine with increasing order.
expConfigs = {
    # Everything except trees can use these
    "size64_r0": ExpCfg(seconds, threads, 0, fillThreads, trials, 64, 0, machine, "size64_r0"),
    "size64_r80": ExpCfg(seconds, threads, 0, fillThreads, trials, 64, 80, machine, "size64_r80"),
    "size256_r0": ExpCfg(seconds, threads, 0, fillThreads, trials, 256, 0, machine, "size256_r0"),
    "size256_r80": ExpCfg(seconds, threads, 0, fillThreads, trials, 256, 80, machine, "size256_r80"),
    "size1K_r0": ExpCfg(seconds, threads, 0, fillThreads, trials, 1024, 0, machine, "size1K_r0"),
    "size1K_r80": ExpCfg(seconds, threads, 0, fillThreads, trials, 1024, 80, machine, "size1K_r80"),
    "size64K_r0": ExpCfg(seconds, threads, 0, fillThreads, trials, 65536, 0, machine, "size64K_r0"),
    "size64K_r80": ExpCfg(seconds, threads, 0, fillThreads, trials, 65536, 80, machine, "size64K_r80"),
    "size1M_r0": ExpCfg(seconds, threads, 0, fillThreads, trials, 1048576, 0, machine, "size1M_r0"),
    "size1M_r80": ExpCfg(seconds, threads, 0, fillThreads, trials, 1048576, 80, machine, "size1M_r80"),
    "size1M_r100": ExpCfg(seconds, threads, 0, fillThreads, trials, 1048576, 100, machine, "size1M_r100"),
    # Trees must use these
    "size64_r0_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 64, 0, machine, "size64_r0_tree"),
    "size64_r80_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 64, 80, machine, "size64_r80_tree"),
    "size256_r0_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 256, 0, machine, "size256_r0_tree"),
    "size256_r80_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 256, 80, machine, "size256_r80_tree"),
    "size1K_r0_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 1024, 0, machine, "size1K_r0_tree"),
    "size1K_r80_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 1024, 80, machine, "size1K_r80_tree"),
    "size64K_r0_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 65536, 0, machine, "size64K_r0_tree"),
    "size64K_r80_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 65536, 80, machine, "size64K_r80_tree"),
    "size1M_r0_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 1048576, 0, machine, "size1M_r0_tree"),
    "size1M_r80_tree": ExpCfg(seconds, threads, 1, fillThreads, trials, 1048576, 80, machine, "size1M_r80_tree"),
}
