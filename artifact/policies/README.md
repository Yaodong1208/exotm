# Synchronization Policies

This folder stores the source code for the synchronization policies that are
discussed in the paper.

## baseline Policy

This is not really a synchronization policy.  It holds the per-thread
functionality that is shared among the baseline lock-free and lock-based
algorithms that we use in our comparison.  This includes things like a
per-thread pseudorandom number generator, a hash function, and safe memory
reclamation.  Note that these features are also present in each of our
synchronization policies.

## exoTM Mechanism

This folder contains the implementation of the exoTM synchronization mechanism.

## handSTM Policy

This folder contains the handSTM policy.  It uses the exoTM mechanism for its
synchronization.  There are five categories of policy:

- eager_c1: encounter-time locking, undo logging, check-once orecs
- eager_c2: encounter-time locking, undo logging, check-twice orecs
- wb_c1: encounter-time locking, redo logging, check-once orecs
- wb_c2: encounter-time locking, redo logging, check-twice orecs
- lazy: commit-time locking, redo logging, check-once orecs

Note that each can be instantiated with per-object (PO) or per-stripe (PS)
orecs.

## hybrid Policy

This folder contains the hybrid policy (handSTM + STMCAS).  It uses the exoTM
mechanism for its synchronization.  There are three categories of policy:

- wb_c1: encounter-time locking, redo logging, check-once orecs
- wb_c2: encounter-time locking, redo logging, check-twice orecs
- lazy: commit-time locking, redo logging, check-once orecs

Note that each can be instantiated with per-object (PO) or per-stripe (PS)
orecs.

## STMCAS Policy

This folder contains the STMCAS policy.  It uses the exoTM mechanism for its
synchronization.  It can be instantiated with per-object (PO) or per-stripe (PS)
orecs.

## xSTM Policy

This folder holds a modified version of the
[llvm-transmem](https://github.com/mfs409/llvm-transmem) plugin, which provides
support for TM in C++.  It consists of two sub-components: a plugin (`plugin/`)
for llvm-15, which instruments transactions, and a set of libraries (`libs/`)
that implement various TM algorithms.

The folder is modified in the following ways:

- Some names are updated to "xSTM"
- The plugin has been ported to llvm-15
- The plugin has support for an RAII interface, which is faster than the
  original lambda interface.
- Additional libraries have been added, which use the exoTM mechanism instead of
  other implementations of orecs and clocks.
- We removed some TM algorithms (PTM, HTM)
- We moved some common libraries out of the folder, if they are shared by our
  exoTM-based policies.

Details about this plugin can be found in the following paper: 

"Simplifying Transactional Memory Support in C++", by PanteA Zardoshti, Tingzhe
Zhou, Pavithra Balaji, Michel L. Scott, and Michael Spear. ACM Transactions on
Architecture and Code Optimization (TACO), 2019.

Note that these are all per-stripe (PS) policies, because that is the only
appropriate way to map orecs to program data in a language-level STM for C++.