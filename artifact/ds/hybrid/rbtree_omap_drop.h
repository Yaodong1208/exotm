#pragma once

/// An ordered map, implemented as a balanced, internal binary search tree. This
/// map supports get(), insert(), and remove() operations.
///
/// @param K          The type of the keys stored in this map
/// @param V          The type of the values stored in this map
/// @param HYPOL      A thread descriptor type, for safe memory reclamation
/// @param dummy_key  A default key to use
/// @param dummy_val  A default value to use
template <typename K, typename V, class HYPOL, K dummy_key, V dummy_val>
class rbtree_omap_drop {
  using WOSTM = typename HYPOL::WOSTM;
  using ROSTM = typename HYPOL::ROSTM;
  using RSTEP = typename HYPOL::RSTEP;
  using WSTEP = typename HYPOL::WSTEP;
  using ownable_t = typename HYPOL::ownable_t;
  template <typename T> using FIELD = typename HYPOL::template sxField<T>;

  /// An easy-to-remember way of indicating the left and right children
  enum DIRS { LEFT = 0, RIGHT = 1 };

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
      child[0].xSet(wo, this, child0);
      child[1].xSet(wo, this, child1);
    }
  };

  node_t *sentinel; // The (sentinel) root node of the tree

  /// The pair returned by get_leq; equivalent to the type in snapshots
  struct leq_t {
    node_t *_obj = nullptr; // The object
    uint64_t _ver = 0;      // The observed version of the object
  };

  /// A pair holding a child node and its parent, with orec validation info
  struct ret_pair_t {
    leq_t child;  // The child
    leq_t parent; // The parent of that child
  };

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
  ret_pair_t get_node(HYPOL *me, const K &key) const {
    // This loop delineates the search transaction.  It commences from the end
    // of the longest consistent prefix in the checkpoint stack
    while (true) {
      // Open a RO transaction to traverse downward to the target node:
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
        child._obj = parent._obj->child[LEFT].sGet(tx);
        parent._ver = tx.check_orec(parent._obj);
        if (parent._ver == HYPOL::END_OF_TIME)
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
        auto parent_key = parent._obj->key.sGet(tx);
        child._obj = parent._obj->child[(key < parent_key) ? 0 : 1].sGet(tx);
        // Validate that the reads of parent were valid
        if (!tx.check_continuation(parent._obj, parent._ver))
          continue;
      }

      // Traverse downward from the parent until we find null child or `key`
      while (true) {
        // nullptr == not found, so stop.  Parent was valid, so return it
        if (!child._obj)
          return {{nullptr, 0}, parent};

        // It's time to move downward.  Read fields of child, then validate it.
        //
        // NB: we may not use grandchild, but it's better to read it here
        auto child_key = child._obj->key.sGet(tx);
        auto grandchild =
            child._obj->child[(key < child_key) ? LEFT : RIGHT].sGet(tx);
        child._ver = tx.check_orec(child._obj);
        if (child._ver == HYPOL::END_OF_TIME)
          break; // retry

        // If the child key matches, return {child, parent}.  We know both are
        // valid (parent came from stack; we just checked child)
        //
        // NB: the snapshot code requires that no node with matching key goes
        //     into `snapshots`
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
  ret_pair_t get_succ_pair(HYPOL *me, leq_t &node) {
    // NB: We expect the successor to be relatively close to the node, so we
    //     don't bother with checkpoints.  However, we are willing to retry,
    //     since it's unlikely that `node` itself will change.
    while (true) {
      RSTEP tx(me);
      // Ensure `node` is not deleted before reading its fields
      if (!tx.check_continuation(node._obj, node._ver))
        return {{nullptr, 0}, {nullptr, 0}};

      // Read the right child, ensure consistency
      leq_t parent = node, child = {node._obj->child[RIGHT].sGet(tx), 0};
      if (!tx.check_continuation(node._obj, node._ver))
        return {{nullptr, 0}, {nullptr, 0}};

      // Find the leftmost non-null node in the tree rooted at child
      while (true) {
        auto next = child._obj->child[LEFT].sGet(tx);
        child._ver = tx.check_orec(child._obj);
        if (child._ver == HYPOL::END_OF_TIME)
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
  /// Construct a tree by creating a sentinel node at the top
  rbtree_omap_drop(HYPOL *me, auto *) {
    BEGIN_WO(me);
    sentinel = new node_t(wo, BLACK, dummy_key, dummy_val, nullptr, 0, nullptr,
                          nullptr);
  }

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
  bool get(HYPOL *me, const K &key, V &val) const {
    me->snapshots.clear();
    while (true) {
      // Get the node that holds `key`, if it is present. If it isn't present,
      // we'll get the parent of where it would be.  Whatever we get is
      // validated, so if it's the sentinel, we're done.
      auto curr = get_node(me, key).child;
      if (curr._obj == nullptr)
        return false;

      // Use an optimistic read if V can be read atomically
      if (std::is_scalar<V>::value) {
        RSTEP tx(me);
        auto *dn = curr._obj;
        auto dn_key = dn->key.sGet(tx);
        // TODO: Use atomic_ref?
        V val_copy = reinterpret_cast<std::atomic<V> *>(&dn->val)->load(
            std::memory_order_acquire);
        if (!tx.check_continuation(curr._obj, curr._ver))
          continue;
        if (dn_key != key)
          return false;
        val = val_copy;
        return true;
      } else {
        // Using STM here is really easy, and a setjmp is more scalable than
        // having to CAS...
        //
        // TODO: In HandSTM, do we have a hidden requirement that values are
        //       always scalar?  If so, we don't even need this...
        BEGIN_RO(me);
        if (ro.inheritOrec(curr._obj, curr._ver)) {
          val = curr._obj->val.xGet(ro, curr._obj);
          return true;
        } // else commit and repeat the while loop :)
      }
    }
  }

  // insert a node with k/v as its pair if no such key exists in the tree
  bool insert(HYPOL *me, const K &key, V &val) {
    me->snapshots.clear();
    while (true) {
      // Find insertion point using an STMCAS step.  If child isn't null, `key`
      // is already present, so we can finish without another STEP or STM
      auto [child_, parent_] = get_node(me, key);
      if (child_._obj != nullptr)
        return false;

      BEGIN_WO(me);

      // Make this WOSTM a continuation of the preceding RSTEP
      if (!wo.inheritOrec(parent_._obj, parent_._ver))
        continue;

      // The remaining code needs to know if we're inserting to the left or
      // right of leq._obj.  If we're at sentinel, it's left.  Otherwise, use
      // the key to decide.
      node_t *curr = parent_._obj;
      int cID = curr == sentinel ? 0 : (key < curr->key.xGet(wo, curr) ? 0 : 1);

      node_t *child =
          new node_t(wo, RED, key, val, curr, cID, nullptr, nullptr);
      curr->child[cID].xSet(wo, curr, child);

      // balance the tree
      while (true) {
        // Get the parent, grandparent, and their relationship
        node_t *parent = child->parent.xGet(wo, child);
        int pID = parent->ID.xGet(wo, parent);
        node_t *gparent = parent->parent.xGet(wo, parent);

        // Easy exit condition: no more propagation needed
        if ((gparent == sentinel) || (BLACK == parent->color.xGet(wo, parent)))
          return true;

        // If parent's sibling is also red, we push red up to grandparent
        node_t *psib = gparent->child[1 - pID].xGet(wo, gparent);
        if ((psib != nullptr) && (RED == psib->color.xGet(wo, psib))) {
          parent->color.xSet(wo, parent, BLACK);
          psib->color.xSet(wo, psib, BLACK);
          gparent->color.xSet(wo, gparent, RED);
          child = gparent;
          continue; // restart loop at gparent level
        }

        int cID = child->ID.xGet(wo, child);
        if (cID != pID) {
          // set child's child to parent's cID'th child
          node_t *baby = child->child[1 - cID].xGet(wo, child);
          parent->child[cID].xSet(wo, parent, baby);
          if (baby != nullptr) {
            baby->parent.xSet(wo, baby, parent);
            baby->ID.xSet(wo, baby, cID);
          }
          // move parent into baby's position as a child of child
          child->child[1 - cID].xSet(wo, child, parent);
          parent->parent.xSet(wo, parent, child);
          parent->ID.xSet(wo, parent, 1 - cID);
          // move child into parent's spot as pID'th child of gparent
          gparent->child[pID].xSet(wo, gparent, child);
          child->parent.xSet(wo, child, gparent);
          child->ID.xSet(wo, child, pID);
          // now swap child with curr and fall through
          node_t *temp = child;
          child = parent;
          parent = temp;
        }

        parent->color.xSet(wo, parent, BLACK);
        gparent->color.xSet(wo, gparent, RED);
        // promote parent
        node_t *ggparent = gparent->parent.xGet(wo, gparent);
        int gID = gparent->ID.xGet(wo, gparent);
        node_t *ochild = parent->child[1 - pID].xGet(wo, parent);
        // make gparent's pIDth child ochild
        gparent->child[pID].xSet(wo, gparent, ochild);
        if (ochild != nullptr) {
          ochild->parent.xSet(wo, ochild, gparent);
          ochild->ID.xSet(wo, ochild, pID);
        }
        // make gparent the 1-pID'th child of parent
        parent->child[1 - pID].xSet(wo, parent, gparent);
        gparent->parent.xSet(wo, gparent, parent);
        gparent->ID.xSet(wo, gparent, 1 - pID);
        // make parent the gIDth child of ggparent
        ggparent->child[gID].xSet(wo, ggparent, parent);
        parent->parent.xSet(wo, parent, ggparent);
        parent->ID.xSet(wo, parent, gID);
      }

      // now just set the root to black
      node_t *root = sentinel->child[0].xGet(wo, sentinel);
      if (root->color.xGet(wo, root) != BLACK)
        root->color.xSet(wo, root, BLACK);
      return true;
    }
  }

  // remove the node with k as its key if it exists in the tree
  bool remove(HYPOL *me, const K &key) {
    me->snapshots.clear();
    while (true) {
      // Find insertion point using an STMCAS step.  If child is null, `key` is
      // not present, so we can finish without another STEP or STM.
      auto [child_, parent_] = get_node(me, key);
      if (child_._obj == nullptr)
        return false;

      // If the found node has two children, then we're going to need to swap it
      // with its successor.  That could mean a big traversal, so let's use an
      // RSTEP instead of jumping right into a WOSTM that has to validate its
      // read set.

      // First, an RSTEP to see if it has two children
      node_t *l = nullptr, *r = nullptr;
      {
        RSTEP tx(me);
        r = child_._obj->child[1].sGet(tx);
        l = child_._obj->child[1].sGet(tx);
        if (!tx.check_continuation(child_._obj, child_._ver))
          continue;
      }

      // If so, then an RSTEP to get the successor and successor parent
      leq_t successor = {nullptr, 0}, successor_parent = {nullptr, 0};
      if (r != nullptr && l != nullptr) {
        auto [succ, s_parent] = get_succ_pair(me, child_);
        if (!succ._obj)
          continue;
        successor = succ;
        successor_parent = s_parent;
      }

      {
        BEGIN_WO(me);

        // Make this WOSTM a continuation of the preceding RSTEP
        if (!wo.inheritOrec(child_._obj, child_._ver))
          continue;

        // NB: We get segfaults if we don't also inheritOrec on the parent.  We
        //     need to investigate this further.
        if (!wo.inheritOrec(parent_._obj, parent_._ver))
          continue;

        // find key
        node_t *curr = child_._obj;

        // If `curr` has two children, we need to swap it with its successor
        if (l != nullptr && r != nullptr) {
          // First we have to make `wo` a continuation of the other RSTEP
          if (!wo.inheritOrec(successor._obj, successor._ver))
            continue;
          if (!wo.inheritOrec(successor_parent._obj, successor_parent._ver))
            continue;

          curr->key.xSet(wo, curr,
                         successor._obj->key.xGet(wo, successor._obj));
          curr->val.xSet(wo, curr,
                         successor._obj->val.xGet(wo, successor._obj));
          curr = successor._obj;
          parent_ = successor_parent;
        }

        // extract x from the tree and prep it for deletion
        node_t *parent = parent_._obj;
        node_t *child =
            curr->child[(curr->child[0].xGet(wo, curr) != nullptr) ? 0 : 1]
                .xGet(wo, curr);
        int xID = curr->ID.xGet(wo, curr);
        parent->child[xID].xSet(wo, parent, child);
        if (child != nullptr) {
          child->parent.xSet(wo, child, parent);
          child->ID.xSet(wo, child, xID);
        }

        // fix black height violations
        if ((BLACK == curr->color.xGet(wo, curr)) && (child != nullptr)) {
          if (RED == child->color.xGet(wo, child)) {
            curr->color.xSet(wo, curr, RED);
            child->color.xSet(wo, child, BLACK);
          }
        }

        // rebalance... be sure to save the deletion target!
        node_t *to_delete = curr;
        while (true) {
          parent = curr->parent.xGet(wo, curr);
          if ((parent == sentinel) || (RED == curr->color.xGet(wo, curr)))
            break;
          int cID = curr->ID.xGet(wo, curr);
          node_t *sibling = parent->child[1 - cID].xGet(wo, parent);

          // we'd like y's sibling s to be black
          // if it's not, promote it and recolor
          if (RED == sibling->color.xGet(wo, sibling)) {
            /*
                Bp          Bs
               / \         / \
              By  Rs  =>  Rp  B2
              / \        / \
             B1 B2     By  B1
           */
            parent->color.xSet(wo, parent, RED);
            sibling->color.xSet(wo, sibling, BLACK);
            // promote sibling
            node_t *gparent = parent->parent.xGet(wo, parent);
            int pID = parent->ID.xGet(wo, parent);
            node_t *nephew = sibling->child[cID].xGet(wo, sibling);
            // set nephew as 1-cID child of parent
            parent->child[1 - cID].xSet(wo, parent, nephew);
            nephew->parent.xSet(wo, nephew, parent);
            nephew->ID.xSet(wo, nephew, 1 - cID);
            // make parent the cID child of the sibling
            sibling->child[cID].xSet(wo, sibling, parent);
            parent->parent.xSet(wo, parent, sibling);
            parent->ID.xSet(wo, parent, cID);
            // make sibling the pID child of gparent
            gparent->child[pID].xSet(wo, gparent, sibling);
            sibling->parent.xSet(wo, sibling, gparent);
            sibling->ID.xSet(wo, sibling, pID);
            // reset sibling
            sibling = nephew;
          }

          // Handle when the far nephew is red
          node_t *n = sibling->child[1 - cID].xGet(wo, sibling);
          if ((n != nullptr) && (RED == (n->color.xGet(wo, n)))) {
            /*
               ?p          ?s
               / \         / \
              By  Bs  =>  Bp  Bn
             / \         / \
            ?1 Rn      By  ?1
            */
            sibling->color.xSet(wo, sibling, parent->color.xGet(wo, parent));
            parent->color.xSet(wo, parent, BLACK);
            n->color.xSet(wo, n, BLACK);
            // promote sibling
            node_t *gparent = parent->parent.xGet(wo, parent);
            int pID = parent->ID.xGet(wo, parent);
            node_t *nephew = sibling->child[cID].xGet(wo, sibling);
            // make nephew the 1-cID child of parent
            parent->child[1 - cID].xSet(wo, parent, nephew);
            if (nephew != nullptr) {
              nephew->parent.xSet(wo, nephew, parent);
              nephew->ID.xSet(wo, nephew, 1 - cID);
            }
            // make parent the cID child of the sibling
            sibling->child[cID].xSet(wo, sibling, parent);
            parent->parent.xSet(wo, parent, sibling);
            parent->ID.xSet(wo, parent, cID);
            // make sibling the pID child of gparent
            gparent->child[pID].xSet(wo, gparent, sibling);
            sibling->parent.xSet(wo, sibling, gparent);
            sibling->ID.xSet(wo, sibling, pID);
            break; // problem solved
          }

          n = sibling->child[cID].xGet(wo, sibling);
          if ((n != nullptr) && (RED == (n->color.xGet(wo, n)))) {
            /*
                 ?p          ?p
                 / \         / \
               By  Bs  =>  By  Bn
                   / \           \
                  Rn B1          Rs
                                   \
                                   B1
            */
            sibling->color.xSet(wo, sibling, RED);
            n->color.xSet(wo, n, BLACK);
            // promote n
            node_t *gneph = n->child[1 - cID].xGet(wo, n);
            // make gneph the cID child of sibling
            sibling->child[cID].xSet(wo, sibling, gneph);
            if (gneph != nullptr) {
              gneph->parent.xSet(wo, gneph, sibling);
              gneph->ID.xSet(wo, gneph, cID);
            }
            // make sibling the 1-cID child of n
            n->child[1 - cID].xSet(wo, n, sibling);
            sibling->parent.xSet(wo, sibling, n);
            sibling->ID.xSet(wo, sibling, 1 - cID);
            // make n the 1-cID child of parent
            parent->child[1 - cID].xSet(wo, parent, n);
            n->parent.xSet(wo, n, parent);
            n->ID.xSet(wo, n, 1 - cID);
            // swap sibling and `n`
            node_t *temp = sibling;
            sibling = n;
            n = temp;

            // now the far nephew is red... copy of code from above
            sibling->color.xSet(wo, sibling, parent->color.xGet(wo, parent));
            parent->color.xSet(wo, parent, BLACK);
            n->color.xSet(wo, n, BLACK);
            // promote sibling
            node_t *gparent = parent->parent.xGet(wo, parent);
            int pID = parent->ID.xGet(wo, parent);
            node_t *nephew = sibling->child[cID].xGet(wo, sibling);
            // make nephew the 1-cID child of parent
            parent->child[1 - cID].xSet(wo, parent, nephew);
            if (nephew != nullptr) {
              nephew->parent.xSet(wo, nephew, parent);
              nephew->ID.xSet(wo, nephew, 1 - cID);
            }
            // make parent the cID child of the sibling
            sibling->child[cID].xSet(wo, sibling, parent);
            parent->parent.xSet(wo, parent, sibling);
            parent->ID.xSet(wo, parent, cID);
            // make sibling the pID child of gparent
            gparent->child[pID].xSet(wo, gparent, sibling);
            sibling->parent.xSet(wo, sibling, gparent);
            sibling->ID.xSet(wo, sibling, pID);

            break; // problem solved
          }

          /*
               ?p          ?p
               / \         / \
             Bx  Bs  =>  Bp  Rs
                 / \         / \
                B1 B2      B1  B2
           */

          sibling->color.xSet(wo, sibling, RED); // propagate upwards

          // advance to parent and balance again
          curr = parent;
        }

        // if curr was red, this fixes the balance
        if (curr->color.xGet(wo, curr) == RED)
          curr->color.xSet(wo, curr, BLACK);

        // free the node and return
        wo.reclaim(to_delete);

        return true;
      }
    }
  }
};
