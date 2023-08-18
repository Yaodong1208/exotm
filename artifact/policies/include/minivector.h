#pragma once

#include <cstdint>
#include <cstring>
#include <iterator>

/// minivector is a self-growing array, like std::vector, but with less
/// overhead.  It can only be used to store types with trivial constructors and
/// destructors.
///
/// @tparam T The type of the elements stored in the minivector
template <class T> class minivector {
  T *items;       // The internal (dynamic) array of items
  uint32_t cap;   // Maximum capacity of `items`
  uint32_t count; // Number of used slots in `items`

  /// Resize the array of items, and move current data into it
  ///
  /// NB: We assume that T has a trivial copy constructor, and thus we can
  ///     memcpy the data
  __attribute__((noinline)) void expand() {
    auto temp = items;
    cap *= 2;
    items = new T[cap]();
    memcpy(items, temp, sizeof(T) * count);
    delete[] temp;
  }

public:
  /// Construct an empty minivector with a default capacity
  ///
  /// NB: _cap must be positive.  The constructor will crash if `_cap` == 0
  ///
  /// @param _cap The initial capacity of the minivector
  minivector(uint32_t _cap = 64) : items(new T[_cap]()), cap(_cap), count(0) {
    if (_cap == 0)
      std::terminate();
  }

  /// Reclaim memory when the minivector is destroyed
  ~minivector() { delete[] items; }

  /// Fast-clear the minivector
  ///
  /// NB: this works because `T` does not have a destructor
  void clear() { count = 0; }

  /// Pop multiple items from the minivector
  ///
  /// NB: The `newcount` parameter is not checked.  It is up to the caller to
  ///     ensure that this never enlarges the minivector
  ///
  /// @param newcount The number of items that should remain in the minivector
  void reset(uint32_t newcount) { count = newcount; }

  /// Pop one element from the minivector
  ///
  /// NB: The current size of the minivector is not checked.  It is up to the
  ///     caller to ensure that this never causes underflow.
  void drop() { --count; }

  /// Insert an element into the minivector
  ///
  /// We maintain the invariant that when insert() returns, there is always room
  /// for one more element to be added.  This means we may expand() after
  /// insertion, but doing so should be rare.
  ///
  /// @param data The element to insert
  void push_back(T data) {
    items[count] = data;
    // If the list is full, double the capacity
    if (++count == cap)
      expand();
  }

  /// Getter to report the array size (to test for empty)
  unsigned long size() const { return count; }

  /// Return true if the minivector has no elements, false otherwise
  bool empty() { return count == 0; }

  /// Return the element at the top oif the stack, but don't remove it
  T top() { return items[count - 1]; }

  /// minivector iterator type
  using iterator = T *;

  /// minivector reverse iterator type
  using reverse_iterator = std::reverse_iterator<iterator>;

  /// Get an iterator to the start of the array
  iterator begin() const { return items; }

  /// Get an iterator to one past the end of the array
  iterator end() const { return items + count; }

  /// Get the starting point for a reverse iterator
  reverse_iterator rbegin() { return reverse_iterator(end()); }

  /// Get the ending point for a reverse iterator
  reverse_iterator rend() { return reverse_iterator(begin()); }
};
