#pragma once

#include "../../policies/xSTM/common/tm_api.h"

// NB: We need an operator new().  It can just forward to malloc()
TX_RENAME(_Znwm) void *my_new(std::size_t size) {
  void *ptr = malloc(size);
  return ptr;
}

/// An ordered map, implemented as a balanced, internal binary search tree. This
/// map supports get(), insert(), and remove() operations.
///
/// @param K          The type of the keys stored in this map
/// @param V          The type of the values stored in this map
/// @param DESCRIPTOR A thread descriptor type, for safe memory reclamation
/// @param dummy_key  A default key to use
/// @param dummy_val  A default value to use
template <typename K, typename V, class DESCRIPTOR, K dummy_key, V dummy_val>
class rbtree_omap {
  /// An enum for node colors
  enum Color { RED, BLACK };

  /// nodes in a red/black tree
  struct node_t {
    K key;            // Key stored at this node
    V val;            // Value stored at this node
    Color color;      // color (RED or BLACK)
    node_t *parent;   // pointer to parent
    int ID;           // 0 or 1 to indicate if left or right child
    node_t *child[2]; // pointers to children

    /// basic constructor
    node_t(Color color, K key, V val, node_t *parent, long ID, node_t *child0,
           node_t *child1)
        : key(key), val(val), color(color), parent(parent), ID(ID) {
      TX_CTOR;
      child[0] = child0;
      child[1] = child1;
    }
  };

  node_t *sentinel; // The (sentinel) root node of the tree

public:
  /// Construct a tree by creating a sentinel node at the head
  rbtree_omap(DESCRIPTOR *, auto *)
      : sentinel(new node_t(BLACK, dummy_key, dummy_val, nullptr, 0, nullptr,
                            nullptr)) {}

  // binary search for the node that has v as its value
  bool get(DESCRIPTOR *, const K &key, V &val) const {
    void *dummy;
    bool res = false;
    TX_PRIVATE_STACK_REGION(&dummy);
    {
      TX_RAII;
      const node_t *curr = sentinel->child[0];
      while (curr != nullptr && curr->key != key)
        curr = curr->child[(key < curr->key) ? 0 : 1];
      res = (curr != nullptr) && (curr->key == key);
      if (res)
        val = curr->val;
    }
    return res;
  }

  // insert a node with k/v as its pair if no such key exists in the tree
  bool insert(DESCRIPTOR *, const K &key, V &val) {
    void *dummy;
    bool res = false;
    TX_PRIVATE_STACK_REGION(&dummy);
    {
      TX_RAII;
      // find insertion point
      node_t *curr = sentinel;
      int cID = 0;
      node_t *child = curr->child[cID];
      while (child != nullptr) {
        long ckey = child->key;
        if (ckey == key)
          return false;
        cID = key < ckey ? 0 : 1;
        curr = child;
        child = curr->child[cID];
      }

      // make a red node and connect it to `curr`
      res = true;
      child = new node_t(RED, key, val, curr, cID, nullptr, nullptr);
      curr->child[cID] = child;

      // balance the tree
      while (true) {
        // Get the parent, grandparent, and their relationship
        node_t *parent = child->parent;
        int pID = parent->ID;
        node_t *gparent = parent->parent;

        // Easy exit condition: no more propagation needed
        if ((gparent == sentinel) || (BLACK == parent->color))
          break;

        // If parent's sibling is also red, we push red up to grandparent
        node_t *psib = gparent->child[1 - pID];
        if ((psib != nullptr) && (RED == psib->color)) {
          parent->color = BLACK;
          psib->color = BLACK;
          gparent->color = RED;
          child = gparent;
          continue; // restart loop at gparent level
        }

        int cID = child->ID;
        if (cID != pID) {
          // set child's child to parent's cID'th child
          node_t *baby = child->child[1 - cID];
          parent->child[cID] = baby;
          if (baby != nullptr) {
            baby->parent = parent;
            baby->ID = cID;
          }
          // move parent into baby's position as a child of child
          child->child[1 - cID] = parent;
          parent->parent = child;
          parent->ID = 1 - cID;
          // move child into parent's spot as pID'th child of gparent
          gparent->child[pID] = child;
          child->parent = gparent;
          child->ID = pID;
          // now swap child with curr and fall through
          node_t *temp = child;
          child = parent;
          parent = temp;
        }

        parent->color = BLACK;
        gparent->color = RED;
        // promote parent
        node_t *ggparent = gparent->parent;
        int gID = gparent->ID;
        node_t *ochild = parent->child[1 - pID];
        // make gparent's pIDth child ochild
        gparent->child[pID] = ochild;
        if (ochild != nullptr) {
          ochild->parent = gparent;
          ochild->ID = pID;
        }
        // make gparent the 1-pID'th child of parent
        parent->child[1 - pID] = gparent;
        gparent->parent = parent;
        gparent->ID = 1 - pID;
        // make parent the gIDth child of ggparent
        ggparent->child[gID] = parent;
        parent->parent = ggparent;
        parent->ID = gID;
      }

      // now just set the root to black
      node_t *root = sentinel->child[0];
      if (root->color != BLACK)
        root->color = BLACK;
    }

    return res;
  }

  // remove the node with k as its key if it exists in the tree
  bool remove(DESCRIPTOR *, const K &key) {
    TX_RAII;
    // find key
    node_t *curr = sentinel->child[0];

    while (curr != nullptr) {
      int ckey = curr->key;
      if (ckey == key)
        break;
      curr = curr->child[key < ckey ? 0 : 1];
    }

    // if we didn't find v, we're done
    if (curr == nullptr)
      return false;

    // If `curr` has two children, we need to swap it with its successor
    if ((curr->child[1] != nullptr) && ((curr->child[0]) != nullptr)) {
      node_t *leftmost = curr->child[1];
      while (leftmost->child[0] != nullptr)
        leftmost = leftmost->child[0];
      curr->key = leftmost->key;
      curr->val = leftmost->val;
      curr = leftmost;
    }

    // extract x from the tree and prep it for deletion
    node_t *parent = curr->parent;
    node_t *child = curr->child[(curr->child[0] != nullptr) ? 0 : 1];
    int xID = curr->ID;
    parent->child[xID] = child;
    if (child != nullptr) {
      child->parent = parent;
      child->ID = xID;
    }

    // fix black height violations
    if ((BLACK == curr->color) && (child != nullptr)) {
      if (RED == child->color) {
        curr->color = RED;
        child->color = BLACK;
      }
    }

    // rebalance... be sure to save the deletion target!
    node_t *to_delete = curr;
    while (true) {
      parent = curr->parent;
      if ((parent == sentinel) || (RED == curr->color))
        break;
      int cID = curr->ID;
      node_t *sibling = parent->child[1 - cID];

      // we'd like y's sibling s to be black
      // if it's not, promote it and recolor
      if (RED == sibling->color) {
        /*
            Bp          Bs
           / \         / \
          By  Rs  =>  Rp  B2
          / \        / \
         B1 B2     By  B1
       */
        parent->color = RED;
        sibling->color = BLACK;
        // promote sibling
        node_t *gparent = parent->parent;
        int pID = parent->ID;
        node_t *nephew = sibling->child[cID];
        // set nephew as 1-cID child of parent
        parent->child[1 - cID] = nephew;
        nephew->parent = parent;
        nephew->ID = (1 - cID);
        // make parent the cID child of the sibling
        sibling->child[cID] = parent;
        parent->parent = sibling;
        parent->ID = cID;
        // make sibling the pID child of gparent
        gparent->child[pID] = sibling;
        sibling->parent = gparent;
        sibling->ID = pID;
        // reset sibling
        sibling = nephew;
      }

      // Handle when the far nephew is red
      node_t *n = sibling->child[1 - cID];
      if ((n != nullptr) && (RED == (n->color))) {
        /*
           ?p          ?s
           / \         / \
          By  Bs  =>  Bp  Bn
         / \         / \
        ?1 Rn      By  ?1
        */
        sibling->color = parent->color;
        parent->color = BLACK;
        n->color = BLACK;
        // promote sibling
        node_t *gparent = parent->parent;
        int pID = parent->ID;
        node_t *nephew = sibling->child[cID];
        // make nephew the 1-cID child of parent
        parent->child[1 - cID] = nephew;
        if (nephew != nullptr) {
          nephew->parent = parent;
          nephew->ID = 1 - cID;
        }
        // make parent the cID child of the sibling
        sibling->child[cID] = parent;
        parent->parent = sibling;
        parent->ID = cID;
        // make sibling the pID child of gparent
        gparent->child[pID] = sibling;
        sibling->parent = gparent;
        sibling->ID = pID;
        break; // problem solved
      }

      n = sibling->child[cID];
      if ((n != nullptr) && (RED == (n->color))) {
        /*
             ?p          ?p
             / \         / \
           By  Bs  =>  By  Bn
               / \           \
              Rn B1          Rs
                               \
                               B1
        */
        sibling->color = RED;
        n->color = BLACK;
        // promote n
        node_t *gneph = n->child[1 - cID];
        // make gneph the cID child of sibling
        sibling->child[cID] = gneph;
        if (gneph != nullptr) {
          gneph->parent = sibling;
          gneph->ID = cID;
        }
        // make sibling the 1-cID child of n
        n->child[1 - cID] = sibling;
        sibling->parent = n;
        sibling->ID = 1 - cID;
        // make n the 1-cID child of parent
        parent->child[1 - cID] = n;
        n->parent = parent;
        n->ID = 1 - cID;
        // swap sibling and `n`
        node_t *temp = sibling;
        sibling = n;
        n = temp;

        // now the far nephew is red... copy of code from above
        sibling->color = (parent->color);
        parent->color = BLACK;
        n->color = BLACK;
        // promote sibling
        node_t *gparent = parent->parent;
        int pID = parent->ID;
        node_t *nephew = sibling->child[cID];
        // make nephew the 1-cID child of parent
        parent->child[1 - cID] = nephew;
        if (nephew != nullptr) {
          nephew->parent = parent;
          nephew->ID = 1 - cID;
        }
        // make parent the cID child of the sibling
        sibling->child[cID] = parent;
        parent->parent = sibling;
        parent->ID = cID;
        // make sibling the pID child of gparent
        gparent->child[pID] = sibling;
        sibling->parent = gparent;
        sibling->ID = pID;

        break; // problem solved
      }

      /*
           ?p          ?p
           / \         / \
         Bx  Bs  =>  Bp  Rs
             / \         / \
            B1 B2      B1  B2
       */

      sibling->color = RED; // propagate upwards

      // advance to parent and balance again
      curr = parent;
    }

    // if curr was red, this fixes the balance
    curr->color = BLACK;

    // free the node and return
    free(to_delete);

    return true;
  }
};
