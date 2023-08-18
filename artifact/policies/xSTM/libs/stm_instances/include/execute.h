/// execute.h implements the portion of the API related to launching
/// transactions and managing transaction descriptors.

#pragma once

#include <functional>
#include <setjmp.h>

#include "../../../common/tm_defines.h"

/// Create helper methods that create a thread-local pointer to the TxThread,
/// and help a caller to get/construct one
#define API_TM_DESCRIPTOR                                                      \
  namespace {                                                                  \
  thread_local TxThread *self = nullptr;                                       \
  static TxThread *get_self() {                                                \
    if (__builtin_expect(self == nullptr, false)) {                            \
      self = new TxThread();                                                   \
    }                                                                          \
    return self;                                                               \
  }                                                                            \
  }

/// Create the API functions that are used to launch transactions.  These are
/// the versions for when we are not concerned about exceptions escaping from
/// transactions (that is, they don't use try/catch internally).
///
/// EXECUTE_C_INTERNAL is the call when the C compiler could find a clone.
/// EXECUTE_C is for when the compiler could not find a clone at compile time,
/// and needs to do the lookup at run time.  EXECUTE is for the C++ API, which
/// always uses a lambda, and thus doesn't need to worry about lookup.
#define API_TM_EXECUTE_NOEXCEPT                                                \
  extern "C" {                                                                 \
  void TM_EXECUTE_C_INTERNAL(void *, void (*)(void *), void *args,             \
                             void (*anno_func)(void *)) {                      \
    /* get TxThread before making checkpoint, so it doesn't re-run on abort */ \
    TxThread *self = get_self();                                               \
    jmp_buf _jmpbuf;                                                           \
    setjmp(_jmpbuf);                                                           \
    self->beginTx(&_jmpbuf);                                                   \
    anno_func(args);                                                           \
    self->commitTx();                                                          \
  }                                                                            \
  void TM_EXECUTE_C(void *, void (*func)(void *), void *args) {                \
    /* casting function ptr to void* is illegal, but it works on x86 */        \
    union {                                                                    \
      void *voidstar;                                                          \
      TM_C_FUNC cfunc;                                                         \
    } clone;                                                                   \
    clone.voidstar = get_clone((void *)func);                                  \
    /* get TxThread before making checkpoint, so it doesn't re-run on abort */ \
    TxThread *self = get_self();                                               \
    jmp_buf _jmpbuf;                                                           \
    setjmp(_jmpbuf);                                                           \
    self->beginTx(&_jmpbuf);                                                   \
    /* If no clone, become irrevocable */                                      \
    if (clone.voidstar == nullptr) {                                           \
      self->becomeIrrevocable();                                               \
      func(args);                                                              \
    } else {                                                                   \
      clone.cfunc(args);                                                       \
    }                                                                          \
    self->commitTx();                                                          \
  }                                                                            \
  void TM_EXECUTE(void *, std::function<void(TM_OPAQUE *)> func) {             \
    /* get TxThread before making checkpoint, so it doesn't re-run on abort */ \
    TxThread *self = get_self();                                               \
    jmp_buf _jmpbuf;                                                           \
    setjmp(_jmpbuf);                                                           \
    self->beginTx(&_jmpbuf);                                                   \
    func((TM_OPAQUE *)0xCAFE);                                                 \
    self->commitTx();                                                          \
  }                                                                            \
  bool TM_RAII_BEGIN(jmp_buf &buffer) {                                        \
    TxThread *self = get_self();                                               \
    self->beginTx(&buffer);                                                    \
    return true;                                                               \
  }                                                                            \
  void TM_RAII_END() {                                                         \
    TxThread *self = get_self();                                               \
    self->commitTx();                                                          \
  }                                                                            \
  }

/// Create the API functions that are used to launch transactions.  These are
/// the versions for when we never run instrumented code (e.g., HTM, Mutex)
#define API_TM_EXECUTE_NOEXCEPT_NOINST                                         \
  extern "C" {                                                                 \
  void TM_EXECUTE_C_INTERNAL(void *, void (*func)(void *), void *args,         \
                             void (*)(void *)) {                               \
    /* get TxThread before making checkpoint, so it doesn't re-run on abort */ \
    TxThread *self = get_self();                                               \
    self->beginTx();                                                           \
    func(args);                                                                \
    self->commitTx();                                                          \
  }                                                                            \
  void TM_EXECUTE_C(void *, void (*func)(void *), void *args) {                \
    /* get TxThread before making checkpoint, so it doesn't re-run on abort */ \
    TxThread *self = get_self();                                               \
    self->beginTx();                                                           \
    func(args);                                                                \
    self->commitTx();                                                          \
  }                                                                            \
  void TM_EXECUTE(void *, std::function<void(TM_OPAQUE *)> func) {             \
    /* get TxThread before making checkpoint, so it doesn't re-run on abort */ \
    TxThread *self = get_self();                                               \
    self->beginTx();                                                           \
    func(0);                                                                   \
    self->commitTx();                                                          \
  }                                                                            \
  void TM_RAII_LITE_BEGIN() {                                                  \
    TxThread *self = get_self();                                               \
    self->beginTx();                                                           \
  }                                                                            \
  void TM_RAII_LITE_END() { self->commitTx(); }                                \
  bool TM_RAII_BEGIN(jmp_buf &) {                                              \
    TxThread *self = get_self();                                               \
    self->beginTx();                                                           \
    return false;                                                              \
  }                                                                            \
  void TM_RAII_END() {                                                         \
    TxThread *self = get_self();                                               \
    self->commitTx();                                                          \
  }                                                                            \
  }