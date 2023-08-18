import Types
import ExpCfg
import ChartCfg

#
# A bunch of globals that culminate in a declaration of `all_targets`
#

Chart = Types.ChartCfg
Curve = Types.CurveCfg
exeNames = ExpCfg.exeNames
dsRules = ExpCfg.dsRules
lineStyles = ChartCfg.lineStyles

# Curves for the linked list charts with key range 1K
list_1K_curves = [
    Curve(exeNames["base_lazylist"], dsRules["list_default"],
          lineStyles["red"], "Lazy List"),
    Curve(exeNames["handstm_slist"], dsRules["list_default"],
          lineStyles["green"], "handSTM"),
    Curve(exeNames["stmcas_slist"], dsRules["list_default"],
          lineStyles["blue"], "STMCAS"),
    Curve(exeNames["stmcas_slist_noopt"], dsRules["list_default"],
          lineStyles["magenta"], "STMCAS (noopt)"),
    Curve(exeNames["stmcas_slist"], dsRules["list_nosnap"],
          lineStyles["gray"], "STMCAS (nosnap)"),
    Curve(exeNames["stmcas_dlist"], dsRules["list_default"],
          lineStyles["black"], "STMCAS (dlist)"),
    Curve(exeNames["stmcas_dlist_noopt"], dsRules["list_default"],
          lineStyles["cyan"], "STMCAS (dlist noopt)"),
    Curve(exeNames["stmcas_dlist"], dsRules["list_nosnap"],
          lineStyles["yellow"], "STMCAS (dlist nosnap)")
]

# Curves for the linked list charts with key range 64
list_64_curves = [
    Curve(exeNames["base_lazylist"], dsRules["list_default"],
          lineStyles["red"], "Lazy List"),
    Curve(exeNames["handstm_slist"], dsRules["list_default"],
          lineStyles["green"], "handSTM"),
    Curve(exeNames["stmcas_slist"], dsRules["list_default"],
          lineStyles["blue"], "STMCAS"),
    Curve(exeNames["stmcas_slist"], dsRules["list_nosnap"],
          lineStyles["magenta"], "STMCAS (nosnap)"),
    Curve(exeNames["stmcas_slist_ps"], dsRules["list_default"],
          lineStyles["black"], "STMCAS (ps)"),
    Curve(exeNames["stmcas_slist_ps"], dsRules["list_nosnap"],
          lineStyles["cyan"], "STMCAS (ps nosnap)")
]

# The four list charts (both key ranges, two lookup ratios)
list_64 = Chart(
    list_64_curves, ExpCfg.expConfigs["size64_r80"], "Threads", "Operations/Second", "list_64")
list_64_wo = Chart(
    list_64_curves, ExpCfg.expConfigs["size64_r0"], "Threads", "Operations/Second", "list_64_wo")
list_1K = Chart(
    list_1K_curves, ExpCfg.expConfigs["size1K_r80"], "Threads", "Operations/Second", "list_1K")
list_1K_wo = Chart(
    list_1K_curves, ExpCfg.expConfigs["size1K_r0"], "Threads", "Operations/Second", "list_1K_wo")

# Curves for all skiplist charts
skiplist_curves = [
    Curve(exeNames["base_skiplist"], dsRules["skiplist_default"],
          lineStyles["red"], "fraser"),
    Curve(exeNames["handstm_skiplist"], dsRules["skiplist_default"],
          lineStyles["green"], "handSTM"),

    Curve(exeNames["stmcas_skiplist_cached"], dsRules["skiplist_default"],
          lineStyles["black"], "STMCAS"),
]

# The four skiplist charts (two key ranges, two lookup ratios)
sl_64K = Chart(skiplist_curves,
               ExpCfg.expConfigs["size64K_r80"], "Threads", "Operations/Second", "sl_64K")
sl_64K_wo = Chart(
    skiplist_curves, ExpCfg.expConfigs["size64K_r0"], "Threads", "Operations/Second", "sl_64K_wo")
sl_1M = Chart(skiplist_curves,
              ExpCfg.expConfigs["size1M_r80"], "Threads", "Operations/Second", "sl_1M")
sl_1M_wo = Chart(
    skiplist_curves, ExpCfg.expConfigs["size1M_r0"], "Threads", "Operations/Second", "sl_1M_wo")

# Curves for all unordered map charts
umap_curves = [
    Curve(exeNames["base_caumap"], dsRules["umap_default"],
          lineStyles["red"], "Lazy Hash"),
    Curve(exeNames["handstm_caumap"], dsRules["umap_default"],
          lineStyles["green"], "handSTM"),
    Curve(exeNames["stmcas_caumap"], dsRules["umap_default"],
          lineStyles["blue"], "STMCAS"),
    Curve(exeNames["stmcas_caumap_slist"], dsRules["umap_default"],
          lineStyles["black"], "STMCAS (slist)"),
    Curve(exeNames["stmcas_caumap_noopt"], dsRules["umap_default"],
          lineStyles["magenta"], "STMCAS (noopt)"),
    Curve(exeNames["stmcas_carumap"], dsRules["umap_default"],
          lineStyles["yellow"], "STMCAS (resizable)"),
    Curve(exeNames["hybrid_carumap"], dsRules["umap_default"],
          lineStyles["cyan"], "hybrid (resizable)"),
    Curve(exeNames["handstm_carumap"], dsRules["umap_default"],
          lineStyles["gray"], "handSTM (resizable)")
]

# The two umap charts (two lookup ratios, one key range)
umap_1M = Chart(
    umap_curves, ExpCfg.expConfigs["size1M_r80"], "Threads", "Operations/Second", "umap_1M")
umap_1M_wo = Chart(
    umap_curves, ExpCfg.expConfigs["size1M_r0"], "Threads", "Operations/Second", "umap_1M_wo")

# Curves for all bst charts
bst_curves = [
    Curve(exeNames["base_ebst"], dsRules["bst_default"],
          lineStyles["red"], "baseline"),
    Curve(exeNames["base_ibst"], dsRules["bst_default"],
          lineStyles["cyan"], "pathCAS"),
    Curve(exeNames["handstm_ibst"], dsRules["bst_default"],
          lineStyles["green"], "handSTM"),
    Curve(exeNames["xstm_ibst"], dsRules["bst_default"],
          lineStyles["yellow"], "xSTM"),
    Curve(exeNames["xstm_ibst_tiny"], dsRules["bst_default"],
          lineStyles["magenta"], "TinySTM"),
    Curve(exeNames["stmcas_ibst"], dsRules["bst_default"],
          lineStyles["blue"], "STMCAS"),
    Curve(exeNames["stmcas_ibst_ps"], dsRules["bst_default"],
          lineStyles["black"], "STMCAS (ps)"),
]

# The four bst charts (two key ranges, two lookup ratios)
bst_64K = Chart(
    bst_curves, ExpCfg.expConfigs["size64K_r80_tree"], "Threads", "Operations/Second", "bst_64K")
bst_64K_wo = Chart(
    bst_curves, ExpCfg.expConfigs["size64K_r0_tree"], "Threads", "Operations/Second", "bst_64K_wo")
bst_1M = Chart(
    bst_curves, ExpCfg.expConfigs["size1M_r80_tree"], "Threads", "Operations/Second", "bst_1M")
bst_1M_wo = Chart(
    bst_curves, ExpCfg.expConfigs["size1M_r0_tree"], "Threads", "Operations/Second", "bst_1M_wo")

# Curves for all bbst charts
bbst_curves = [
    Curve(exeNames["base_ibbst"], dsRules["bst_default"],
          lineStyles["red"], "pathcas_avl"),
    Curve(exeNames["handstm_irbtree"], dsRules["bst_default"],
          lineStyles["green"], "handSTM"),
    Curve(exeNames["hybrid_irbtree"], dsRules["bst_default"],
          lineStyles["yellow"], "hybrid"),
    Curve(exeNames["stmcas_irbtree_po"], dsRules["bst_default"],
          lineStyles["blue"], "STMCAS"),
]

# the four bbsts charts (two key ranges, two lookup ratios)
bbst_64K = Chart(
    bbst_curves, ExpCfg.expConfigs["size64K_r80_tree"], "Threads", "Operations/Second", "bbst_64K")
bbst_64K_wo = Chart(
    bbst_curves, ExpCfg.expConfigs["size64K_r0_tree"], "Threads", "Operations/Second", "bbst_64K_wo")
bbst_1M = Chart(
    bbst_curves, ExpCfg.expConfigs["size1M_r80_tree"], "Threads", "Operations/Second", "bbst_1M")
bbst_1M_wo = Chart(
    bbst_curves, ExpCfg.expConfigs["size1M_r0_tree"], "Threads", "Operations/Second", "bbst_1M_wo")

# Now we can define `targets`, a complete description of all the data we need,
# and how it should be grouped
all_targets = [
    list_64,  list_64_wo, list_1K, list_1K_wo,
    sl_64K, sl_64K_wo, sl_1M, sl_1M_wo,
    umap_1M, umap_1M_wo,
    bst_64K, bst_64K_wo, bst_1M, bst_1M_wo,
    bbst_64K, bbst_64K_wo, bbst_1M, bbst_1M_wo
]
# all_targets = [umap_1M_wo, umap_1M]
