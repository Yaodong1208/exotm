#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

/// An ordered map, implemented as an unbalanced, internal binary search tree.
/// This map supports get(), insert(), and remove() operations.
///
/// @param K      The type of the keys stored in this map
/// @param V      The type of the values stored in this map
/// @param STMCAS The STMCAS implementation (PO or PS)
template <typename K, typename V, class STMCAS> class ibst_omap {
  using WSTEP = typename STMCAS::WSTEP;
  using RSTEP = typename STMCAS::RSTEP;
  using snapshot_t = typename STMCAS::snapshot_t;
  using ownable_t = typename STMCAS::ownable_t;
  template <typename T> using FIELD = typename STMCAS::template sField<T>;

  /// An easy-to-remember way of indicating the left and right children
  enum DIRS { LEFT = 0, RIGHT = 1 };

  /// node_t is the base type for all tree nodes.  It doesn't have key/value
  /// fields.
  struct node_t : public ownable_t {
    /// The node's children.  Be sure to use LEFT and RIGHT to index it
    FIELD<node_t *> children[2];

    /// Construct a node_t.  This should only be called from a writer
    /// transaction
    ///
    /// @param tx     A writing transactional context
    /// @param _left  The left child of this node
    /// @param _right The right child of this node
    node_t(WSTEP &tx, node_t *_left = nullptr, node_t *_right = nullptr)
        : ownable_t() {
      children[LEFT].set(_left, tx);
      children[RIGHT].set(_right, tx);
    }
  };

  /// A pair with ownable and orec value; equivalent to the type in snapshots
  struct leq_t {
    node_t *_obj = nullptr; // The object
    uint64_t _ver = 0;      // The observed version of the object
  };

  /// A pair holding a child node and its parent, with orec validation info
  struct ret_pair_t {
    leq_t child;  // The child
    leq_t parent; // The parent of that child
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
    FIELD<K> key; // The key stored in this node
    V val;        // The value stored in this node

    /// Construct a node
    ///
    /// @param tx a WSTEP_TM reference
    /// @param _left left child of the node
    /// @param _right right child of the node
    /// @param _key the key of the node
    /// @param _val the value of the node
    data_t(WSTEP &tx, node_t *_left, node_t *_right, const K &_key, V &_val)
        : node_t(tx, _left, _right), key(_key), val(_val) {}
  };

public:
  /// Default construct an empty tree
  ///
  /// @param _op The operation that is constructing the list
  /// @param cfg A configuration object
  ibst_omap(STMCAS *me, auto *cfg) {
    // NB: Even though the constructor is operating on private data, it needs a
    //     TM context in order to use tm_fields
    WSTEP tx(me);
    sentinel = new node_t(tx);
  }

private:
  /// Search for a `key` in the tree, and return the node holding it, as well
  /// as the node's parent.  If the key is not found, return null, and the
  /// node that ought to be parent of the (not found) `key`.
  ///
  /// NB: The caller is responsible for clearing the checkpoint stack before
  ///     calling get_node().
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key to search for
  ///
  /// @return {{found, orec}, {parent, orec}} if `key` is in the tree
  ///         {{nullptr, 0},  {parent, orec}} if `key` is not in the tree
  ret_pair_t get_node(STMCAS *me, const K &key) {
    // This loop delineates the search transaction.  It commences from the end
    // of the longest consistent prefix in the checkpoint stack
    while (true) {
      // Open a RSTEP transaction to traverse downward to the target node:
      leq_t parent = {nullptr, 0}, child = {nullptr, 0};
      RSTEP tx(me);

      // Validate the checkpoints to find a starting point.  When this is done,
      // there must be at least one entry in the checkpoints (the sentinel), and
      // it must be valid.
      //
      // NB: When this step is done, the curr->child relationship is validated,
      //     but we haven't read any of child's fields, or checked child's orec.
      //     Every checkpointed node must be valid at the time of checkpointing.
      //

      // If the stack is empty or only holds the sentinel, start from {sentinel,
      // root}
      if (me->snapshots.size() <= 1) {
        parent._obj = sentinel;
        child._obj = parent._obj->children[LEFT].get(tx);
        parent._ver = tx.check_orec(parent._obj);
        if (parent._ver == STMCAS::END_OF_TIME)
          continue; // retry
        me->snapshots.clear();
        me->snapshots.push_back({parent._obj, parent._ver});
      }
      // If the stack is larger, we can find the longest valid prefix
      else {
        // Trim the stack to a set of consistent checkpoints
        for (auto cp = me->snapshots.begin(); cp != me->snapshots.end(); ++cp) {
          if (!tx.check_continuation(cp->_obj, cp->_ver)) {
            me->snapshots.reset(cp - me->snapshots.begin());
            break; // the rest of the checkpoints aren't valid
          }
        }
        // If we don't have more than a sentinel, restart
        if (me->snapshots.size() <= 1)
          continue;
        // Use the key to choose a child of the last good checkpoint
        auto top = me->snapshots.top();
        parent._obj = static_cast<node_t *>(top._obj);
        parent._ver = top._ver;
        auto parent_key = static_cast<data_t *>(parent._obj)->key.get(tx);
        child._obj = parent._obj->children[(key < parent_key) ? 0 : 1].get(tx);
        // Validate that the read was valid
        if (!tx.check_continuation(parent._obj, parent._ver))
          continue;
      }

      // Traverse downward from the parent until we find null child or `key`
      while (true) {
        // nullptr == not found, so stop.  We know parent was valid, so we can
        // just return it
        if (!child._obj)
          return {{nullptr, 0}, parent};

        // It's time to move downward.  Read fields of child, then validate it.
        //
        // NB: we may not use grandchild, but it's better to read it here
        auto child_key = static_cast<data_t *>(child._obj)->key.get(tx);
        auto grandchild =
            child._obj->children[(key < child_key) ? LEFT : RIGHT].get(tx);
        child._ver = tx.check_orec(child._obj);
        if (child._ver == STMCAS::END_OF_TIME)
          break; // retry

        // If the child key matches, return {child, parent}.  We know both are
        // valid (parent came from stack; we just checked child)
        //
        // NB: the snapshotting code requires that no node with matching key
        //     goes into `snapshots`
        if (child_key == key)
          return {child, parent};

        // Otherwise add the child to the checkpoint stack and traverse downward
        me->snapshots.push_back({child._obj, child._ver});
        parent = child;
        child = {grandchild, 0};
      }
    }
  }

  /// Given a node and its orec value, find the tree node that holds the key
  /// that logically succeeds it (i.e., the leftmost descendent of the right
  /// child)
  ///
  /// NB: The caller must ensure that `node` has a valid right child before
  ///     calling this method
  ///
  /// @param me   The calling thread's descriptor
  /// @param node An object and orec value to use as the starting point
  ///
  /// @return {{found, orec}, {parent, orec}} if no inconsistency occurs
  ///         {{nullptr, 0},  {nullptr, 0}}   on any consistency violation
  ret_pair_t get_succ_pair(STMCAS *me, leq_t &node) {
    // NB: We expect the successor to be relatively close to the node, so we
    //     don't bother with checkpoints.  However, we are willing to retry,
    //     since it's unlikely that `node` itself will change.
    while (true) {
      RSTEP tx(me);
      // Ensure `node` is not deleted before reading its fields
      if (!tx.check_continuation(node._obj, node._ver))
        return {{nullptr, 0}, {nullptr, 0}};

      // Read the right child, ensure consistency
      leq_t parent = node, child = {node._obj->children[RIGHT].get(tx), 0};
      if (!tx.check_continuation(node._obj, node._ver))
        return {{nullptr, 0}, {nullptr, 0}};

      // Find the leftmost non-null node in the tree rooted at child
      while (true) {
        auto next = child._obj->children[LEFT].get(tx);
        child._ver = tx.check_orec(child._obj);
        if (child._ver == STMCAS::END_OF_TIME)
          break; // retry
        // If next is null, `child` is the successor.  Otherwise keep traversing
        if (!next)
          return {child, parent};
        parent = child;
        child = {next, 0};
      }
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
  bool get(STMCAS *me, const K &key, V &val) {
    me->snapshots.clear();
    while (true) {
      // Get the node that holds `key`, if it is present, and also its parent.
      // If it isn't present, we'll get a null pointer.  That corresponds to a
      // consistent read of the parent, which means we already linearized and
      // we're done
      auto [curr, _] = get_node(me, key);
      if (curr._obj == nullptr)
        return false;

      // Use an optimistic read if V can be read atomically
      if (std::is_scalar<V>::value) {
        RSTEP tx(me);
        auto *dn = static_cast<data_t *>(curr._obj);
        V val_copy = reinterpret_cast<std::atomic<V> *>(&dn->val)->load(
            std::memory_order_acquire);
        if (!tx.check_continuation(curr._obj, curr._ver))
          continue;
        val = val_copy;
        return true;
      } else {
        WSTEP tx(me);
        if (!tx.acquire_continuation(curr._obj, curr._ver)) {
          tx.unwind();
          continue;
        }
        auto dn = static_cast<data_t *>(curr._obj);
        val = dn->val;
        tx.unwind(); // because this WSTEP_TM didn't write anything
        return true;
      }
    }
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
  bool insert(STMCAS *me, const K &key, V &val) {
    me->snapshots.clear();
    while (true) {
      auto [child, parent] = get_node(me, key);
      if (child._obj)
        return false;
      WSTEP tx(me);
      if (tx.acquire_continuation(parent._obj, parent._ver)) {
        // We must have a null child and a valid parent.  If it's sentinel, we
        // must insert as LEFT.  Otherwise, compute which child to set.
        auto cID = (parent._obj == sentinel ? LEFT : RIGHT) &
                   (key > static_cast<data_t *>(parent._obj)->key.get(tx));
        auto child = new data_t(tx, nullptr, nullptr, key, val);
        parent._obj->children[cID].set(child, tx);
        return true;
      }
    }
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(STMCAS *me, const K &key) {
    me->snapshots.clear();
    while (true) {
      auto [target, parent] = get_node(me, key);
      if (target._obj == nullptr)
        return false;

      // Consistently read the target node's children
      //
      // NB: a concurrent thread could delete `target`, or could move `target`
      //     as part of some other `remove`.  The call to `check_continuation()`
      //     will detect these cases and restart.
      data_t *t_child[2];
      {
        RSTEP tx(me);
        t_child[RIGHT] =
            static_cast<data_t *>(target._obj->children[RIGHT].get(tx));
        t_child[LEFT] =
            static_cast<data_t *>(target._obj->children[LEFT].get(tx));
        if (!tx.check_continuation(target._obj, target._ver))
          continue;
      }

      // If either child is null, and if the parent is still valid, then we can
      // unstitch the target, link the parent to a grandchild and we're done.
      if (!t_child[LEFT] || !t_child[RIGHT]) {
        // Acquire the (possibly null) grandchild to link to the parent
        auto gID = t_child[LEFT] ? LEFT : RIGHT;
        WSTEP tx(me);
        if (!tx.acquire_continuation(target._obj, target._ver) ||
            !tx.acquire_continuation(parent._obj, parent._ver)) {
          tx.unwind();
          continue;
        }

        // Which child of the parent is target?
        auto cID =
            parent._obj->children[LEFT].get(tx) == target._obj ? LEFT : RIGHT;

        // Unstitch and reclaim
        parent._obj->children[cID].set(t_child[gID], tx);
        tx.reclaim(target._obj);
        return true;
      }

      // `target` has two children.  WLOG, the leftmost descendent of the right
      // child is `target`'s successor, and must have at most one child.  We
      // want to put that node's key and value into `target`, and then remove
      // that node by setting its parent's LEFT to its RIGHT (which might be
      // null).
      auto [succ, s_parent] = get_succ_pair(me, target);
      if (!succ._obj)
        continue;

      // If target's successor is target's right child, then target._ver must
      // equal s_parent._ver.  As long as we lock target._obj before we try
      // to lock s_parent._obj, we'll get the check for free.
      {
        WSTEP tx(me);

        if (!tx.acquire_continuation(target._obj, target._ver) ||
            !tx.acquire_continuation(succ._obj, succ._ver) ||
            !tx.acquire_continuation(s_parent._obj, s_parent._ver)) {
          tx.unwind();
          continue;
        } // Postcondition of acquisition: target, succ, and s_parent are valid

        // Copy `succ`'s key/value into `target`
        static_cast<data_t *>(target._obj)
            ->key.set(static_cast<data_t *>(succ._obj)->key.get(tx), tx);
        static_cast<data_t *>(target._obj)->val =
            static_cast<data_t *>(succ._obj)->val;

        // Unstitch `succ` by setting its parent's left to its right
        // Case 1: there are intermediate nodes between target and successor
        if (s_parent._obj != target._obj)
          s_parent._obj->children[LEFT].set(succ._obj->children[RIGHT].get(tx),
                                            tx);
        // Case 2: target is successor's parent
        else
          s_parent._obj->children[RIGHT].set(succ._obj->children[RIGHT].get(tx),
                                             tx);
        tx.reclaim(succ._obj);
        return true;
      }
    }
  }
};
