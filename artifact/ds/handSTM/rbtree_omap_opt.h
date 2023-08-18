#pragma once

/// An ordered map, implemented as a balanced, internal binary search tree. This
/// map supports get(), insert(), and remove() operations.
///
/// @param K          The type of the keys stored in this map
/// @param V          The type of the values stored in this map
/// @param HANDSTM    A thread descriptor type, for safe memory reclamation
/// @param dummy_key  A default key to use
/// @param dummy_val  A default value to use
template <typename K, typename V, class HANDSTM, K dummy_key, V dummy_val>
class rbtree_omap_opt {
  using WOSTM = typename HANDSTM::WOSTM;
  using ROSTM = typename HANDSTM::ROSTM;
  using STM = typename HANDSTM::STM;
  using ownable_t = typename HANDSTM::ownable_t;
  template <typename T> using FIELD = typename HANDSTM::template xField<T>;

  static const int RED = 0;   // Enum for red
  static const int BLACK = 1; // Enum for black

  /// nodes in a red/black tree
  struct node_t : ownable_t {
    FIELD<K> key;             // Key stored at this node
    FIELD<V> val;             // Value stored at this node
    FIELD<int> color;         // color (RED or BLACK)
    FIELD<node_t *> parent;   // pointer to parent
    FIELD<int> ID;            // 0/1 for left/right child
    FIELD<node_t *> child[2]; // L/R children

    /// basic constructor
    node_t(WOSTM &wo, int color, K key, V val, node_t *parent, long ID,
           node_t *child0, node_t *child1)
        : key(key), val(val), color(color), parent(parent), ID(ID) {
      child[0].set_cap(wo, this, child0);
      child[1].set_cap(wo, this, child1);
    }
  };

  node_t *sentinel; // The (sentinel) root node of the tree

public:
  /// Construct a list by creating a sentinel node at the head
  rbtree_omap_opt(HANDSTM *me, auto *) {
    BEGIN_WO(me);
    sentinel = new node_t(wo, BLACK, dummy_key, dummy_val, nullptr, 0, nullptr,
                          nullptr);
  }

  // binary search for the node that has v as its value
  bool get(HANDSTM *me, const K &key, V &val) const {
    BEGIN_RO(me);
    node_t *curr = sentinel->child[0].get(ro, sentinel);
    K k = key;
    while (true) {
      if (curr == nullptr)
        break;
      k = curr->key.get(ro, curr);
      if (k == key)
        break;
      curr = curr->child[key < k ? 0 : 1].re_get(ro, curr);
    }
    bool res = (curr != nullptr) && (k == key);
    if (res)
      val = curr->val.re_get(ro, curr);
    return res;
  }

  // insert a node with k/v as its pair if no such key exists in the tree
  bool insert(HANDSTM *me, const K &key, V &val) {
    bool res = false;
    {
      BEGIN_WO(me);
      // find insertion point
      node_t *curr = sentinel;
      int cID = 0;
      node_t *child = curr->child[cID].get(wo, curr);
      while (child != nullptr) {
        auto ckey = child->key.get(wo, child);
        if (ckey == key)
          return false;
        cID = key < ckey ? 0 : 1;
        curr = child;
        child = curr->child[cID].re_get(wo, curr);
      }

      // make a red node and connect it to `curr`
      res = true;
      child = new node_t(wo, RED, key, val, curr, cID, nullptr, nullptr);
      curr->child[cID].set(wo, curr, child);

      // balance the tree
      while (true) {
        // Get the parent, grandparent, and their relationship
        // NB: child is captured or owned
        node_t *parent = child->parent.get_mine(wo, child);
        int pID = parent->ID.get(wo, parent);
        auto parentcolor = parent->color.get_in_seq(wo, parent);
        node_t *gparent = parent->parent.re_get(wo, parent);

        // Easy exit condition: no more propagation needed
        if ((gparent == sentinel) || (BLACK == parentcolor))
          break;

        // If parent's sibling is also red, we push red up to grandparent
        node_t *psib = gparent->child[1 - pID].get(wo, gparent);
        if ((psib != nullptr) && (RED == psib->color.get(wo, psib))) {
          parent->color.set(wo, parent, BLACK);
          psib->color.set(wo, psib, BLACK);
          gparent->color.set(wo, gparent, RED);
          child = gparent;
          continue; // restart loop at gparent level
        }

        int cID = child->ID.get_mine(wo, child);
        if (cID != pID) {
          // set child's child to parent's cID'th child
          node_t *baby = child->child[1 - cID].get_mine(wo, child);
          parent->child[cID].set(wo, parent, baby);
          if (baby != nullptr) {
            baby->parent.set(wo, baby, parent);
            baby->ID.set_mine(wo, baby, cID);
          }
          // move parent into baby's position as a child of child
          child->child[1 - cID].set_mine(wo, child, parent);
          parent->parent.set_mine(wo, parent, child);
          parent->ID.set_mine(wo, parent, 1 - cID);
          // move child into parent's spot as pID'th child of gparent
          gparent->child[pID].set(wo, gparent, child);
          child->parent.set_mine(wo, child, gparent);
          child->ID.set_mine(wo, child, pID);
          // now swap child with parent and fall through
          node_t *temp = child;
          child = parent;
          parent = temp;
        }

        parent->color.set(wo, parent, BLACK);
        gparent->color.set(wo, gparent, RED);
        // promote parent
        node_t *ggparent = gparent->parent.get_mine(wo, gparent);
        int gID = gparent->ID.get_mine(wo, gparent);
        node_t *ochild = parent->child[1 - pID].get_mine(wo, parent);
        // make gparent's pIDth child ochild
        gparent->child[pID].set_mine(wo, gparent, ochild);
        if (ochild != nullptr) {
          ochild->parent.set(wo, ochild, gparent);
          ochild->ID.set_mine(wo, ochild, pID);
        }
        // make gparent the 1-pID'th child of parent
        parent->child[1 - pID].set_mine(wo, parent, gparent);
        gparent->parent.set_mine(wo, gparent, parent);
        gparent->ID.set_mine(wo, gparent, 1 - pID);
        // make parent the gIDth child of ggparent
        ggparent->child[gID].set(wo, ggparent, parent);
        parent->parent.set_mine(wo, parent, ggparent);
        parent->ID.set_mine(wo, parent, gID);
      }

      // now just set the root to black
      node_t *root = sentinel->child[0].get(wo, sentinel);
      if (root->color.get(wo, root) != BLACK)
        root->color.set(wo, root, BLACK);
    }

    return res;
  }

  // remove the node with k as its key if it exists in the tree
  bool remove(HANDSTM *me, const K &key) {
    BEGIN_WO(me);
    // find key
    node_t *curr = sentinel->child[0].get(wo, sentinel);

    while (curr != nullptr) {
      auto ckey = curr->key.get(wo, curr);
      if (ckey == key)
        break;
      curr = curr->child[key < ckey ? 0 : 1].re_get(wo, curr);
    }

    // if we didn't find v, we're done
    if (curr == nullptr)
      return false;

    // If `curr` has two children, we need to swap it with its successor
    if ((curr->child[1].get(wo, curr) != nullptr) &&
        ((curr->child[0].get(wo, curr)) != nullptr)) {
      auto lchild = curr->child[0].get_in_seq(wo, curr);
      auto rchild = curr->child[1].re_get(wo, curr);
      if ((lchild != nullptr) && (rchild != nullptr)) {
        node_t *leftmost = rchild;
        while (true) {
          auto next = leftmost->child[0].get(wo, leftmost);
          if (next == nullptr)
            break;
          leftmost = next;
        }
        auto lk = leftmost->key.get_in_seq(wo, leftmost);
        auto lv = leftmost->val.re_get(wo, leftmost);
        curr->key.set(wo, curr, lk);
        curr->val.set_mine(wo, curr, lv);
        curr = leftmost;
      }
    }

    // extract x from the tree and prep it for deletion
    node_t *parent = curr->parent.get(wo, curr);
    node_t *lchild = curr->child[0].get_in_seq(wo, curr);
    node_t *rchild = curr->child[1].get_in_seq(wo, curr);
    node_t *child = (lchild != nullptr) ? lchild : rchild;
    int xID = curr->ID.re_get(wo, curr);
    parent->child[xID].set(wo, parent, child);
    if (child != nullptr) {
      child->parent.set(wo, child, parent);
      child->ID.set_mine(wo, child, xID);
    }

    // fix black height violations
    if ((BLACK == curr->color.re_get(wo, curr)) && (child != nullptr)) {
      if (RED == child->color.get_mine(wo, child)) {
        curr->color.set(wo, curr, RED);
        child->color.set_mine(wo, child, BLACK);
      }
    }

    // rebalance... be sure to save the deletion target!
    node_t *to_delete = curr;
    while (true) {
      parent = curr->parent.get(wo, curr);
      if ((parent == sentinel) || (RED == curr->color.re_get(wo, curr)))
        break;
      int cID = curr->ID.re_get(wo, curr);
      node_t *sibling = parent->child[1 - cID].get(wo, parent);

      // we'd like y's sibling s to be black
      // if it's not, promote it and recolor
      if (RED == sibling->color.get(wo, sibling)) {
        /*
            Bp          Bs
           / \         / \
          By  Rs  =>  Rp  B2
          / \        / \
         B1 B2     By  B1
       */
        parent->color.set(wo, parent, RED);
        sibling->color.set(wo, sibling, BLACK);
        // promote sibling
        node_t *gparent = parent->parent.get_mine(wo, parent);
        int pID = parent->ID.get_mine(wo, parent);
        node_t *nephew = sibling->child[cID].get_mine(wo, sibling);
        // set nephew as 1-cID child of parent
        parent->child[1 - cID].set_mine(wo, parent, nephew);
        nephew->parent.set(wo, nephew, parent);
        nephew->ID.set_mine(wo, nephew, 1 - cID);
        // make parent the cID child of the sibling
        sibling->child[cID].set_mine(wo, sibling, parent);
        parent->parent.set_mine(wo, parent, sibling);
        parent->ID.set_mine(wo, parent, cID);
        // make sibling the pID child of gparent
        gparent->child[pID].set(wo, gparent, sibling);
        sibling->parent.set_mine(wo, sibling, gparent);
        sibling->ID.set_mine(wo, sibling, pID);
        // reset sibling
        sibling = nephew;
      }

      // Handle when the far nephew is red
      node_t *n = sibling->child[1 - cID].re_get(wo, sibling);
      if ((n != nullptr) && (RED == (n->color.get(wo, n)))) {
        /*
           ?p          ?s
           / \         / \
          By  Bs  =>  Bp  Bn
         / \         / \
        ?1 Rn      By  ?1
        */
        sibling->color.set(wo, sibling, parent->color.re_get(wo, parent));
        parent->color.set(wo, parent, BLACK);
        n->color.set(wo, n, BLACK);
        // promote sibling
        node_t *gparent = parent->parent.get_mine(wo, parent);
        int pID = parent->ID.get_mine(wo, parent);
        node_t *nephew = sibling->child[cID].get_mine(wo, sibling);
        // make nephew the 1-cID child of parent
        parent->child[1 - cID].set_mine(wo, parent, nephew);
        if (nephew != nullptr) {
          nephew->parent.set(wo, nephew, parent);
          nephew->ID.set_mine(wo, nephew, 1 - cID);
        }
        // make parent the cID child of the sibling
        sibling->child[cID].set_mine(wo, sibling, parent);
        parent->parent.set_mine(wo, parent, sibling);
        parent->ID.set_mine(wo, parent, cID);
        // make sibling the pID child of gparent
        gparent->child[pID].set(wo, gparent, sibling);
        sibling->parent.set_mine(wo, sibling, gparent);
        sibling->ID.set_mine(wo, sibling, pID);
        break; // problem solved
      }

      n = sibling->child[cID].re_get(wo, sibling);
      if ((n != nullptr) && (RED == (n->color.get(wo, n)))) {
        /*
             ?p          ?p
             / \         / \
           By  Bs  =>  By  Bn
               / \           \
              Rn B1          Rs
                               \
                               B1
        */
        sibling->color.set(wo, sibling, RED);
        n->color.set(wo, n, BLACK);
        // promote n
        node_t *gneph = n->child[1 - cID].get_mine(wo, n);
        // make gneph the cID child of sibling
        sibling->child[cID].set_mine(wo, sibling, gneph);
        if (gneph != nullptr) {
          gneph->parent.set(wo, gneph, sibling);
          gneph->ID.set_mine(wo, gneph, cID);
        }
        // make sibling the 1-cID child of n
        n->child[1 - cID].set_mine(wo, n, sibling);
        sibling->parent.set_mine(wo, sibling, n);
        sibling->ID.set_mine(wo, sibling, 1 - cID);
        // make n the 1-cID child of parent
        parent->child[1 - cID].set(wo, parent, n);
        n->parent.set_mine(wo, n, parent);
        n->ID.set_mine(wo, n, 1 - cID);
        // swap sibling and `n`
        node_t *temp = sibling;
        sibling = n;
        n = temp;

        // now the far nephew is red... copy of code from above
        sibling->color.set_mine(wo, sibling,
                                parent->color.get_mine(wo, parent));
        parent->color.set_mine(wo, parent, BLACK);
        n->color.set_mine(wo, n, BLACK);
        // promote sibling
        node_t *gparent = parent->parent.get_mine(wo, parent);
        int pID = parent->ID.get_mine(wo, parent);
        node_t *nephew = sibling->child[cID].get_mine(wo, sibling);
        // make nephew the 1-cID child of parent
        parent->child[1 - cID].set_mine(wo, parent, nephew);
        if (nephew != nullptr) {
          nephew->parent.set(wo, nephew, parent);
          nephew->ID.set_mine(wo, nephew, 1 - cID);
        }
        // make parent the cID child of the sibling
        sibling->child[cID].set_mine(wo, sibling, parent);
        parent->parent.set_mine(wo, parent, sibling);
        parent->ID.set_mine(wo, parent, cID);
        // make sibling the pID child of gparent
        gparent->child[pID].set(wo, gparent, sibling);
        sibling->parent.set_mine(wo, sibling, gparent);
        sibling->ID.set_mine(wo, sibling, pID);

        break; // problem solved
      }

      /*
           ?p          ?p
           / \         / \
         Bx  Bs  =>  Bp  Rs
             / \         / \
            B1 B2      B1  B2
       */

      sibling->color.set(wo, sibling, RED); // propagate upwards

      // advance to parent and balance again
      curr = parent;
    }

    // if curr was red, this fixes the balance
    curr->color.set(wo, curr, BLACK);

    // free the node and return
    wo.reclaim(to_delete);

    return true;
  }
};
