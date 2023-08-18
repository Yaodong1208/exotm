#pragma once

#include <cstdint>
#include <setjmp.h>

/// STM is the base for the ROSTM and WOSTM RAII objects, which delineate
/// HandSTM-like transactions
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

public:
  /// Inherit an orec from a previous STMCAS step
  ///
  /// @param obj The ownable object whose orec needs to match some value
  /// @param val The value that we must see
  ///
  /// @return True if the value matches and the STM can continue, false
  ///         otherwise
  bool inheritOrec(typename DESCRIPTOR::ownable_t *obj, uint64_t val) {
    return op->inheritOrec(obj, val);
  }
};

/// ROSTM is an RAII object for HandSTM-like read-only transactions
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

/// WOSTM is an RAII object for HandSTM-like writing transactions
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

/// STEP is the base for the STMCAS-like RSTEP and WSTEP RAII wrappers for the
/// exoTM API
template <class DESCRIPTOR> struct Step {
protected:
  DESCRIPTOR *op; // The thread descriptor for this operation

  /// The TM constructor just records the descriptor
  ///
  /// @param me The thread descriptor
  Step(DESCRIPTOR *me) : op(me) {}

public:
  /// Check if an object's orec value is still `val`
  ///
  /// @param obj The object whose orec is being checked
  /// @param val The expected value of obj's orec
  ///
  /// @return True if it still matches, false otherwise
  bool check_continuation(typename DESCRIPTOR::ownable_t *obj, uint64_t val) {
    return this->op->exo.check_continuation(obj->orec(), val);
  }

  /// Validate that an object's orec is usable by the step
  ///
  /// @param obj The object that we want to use
  ///
  /// @return END_OF_TIME if obj's orec is not usable, else the orec version
  uint64_t check_orec(typename DESCRIPTOR::ownable_t *obj) {
    return this->op->exo.check_orec(obj->orec());
  }

  /// Return the start time of the step
  uint64_t get_start_time() { return this->op->exo.get_start_time(); }
};

/// RO is an RAII object for managing STMCAS-like read-only steps
template <class DESCRIPTOR> struct RStep : Step<DESCRIPTOR> {
  /// Construct to start a read-only step
  ///
  /// @param me The thread descriptor
  RStep(DESCRIPTOR *me) : Step<DESCRIPTOR>(me) { this->op->exo.ro_begin(); }

  /// Destruct the object to end the reading step
  ~RStep() { this->op->exo.ro_end(); }
};

/// WO is an RAII object for managing STMCAS-like writing steps
template <class DESCRIPTOR> struct WStep : Step<DESCRIPTOR> {
  /// Construct to start a writing step
  ///
  /// @param me The thread descriptor
  WStep(DESCRIPTOR *me) : Step<DESCRIPTOR>(me) { this->op->exo.wo_begin(); }

  /// Destruct the object to end the writing step
  ~WStep() { this->op->exo.wo_end(); }

  /// Acquire obj's orec, but only if its orec matches val
  ///
  /// @param obj The object whose orec we want to acquire
  /// @param val The value that object's orec must have
  ///
  /// @return True if the object's orec is successfully acquired
  bool acquire_continuation(typename DESCRIPTOR::ownable_t *obj, uint64_t val) {
    return this->op->exo.acquire_continuation(obj->orec(), val);
  }

  /// Acquire obj's orec, but only if it is consistent with the start time of
  /// this step.
  ///
  /// @param obj The object whose orec we want to acquire
  ///
  /// @return True if the orec was acquired, false otherwise
  bool acquire_consistent(typename DESCRIPTOR::ownable_t *obj) {
    return this->op->exo.acquire_consistent(obj->orec());
  }

  /// Acquire obj's orec, even if its orec would be inconsistent with the
  /// step
  ///
  /// @param obj The object whose orec we want to acquire
  ///
  /// @return True if object's orec is successfully acquired
  bool acquire_aggressive(typename DESCRIPTOR::ownable_t *obj) {
    return this->op->exo.acquire_aggressive(obj->orec());
  }

  /// Unwind the step, so that it can be restarted
  void unwind() { this->op->exo.unwind(); }

  /// Schedule an object for reclamation.  This should only be called from
  /// writing steps that won't unwind.
  ///
  /// NB: The programmer can only reclaim from WSTEPs, not from RSTEPs.
  ///
  /// @param obj The object to reclaim
  void reclaim(typename DESCRIPTOR::ownable_t *obj) {
    this->op->smr.reclaim(obj);
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
