# Data Structures

This folder stores the data structures that we use in our evaluation.  They are
organized according to the synchronization policy they employ.

The data structures in the `baseline` folder are taken from the open-source
repositories that correspond to those works.  We have modified them in the
following ways:

- We have converted code, as necessary, to move the data structure
  implementation entirely to headers.
- We have modified the data structures to use the facilities in the
  `policies/baseline` policy, so that there is an apples-to-apples comparison
  with regard to hashing, random numbers, and safe memory reclamation.
