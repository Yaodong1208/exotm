/* =============================================================================
 *
 * rbtree.h
 * -- Red-black balanced binary search tree
 *
 * =============================================================================
 *
 * Copyright (C) Sun Microsystems Inc., 2006.  All Rights Reserved.
 * Authors: Dave Dice, Nir Shavit, Ori Shalev.
 *
 * STM: Transactional Locking for Disjoint Access Parallelism
 *
 * Transactional Locking II,
 * Dave Dice, Ori Shalev, Nir Shavit
 * DISC 2006, Sept 2006, Stockholm, Sweden.
 */

#pragma once

#include "../../policies/xSTM/common/tm_api.h"

template <typename K, typename V, class DESCRIPTOR, K dummy_key, V dummy_val>
class rbtree_tl2_omap {

  static const int RED = 0;
  static const int BLACK = 1;

  struct node_t {
    K key;
    V val;
    node_t *p;
    node_t *l;
    node_t *r;
    long c;
    char dummy[64];
  };

  node_t *root;
  char dummy[64];

  node_t *lookup(K k) {
    node_t *p = this->root;
    while (p != nullptr) {
      if (k == p->key) {
        return p;
      }
      p = (k < p->key) ? p->l : p->r;
    }
    return nullptr;
  }

  void rotateLeft(node_t *x) {
    node_t *r = x->r;
    node_t *rl = r->l;
    x->r = rl;
    if (rl != nullptr) {
      rl->p = (x);
    }

    node_t *xp = (((((x))->p)));
    ((r)->p) = (xp);
    if (xp == nullptr) {
      ((this)->root) = (r);
    } else if ((((((xp))->l))) == x) {
      ((xp)->l) = (r);
    } else {
      ((xp)->r) = (r);
    }
    ((r)->l) = (x);
    ((x)->p) = (r);
  }

  void rotateRight(node_t *x) {
    node_t *l = (((((x))->l)));
    node_t *lr = (((((l))->r)));
    ((x)->l) = (lr);
    if (lr != nullptr) {
      ((lr)->p) = (x);
    }
    node_t *xp = (((((x))->p)));
    ((l)->p) = (xp);
    if (xp == nullptr) {
      ((this)->root) = (l);
    } else if ((((((xp))->r))) == x) {
      ((xp)->r) = (l);
    } else {
      ((xp)->l) = (l);
    }
    ((l)->r) = (x);
    ((x)->p) = (l);
  }

  node_t *parentOf(node_t *n) { return (n ? (((((n))->p))) : nullptr); }

  node_t *leftOf(node_t *n) { return (n ? (((((n))->l))) : nullptr); }

  node_t *rightOf(node_t *n) { return (n ? (((((n))->r))) : nullptr); }

  long colorOf(node_t *n) { return (n ? (long)(((((n))->c))) : BLACK); }

  void setColor(node_t *n, long c) {
    if (n != nullptr) {
      ((n)->c) = (c);
    }
  }

  void fixAfterInsertion(node_t *x) {
    ((x)->c) = (RED);
    while (x != nullptr && x != (((((this))->root)))) {
      node_t *xp = (((((x))->p)));
      if (((xp)->c) != RED) {
        break;
      }

      if (parentOf(x) == leftOf(parentOf(parentOf(x)))) {
        node_t *y = rightOf(parentOf(parentOf(x)));
        if (colorOf(y) == RED) {
          setColor(parentOf(x), BLACK);
          setColor(y, BLACK);
          setColor(parentOf(parentOf(x)), RED);
          x = parentOf(parentOf(x));
        } else {
          if (x == rightOf(parentOf(x))) {
            x = parentOf(x);
            rotateLeft(x);
          }
          setColor(parentOf(x), BLACK);
          setColor(parentOf(parentOf(x)), RED);
          if (parentOf(parentOf(x)) != nullptr) {
            rotateRight(parentOf(parentOf(x)));
          }
        }
      } else {
        node_t *y = leftOf(parentOf(parentOf(x)));
        if (colorOf(y) == RED) {
          setColor(parentOf(x), BLACK);
          setColor(y, BLACK);
          setColor(parentOf(parentOf(x)), RED);
          x = parentOf(parentOf(x));
        } else {
          if (x == leftOf(parentOf(x))) {
            x = parentOf(x);
            rotateRight(x);
          }
          setColor(parentOf(x), BLACK);
          setColor(parentOf(parentOf(x)), RED);
          if (parentOf(parentOf(x)) != nullptr) {
            rotateLeft(parentOf(parentOf(x)));
          }
        }
      }
    }
    node_t *ro = (((((this))->root)));
    if (((ro)->c) != BLACK) {
      ((ro)->c) = (BLACK);
    }
  }

  node_t *insertIt(K k, V v, node_t *n) {
    node_t *t = (((((this))->root)));
    if (t == nullptr) {
      if (n == nullptr) {
        return nullptr;
      }

      ((n)->l) = (nullptr);
      ((n)->r) = (nullptr);
      ((n)->p) = (nullptr);
      ((n)->key) = (k);
      ((n)->val) = (v);
      ((n)->c) = (BLACK);
      ((this)->root) = (n);
      return nullptr;
    }

    for (;;) {
      if (k == t->key) {
        return t;
      } else if (k < t->key) {
        node_t *tl = (((((t))->l)));
        if (tl != nullptr) {
          t = tl;
        } else {
          ((n)->l) = (nullptr);
          ((n)->r) = (nullptr);
          ((n)->key) = (k);
          ((n)->val) = (v);
          ((n)->p) = (t);
          ((t)->l) = (n);
          fixAfterInsertion(n);
          return nullptr;
        }
      } else {
        node_t *tr = (((((t))->r)));
        if (tr != nullptr) {
          t = tr;
        } else {
          ((n)->l) = (nullptr);
          ((n)->r) = (nullptr);
          ((n)->key) = (k);
          ((n)->val) = (v);
          ((n)->p) = (t);
          ((t)->r) = (n);
          fixAfterInsertion(n);
          return nullptr;
        }
      }
    }
  }

  node_t *successor(node_t *t) {
    if (t == nullptr) {
      return nullptr;
    } else if ((((((t))->r))) != nullptr) {
      node_t *p = (((((t))->r)));
      while ((((((p))->l))) != nullptr) {
        p = (((((p))->l)));
      }
      return p;
    } else {
      node_t *p = (((((t))->p)));
      node_t *ch = t;
      while (p != nullptr && ch == (((((p))->r)))) {
        ch = p;
        p = (((((p))->p)));
      }
      return p;
    }
  }

  void fixAfterDeletion(node_t *x) {
    while (x != (((((this))->root))) && colorOf(x) == BLACK) {
      if (x == leftOf(parentOf(x))) {
        node_t *sib = rightOf(parentOf(x));
        if (colorOf(sib) == RED) {
          setColor(sib, BLACK);
          setColor(parentOf(x), RED);
          rotateLeft(parentOf(x));
          sib = rightOf(parentOf(x));
        }
        if (colorOf(leftOf(sib)) == BLACK && colorOf(rightOf(sib)) == BLACK) {
          setColor(sib, RED);
          x = parentOf(x);
        } else {
          if (colorOf(rightOf(sib)) == BLACK) {
            setColor(leftOf(sib), BLACK);
            setColor(sib, RED);
            rotateRight(sib);
            sib = rightOf(parentOf(x));
          }
          setColor(sib, colorOf(parentOf(x)));
          setColor(parentOf(x), BLACK);
          setColor(rightOf(sib), BLACK);
          rotateLeft(parentOf(x));

          x = (((((this))->root)));
        }
      } else {
        node_t *sib = leftOf(parentOf(x));
        if (colorOf(sib) == RED) {
          setColor(sib, BLACK);
          setColor(parentOf(x), RED);
          rotateRight(parentOf(x));
          sib = leftOf(parentOf(x));
        }
        if (colorOf(rightOf(sib)) == BLACK && colorOf(leftOf(sib)) == BLACK) {
          setColor(sib, RED);
          x = parentOf(x);
        } else {
          if (colorOf(leftOf(sib)) == BLACK) {
            setColor(rightOf(sib), BLACK);
            setColor(sib, RED);
            rotateLeft(sib);
            sib = leftOf(parentOf(x));
          }
          setColor(sib, colorOf(parentOf(x)));
          setColor(parentOf(x), BLACK);
          setColor(leftOf(sib), BLACK);
          rotateRight(parentOf(x));

          x = (((((this))->root)));
        }
      }
    }

    if (x != nullptr && ((x)->c) != BLACK) {
      ((x)->c) = (BLACK);
    }
  }

  node_t *delete_node(node_t *p) {

    if ((((((p))->l))) != nullptr && (((((p))->r))) != nullptr) {
      node_t *s = successor(p);
      ((p)->key) = ((((((s))->key))));
      ((p)->val) = ((((((s))->val))));
      p = s;
    }

    node_t *replacement =
        (((((((p))->l))) != nullptr) ? (((((p))->l))) : (((((p))->r))));

    if (replacement != nullptr) {

      ((replacement)->p) = ((((((p))->p))));
      node_t *pp = (((((p))->p)));
      if (pp == nullptr) {
        ((this)->root) = (replacement);
      } else if (p == (((((pp))->l)))) {
        ((pp)->l) = (replacement);
      } else {
        ((pp)->r) = (replacement);
      }

      ((p)->l) = (nullptr);
      ((p)->r) = (nullptr);
      ((p)->p) = (nullptr);

      if (((p)->c) == BLACK) {
        fixAfterDeletion(replacement);
      }
    } else if ((((((p))->p))) == nullptr) {
      ((this)->root) = (nullptr);
    } else {
      if (((p)->c) == BLACK) {
        fixAfterDeletion(p);
      }
      node_t *pp = (((((p))->p)));
      if (pp != nullptr) {
        if (p == (((((pp))->l)))) {
          ((pp)->l) = (nullptr);
        } else if (p == (((((pp))->r)))) {
          ((pp)->r) = (nullptr);
        }
        ((p)->p) = (nullptr);
      }
    }
    return p;
  }

  long compareKeysDefault(const void *a, const void *b) {
    return ((long)a - (long)b);
  }

  void releaseNode(node_t *n) { free(n); }

  void freeNode(node_t *n) {
    if (n) {
      freeNode(n->l);
      freeNode(n->r);
      releaseNode(n);
    }
  }

  node_t *getNode() {
    node_t *n = (node_t *)malloc(sizeof(*n));
    return n;
  }

public:
  rbtree_tl2_omap(DESCRIPTOR *me, auto *cfg) : root(nullptr) {}

  void rbtree_free(rbtree_tl2_omap *r) {
    freeNode(r->root);
    free(r);
  }

  bool insert(DESCRIPTOR *me, const K &key, V &val) {
    void *dummy;
    TX_PRIVATE_STACK_REGION(&dummy);
    {
      TX_RAII;
      node_t *node = getNode();
      node_t *ex = insertIt(key, val, node);
      if (ex != nullptr) {
        releaseNode(node);
      }
      return ((ex == nullptr) ? true : false);
    }
  }

  bool remove(DESCRIPTOR *me, const K &key) {
    void *dummy;
    TX_PRIVATE_STACK_REGION(&dummy);
    {
      TX_RAII;
      node_t *node = nullptr;
      node = lookup(key);
      if (node != nullptr) {
        node = delete_node(node);
      }
      if (node != nullptr) {
        releaseNode(node);
      }
      return ((node != nullptr) ? true : false);
    }
  }

  bool rbtree_update(K key, V val) {
    node_t *nn = getNode();
    node_t *ex = insert(key, val, nn);
    if (ex != nullptr) {
      ((ex)->val) = (val);
      releaseNode(nn);
      return true;
    }
    return false;
  }

  V get(DESCRIPTOR *me, const K &key, V &val) {
    void *dummy;
    TX_PRIVATE_STACK_REGION(&dummy);
    {
      TX_RAII;
      node_t *n = lookup(key);
      if (n != nullptr) {
        val = ((n)->val);
        return true;
      }
      return false;
    }
  }

  long rbtree_contains(K key) {
    node_t *n = lookup(key);
    return (n != nullptr);
  }
};
