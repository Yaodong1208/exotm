/******************************************************************************
 * Skip lists, allowing concurrent update by use of CAS primitives.
 *
 * Copyright (c) 2001-2003, K A Fraser
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * The name of the author may not be used to endorse or promote products
 * derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <assert.h>
#include <atomic>
#include <functional>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * A really simple random-number generator. Crappy linear congruential
 * taken from glibc, but has at least a 2^32 period.
 */

#define rand_next(_ptst) ((_ptst)->rand = ((_ptst)->rand * 1103515245) + 12345)

struct ptst_t {
  unsigned long rand;
};

extern pthread_key_t ptst_key;

typedef unsigned long long tick_t;
#define RDTICK()                                                               \
  ({                                                                           \
    tick_t __t;                                                                \
    __asm__ __volatile__("rdtsc" : "=A"(__t));                                 \
    __t;                                                                       \
  })

/*
 * POINTER MARKING
 */

#define get_marked_ref(_p) ((void *)(((unsigned long)(_p)) | 1))
#define get_unmarked_ref(_p) ((void *)(((unsigned long)(_p)) & ~1))
#define is_marked_ref(_p) (((unsigned long)(_p)) & 1)

/* Number of unique block sizes we can deal with. */
#define MAX_SIZES 20

typedef unsigned long setkey_t;
typedef void *setval_t;

/*************************************
 * INTERNAL DEFINITIONS
 */

/* Fine for 2^NUM_LEVELS nodes. */
#define NUM_LEVELS 20

/* Internal key values with special meanings. */
#define SENTINEL_KEYMIN (1UL)  /* Key value of first dummy node. */
#define SENTINEL_KEYMAX (~0UL) /* Key value of last dummy node.  */

/*
 * Used internally by set access functions, so that callers can use
 * key values 0 and 1, without knowing these have special meanings.
 */
#define CALLER_TO_INTERNAL_KEY(_k) ((_k) + 2)

pthread_key_t ptst_key;

/*
 * Enter/leave a critical region. A thread gets a state handle for
 * use during critical regions.
 */
void critical_exit(ptst_t *) {}

ptst_t *critical_enter(void) {
  ptst_t *ptst;

  ptst = (ptst_t *)pthread_getspecific(ptst_key);
  if (ptst == NULL) {

    ptst = (ptst_t *)malloc(sizeof(*ptst));
    if (ptst == NULL)
      exit(1);
    memset(ptst, 0, sizeof(*ptst));
    // ptst->gc = gc_init();
    ptst->rand = RDTICK();

    pthread_setspecific(ptst_key, ptst);
  }

  // gc_enter(ptst);
  return (ptst);
}

static void ptst_destructor(ptst_t *ptst) {}

void _init_ptst_subsystem(void) {
  if (pthread_key_create(&ptst_key, (void (*)(void *))ptst_destructor)) {
    exit(1);
  }
}

/*
 * SKIP LIST
 */

struct node_t {
  std::atomic<int> level;
#define LEVEL_MASK 0x0ff
#define READY_FOR_FREE 0x100
  setkey_t k;
  std::atomic<setval_t> v;
  std::atomic<node_t *> next[1];
};

struct set_t {
  node_t head;
};

/*
 * PRIVATE FUNCTIONS
 */

/*
 * Random level generator. Drop-off rate is 0.5 per level.
 * Returns value 1 <= level <= NUM_LEVELS.
 */
static int get_level(ptst_t *ptst) {
  unsigned long r = rand_next(ptst);
  int l = 1;
  r = (r >> 4) & ((1 << (NUM_LEVELS - 1)) - 1);
  while ((r & 1)) {
    l++;
    r >>= 1;
  }
  return (l);
}

/*
 * Allocate a new node, and initialise its @level field.
 * NB. Initialisation will eventually be pushed into garbage collector,
 * because of dependent read reordering.
 */
static node_t *alloc_node(ptst_t *ptst) {
  int l;
  node_t *n;
  l = get_level(ptst);
  // NB: Removed call to gc_alloc, because everything should use the same
  //     allocator
  // n = (node_t *)gc_alloc(ptst, gc_id[l - 1]);
  n = (node_t *)malloc(sizeof(*n) + l * sizeof(node_t *));
  n->level.store(l, std::memory_order_relaxed);
  return (n);
}

/* Free a node to the garbage collector. */
static void free_node(ptst_t *ptst, node_t *n) {
  // NB: For now, just leak it...
  // gc_free(ptst, (void *)n, gc_id[(n->level & LEVEL_MASK) - 1]);
}

/*
 * Search for first non-deleted node, N, with key >= @k at each level in @l.
 * RETURN VALUES:
 *  Array @pa: @pa[i] is non-deleted predecessor of N at level i
 *  Array @na: @na[i] is N itself, which should be pointed at by @pa[i]
 *  MAIN RETURN VALUE: same as @na[0].
 */
static node_t *strong_search_predecessors(set_t *l, setkey_t k, node_t **pa,
                                          node_t **na) {
  node_t *x, *x_next, *old_x_next, *y, *y_next;
  setkey_t y_k;
  int i;

retry:
  // RMB(); // NB: This fence was unnecessary

  x = &l->head;
  for (i = NUM_LEVELS - 1; i >= 0; i--) {
    /* We start our search at previous level's unmarked predecessor. */
    x_next = x->next[i].load();
    /* If this pointer's marked, so is @pa[i+1]. May as well retry. */
    if (is_marked_ref(x_next))
      goto retry;

    for (y = x_next;; y = y_next) {
      /* Shift over a sequence of marked nodes. */
      for (;;) {
        y_next = y->next[i].load();
        if (!is_marked_ref(y_next))
          break;
        y = (node_t *)get_unmarked_ref(y_next);
      }

      y_k = y->k;
      if (y_k >= k)
        break;

      /* Update estimate of predecessor at this level. */
      x = y;
      x_next = y_next;
    }

    /* Swing forward pointer over any marked nodes. */
    if (x_next != y) {
      old_x_next = x_next;
      x->next[i].compare_exchange_strong(old_x_next, y);
      if (old_x_next != x_next)
        goto retry;
    }

    if (pa)
      pa[i] = x;
    if (na)
      na[i] = y;
  }

  return (y);
}

/* This function does not remove marked nodes. Use it optimistically. */
node_t *weak_search_predecessors(set_t *l, setkey_t k, node_t **pa,
                                 node_t **na) {
  node_t *x, *x_next;
  setkey_t x_next_k;
  int i;

  x = &l->head;
  for (i = NUM_LEVELS - 1; i >= 0; i--) {
    for (;;) {
      x_next = x->next[i].load();
      x_next = (node_t *)get_unmarked_ref(x_next);

      x_next_k = x_next->k;
      if (x_next_k >= k)
        break;

      x = x_next;
    }

    if (pa)
      pa[i] = x;
    if (na)
      na[i] = x_next;
  }

  return (x_next);
}

/*
 * Mark @x deleted at every level in its list from @level down to level 1.
 * When all forward pointers are marked, node is effectively deleted.
 * Future searches will properly remove node by swinging predecessors'
 * forward pointers.
 */
static void mark_deleted(node_t *x, int level) {
  node_t *x_next;

  while (--level >= 0) {
    x_next = x->next[level].load();
    while (!is_marked_ref(x_next)) {
      x->next[level].compare_exchange_strong(x_next,
                                             (node_t *)get_marked_ref(x_next));
    }
  }
}

static int check_for_full_delete(node_t *x) {
  int level = x->level.load();
  return ((level & READY_FOR_FREE) ||
          !x->level.compare_exchange_strong(level, level | READY_FOR_FREE));
}

static void do_full_delete(ptst_t *ptst, set_t *l, node_t *x, int level) {
  int k = x->k;
  (void)strong_search_predecessors(l, k, NULL, NULL);
  free_node(ptst, x);
}

/*
 * PUBLIC FUNCTIONS
 */

set_t *set_alloc(void) {
  set_t *l;
  node_t *n;
  int i;

  n = (node_t *)malloc(sizeof(*n) + (NUM_LEVELS - 1) * sizeof(node_t *));
  n->k = SENTINEL_KEYMAX;

  /*
   * Set the forward pointers of final node to other than NULL,
   * otherwise READ_FIELD() will continually execute costly barriers.
   * Note use of 0xfe -- that doesn't look like a marked value!
   */
  for (i = 0; i < NUM_LEVELS; i++) {
    n->next[i].store((node_t *)0xfe);
  }

  l = (set_t *)malloc(sizeof(*l) + (NUM_LEVELS - 1) * sizeof(node_t *));
  l->head.k = SENTINEL_KEYMIN;
  l->head.level.store(NUM_LEVELS, std::memory_order_relaxed);
  for (i = 0; i < NUM_LEVELS; i++) {
    l->head.next[i].store(n);
  }

  return (l);
}

bool set_update(set_t *l, setkey_t k, setval_t &v, int overwrite) {
  setval_t ov, new_ov;
  ptst_t *ptst;
  node_t *preds[NUM_LEVELS];
  node_t *succs[NUM_LEVELS];
  node_t *pred, *succ, *_new = NULL, *new_next, *old_next;
  int i, level;
  bool result = false;

  k = CALLER_TO_INTERNAL_KEY(k);

  ptst = critical_enter();

  succ = weak_search_predecessors(l, k, preds, succs);

retry:
  ov = NULL;
  result = false;

  if (succ->k == k) {
    /* Already a @k node in the list: update its mapping. */
    new_ov = succ->v;
    // NB: Removed overwrite ability, for compatibility with harness
    // do {
    if ((ov = new_ov) == NULL) {
      /* Finish deleting the node, then retry. */
      level = succ->level;
      mark_deleted(succ, level & LEVEL_MASK);
      succ = strong_search_predecessors(l, k, preds, succs);
      goto retry;
    }
    // } while (overwrite && ((new_ov = CASPO(&succ->v, ov, v)) != ov));

    if (_new != NULL)
      free_node(ptst, _new);
    goto out;
  }

  result = true;

  /* Not in the list, so initialise a new node for insertion. */
  if (_new == NULL) {
    _new = alloc_node(ptst);
    _new->k = k;
    _new->v.store(v, std::memory_order_relaxed);
  }
  level = _new->level;

  /* If successors don't change, this saves us some CAS operations. */
  for (i = 0; i < level; i++) {
    _new->next[i].store(succs[i], std::memory_order_relaxed);
  }

  /* We've committed when we've inserted at level 1. */
  // WMB_NEAR_CAS(); /* make sure node fully initialised before inserting */
  old_next = succ;
  preds[0]->next[0].compare_exchange_strong(old_next, _new);
  if (old_next != succ) {
    succ = strong_search_predecessors(l, k, preds, succs);
    goto retry;
  }

  /* Insert at each of the other levels in turn. */
  i = 1;
  while (i < level) {
    pred = preds[i];
    succ = succs[i];

    /* Someone *can* delete @_new under our feet! */
    new_next = _new->next[i].load();
    if (is_marked_ref(new_next))
      goto success;

    /* Ensure forward pointer of new node is up to date. */
    if (new_next != succ) {
      old_next = new_next;
      _new->next[i].compare_exchange_strong(old_next, succ);
      if (is_marked_ref(old_next))
        goto success;
      assert(old_next == new_next);
    }

    /* Ensure we have unique key values at every level. */
    if (succ->k == k)
      goto new_world_view;
    assert((pred->k < k) && (succ->k > k));

    /* Replumb predecessor's forward pointer. */
    old_next = succ;
    pred->next[i].compare_exchange_strong(old_next, _new);
    if (old_next != succ) {
    new_world_view:
      // RMB(); /* get up-to-date view of the world. */
      (void)strong_search_predecessors(l, k, preds, succs);
      continue;
    }

    /* Succeeded at this level. */
    i++;
  }

success:
  /* Ensure node is visible at all levels before punting deletion. */
  // WEAK_DEP_ORDER_WMB();
  if (check_for_full_delete(_new)) {
    // MB(); /* make sure we see all marks in @new. */
    do_full_delete(ptst, l, _new, level - 1);
  }
out:
  critical_exit(ptst);
  return (result);
}

bool set_remove(set_t *l, setkey_t k, setval_t &v) {
  setval_t new_v;
  ptst_t *ptst;
  node_t *preds[NUM_LEVELS], *x;
  int level, i;
  bool result = false;

  k = CALLER_TO_INTERNAL_KEY(k);
  v = NULL;

  ptst = critical_enter();

  x = weak_search_predecessors(l, k, preds, NULL);

  if (x->k > k)
    goto out;
  level = x->level;
  level = level & LEVEL_MASK;

  /* Once we've marked the value field, the node is effectively deleted. */
  new_v = x->v;
  for (;;) {
    v = new_v;
    if (v == NULL)
      goto out;

    x->v.compare_exchange_strong(new_v, NULL);
    if (new_v == v)
      break;
  }

  result = true;

  /* Committed to @x: mark lower-level forward pointers. */
  // WEAK_DEP_ORDER_WMB(); /* enforce above as linearisation point */
  mark_deleted(x, level);

  /*
   * We must swing predecessors' pointers, or we can end up with
   * an unbounded number of marked but not fully deleted nodes.
   * Doing this creates a bound equal to number of threads in the system.
   * Furthermore, we can't legitimately call 'free_node' until all shared
   * references are gone.
   */
  for (i = level - 1; i >= 0; i--) {
    node_t *tmp = x;
    preds[i]->next[i].compare_exchange_strong(
        tmp, (node_t *)get_unmarked_ref(x->next[i].load()));
    if (tmp != x) {
      if ((i != (level - 1)) || check_for_full_delete(x)) {
        // MB(); /* make sure we see node at all levels. */
        do_full_delete(ptst, l, x, i);
      }
      goto out;
    }
  }

  free_node(ptst, x);

out:
  critical_exit(ptst);
  return (result);
}

bool set_lookup(set_t *l, setkey_t k, setval_t &v) {
  ptst_t *ptst;
  node_t *x;

  k = CALLER_TO_INTERNAL_KEY(k);

  ptst = critical_enter();
  bool res = false;

  x = weak_search_predecessors(l, k, NULL, NULL);
  if (x->k == k) {
    v = x->v;
    res = true;
  }
  critical_exit(ptst);

  return res;
}

template <typename K, typename V, class DESCRIPTOR> class fraser_skiplist {
  set_t *sl;

public:
  fraser_skiplist(DESCRIPTOR *me, auto *cfg) {
    _init_ptst_subsystem();
    sl = set_alloc();
  }

  ~fraser_skiplist() {}

  bool get(DESCRIPTOR *me, const K &k, V &v) { return set_lookup(sl, k, v); }

  bool insert(DESCRIPTOR *me, const K &key, V &v) {
    K k = key;
    return set_update(sl, k, v, 0);
  }

  bool remove(DESCRIPTOR *me, const K &k) {
    V v;
    return set_remove(sl, k, v);
  }
};
