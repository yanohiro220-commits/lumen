#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lumen/object.hpp"
#include "lumen/value.hpp"

namespace lumen {

// Mark-and-sweep garbage collector.
//
// Mark-sweep rather than reference counting, for the reason every language with
// closures ends up here: a closure that captures a variable holding the closure
// is a cycle, and reference counting leaks it. Tracing also keeps the mutator's
// write path free - no refcount traffic on every assignment.
//
// Collection is precise: every root is enumerated explicitly rather than
// scanned conservatively off the C++ stack. That means an object reachable only
// from a C++ local during a native call must be protected, which is what the
// temporary root stack is for.
class GC {
 public:
  struct Stats {
    std::size_t collections = 0;
    std::size_t bytes_allocated = 0;
    std::size_t next_gc = 0;
    std::size_t objects_live = 0;
    std::size_t objects_freed = 0;
    std::size_t strings_interned = 0;
  };

  // Called during marking so the owner (the VM) can enumerate its roots.
  using RootMarker = std::function<void(GC&)>;

  GC() = default;
  ~GC();

  GC(const GC&) = delete;
  GC& operator=(const GC&) = delete;

  void set_root_marker(RootMarker marker) { root_marker_ = std::move(marker); }

  // Collect on every allocation. Off by default and on in the test suite:
  // a collector that only runs when the heap grows is a collector whose bugs
  // only appear under memory pressure, which is the worst time to find them.
  void set_stress(bool on) { stress_ = on; }
  bool stress() const { return stress_; }

  // Interning makes string equality a pointer comparison and makes global
  // variable lookup a hash-table probe with a precomputed hash. The table
  // holds weak references: an entry whose string is unreachable is removed
  // during collection, or nothing would ever be freed.
  //
  // Used for identifiers and source literals, which are few, repeated, and
  // short.
  ObjString* intern(const std::string& text);

  // Allocates without interning. Used for strings computed at run time.
  ObjString* new_string(std::string text);

  // Interns short strings and allocates long ones.
  //
  // Interning unconditionally is the obvious design and it makes string
  // building quadratic: `s = s + "x"` in a loop hashes the entire accumulated
  // string and inserts it into a table that never shrinks, once per iteration.
  // Above the threshold the copy is allocated fresh, and equality falls back to
  // comparing contents - which is why Value::operator== cannot assume that
  // equal strings are the same pointer.
  ObjString* string(std::string text);

  static constexpr std::size_t kInternThreshold = 32;

  ObjFunction* new_function();
  ObjClosure* new_closure(ObjFunction* fn);
  ObjUpvalue* new_upvalue(Value* slot);
  ObjNative* new_native(NativeFn fn, ObjString* name, int arity);
  ObjList* new_list();

  void mark_value(const Value& v);
  void mark_object(Obj* obj);

  void collect();
  void maybe_collect();

  // Protects an object that is reachable only from a C++ local.
  void push_temp(Obj* obj) { temp_roots_.push_back(obj); }
  void pop_temp(std::size_t n = 1);

  const Stats& stats() const { return stats_; }
  std::size_t live_objects() const;

 private:
  void trace_references();
  void blacken(Obj* obj);
  void sweep();
  void remove_unmarked_strings();
  void track(Obj* obj, std::size_t size);

  Obj* objects_ = nullptr;
  std::vector<Obj*> gray_;
  std::vector<Obj*> temp_roots_;
  std::unordered_map<std::string, ObjString*> strings_;
  RootMarker root_marker_;

  bool collecting_ = false;
  bool stress_ = false;
  Stats stats_;

  static constexpr std::size_t kInitialThreshold = 1 << 20;
  static constexpr std::size_t kHeapGrowFactor = 2;
};

// Scoped temporary root. Declaring one is the difference between a native
// function that works and one that fails only when a collection lands between
// two allocations.
class TempRoot {
 public:
  TempRoot(GC& gc, Obj* obj) : gc_(gc) { gc_.push_temp(obj); }
  ~TempRoot() { gc_.pop_temp(); }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;

 private:
  GC& gc_;
};

}  // namespace lumen
