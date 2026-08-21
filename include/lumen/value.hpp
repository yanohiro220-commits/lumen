#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace lumen {

struct Obj;
struct ObjString;
struct ObjFunction;
struct ObjClosure;
struct ObjUpvalue;
struct ObjNative;

// A dynamically typed value in 8 bytes, via NaN boxing.
//
// An IEEE-754 double has 2^52 distinct quiet-NaN bit patterns that no
// arithmetic ever produces. Everything that is not a number is encoded as one
// of them, so a value is a single 64-bit word: no tag byte, no padding to 16
// bytes, and the common case - a double - needs no decoding at all.
//
//   sign(1) exponent(11)   quiet  payload(51)
//   [s]     [11111111111]  [1]    [.................]
//
// The three singletons nil/false/true live in the low bits; pointers use the
// sign bit as their tag, which is safe because every pointer we store comes
// from the allocator and fits in 48 bits on every platform this targets.
class Value {
 public:
  constexpr Value() : bits_(kNil) {}

  static Value number(double d) {
    Value v;
    std::memcpy(&v.bits_, &d, sizeof(double));
    return v;
  }
  static constexpr Value nil() { return Value(kNil); }
  static constexpr Value boolean(bool b) { return Value(b ? kTrue : kFalse); }
  static Value object(Obj* o) {
    return Value(kSignBit | kQuietNan |
                 static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(o)));
  }

  bool is_number() const { return (bits_ & kQuietNan) != kQuietNan; }
  bool is_nil() const { return bits_ == kNil; }
  bool is_bool() const { return (bits_ | 1) == kTrue; }
  bool is_object() const {
    return (bits_ & (kQuietNan | kSignBit)) == (kQuietNan | kSignBit);
  }

  double as_number() const {
    double d;
    std::memcpy(&d, &bits_, sizeof(double));
    return d;
  }
  bool as_bool() const { return bits_ == kTrue; }
  Obj* as_object() const {
    return reinterpret_cast<Obj*>(
        static_cast<std::uintptr_t>(bits_ & ~(kSignBit | kQuietNan)));
  }

  // Only nil and false are falsey. Zero and the empty string are truthy, which
  // is a language design choice, not an accident: it removes a class of bug
  // where a valid empty value silently takes the wrong branch.
  bool truthy() const { return !(is_nil() || (is_bool() && !as_bool())); }

  bool operator==(const Value& other) const;
  bool operator!=(const Value& other) const { return !(*this == other); }

  std::uint64_t bits() const { return bits_; }
  std::string to_string() const;
  const char* type_name() const;

 private:
  explicit constexpr Value(std::uint64_t b) : bits_(b) {}

  static constexpr std::uint64_t kSignBit = 0x8000000000000000ULL;
  static constexpr std::uint64_t kQuietNan = 0x7ffc000000000000ULL;
  static constexpr std::uint64_t kNil = kQuietNan | 1;
  static constexpr std::uint64_t kFalse = kQuietNan | 2;
  static constexpr std::uint64_t kTrue = kQuietNan | 3;

  std::uint64_t bits_;
};

}  // namespace lumen
