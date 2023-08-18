# This file provides the names of all of our TM algorithms, in a format that 
# can be used by Makefiles
STM_NAMES = orec_gv1_eager_c1_q orec_gv1_eager_c2_q \
            orec_tsc_eager_c1_q orec_tsc_eager_c2_q \
            orec_gv1_lazy_c1_q  orec_gv1_lazy_c2_q  \
            orec_tsc_lazy_c1_q  orec_tsc_lazy_c2_q  \
            exo_eager_c1_q      exo_eager_c2_q      \
            exo_lazy_c1_q       exo_lazy_c2_q

TM_LIB_NAMES = $(STM_NAMES)
