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

// NB: The tl2 rbtree does not have a sentinel that points to the root.
//     Consequently, we will make this an ownable_t, so it can have an orec to
//     protect the root pointer.
template <typename K, typename V, class HANDSTM, K dummy_key, V dummy_val>
class rbtree_tl2_omap {
  using WOSTM = typename HANDSTM::WOSTM;
  using ROSTM = typename HANDSTM::ROSTM;
  using STM = typename HANDSTM::STM;
  using ownable_t = typename HANDSTM::ownable_t;
  template <typename T> using FIELD = typename HANDSTM::template xField<T>;

  static const int RED = 0;   // Enum for red
  static const int BLACK = 1; // Enum for black

  struct node_t : ownable_t {
    FIELD<K> key;
    FIELD<V> val;
    FIELD<node_t *> p;
    FIELD<node_t *> l;
    FIELD<node_t *> r;
    FIELD<int> c;
    char dummy[64];

    node_t(WOSTM &wo, const K &_key, V &_val) {
      key.set(wo, this, _key);
      val.set(wo, this, _val);
      p.set(wo, this, nullptr);
      l.set(wo, this, nullptr);
      r.set(wo, this, nullptr);
      c.set(wo, this, RED);
    }
  };

  ownable_t root_orec;
  FIELD<node_t *> root;

  char dummy[64];

  node_t *lookup(STM &tx, K k) {
    node_t *p = this->root.get(tx, &root_orec);
    while (p != nullptr) {
      if (k == p->key.get(tx, p)) {
        return p;
      }
      p = (k < p->key.get(tx, p)) ? leftOf(tx, p) : rightOf(tx, p);
    }
    return nullptr;
  }

  void rotateLeft(WOSTM &wo, node_t *x) {
    node_t *r = rightOf(wo, x);
    node_t *rl = leftOf(wo, r);
    setRight(wo, x, rl);
    if (rl != nullptr) {
      setParent(wo, rl, x);
    }

    node_t *xp = parentOf(wo, x);
    setParent(wo, r, xp);
    if (xp == nullptr) {
      this->root.set(wo, &root_orec, r);
    } else if (leftOf(wo, xp) == x) {
      setLeft(wo, xp, r);
    } else {
      setRight(wo, xp, r);
    }
    setLeft(wo, r, x);
    setParent(wo, x, r);
  }

  void rotateRight(WOSTM &wo, node_t *x) {
    node_t *l = leftOf(wo, x);
    node_t *lr = rightOf(wo, l);
    setLeft(wo, x, lr);
    if (lr != nullptr) {
      setParent(wo, lr, x);
    }
    node_t *xp = parentOf(wo, x);
    setParent(wo, l, xp);
    if (xp == nullptr) {
      this->root.set(wo, &root_orec, l);
    } else if (rightOf(wo, xp) == x) {
      setRight(wo, xp, l);
    } else {
      setLeft(wo, xp, l);
    }
    setRight(wo, l, x);
    setParent(wo, x, l);
  }

  node_t *parentOf(WOSTM &wo, node_t *n) {
    return (n ? n->p.get(wo, n) : nullptr);
  }

  node_t *leftOf(STM &tx, node_t *n) { return (n ? n->l.get(tx, n) : nullptr); }

  node_t *rightOf(STM &tx, node_t *n) {
    return (n ? n->r.get(tx, n) : nullptr);
  }

  int colorOf(WOSTM &wo, node_t *n) { return (n ? n->c.get(wo, n) : BLACK); }

  void setColor(WOSTM &wo, node_t *n, int c) {
    if (n != nullptr) {
      n->c.set(wo, n, c);
    }
  }

  void setParent(WOSTM &wo, node_t *n, node_t *p) {
    if (n != nullptr) {
      n->p.set(wo, n, p);
    }
  }

  void setLeft(WOSTM &wo, node_t *n, node_t *l) {
    if (n != nullptr) {
      n->l.set(wo, n, l);
    }
  }

  void setRight(WOSTM &wo, node_t *n, node_t *r) {
    if (n != nullptr) {
      n->r.set(wo, n, r);
    }
  }

  void fixAfterInsertion(WOSTM &wo, node_t *x) {
    setColor(wo, x, RED);
    while (x != nullptr && x != this->root.get(wo, &root_orec)) {
      node_t *xp = parentOf(wo, x);
      if (colorOf(wo, xp) != RED) {
        break;
      }

      if (parentOf(wo, x) == leftOf(wo, parentOf(wo, parentOf(wo, x)))) {
        node_t *y = rightOf(wo, parentOf(wo, parentOf(wo, x)));
        if (colorOf(wo, y) == RED) {
          setColor(wo, parentOf(wo, x), BLACK);
          setColor(wo, y, BLACK);
          setColor(wo, parentOf(wo, parentOf(wo, x)), RED);
          x = parentOf(wo, parentOf(wo, x));
        } else {
          if (x == rightOf(wo, parentOf(wo, x))) {
            x = parentOf(wo, x);
            rotateLeft(wo, x);
          }
          setColor(wo, parentOf(wo, x), BLACK);
          setColor(wo, parentOf(wo, parentOf(wo, x)), RED);
          if (parentOf(wo, parentOf(wo, x)) != nullptr) {
            rotateRight(wo, parentOf(wo, parentOf(wo, x)));
          }
        }
      } else {
        node_t *y = leftOf(wo, parentOf(wo, parentOf(wo, x)));
        if (colorOf(wo, y) == RED) {
          setColor(wo, parentOf(wo, x), BLACK);
          setColor(wo, y, BLACK);
          setColor(wo, parentOf(wo, parentOf(wo, x)), RED);
          x = parentOf(wo, parentOf(wo, x));
        } else {
          if (x == leftOf(wo, parentOf(wo, x))) {
            x = parentOf(wo, x);
            rotateRight(wo, x);
          }
          setColor(wo, parentOf(wo, x), BLACK);
          setColor(wo, parentOf(wo, parentOf(wo, x)), RED);
          if (parentOf(wo, parentOf(wo, x)) != nullptr) {
            rotateLeft(wo, parentOf(wo, parentOf(wo, x)));
          }
        }
      }
    }
    node_t *rt = this->root.get(wo, &root_orec);
    if (colorOf(wo, rt) != BLACK) {
      setColor(wo, rt, BLACK);
    }
  }

  node_t *insertOrGet(WOSTM &wo, K k, V v, node_t *n) {
    node_t *t = this->root.get(wo, &root_orec);
    if (t == nullptr) {
      if (n == nullptr) {
        return nullptr;
      }
      setColor(wo, n, BLACK);
      this->root.set(wo, &root_orec, n);
      return nullptr;
    }

    for (;;) {
      if (k == t->key.get(wo, t)) {
        return t;
      } else if (k < t->key.get(wo, t)) {
        node_t *tl = leftOf(wo, t);
        if (tl != nullptr) {
          t = tl;
        } else {
          setParent(wo, n, t);
          setLeft(wo, t, n);
          fixAfterInsertion(wo, n);
          return nullptr;
        }
      } else {
        node_t *tr = rightOf(wo, t);
        if (tr != nullptr) {
          t = tr;
        } else {
          setParent(wo, n, t);
          setRight(wo, t, n);
          fixAfterInsertion(wo, n);
          return nullptr;
        }
      }
    }
  }

  node_t *successor(WOSTM &wo, node_t *t) {
    if (t == nullptr) {
      return nullptr;
    } else if (rightOf(wo, t) != nullptr) {
      node_t *p = rightOf(wo, t);
      while (leftOf(wo, p) != nullptr) {
        p = leftOf(wo, p);
      }
      return p;
    } else {
      node_t *p = parentOf(wo, t);
      node_t *ch = t;
      while (p != nullptr && ch == rightOf(wo, p)) {
        ch = p;
        p = parentOf(wo, p);
      }
      return p;
    }
  }

  void fixAfterDeletion(WOSTM &wo, node_t *x) {
    while (x != this->root.get(wo, &root_orec) && colorOf(wo, x) == BLACK) {
      if (x == leftOf(wo, parentOf(wo, x))) {
        node_t *sib = rightOf(wo, parentOf(wo, x));
        if (colorOf(wo, sib) == RED) {
          setColor(wo, sib, BLACK);
          setColor(wo, parentOf(wo, x), RED);
          rotateLeft(wo, parentOf(wo, x));
          sib = rightOf(wo, parentOf(wo, x));
        }
        if (colorOf(wo, leftOf(wo, sib)) == BLACK &&
            colorOf(wo, rightOf(wo, sib)) == BLACK) {
          setColor(wo, sib, RED);
          x = parentOf(wo, x);
        } else {
          if (colorOf(wo, rightOf(wo, sib)) == BLACK) {
            setColor(wo, leftOf(wo, sib), BLACK);
            setColor(wo, sib, RED);
            rotateRight(wo, sib);
            sib = rightOf(wo, parentOf(wo, x));
          }
          setColor(wo, sib, colorOf(wo, parentOf(wo, x)));
          setColor(wo, parentOf(wo, x), BLACK);
          setColor(wo, rightOf(wo, sib), BLACK);
          rotateLeft(wo, parentOf(wo, x));

          x = this->root.get(wo, &root_orec);
        }
      } else {
        node_t *sib = leftOf(wo, parentOf(wo, x));
        if (colorOf(wo, sib) == RED) {
          setColor(wo, sib, BLACK);
          setColor(wo, parentOf(wo, x), RED);
          rotateRight(wo, parentOf(wo, x));
          sib = leftOf(wo, parentOf(wo, x));
        }
        if (colorOf(wo, rightOf(wo, sib)) == BLACK &&
            colorOf(wo, leftOf(wo, sib)) == BLACK) {
          setColor(wo, sib, RED);
          x = parentOf(wo, x);
        } else {
          if (colorOf(wo, leftOf(wo, sib)) == BLACK) {
            setColor(wo, rightOf(wo, sib), BLACK);
            setColor(wo, sib, RED);
            rotateLeft(wo, sib);
            sib = leftOf(wo, parentOf(wo, x));
          }
          setColor(wo, sib, colorOf(wo, parentOf(wo, x)));
          setColor(wo, parentOf(wo, x), BLACK);
          setColor(wo, leftOf(wo, sib), BLACK);
          rotateRight(wo, parentOf(wo, x));

          x = this->root.get(wo, &root_orec);
        }
      }
    }

    if (x != nullptr && colorOf(wo, x) != BLACK) {
      setColor(wo, x, BLACK);
    }
  }

  node_t *delete_node(WOSTM &wo, node_t *p) {

    if (leftOf(wo, p) != nullptr && rightOf(wo, p) != nullptr) {
      node_t *s = successor(wo, p);
      ((p)->key) = ((((((s))->key))));
      ((p)->val) = ((((((s))->val))));
      p = s;
    }

    node_t *replacement =
        ((leftOf(wo, p) != nullptr) ? leftOf(wo, p) : rightOf(wo, p));

    if (replacement != nullptr) {
      setParent(wo, replacement, parentOf(wo, p));
      node_t *pp = parentOf(wo, p);
      if (pp == nullptr) {
        this->root.set(wo, &root_orec, replacement);
      } else if (p == leftOf(wo, pp)) {
        setLeft(wo, pp, replacement);
      } else {
        setRight(wo, pp, replacement);
      }

      setLeft(wo, p, nullptr);
      setRight(wo, p, nullptr);
      setParent(wo, p, nullptr);

      if (colorOf(wo, p) == BLACK) {
        fixAfterDeletion(wo, replacement);
      }
    } else if (parentOf(wo, p) == nullptr) {
      this->root.set(wo, &root_orec, nullptr);
    } else {
      if (colorOf(wo, p) == BLACK) {
        fixAfterDeletion(wo, p);
      }
      node_t *pp = parentOf(wo, p);
      if (pp != nullptr) {
        if (p == leftOf(wo, pp)) {
          setLeft(wo, pp, nullptr);
        } else if (p == rightOf(wo, pp)) {
          setRight(wo, pp, nullptr);
        }
        setParent(wo, p, nullptr);
      }
    }
    return p;
  }

  void releaseNode(WOSTM &wo, node_t *n) { wo.reclaim(n); }

  void freeNode(WOSTM &wo, node_t *n) {
    if (n) {
      freeNode(leftOf(wo, n));
      freeNode(rightOf(wo, n));
      releaseNode(n);
    }
  }

public:
  rbtree_tl2_omap(HANDSTM *me, auto *cfg) {
    BEGIN_WO(me);
    root.set(wo, &root_orec, nullptr);
  }

  void rbtree_free(rbtree_tl2_omap *r) {
    freeNode(r->root);
    free(r);
  }

  bool insert(HANDSTM *me, const K &key, V &val) {
    BEGIN_WO(me);
    node_t *node = new node_t(wo, key, val);
    node_t *ex = insertOrGet(wo, key, val, node);
    if (ex != nullptr) {
      releaseNode(wo, node);
    }
    return ((ex == nullptr) ? true : false);
  }

  bool remove(HANDSTM *me, const K &key) {
    BEGIN_WO(me);
    node_t *node = nullptr;
    node = lookup(wo, key);
    if (node != nullptr) {
      node = delete_node(wo, node);
    }
    if (node != nullptr) {
      releaseNode(wo, node);
    }
    return ((node != nullptr) ? true : false);
  }

  bool rbtree_update(HANDSTM *me, K key, V val) {
    BEGIN_WO(me);
    node_t *nn = new node_t(key, val);
    node_t *ex = insertOrGet(wo, key, val, nn);
    if (ex != nullptr) {
      ex->val = val;
      releaseNode(nn);
      return true;
    }
    return false;
  }

  V get(HANDSTM *me, const K &key, V &val) {
    BEGIN_RO(me);
    node_t *n = lookup(ro, key);
    if (n != nullptr) {
      val = n->val.get(ro, n);
      return true;
    }
    return false;
  }

  bool rbtree_contains(K key) {
    node_t *n = lookup(key);
    return (n != nullptr);
  }
};
