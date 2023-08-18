/*
 *   File: bst_tk.c
 *   Author: Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *   Description: Asynchronized Concurrency: The Secret to Scaling Concurrent
 *    Search Data Structures, Tudor David, Rachid Guerraoui, Vasileios
 *Trigonakis, ASPLOS '15 bst_tk.c is part of ASCYLIB
 *
 * Copyright (c) 2014 Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>,
 *                   Tudor David <tudor.david@epfl.ch>
 *                   Distributed Programming Lab (LPD), EPFL
 *
 * ASCYLIB is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*
 * File:   ticket.h
 * Author: Trevor Brown
 *
 * Substantial improvements to interface, memory reclamation and bug fixing.
 *
 * Created on June 7, 2017, 1:38 PM
 */

#pragma once

#include "plaf.h"
#include <climits>
#include <cstddef>
#include <stdint.h>
#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#if !defined(COMPILER_BARRIER)
#define COMPILER_BARRIER asm volatile("" ::: "memory")
#endif

typedef union tl32 {
  struct {
    volatile uint16_t version;
    volatile uint16_t ticket;
  };
  volatile uint32_t to_uint32;
} tl32_t;

typedef union tl {
  tl32_t lr[2];
  uint64_t to_uint64;
} tl_t;

static inline int tl_trylock_version(volatile tl_t *tl, volatile tl_t *tl_old,
                                     int right) {
  uint16_t version = tl_old->lr[right].version;
  // uint16_t one = (uint16_t) 1;
  if (unlikely(version != tl_old->lr[right].ticket)) {
    return 0;
  }

#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 6
  tl32_t tlo = {
      {.version = version,
       .ticket = version}}; //{ .version = version, .ticket = version};
  tl32_t tln = {
      {.version = version,
       .ticket =
           (uint16_t)(version +
                      one)}}; //{.version = version, .ticket = (version + 1)};
  return CASV(&tl->lr[right].to_uint32, tlo.to_uint32, tln.to_uint32) ==
         tlo.to_uint32;
#else
  tl32_t tlo = {{version, version}};
  tl32_t tln = {{version, (uint16_t)(version + 1)}};
#endif
  return CASV(&tl->lr[right].to_uint32, tlo.to_uint32, tln.to_uint32) ==
         tlo.to_uint32;
}

#define TLN_REMOVED 0x0000FFFF0000FFFF0000LL

static inline int tl_trylock_version_both(volatile tl_t *tl,
                                          volatile tl_t *tl_old) {
  uint16_t v0 = tl_old->lr[0].version;
  uint16_t v1 = tl_old->lr[1].version;
  if (unlikely(v0 != tl_old->lr[0].ticket || v1 != tl_old->lr[1].ticket)) {
    return 0;
  }

#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 6
  tl_t tlo = {.to_uint64 = tl_old->to_uint64};
  return CASV(&tl->to_uint64, tlo.to_uint64, TLN_REMOVED) == tlo.to_uint64;
#else
  /* tl_t tlo; */
  /* tlo.uint64_t = tl_old->to_uint64; */
  uint64_t tlo = *(uint64_t *)tl_old;

  return CASV((uint64_t *)tl, tlo, TLN_REMOVED) == tlo;
#endif
}

static inline void tl_unlock(volatile tl_t *tl, int right) {
  /* PREFETCHW(tl); */
  COMPILER_BARRIER;
  tl->lr[right].version = tl->lr[right].version + 1;
  COMPILER_BARRIER;
}

static inline void tl_revert(volatile tl_t *tl, int right) {
  /* PREFETCHW(tl); */
  COMPILER_BARRIER;
  tl->lr[right].ticket = tl->lr[right].ticket - 1;
  COMPILER_BARRIER;
}

template <typename skey_t, typename sval_t, class DESCRIPTOR>
struct node_t : DESCRIPTOR::reclaimable_t {
  skey_t key;
  sval_t val;
  struct node_t<skey_t, sval_t, DESCRIPTOR> *volatile left;
  struct node_t<skey_t, sval_t, DESCRIPTOR> *volatile right;
  volatile tl_t lock;
#ifdef USE_PADDING
  char pad[PAD_SIZE];
#endif
};

template <typename skey_t, typename sval_t, class DESCRIPTOR, skey_t _KEY_MIN,
          skey_t _KEY_MAX, sval_t _VAL_RESERVED>
class ticket {
private:
  PAD;
  node_t<skey_t, sval_t, DESCRIPTOR> *root;
  PAD;
  const skey_t KEY_MIN;
  const skey_t KEY_MAX;
  const sval_t NO_VALUE;
  PAD;
  int init[MAX_THREADS_POW2] = {
      0,
  };
  PAD;

  node_t<skey_t, sval_t, DESCRIPTOR> *
  new_node(skey_t key, sval_t val, node_t<skey_t, sval_t, DESCRIPTOR> *l,
           node_t<skey_t, sval_t, DESCRIPTOR> *r);
  node_t<skey_t, sval_t, DESCRIPTOR> *new_node_no_init();

public:
  ticket(DESCRIPTOR *me, auto *cfg)
      : KEY_MIN(_KEY_MIN), KEY_MAX(_KEY_MAX), NO_VALUE(_VAL_RESERVED) {
    node_t<skey_t, sval_t, DESCRIPTOR> *_min =
        new_node(KEY_MIN, NO_VALUE, NULL, NULL);
    node_t<skey_t, sval_t, DESCRIPTOR> *_max =
        new_node(KEY_MAX, NO_VALUE, NULL, NULL);
    root = new_node(KEY_MAX, NO_VALUE, _min, _max);
  }

  ~ticket() {}

  node_t<skey_t, sval_t, DESCRIPTOR> *getRoot() { return root; }

  sval_t get_internal(DESCRIPTOR *me, skey_t &key, sval_t &val);
  sval_t insert_internal(DESCRIPTOR *me, skey_t &key, sval_t &val);
  sval_t remove_internal(DESCRIPTOR *me, skey_t &key);

  node_t<skey_t, sval_t, DESCRIPTOR> *get_root() { return root; }

  bool get(DESCRIPTOR *me, const skey_t &key, sval_t &val) {
    skey_t k = key;
    sval_t v = get_internal(me, k, val);
    if (v == NO_VALUE)
      return false;
    val = v;
    return true;
  }

  bool insert(DESCRIPTOR *me, const skey_t &key, sval_t &val) {
    sval_t v = val;
    skey_t k = key;
    return NO_VALUE == insert_internal(me, k, v);
  }

  bool remove(DESCRIPTOR *me, const skey_t &key) {
    sval_t k = key;
    return NO_VALUE != remove_internal(me, k);
  }
};

template <typename skey_t, typename sval_t, class D, skey_t KMN, skey_t KMX,
          sval_t VR>
node_t<skey_t, sval_t, D> *ticket<skey_t, sval_t, D, KMN, KMX, VR>::new_node(
    skey_t key, sval_t val, node_t<skey_t, sval_t, D> *l,
    node_t<skey_t, sval_t, D> *r) {
  auto node = new_node_no_init();
  node->val = val;
  node->key = key;
  node->left = l;
  node->right = r;
  return node;
}

template <typename skey_t, typename sval_t, class D, skey_t KMN, skey_t KMX,
          sval_t VR>
node_t<skey_t, sval_t, D> *
ticket<skey_t, sval_t, D, KMN, KMX, VR>::new_node_no_init() {
  auto node = new node_t<skey_t, sval_t, D>();
  if (unlikely(node == NULL)) {
    // perror("malloc @ new_node");
    // exit(1);
  }
  node->lock.to_uint64 = 0;
  node->val = NO_VALUE;
  return node;
}

template <typename skey_t, typename sval_t, class D, skey_t KMN, skey_t KMX,
          sval_t VR>
sval_t ticket<skey_t, sval_t, D, KMN, KMX, VR>::get_internal(D *me, skey_t &key,
                                                             sval_t &val) {
  node_t<skey_t, sval_t, D> *curr = root;

  while (likely(curr->left != NULL)) {
    if (key < curr->key) {
      curr = curr->left;
    } else {
      curr = curr->right;
    }
  }

  if (curr->key == key) {
    return curr->val;
  }

  return NO_VALUE;
}

template <typename skey_t, typename sval_t, class D, skey_t KMN, skey_t KMX,
          sval_t VR>
sval_t ticket<skey_t, sval_t, D, KMN, KMX, VR>::insert_internal(D *me,
                                                                skey_t &key,
                                                                sval_t &val) {
  node_t<skey_t, sval_t, D> *curr;
  node_t<skey_t, sval_t, D> *pred = NULL;
  volatile uint64_t curr_ver = 0;
  uint64_t pred_ver = 0, right = 0;

retry : { // reclamation guarded section
  curr = root;
  do {
    curr_ver = curr->lock.to_uint64;

    pred = curr;
    pred_ver = curr_ver;

    if (key < curr->key) {
      right = 0;
      curr = curr->left;
    } else {
      right = 1;
      curr = curr->right;
    }
  } while (likely(curr->left != NULL));

  if (curr->key == key) {
    // insert if absent
    return curr->val;
  }

  //        node_t<skey_t, sval_t>* nn_leaked = new_node(tid, key, val, NULL,
  //        NULL);
  node_t<skey_t, sval_t, D> *nn = new_node(key, val, NULL, NULL);
  node_t<skey_t, sval_t, D> *nr = new_node_no_init();

  if ((!tl_trylock_version(&pred->lock, (volatile tl_t *)&pred_ver, right))) {
    // recmgr->deallocate(tid, nn);
    // recmgr->deallocate(tid, nr);
    goto retry;
  }

  if (key < curr->key) {
    nr->key = curr->key;
    nr->left = nn;
    nr->right = curr;
  } else {
    nr->key = key;
    nr->left = curr;
    nr->right = nn;
  }

  if (right) {
    pred->right = nr;
  } else {
    pred->left = nr;
  }

  tl_unlock(&pred->lock, right);

  return NO_VALUE;
}
}

template <typename skey_t, typename sval_t, class D, skey_t KMN, skey_t KMX,
          sval_t VR>
sval_t ticket<skey_t, sval_t, D, KMN, KMX, VR>::remove_internal(D *me,
                                                                skey_t &key) {
  node_t<skey_t, sval_t, D> *curr;
  node_t<skey_t, sval_t, D> *pred = NULL;
  node_t<skey_t, sval_t, D> *ppred = NULL;
  volatile uint64_t curr_ver = 0;
  uint64_t pred_ver = 0, ppred_ver = 0, right = 0, pright = 0;

retry :

{ // reclamation guarded section
  curr = root;

  do {
    curr_ver = curr->lock.to_uint64;

    ppred = pred;
    ppred_ver = pred_ver;
    pright = right;

    pred = curr;
    pred_ver = curr_ver;

    if (key < curr->key) {
      right = 0;
      curr = curr->left;
    } else {
      right = 1;
      curr = curr->right;
    }
  } while (likely(curr->left != NULL));

  if (curr->key != key) {
    return NO_VALUE;
  }

  if ((!tl_trylock_version(&ppred->lock, (volatile tl_t *)&ppred_ver,
                           pright))) {
    goto retry;
  }

  if ((!tl_trylock_version_both(&pred->lock, (volatile tl_t *)&pred_ver))) {
    tl_revert(&ppred->lock, pright);
    goto retry;
  }

  if (pright) {
    if (right) {
      ppred->right = pred->left;
    } else {
      ppred->right = pred->right;
    }

  } else {
    if (right) {
      ppred->left = pred->left;
    } else {
      ppred->left = pred->right;
    }
  }

  tl_unlock(&ppred->lock, pright);

  me->reclaim(curr); // recmgr->retire(tid, curr);
  me->reclaim(pred); // recmgr->retire(tid, pred);

  return curr->val;
}
}
