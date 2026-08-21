#include "lumen/value.hpp"

#include <cmath>
#include <sstream>

#include "lumen/object.hpp"

namespace lumen {

std::uint32_t hash_string(const std::string& s) {
  // FNV-1a.
  std::uint32_t hash = 2166136261u;
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

bool Value::operator==(const Value& other) const {
  // Numbers must be compared as doubles, not as bit patterns: -0.0 and +0.0
  // have different bits and must compare equal, and NaN has identical bits to
  // itself and must not.
  if (is_number() && other.is_number()) return as_number() == other.as_number();
  if (is_object() && other.is_object()) {
    Obj* a = as_object();
    Obj* b = other.as_object();
    if (a == b) return true;
    // Short strings are interned, so most equal strings are already the same
    // pointer and the check above caught them. Long ones are not, so equality
    // still has to compare contents - the hash makes the common unequal case
    // one comparison.
    if (a->type == ObjType::String && b->type == ObjType::String) {
      const auto* sa = static_cast<ObjString*>(a);
      const auto* sb = static_cast<ObjString*>(b);
      // Length first: it is free and rejects most unequal pairs without
      // touching the bytes or forcing a hash to be computed.
      return sa->chars.size() == sb->chars.size() && sa->chars == sb->chars;
    }
    // Lists compare structurally, which is what a user expects from `==` on a
    // value type.
    if (a->type == ObjType::List && b->type == ObjType::List) {
      const auto& la = static_cast<ObjList*>(a)->items;
      const auto& lb = static_cast<ObjList*>(b)->items;
      if (la.size() != lb.size()) return false;
      for (std::size_t i = 0; i < la.size(); ++i) {
        if (!(la[i] == lb[i])) return false;
      }
      return true;
    }
    return false;
  }
  return bits_ == other.bits_;
}

const char* Value::type_name() const {
  if (is_number()) return "number";
  if (is_nil()) return "nil";
  if (is_bool()) return "bool";
  if (is_object()) {
    switch (as_object()->type) {
      case ObjType::String: return "string";
      case ObjType::Function: return "function";
      case ObjType::Closure: return "function";
      case ObjType::Native: return "native function";
      case ObjType::Upvalue: return "upvalue";
      case ObjType::List: return "list";
    }
  }
  return "unknown";
}

std::string Value::to_string() const {
  if (is_number()) {
    const double d = as_number();
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    // Integral values print without a fractional part; `print 1 + 1` should
    // read `2`, not `2.000000`.
    if (d == std::floor(d) && std::abs(d) < 1e15) {
      return std::to_string(static_cast<long long>(d));
    }
    std::ostringstream os;
    os.precision(15);
    os << d;
    return os.str();
  }
  if (is_nil()) return "nil";
  if (is_bool()) return as_bool() ? "true" : "false";
  if (is_object()) {
    Obj* o = as_object();
    switch (o->type) {
      case ObjType::String:
        return static_cast<ObjString*>(o)->chars;
      case ObjType::Function: {
        auto* fn = static_cast<ObjFunction*>(o);
        return "<fn " + (fn->name ? fn->name->chars : std::string("script")) + ">";
      }
      case ObjType::Closure: {
        auto* fn = static_cast<ObjClosure*>(o)->function;
        return "<fn " + (fn->name ? fn->name->chars : std::string("script")) + ">";
      }
      case ObjType::Native: {
        auto* n = static_cast<ObjNative*>(o);
        return "<native " + (n->name ? n->name->chars : std::string("?")) + ">";
      }
      case ObjType::Upvalue:
        return "<upvalue>";
      case ObjType::List: {
        auto* list = static_cast<ObjList*>(o);
        std::string out = "[";
        for (std::size_t i = 0; i < list->items.size(); ++i) {
          if (i) out += ", ";
          const Value& item = list->items[i];
          // Strings nest quoted, so `[1, "a"]` is unambiguous, while a bare
          // `print "a"` stays unquoted.
          if (is_string(item)) {
            out += "\"" + as_string(item)->chars + "\"";
          } else {
            out += item.to_string();
          }
        }
        return out + "]";
      }
    }
  }
  return "<unknown>";
}

}  // namespace lumen
