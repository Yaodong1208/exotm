#pragma once

#include <assert.h>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <type_traits>

/// An ordered map, implemented as an unbalanced, internal binary search tree.
/// This map supports get(), insert(), and remove() operations.
///
/// @param K      The type of the keys stored in this map
/// @param V      The type of the values stored in this map
/// @param STMCAS The STMCAS implementation (PO or PS)
template <typename K, typename V, class STMCAS> class rbtree_omap {
  using WSTEP = typename STMCAS::WSTEP;
  using RSTEP = typename STMCAS::RSTEP;
  using snapshot_t = typename STMCAS::snapshot_t;
  using ownable_t = typename STMCAS::ownable_t;
  template <typename T> using FIELD = typename STMCAS::template sField<T>;

  /// An easy-to-remember way of indicating the left and right children
  enum DIRS { LEFT = 0, RIGHT = 1 };

  /// the color of a node
  enum COLOR { RED = 0, BLACK = 1 };

  /// node_t is the base type for all tree nodes.  It doesn't have key/value
  /// fields.
  struct node_t : public ownable_t {
    FIELD<node_t *> children[2]; // The node's children; index with LEFT/RIGHT
    FIELD<COLOR> color;          // The node's color

    /// Construct a node_t.  This should only be called from a writer
    /// transaction
    ///
    /// @param tx      A writing transactional context
    /// @param _color  The color for this node
    /// @param _left   The left child of this node
    /// @param _right  The right child of this node
    node_t(WSTEP &tx, COLOR _color, node_t *_left = nullptr,
           node_t *_right = nullptr)
        : ownable_t() {
      color.set(_color, tx);
      children[LEFT].set(_left, tx);
      children[RIGHT].set(_right, tx);
    }
  };

  /// The pair returned by get_leq; equivalent to the type in snapshots
  struct leq_t {
    node_t *_obj = nullptr; // The object
    uint64_t _ver = 0;      // The observed version of the object
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
    FIELD<K> key;           // The key stored in this node
    V val;                  // The value stored in this node
    FIELD<node_t *> parent; // The node's parent

    /// Construct a node
    ///
    /// @param tx      A writing transaction context
    /// @param _parent The node's parent
    /// @param _left   The node's left child
    /// @param _right  The node's right child
    /// @param _key    The node's key
    /// @param _val    The node's value
    /// @param _color  The color of this node
    data_t(WSTEP &tx, node_t *_parent, node_t *_left, node_t *_right,
           const K &_key, V &_val, COLOR _color)
        : node_t(tx, _color, _left, _right), key(_key), val(_val),
          parent(_parent) {}
  };

public:
  /// Default construct an empty tree
  ///
  /// @param me  The operation that is constructing the tree
  /// @param cfg An unused configuration object
  rbtree_omap(STMCAS *me, auto *cfg) {
    // NB: Even though the constructor is operating on private data, it needs a
    //     TM context for the constructor
    WSTEP tx(me);
    sentinel = new node_t(tx, BLACK);
  }

private:
  /// Search for a `key` in the tree, and return the node holding it.  If the
  /// key is not found, return the node that ought to be parent of the (not
  /// found) `key`.
  ///
  /// NB: The caller is responsible for clearing the checkpoint stack before
  ///     calling get_node().
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key to search for
  ///
  /// @return {found, orec}  if `key` is in the tree;
  ///         {parent, orec} if `key` is not in the tree
  leq_t get_node(STMCAS *me, const K &key) const {
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

      // If stack is empty or only holds sentinel, start from {sentinel, root}
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
        //
        // NB: top.key != key, because we never put a matching key into
        //     snapshots, and if a remove caused a key to change, we'll fail to
        //     validate that node.
        auto top = me->snapshots.top();
        parent = {static_cast<node_t *>(top._obj), top._ver};
        auto parent_key = static_cast<data_t *>(parent._obj)->key.get(tx);
        child._obj = parent._obj->children[(key < parent_key) ? 0 : 1].get(tx);
        // Validate that the reads of parent were valid
        if (!tx.check_continuation(parent._obj, parent._ver))
          continue;
      }

      // Traverse downward from the parent until we find null child or `key`
      while (true) {
        // nullptr == not found, so stop.  Parent was valid, so return it
        if (!child._obj)
          return parent;

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
        // NB: the snapshot code requires that no node with matching key goes
        //     into `snapshots`
        if (child_key == key)
          return child;

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
  /// @param me  The calling thread's descriptor
  /// @param node An object and orec value to use as the starting point
  ///
  /// @return {{found, orec}, {parent, orec}} if no inconsistency occurs
  ///         {{nullptr, 0},  {nullptr, 0}}   on any consistency violation
  leq_t get_succ(STMCAS *me, leq_t &node) {
    // NB: We expect the successor to be relatively close to the node, so we
    //     don't bother with checkpoints.  However, we are willing to retry,
    //     since it's unlikely that `node` itself will change.
    while (true) {
      RSTEP tx(me);
      // Read the right child, ensure consistency
      //
      // NB: Since we have smr, we can read `node` even if it is deleted.  The
      //     subsequent validation will suffice.
      leq_t child = {node._obj->children[RIGHT].get(tx), 0};
      if (!tx.check_continuation(node._obj, node._ver))
        return {nullptr, 0};

      // Find the leftmost non-null node in the tree rooted at child
      while (true) {
        auto next = child._obj->children[LEFT].get(tx);
        child._ver = tx.check_orec(child._obj);
        if (child._ver == STMCAS::END_OF_TIME)
          break; // retry
        // If next is null, `child` is the successor.  Otherwise keep traversing
        if (!next)
          return child;
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
  bool get(STMCAS *me, const K &key, V &val) const {
    me->snapshots.clear();
    while (true) {
      // Get the node that holds `key`, if it is present. If it isn't present,
      // we'll get the parent of where it would be.  Whatever we get is
      // validated, so if it's the sentinel, we're done.
      auto curr = get_node(me, key);
      if (curr._obj == sentinel)
        return false;

      // Use an optimistic read if V can be read atomically
      if (std::is_scalar<V>::value) {
        RSTEP tx(me);
        auto *dn = static_cast<data_t *>(curr._obj);
        auto dn_key = dn->key.get(tx);
        V val_copy = reinterpret_cast<std::atomic<V> *>(&dn->val)->load(
            std::memory_order_acquire);
        if (!tx.check_continuation(curr._obj, curr._ver))
          continue;
        if (dn_key != key)
          return false;
        val = val_copy;
        return true;
      } else {
        WSTEP tx(me);
        if (!tx.acquire_continuation(curr._obj, curr._ver)) {
          tx.unwind();
          continue;
        }
        auto dn = static_cast<data_t *>(curr._obj);
        if (dn->key.get(tx) != key) {
          tx.unwind();
          return false;
        }
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
      // Get the node that holds `key`, if it is present.  If it isn't present,
      // we'll get the parent of where it would be.  Whatever we get is
      // validated, so if it matches, we're done.
      auto leq = get_node(me, key);

      // We're going to assume that we'll insert, so open a WSTEP transaction.
      // If we can't lock the node, restart
      WSTEP tx(me);
      if (!tx.acquire_continuation(leq._obj, leq._ver))
        continue;

      // If the key matches, the insertion attempt fails
      if (leq._obj != sentinel &&
          static_cast<data_t *>(leq._obj)->key.get(tx) == key) {
        tx.unwind();
        return false;
      }

      // We must have a null child and a valid parent.  If it's sentinel, we
      // must insert as LEFT.  Otherwise, compute which child to set.
      node_t *parent = leq._obj;
      auto cID = (leq._obj == sentinel ? LEFT : RIGHT) &
                 (key > static_cast<data_t *>(parent)->key.get(tx));

      // We are strict 2PL here: first we must acquire everything that will be
      // written.  `fix_root` tracks if the root will need special cleanup.
      bool fix_root = false;
      if (!insert_acquire_aggressive_all(cID, static_cast<data_t *>(parent), tx,
                                         fix_root)) {
        tx.unwind();
        continue;
      }

      // Now we can link the child to the parent
      auto child = new data_t(tx, parent, nullptr, nullptr, key, val, RED);
      tx.acquire_aggressive(child);
      parent->children[cID].set(child, tx);

      // Rebalance in response to this insertion, then we're done
      insert_fixup(child, tx, fix_root);
      return true;
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
      // Get the node that holds `key`, if it is present.  If it isn't present,
      // we'll get the parent of where it would be.  Whatever we get is
      // validated, so if it's sentinel, we're done.
      auto target = get_node(me, key);
      if (target._obj == sentinel)
        return false;

      // Read the node's key and its children; detect if the key doesn't match
      //
      // NB: we can't open a WSTEP yet, because an upcoming call to get_succ
      // does
      //     is going to use an RSTEP
      data_t *t_child[2];
      {
        RSTEP tx(me);
        auto dn_key = static_cast<data_t *>(target._obj)->key.get(tx);
        t_child[RIGHT] =
            static_cast<data_t *>(target._obj->children[RIGHT].get(tx));
        t_child[LEFT] =
            static_cast<data_t *>(target._obj->children[LEFT].get(tx));
        if (!tx.check_continuation(target._obj, target._ver))
          continue;
        if (dn_key != key)
          return false;
      }

      // If target has <=1 child, then we will un-stitch by pointing its parent
      // to that child.  Otherwise, we'll un-stitch by swapping target with its
      // successor and then removing the successor by pointing successor's
      // parent to successor's child.  Here's where we get the child and
      // successor
      leq_t succ = {target._obj, target._ver}; // succ is target if only 1 child
      data_t *child = nullptr;                 // The child who gets swapped up
      if (!t_child[LEFT]) {
        child = t_child[RIGHT];
      } else if (!t_child[RIGHT]) {
        child = t_child[LEFT];
      } else {
        succ = get_succ(me, target);
        if (!succ._obj)
          continue;
        RSTEP tx(me);
        child = static_cast<data_t *>(succ._obj->children[RIGHT].get(tx));
        if (!tx.check_continuation(succ._obj, succ._ver))
          continue;
      }

      // We're going to assume that we'll remove, so open a WSTEP transaction
      // and acquire succ and child.
      //
      // NB: acquire continuation on succ is necessary regardless of whether
      //     it's target or not, but child can be aggressive.
      WSTEP tx(me);
      if (!tx.acquire_continuation(succ._obj, succ._ver) ||
          (child && !tx.acquire_aggressive(child))) {
        tx.unwind();
        continue;
      }

      // Now acquire child's children, if child is not null
      {
        auto x_l = child ? child->children[LEFT].get(tx) : nullptr;
        auto x_r = child ? child->children[RIGHT].get(tx) : nullptr;
        if ((x_l && !tx.acquire_aggressive(x_l)) ||
            (x_r && !tx.acquire_aggressive(x_r))) {
          tx.unwind();
          continue;
        }
      }

      // We are strict 2PL here: first we must acquire everything that will be
      // written.  remove_acquire_aggressive_all does most of the job:
      if (!remove_acquire_aggressive_all(
              child, static_cast<data_t *>(succ._obj), tx)) {
        tx.unwind();
        continue;
      }

      // Lastly, we need to acquire the target and the successor's parent
      if ((!tx.acquire_continuation(target._obj, target._ver)) ||
          (!tx.acquire_aggressive(
              static_cast<data_t *>(succ._obj)->parent.get(tx)))) {
        tx.unwind();
        continue;
      }

      // Now we can start moving keys and values, unstitching, and cleaning up

      // We need the successor's original color to know if we need to call fixup
      auto original_succ_color = succ._obj->color.get(tx);
      // If we call fixup, we need the child's CID and parent
      DIRS cID_c;
      node_t *c_parent;

      // If either child is null, and if the parent is still valid, then we can
      // un-stitch target, then link target's parent to target's grandchild
      if (!t_child[LEFT] || !t_child[RIGHT]) {
        // get target's parent, figure out which of its children target is:
        c_parent = static_cast<data_t *>(target._obj)->parent.get(tx);
        cID_c = c_parent->children[LEFT].get(tx) == target._obj ? LEFT : RIGHT;
        // Unstitch, reclaim
        c_parent->children[cID_c].set(child, tx);
        if (child)
          child->parent.set(c_parent, tx);
        tx.reclaim(target._obj);
      }
      // When both children of target are not null, we have to swap, then
      // unstitch
      else {
        // Get the successor's parent, then copy succ's k/v into target
        auto s_p = static_cast<data_t *>(succ._obj)->parent.get(tx);
        auto dn = static_cast<data_t *>(target._obj);
        dn->key.set(static_cast<data_t *>(succ._obj)->key.get(tx), tx);
        dn->val = static_cast<data_t *>(succ._obj)->val;

        // Unstitch `succ` by setting its parent's left to its right (i.e.,
        // child)

        // Case 1: there are intermediate nodes between target and successor
        if (s_p != target._obj) {
          s_p->children[LEFT].set(child, tx);
          cID_c = LEFT;
        }
        // Case 2: target is successor's parent
        else {
          s_p->children[RIGHT].set(child, tx);
          cID_c = RIGHT;
        }
        // don't forget the back-link from child to parent
        if (child)
          child->parent.set(s_p, tx);

        tx.reclaim(succ._obj);
        c_parent = s_p;
      }

      // Rebalance and recolor in response to this removal, then we're done
      if (original_succ_color == BLACK)
        remove_fixup(child, static_cast<data_t *>(c_parent), cID_c, tx);
      return true;
    }
  }

private:
  /// Acquire all of the nodes that will need to change if `z_p` is to receive a
  /// new child in position `CID_z`.
  ///
  /// @param cID_z    The index of the child being added to z_p
  /// @param z_p      The (acquired) node who will be receiving a new child
  /// @param tx       A writing transaction context
  /// @param fix_root A reference parameter indicating if the root was reached
  ///
  /// @return True if all nodes were acquired, false otherwise
  bool insert_acquire_aggressive_all(int cID_z, data_t *z_p, WSTEP &tx,
                                     bool &fix_root) {
    // If we're giving the sentinel a child, then we immediately stop
    // traversing upward... the sentinel is already locked.
    if (z_p == sentinel) {
      fix_root = true;
      return true;
    }

    // z is the child of z_p.  In the first round, it's the to-insert node,
    // which we haven't created yet, so we let it be null and pretend it's RED
    data_t *z = nullptr;

    // Invariant: z_p is already on each iteration
    while (z_p->color.get(tx) == RED) {
      // Acquire the grandparent
      data_t *z_p_p = static_cast<data_t *>(z_p->parent.get(tx));
      if (!tx.acquire_aggressive(z_p_p))
        return false;

      // Now acquire z's aunt (z_a) (z_p's sibling) if it exists
      auto cID_z_p = z_p == z_p_p->children[LEFT].get(tx) ? LEFT : RIGHT;
      auto cID_z_a = cID_z_p == LEFT ? RIGHT : LEFT;
      data_t *z_a = static_cast<data_t *>(z_p_p->children[cID_z_a].get(tx));
      if (z_a && !tx.acquire_aggressive(z_a))
        return false;

      // case 1: z_a is RED --> colors will propagate
      // z_p_p
      //   / \.
      // z_p  z_a
      //  |
      //  z
      if (z_a && static_cast<data_t *>(z_a)->color.get(tx) == RED) {
        // NB: {z, z_p, z_a, z_p_p} are acquired, but we're going to jump to the
        //     great grandparent.  The loop invariant requires us to acquire it
        //     now.
        z = static_cast<data_t *>(z_p_p);
        z_p = static_cast<data_t *>(z->parent.get(tx));
        if (!tx.acquire_aggressive(z_p))
          return false;
        if (z_p == sentinel) // we painted z which is root, we need to fix that
          fix_root = true;
        cID_z = z == z_p->children[LEFT].get(tx) ? LEFT : RIGHT;
        continue;
      }

      // Invariant: z_a is black or nullptr

      // case 2: cID_z != cID_z_p --> do a /cID_z_p/ rotation on z_p
      //  z_p_p
      //   / \.
      // z_a z_p
      //     /
      //    z
      //   / \.
      // z_e  z_c
      if (cID_z != cID_z_p) {
        // NB: z == nullptr should only be true in the first iteration
        //
        // NB: {z, z_p, z_p_p} are acquired.  one of z's children will get a new
        //     parent, so acquire it
        data_t *z_c =
            z ? static_cast<data_t *>(z->children[cID_z_p].get(tx)) : nullptr;
        if (z_c && !tx.acquire_aggressive(z_c))
          return false;

        // Now we roll right into case 3 and finish the rebalance:
        // case 2->3: perform a /cID_z_a/_rotation on z_p_p
        // z_p_p_p
        //    |
        //  z_p_p
        //    / \.
        //   z_a z (==>new z_p)
        //      / \.
        //     z_e z_p (==> new z)
        //         /
        //       z_c

        // {z, z_p_p} are already acquired.  We need to acquire z_e and z_p_p_p
        data_t *z_e =
            z ? static_cast<data_t *>(z->children[cID_z_a].get(tx)) : nullptr;
        if (z_e && !tx.acquire_aggressive(z_e))
          return false;

        auto z_p_p_p = z_p_p->parent.get(tx);
        if (!tx.acquire_aggressive(z_p_p_p))
          return false;
        if (z_p_p_p == sentinel)
          fix_root = true;
        return true;
      }

      // case 3: perform a /cID_y/_rotation on z_p_p
      //        z_p_p_p
      //           |
      //         z_p_p
      //          / \.
      //         z_a z_p
      //             / \.
      //             w  z

      // {z_p, z_p_p} are already acquired.  We need to acquire w and z_p_p_p
      auto z_p_p_p = z_p_p->parent.get(tx);
      if (!tx.acquire_aggressive(z_p_p_p))
        return false;
      if (z_p_p_p == sentinel)
        fix_root = true;

      auto w = z_p->children[cID_z_a].get(tx);
      if (w && !tx.acquire_aggressive(w))
        return false;
      return true;
    }

    // At last, we've acquired everything we need, and can stop
    return true;
  }

  /// Do all of the rotations and color changes that correspond to z being
  /// inserted into the tree.  This should only be called after
  /// insert_acquire_aggressive_all has acquired everything that this method
  /// will modify.  Consequently, this code is identical to the sequential code.
  ///
  /// @param z        The new child being added
  /// @param tx       A writing transaction context
  /// @param fix_root Is the root acquired?
  void insert_fixup(data_t *z, WSTEP &tx, bool fix_root) {
    auto z_p = z->parent.get(tx);
    // Normal case: z is not the root
    while (z_p->color.get(tx) == RED) {
      auto cID_z = z == z_p->children[LEFT].get(tx) ? LEFT : RIGHT;
      node_t *z_p_p = static_cast<data_t *>(z_p)->parent.get(tx);
      auto cID_z_p = z_p == z_p_p->children[LEFT].get(tx) ? LEFT : RIGHT;
      auto cID_z_a = cID_z_p == LEFT ? RIGHT : LEFT;
      data_t *z_a = static_cast<data_t *>(z_p_p->children[cID_z_a].get(tx));
      // case 1:
      if (z_a && z_a->color.get(tx) == RED) {
        z_p->color.set(BLACK, tx);
        z_a->color.set(BLACK, tx);
        z_p_p->color.set(RED, tx);
        z = static_cast<data_t *>(z_p_p);
        z_p = z->parent.get(tx);
        continue;
      }

      // case 2
      if (cID_z == cID_z_a) {
        z = static_cast<data_t *>(z_p);
        if (cID_z == RIGHT)
          left_rotate(z, tx);
        else
          right_rotate(z, tx);
        z_p = z->parent.get(tx);
        z_p_p = static_cast<data_t *>(z_p)->parent.get(tx);
      }

      // case 3 (includes fallthrough from 2->3)
      z_p->color.set(BLACK, tx);
      z_p_p->color.set(RED, tx);
      if (cID_z_a == RIGHT)
        right_rotate(static_cast<data_t *>(z_p_p), tx);
      else
        left_rotate(static_cast<data_t *>(z_p_p), tx);
    }

    // Clean up the root if necessary
    if (fix_root) {
      auto r = sentinel->children[LEFT].get(tx);
      static_cast<data_t *>(r)->color.set(BLACK, tx);
    }
  }

  /// Acquire all of the nodes that will need to change if `y` is to be removed
  /// and `x` is to move into its place
  ///
  /// @param x The node that moves upward
  /// @param y The node that will be removed
  /// @param tx A writing transaction context
  ///
  /// @return True if all nodes were acquired, false otherwise
  bool remove_acquire_aggressive_all(data_t *x, data_t *y, WSTEP &tx) {
    // If `y` isn't black, we won't have to do any rebalancing
    if (y->color.get(tx) != BLACK)
      return true;

    // When x swaps into y's place, it gets y's parent but keeps its color
    auto x_color = x ? x->color.get(tx) : BLACK;
    x = y;

    // loop invariants: x != nullptr, x.color == black, x is acquired
    while (x->parent.get(tx) != sentinel && x_color == BLACK) {
      data_t *x_p = static_cast<data_t *>(x->parent.get(tx));
      if (!tx.acquire_aggressive(x_p))
        return false;

      // We need to know if x is left or right, and we need its sibling (w)
      DIRS cID_x = LEFT, cID_w = RIGHT;
      if (x != x_p->children[LEFT].get(tx)) {
        cID_x = RIGHT;
        cID_w = LEFT;
      }
      auto w = x_p->children[cID_w].get(tx);
      if (!tx.acquire_aggressive(w))
        return false;

      // case 1: Do a cID_x rotation to push x down
      // x_p_p
      //   |
      //  x_p
      //    \.
      //     w
      //    /
      //   w_c
      if (static_cast<data_t *>(w)->color.get(tx) == RED) {
        // {x_p, w} are acquired.  Need to acquire cID_x'th child of w and x_p_p
        data_t *w_c = static_cast<data_t *>(w->children[cID_x].get(tx));
        auto x_p_p = x_p->parent.get(tx);
        if ((w_c && !tx.acquire_aggressive(w_c)) ||
            (!tx.acquire_aggressive(x_p_p)))
          return false;
        // w_c's children's colors determine if we need to propagate.
        // Since we're going to read both children's colors, we need to acquire
        // them (as w_c_c and w_c_e)
        data_t *w_c_c =
            w_c ? static_cast<data_t *>(w_c->children[cID_x].get(tx)) : nullptr;
        data_t *w_c_e =
            w_c ? static_cast<data_t *>(w_c->children[cID_w].get(tx)) : nullptr;

        if ((w_c_c && !tx.acquire_aggressive(w_c_c)) ||
            (w_c_e && !tx.acquire_aggressive(w_c_e)))
          return false;

        // If case 1 becomes case 3, we do a cID_w-rotation on w_c
        //  x_p
        //    \.
        //    w_c
        //    /
        //  w_c_c
        //    \.
        //     w_c_c_e
        // If case 1 becomes case 3 becomes case 4, we need to do a
        // /cID_x/_rotation on x_p
        //   w
        //  /
        // x_p
        //   \.
        //   w_c_c
        //   /
        // w_c_c_c
        if (((w_c_e && w_c_e->color.get(tx) == BLACK) || !w_c_e) && w_c_c &&
            w_c_c->color.get(tx) == RED) {
          // {w, w_c_c, x_p} are already acquired.  Need to acquire w_c_c's
          // children
          data_t *w_c_c_c =
              w_c_c ? static_cast<data_t *>(w_c_c->children[cID_x].get(tx))
                    : nullptr;
          data_t *w_c_c_e =
              w_c_c ? static_cast<data_t *>(w_c_c->children[cID_w].get(tx))
                    : nullptr;
          if ((w_c_c_e && !tx.acquire_aggressive(w_c_c_e)) ||
              (w_c_c_c && !tx.acquire_aggressive(w_c_c_c)))
            return false;
          return true;
        }

        // If case 1 becomes case 4, we need to do a /cID_x/_rotation on x_p
        //   w
        //  /
        // x_p
        //  \.
        //  w_c
        //  /
        // w_c_c
        if (w_c_e && w_c_e->color.get(tx) == RED) {
          // {w, w_c, w_c_c, x_p} are already acquired, so we're done
          return true;
        }
        // otherwise : case1 becomes case2, propagate, see below
      }
      // W's children's colors determine if we need to propagate, so acquire
      // both
      else {
        data_t *w_c = static_cast<data_t *>(w->children[cID_x].get(tx));
        data_t *w_e = static_cast<data_t *>(w->children[cID_w].get(tx));
        if ((w_c && !tx.acquire_aggressive(w_c)) ||
            (w_e && !tx.acquire_aggressive(w_e)))
          return false;
        // case 3: we need to do a /cID_w/_rotation on w
        //  x_p
        //    \.
        //     w
        //    /
        //   w_c
        //    \.
        //    w_c_e
        // {w, x_p, w_c} are already acquired.  Acquire w_c's cID_w child
        // case 3 -> case 4, we need to perform a /cID_x/ rotation on x_p
        //  x_p_p
        //   |
        //  x_p
        //    \.
        //    w_c
        //    /
        //  w_c_c
        // {w, x_p} are already acquired: acquire w_c's cID_x child, and x_p_p
        data_t *x_p_p = static_cast<data_t *>(x_p->parent.get(tx));
        if (x_p_p && !tx.acquire_aggressive(x_p_p))
          return false;

        if (((w_e && w_e->color.get(tx) == BLACK) || !w_e) && w_c &&
            w_c->color.get(tx) == RED) {
          data_t *w_c_c =
              w_c ? static_cast<data_t *>(w_c->children[cID_x].get(tx))
                  : nullptr;
          data_t *w_c_e =
              w_c ? static_cast<data_t *>(w_c->children[cID_w].get(tx))
                  : nullptr;
          if ((w_c_e && !tx.acquire_aggressive(w_c_e)) ||
              (w_c_c && !tx.acquire_aggressive(w_c_c)))
            return false;
          return true;
        }

        // case 4: w_c and/or w_e is red, so cID_x rotate x_p
        // {x_p, x_p_p, w, w_c} already acquired, nothing needed
        if ((w_c && w_c->color.get(tx) == RED) ||
            (w_e && w_e->color.get(tx) == RED))
          return true;
      }
      // case 2 : propagate the fixup to x's parent.
      x = static_cast<data_t *>(x_p);
      x_color = x->color.get(tx);
    }
    return true;
  }

  /// Do all of the color changes and rotations that correspond to x's parent
  /// being deleted, resulting in x becoming the child of x_p.  This should only
  /// be called after remove_acquire_aggressive_all has acquired everything that
  /// this method will modify.  Consequently, this code is identical to the
  /// sequential code.
  ///
  /// @param x     The node that moved up
  /// @param x_p   The new parent of `x`
  /// @param cID_x Which child of `x_p` is `x`?
  /// @param tx    A writing transaction context
  void remove_fixup(data_t *x, data_t *x_p, DIRS cID_x, WSTEP &tx) {
    auto x_color = x ? x->color.get(tx) : BLACK;
    while (x_p != sentinel && x_color == BLACK) {
      // `w` is the sibling of `x`
      auto cID_w = cID_x == LEFT ? RIGHT : LEFT;
      data_t *w = static_cast<data_t *>(x_p->children[cID_w].get(tx));
      if (w && static_cast<data_t *>(w)->color.get(tx) == RED) {
        static_cast<data_t *>(w)->color.set(BLACK, tx);
        static_cast<data_t *>(x_p)->color.set(RED, tx);
        if (cID_x == LEFT)
          left_rotate(static_cast<data_t *>(x_p), tx);
        else
          right_rotate(static_cast<data_t *>(x_p), tx);
        w = static_cast<data_t *>(x_p->children[cID_w].get(tx));
      }
      // check both children's colors to decide about propagating:
      data_t *w_c =
          w ? static_cast<data_t *>(w->children[cID_x].get(tx)) : nullptr;
      data_t *w_e =
          w ? static_cast<data_t *>(w->children[cID_w].get(tx)) : nullptr;
      if ((w_c && w_c->color.get(tx) == RED) ||
          (w_e && w_e->color.get(tx) == RED)) {
        if (!w_e || w_e->color.get(tx) == BLACK) {
          w_c->color.set(BLACK, tx);
          w->color.set(RED, tx);
          if (cID_x == LEFT)
            right_rotate(w, tx);
          else
            left_rotate(w, tx);
          w = static_cast<data_t *>(x_p->children[cID_w].get(tx));
        }
        w->color.set(x_p->color.get(tx), tx);
        x_p->color.set(BLACK, tx);
        auto w_e = w->children[cID_w].get(tx);
        static_cast<data_t *>(w_e)->color.set(BLACK, tx);
        if (cID_x == LEFT)
          left_rotate(x_p, tx);
        else
          right_rotate(x_p, tx);
        break;
      } else {
        w->color.set(RED, tx);
        x = x_p;
        x_p = static_cast<data_t *>(x->parent.get(tx));
        x_color = x->color.get(tx);
        cID_x = x == x_p->children[LEFT].get(tx) ? LEFT : RIGHT;
      }
    }
    if (x)
      x->color.set(BLACK, tx);
  }

  /// Perform a left rotation on `x`, pushing it downward
  ///
  /// @param x  The node to rotate downward
  /// @param tx A writing transaction context
  void left_rotate(data_t *x, WSTEP &tx) {
    auto y = static_cast<data_t *>(x->children[RIGHT].get(tx));
    auto y_l = y->children[LEFT].get(tx);
    x->children[RIGHT].set(y_l, tx);
    if (y_l)
      static_cast<data_t *>(y_l)->parent.set(x, tx);

    auto x_p = x->parent.get(tx);
    y->parent.set(x_p, tx);
    if (x_p == sentinel)
      sentinel->children[LEFT].set(y, tx);
    else
      x_p->children[x == x_p->children[LEFT].get(tx) ? LEFT : RIGHT].set(y, tx);

    y->children[LEFT].set(x, tx);
    x->parent.set(y, tx);
  }

  /// Perform a right rotation on `y`, pushing it downward
  ///
  /// @param y  The node to rotate downward
  /// @param tx A writing transaction context
  void right_rotate(data_t *y, WSTEP &tx) {
    auto x = static_cast<data_t *>(y->children[LEFT].get(tx));
    auto x_r = x->children[RIGHT].get(tx);
    y->children[LEFT].set(x_r, tx);
    if (x_r)
      static_cast<data_t *>(x_r)->parent.set(y, tx);

    auto y_p = y->parent.get(tx);
    x->parent.set(y_p, tx);
    if (y_p == sentinel)
      sentinel->children[LEFT].set(x, tx);
    else
      y_p->children[y == y_p->children[RIGHT].get(tx) ? RIGHT : LEFT].set(x,
                                                                          tx);

    x->children[RIGHT].set(y, tx);
    y->parent.set(x, tx);
  }
};
