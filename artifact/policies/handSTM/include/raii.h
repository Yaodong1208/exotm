#pragma once

#include <cstdint>
#include <setjmp.h>

/// STM is the base for the ROSTM and WOSTM RAII objects, which delineate
/// HandSTM transactions
template <class DESCRIPTOR> struct Stm {
  // Fields need to be friends, so they can access `op`
  template <typename T, typename D> friend class field_base_t;

protected:
  DESCRIPTOR *op; // The thread descriptor for this operation

  /// Construct by recording the descriptor and jump buffer
  ///
  /// @param me The thread descriptor
  /// @param jb The register checkpoint (jump buffer)
  Stm(DESCRIPTOR *me, jmp_buf *jb) : op(me) { me->checkpoint = jb; }
};

/// ROSTM is an RAII object for read-only transactions
template <class DESCRIPTOR> struct RoStm : Stm<DESCRIPTOR> {
  /// Construct to start a transaction
  ///
  /// @param me The thread descriptor
  /// @param jb The register checkpoint (jump buffer)
  RoStm(DESCRIPTOR *me, jmp_buf *jb) : Stm<DESCRIPTOR>(me, jb) {
    this->op->exo.ro_begin();
  }

  /// Destruct the object to commit the transaction
  ~RoStm() {
    this->op->exo.ro_end();
    this->op->readset.clear();
  }
};

/// WOSTM is an RAII object for writing transactions
template <class DESCRIPTOR> struct WoStm : Stm<DESCRIPTOR> {
  /// Construct to start a transaction
  ///
  /// @param me The thread descriptor
  /// @param jb The register checkpoint (jump buffer)
  WoStm(DESCRIPTOR *me, jmp_buf *jb) : Stm<DESCRIPTOR>(me, jb) {
    this->op->exo.wo_begin();
  }

  /// Destruct the object to commit the transaction
  ~WoStm() { this->op->commit(); }

  /// Whenever a node is speculatively allocated, use this to log it
  ///
  /// @param node The ownable_t to log
  ///
  /// @return `node`, to facilitate chaining
  template <class T> T *LOG_NEW(T *node) {
    this->op->mallocs.push_back(node);
    return node;
  }

  /// Schedule an object for reclamation if the transaction commits
  ///
  /// NB: It might seem odd that the reclamation is only for WOSTM, and that
  ///     it's tied to the WOSTM object instead of DESCRIPTOR itself.  It works
  ///     for now.
  ///
  /// @param obj The object to reclaim
  void reclaim(typename DESCRIPTOR::ownable_t *obj) {
    this->op->frees.push_back(obj);
  }
};

/// Start a writing transaction by calling setjmp and then creating a WOSTM
#define BEGIN_WO(op)                                                           \
  jmp_buf checkpoint;                                                          \
  setjmp(checkpoint);                                                          \
  WOSTM wo(op, &checkpoint)

/// Start a reading transaction by calling setjmp and then creating a ROSTM
#define BEGIN_RO(op)                                                           \
  jmp_buf checkpoint;                                                          \
  setjmp(checkpoint);                                                          \
  ROSTM ro(op, &checkpoint)