#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lumen/chunk.hpp"
#include "lumen/value.hpp"

namespace lumen {

enum class ObjType : std::uint8_t { String, Function, Closure, Upvalue, Native, List };

// Base of every heap object.
//
// `next` threads all live objects into an intrusive list so the sweep phase can
// walk the heap without a separate registry, and `marked` is the mark bit. One
// word of header per object, which for a language whose common object is a
// short string is the difference between viable and not.
struct Obj {
  ObjType type;
  bool marked = false;
  Obj* next = nullptr;

  explicit Obj(ObjType t) : type(t) {}
  virtual ~Obj() = default;
};

// FNV-1a over the bytes.
std::uint32_t hash_string(const std::string& s);

struct ObjString : Obj {
  std::string chars;

  explicit ObjString(std::string s) : Obj(ObjType::String), chars(std::move(s)) {}

  // Computed on first use, not at construction.
  //
  // Hashing eagerly costs a full pass over the bytes for every string the
  // program builds, and most computed strings are never used as a key or
  // compared to a string they are not identical to. `s = s + "x"` in a loop
  // pays that pass on a string that grows every iteration - a second quadratic
  // term on top of the copy the concatenation already requires.
  std::uint32_t hash() const {
    if (!hashed_) {
      hash_ = hash_string(chars);
      hashed_ = true;
    }
    return hash_;
  }

 private:
  mutable std::uint32_t hash_ = 0;
  mutable bool hashed_ = false;
};

struct ObjFunction : Obj {
  int arity = 0;
  int upvalue_count = 0;
  Chunk chunk;
  ObjString* name = nullptr;

  ObjFunction() : Obj(ObjType::Function) {}
};

using NativeFn = std::function<bool(int argc, Value* argv, Value* result, std::string* error)>;

struct ObjNative : Obj {
  NativeFn function;
  ObjString* name = nullptr;
  int arity = -1;  // -1 means variadic

  ObjNative(NativeFn fn, ObjString* n, int a)
      : Obj(ObjType::Native), function(std::move(fn)), name(n), arity(a) {}
};

// A captured variable.
//
// While the enclosing function is still on the stack the upvalue points at the
// stack slot, so reads and writes are shared with the local. When the frame
// returns the value is copied into `closed` and the pointer is redirected at
// it. Without that second step a closure outliving its creator would read a
// reclaimed stack slot - the classic upward funarg bug.
struct ObjUpvalue : Obj {
  Value* location = nullptr;
  Value closed;
  ObjUpvalue* next_open = nullptr;

  explicit ObjUpvalue(Value* slot) : Obj(ObjType::Upvalue), location(slot) {}
};

struct ObjClosure : Obj {
  ObjFunction* function = nullptr;
  std::vector<ObjUpvalue*> upvalues;

  explicit ObjClosure(ObjFunction* fn)
      : Obj(ObjType::Closure), function(fn), upvalues(fn->upvalue_count, nullptr) {}
};

struct ObjList : Obj {
  std::vector<Value> items;

  ObjList() : Obj(ObjType::List) {}
};

inline bool is_obj_type(const Value& v, ObjType t) {
  return v.is_object() && v.as_object()->type == t;
}
inline bool is_string(const Value& v) { return is_obj_type(v, ObjType::String); }
inline bool is_list(const Value& v) { return is_obj_type(v, ObjType::List); }
inline ObjString* as_string(const Value& v) {
  return static_cast<ObjString*>(v.as_object());
}
inline ObjList* as_list(const Value& v) {
  return static_cast<ObjList*>(v.as_object());
}

}  // namespace lumen
