#pragma once

#include "../../policies/xSTM/common/tm_api.h"

// NB: We need an operator new().  It can just forward to malloc()
TX_RENAME(_Znwm) void *my_new(std::size_t size) {
  void *ptr = malloc(size);
  return ptr;
}

/// An ordered map, implemented as an unbalanced, internal binary search tree.
/// This map supports get(), insert(), and remove() operations.
///
/// @param K          The type of the keys stored in this map
/// @param V          The type of the values stored in this map
/// @param DESCRIPTOR A thread descriptor type, for safe memory reclamation
template <typename K, typename V, class DESCRIPTOR> class ibst_omap {

  /// An easy-to-remember way of indicating the left and right children
  enum DIRS { LEFT = 0, RIGHT = 1 };

  /// node_t is the base type for all tree nodes.  It doesn't have key/value
  /// fields.
  struct node_t {
    /// The node's children.  Be sure to use LEFT and RIGHT to index it
    node_t *children[2];

    /// Construct a node_t.  This should only be called from a writer
    /// transaction
    ///
    /// @param _left  The left child of this node
    /// @param _right The right child of this node
    node_t(node_t *_left = nullptr, node_t *_right = nullptr) {
      TX_CTOR;
      children[LEFT] = _left;
      children[RIGHT] = _right;
    }
  };

  /// A pair holding a child node and its parent
  struct ret_pair_t {
    node_t *child;  // The child
    node_t *parent; // The parent of that child
  };

  /// Our tree uses a sentinel root node, so that we always have a valid node
  /// for which to compute an orec.  The sentinel's *LEFT* child is the true
  /// root of the tree.  That is, logically sentinel has the value "TOP".
  node_t *sentinel;

  /// data_t is the type for all internal and leaf nodes in the data structure.
  /// It extends the base type with a key and value.
  ///
  /// NB: keys are *not* const, because we want to overwrite nodes instead of
  ///     swapping them
  struct data_t : public node_t {
    K key; // The key stored in this node
    V val; // The value stored in this node

    /// Construct a node
    ///
    /// @param _left left child of the node
    /// @param _right right child of the node
    /// @param _key the key of the node
    /// @param _val the value of the node
    data_t(node_t *_left, node_t *_right, const K &_key, V &_val)
        : node_t(_left, _right), key(_key), val(_val) {
      TX_CTOR;
    }
  };

public:
  /// Default construct an empty tree
  ///
  /// @param me  The operation that is constructing the list
  /// @param cfg A configuration object
  ibst_omap(DESCRIPTOR *, auto *cfg) {
    // NB: Even though the constructor is operating on private data, it needs a
    //     TM context in order to use tm_fields
    sentinel = new node_t();
  }

private:
  /// Search for a `key` in the tree, and return the node holding it, as well
  /// as the node's parent.  If the key is not found, return null, and the
  /// node that ought to be parent of the (not found) `key`.
  ///
  /// NB: The caller is responsible for clearing the checkpoint stack before
  ///     calling get_node().
  ///
  /// @param key The key to search for
  ///
  /// @return {found, parent} if `key` is in the tree
  ///         {nullptr, parent} if `key` is not in the tree
  ret_pair_t get_node(const K &key) {
    // Traverse downward to the target node:
    node_t *parent = sentinel;
    node_t *child = parent->children[LEFT];

    // Traverse downward from the parent until we find null child or `key`
    while (true) {
      // nullptr == not found, so stop.  We know parent was valid, so we can
      // just return it
      if (!child)
        return {nullptr, parent};

      // It's time to move downward.  Read fields of child and grandchild
      //
      // NB: we may not use grandchild, but it's better to read it here
      auto child_key = static_cast<data_t *>(child)->key;
      auto grandchild = child->children[(key < child_key) ? LEFT : RIGHT];

      // If the child key matches, return {child, parent}.  We know both are
      // valid (parent came from stack; we just checked child)
      //
      // NB: the snapshotting code requires that no node with matching key
      //     goes into `snapshots`
      if (child_key == key)
        return {child, parent};

      // Otherwise traverse downward
      parent = child;
      child = grandchild;
    }
  }

  /// Given a node and its orec value, find the tree node that holds the key
  /// that logically succeeds it (i.e., the leftmost descendent of the right
  /// child)
  ///
  /// NB: The caller must ensure that `node` has a valid right child before
  ///     calling this method
  ///
  /// @param node An object and orec value to use as the starting point
  ///
  /// @return {{found, orec}, {parent, orec}} if no inconsistency occurs
  ///         {{nullptr, 0},  {nullptr, 0}}   on any consistency violation
  ret_pair_t get_succ_pair(node_t *node) {
    // Read the right child
    node_t *parent = node, *child = node->children[RIGHT];

    // Find the leftmost non-null node in the tree rooted at child
    while (true) {
      auto next = child->children[LEFT];
      // If next is null, `child` is the successor.  Otherwise keep traversing
      if (!next)
        return {child, parent};
      parent = child;
      child = next;
    }
  }

public:
  /// Search the data structure for a node with key `key`.  If not found, return
  /// false.  If found, return true, and set `val` to the value associated with
  /// `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key to search
  /// @param val A ref parameter for returning key's value, if found
  ///
  /// @return True if the key is found, false otherwise.  The reference
  ///         parameter `val` is only valid when the return value is true.
  bool get(DESCRIPTOR *, const K &key, V &val) {
    TX_RAII;
    // Get the node that holds `key`, if it is present, and also its parent.
    // If it isn't present, we'll get a null pointer.  That corresponds to a
    // consistent read of the parent, which means we already linearized and
    // we're done
    auto [curr, _] = get_node(key);
    if (curr == nullptr)
      return false;

    // read the value
    auto dn = static_cast<data_t *>(curr);
    val = dn->val;
    return true;
  }

  /// Create a mapping from the provided `key` to the provided `val`, but only
  /// if no such mapping already exists.  This method does *not* have upsert
  /// behavior for keys already present.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to create
  /// @param val The value for the mapping to create
  ///
  /// @return True if the value was inserted, false otherwise.
  bool insert(DESCRIPTOR *, const K &key, V &val) {
    TX_RAII;
    auto [child, parent] = get_node(key);
    if (child)
      return false;
    // We must have a null child and a valid parent.  If it's sentinel, we
    // must insert as LEFT.  Otherwise, compute which child to set.
    auto cID = (parent == sentinel ? LEFT : RIGHT) &
               (key > static_cast<data_t *>(parent)->key);
    auto new_child = new data_t(nullptr, nullptr, key, val);
    parent->children[cID] = new_child;
    return true;
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(DESCRIPTOR *, const K &key) {
    TX_RAII;
    auto [target, parent] = get_node(key);
    if (target == nullptr)
      return false;

    // Read the target node's children
    data_t *t_child[2];
    t_child[RIGHT] = static_cast<data_t *>(target->children[RIGHT]);
    t_child[LEFT] = static_cast<data_t *>(target->children[LEFT]);

    // If either child is null, and if the parent is still valid, then we can
    // unstitch the target, link the parent to a grandchild and we're done.
    if (!t_child[LEFT] || !t_child[RIGHT]) {
      // Acquire the (possibly null) grandchild to link to the parent
      auto gID = t_child[LEFT] ? LEFT : RIGHT;

      // Which child of the parent is target?
      auto cID = parent->children[LEFT] == target ? LEFT : RIGHT;

      // Unstitch and reclaim
      parent->children[cID] = t_child[gID];
      delete (target);
      return true;
    }

    // `target` has two children.  WLOG, the leftmost descendent of the right
    // child is `target`'s successor, and must have at most one child.  We
    // want to put that node's key and value into `target`, and then remove
    // that node by setting its parent's LEFT to its RIGHT (which might be
    // null).
    auto [succ, s_parent] = get_succ_pair(target);

    // If target's successor is target's right child, then target._ver must
    // equal s_parent._ver.  As long as we lock target before we try
    // to lock s_parent, we'll get the check for free.

    // Copy `succ`'s key/value into `target`
    static_cast<data_t *>(target)->key = static_cast<data_t *>(succ)->key;
    static_cast<data_t *>(target)->val = static_cast<data_t *>(succ)->val;

    // Unstitch `succ` by setting its parent's left to its right
    // Case 1: there are intermediate nodes between target and successor
    if (s_parent != target)
      s_parent->children[LEFT] = succ->children[RIGHT];
    // Case 2: target is successor's parent
    else
      s_parent->children[RIGHT] = succ->children[RIGHT];
    delete (succ);
    return true;
  }
};
