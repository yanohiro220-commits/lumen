#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

#include "lumen/gc.hpp"
#include "lumen/object.hpp"

namespace lumen {

// The bytecode interpreter.
//
// One VM owns one heap. The compiler allocates into the same GC, so functions
// and interned strings produced at compile time are ordinary heap objects
// traced by the same collector - there is no separate lifetime to reason about.
class VM {
 public:
  enum class Result { Ok, CompileError, RuntimeError };

  explicit VM(std::ostream& out);
  ~VM();

  VM(const VM&) = delete;
  VM& operator=(const VM&) = delete;

  GC& gc() { return gc_; }

  // Runs a compiled script to completion.
  Result run(ObjFunction* script);

  const std::string& error() const { return error_; }

  struct Stats {
    std::uint64_t instructions = 0;
    std::uint64_t calls = 0;
    std::size_t max_stack_depth = 0;
    std::size_t max_frame_depth = 0;
  };
  const Stats& stats() const { return stats_; }

  // Instruction counting costs a branch per instruction, so it is off unless
  // asked for. The benchmarks report it; a normal run does not pay for it.
  void set_trace_counts(bool on) { count_instructions_ = on; }

  void define_native(const std::string& name, int arity, NativeFn fn);

  // Exposed for tests: the value of a global after a run.
  bool global(const std::string& name, Value* out) const;

 private:
  struct CallFrame {
    ObjClosure* closure = nullptr;
    const std::uint8_t* ip = nullptr;
    Value* slots = nullptr;
  };

  // A fixed stack, not a std::vector. Open upvalues hold raw pointers into it,
  // so a reallocation would leave every live closure pointing at freed memory.
  // Growing would mean indirecting every upvalue through an index, which costs
  // more on the hot path than this costs in address space.
  static constexpr std::size_t kFramesMax = 256;
  static constexpr std::size_t kStackMax = kFramesMax * 256;

  void reset_stack();
  void push(Value v) { *stack_top_++ = v; }
  Value pop() { return *--stack_top_; }
  Value peek(int distance) const { return stack_top_[-1 - distance]; }

  bool call_value(Value callee, int arg_count);
  bool call_closure(ObjClosure* closure, int arg_count);
  ObjUpvalue* capture_upvalue(Value* local);
  void close_upvalues(Value* last);

  bool add_values(const Value& a, const Value& b, Value* out);

  void runtime_error(const std::string& message);
  void mark_roots(GC& gc);
  void define_builtins();

  Result execute();

  GC gc_;
  std::ostream& out_;

  Value* stack_ = nullptr;
  Value* stack_top_ = nullptr;
  CallFrame frames_[kFramesMax];
  int frame_count_ = 0;

  std::unordered_map<ObjString*, Value> globals_;
  ObjUpvalue* open_upvalues_ = nullptr;

  std::string error_;
  Stats stats_;
  bool count_instructions_ = false;
};

}  // namespace lumen
