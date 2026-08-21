#include "lumen/vm.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <ostream>

namespace lumen {

// Computed-goto dispatch where the compiler supports labels-as-values.
//
// A switch compiles to one indirect branch shared by every opcode, so the
// processor's branch predictor sees a single site with dozens of targets and
// mispredicts constantly. Threading the dispatch into the tail of each handler
// gives each opcode its own branch site, and opcode sequences are highly
// correlated - a GET_LOCAL is usually followed by the same thing - so those
// sites predict well. The switch path below is kept working because it is the
// only portable one, and both are exercised by the test suite.
#if defined(__GNUC__) || defined(__clang__)
#define LUMEN_COMPUTED_GOTO 1
#endif

VM::VM(std::ostream& out) : out_(out) {
  stack_ = new Value[kStackMax];
  reset_stack();
  gc_.set_root_marker([this](GC& g) { mark_roots(g); });
  define_builtins();
}

VM::~VM() { delete[] stack_; }

void VM::reset_stack() {
  stack_top_ = stack_;
  frame_count_ = 0;
  open_upvalues_ = nullptr;
}

void VM::mark_roots(GC& gc) {
  for (Value* slot = stack_; slot < stack_top_; ++slot) gc.mark_value(*slot);
  for (int i = 0; i < frame_count_; ++i) gc.mark_object(frames_[i].closure);
  for (ObjUpvalue* u = open_upvalues_; u != nullptr; u = u->next_open) {
    gc.mark_object(u);
  }
  for (const auto& [name, value] : globals_) {
    gc.mark_object(name);
    gc.mark_value(value);
  }
}

void VM::runtime_error(const std::string& message) {
  std::string out = message;
  // Stack trace, innermost frame first. Without it a failure in a helper
  // reports a line the user never wrote.
  for (int i = frame_count_ - 1; i >= 0; --i) {
    const CallFrame& frame = frames_[i];
    const ObjFunction* fn = frame.closure->function;
    const std::size_t offset =
        static_cast<std::size_t>(frame.ip - fn->chunk.code().data() - 1);
    out += "\n  [line " + std::to_string(fn->chunk.line_at(offset)) + "] in " +
           (fn->name ? fn->name->chars + "()" : std::string("script"));
  }
  error_ = out;
  reset_stack();
}

void VM::define_native(const std::string& name, int arity, NativeFn fn) {
  ObjString* key = gc_.intern(name);
  TempRoot root(gc_, key);
  ObjNative* native = gc_.new_native(std::move(fn), key, arity);
  globals_[key] = Value::object(native);
}

bool VM::global(const std::string& name, Value* out) const {
  // const_cast because interning may allocate; looking up a name that was
  // never interned cannot match anything, so search the map directly instead.
  for (const auto& [key, value] : globals_) {
    if (key->chars == name) {
      if (out) *out = value;
      return true;
    }
  }
  return false;
}

void VM::define_builtins() {
  define_native("clock", 0, [](int, Value*, Value* result, std::string*) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    *result = Value::number(
        std::chrono::duration<double>(now).count());
    return true;
  });

  define_native("len", 1, [](int, Value* argv, Value* result, std::string* err) {
    if (is_string(argv[0])) {
      *result = Value::number(static_cast<double>(as_string(argv[0])->chars.size()));
      return true;
    }
    if (is_list(argv[0])) {
      *result = Value::number(static_cast<double>(as_list(argv[0])->items.size()));
      return true;
    }
    *err = std::string("len() expects a string or list, got ") + argv[0].type_name();
    return false;
  });

  define_native("str", 1, [this](int, Value* argv, Value* result, std::string*) {
    *result = Value::object(gc_.string(argv[0].to_string()));
    return true;
  });

  define_native("num", 1, [](int, Value* argv, Value* result, std::string* err) {
    if (argv[0].is_number()) {
      *result = argv[0];
      return true;
    }
    if (!is_string(argv[0])) {
      *err = std::string("num() expects a string or number, got ") + argv[0].type_name();
      return false;
    }
    const std::string& text = as_string(argv[0])->chars;
    try {
      std::size_t consumed = 0;
      const double d = std::stod(text, &consumed);
      // A partial parse means the input was not a number. Returning the prefix
      // would silently turn "12abc" into 12.
      if (consumed != text.size()) {
        *result = Value::nil();
        return true;
      }
      *result = Value::number(d);
    } catch (...) {
      *result = Value::nil();
    }
    return true;
  });

  define_native("type", 1, [this](int, Value* argv, Value* result, std::string*) {
    *result = Value::object(gc_.intern(argv[0].type_name()));
    return true;
  });

  define_native("push", 2, [](int, Value* argv, Value* result, std::string* err) {
    if (!is_list(argv[0])) {
      *err = std::string("push() expects a list, got ") + argv[0].type_name();
      return false;
    }
    as_list(argv[0])->items.push_back(argv[1]);
    *result = argv[0];
    return true;
  });

  define_native("pop", 1, [](int, Value* argv, Value* result, std::string* err) {
    if (!is_list(argv[0])) {
      *err = std::string("pop() expects a list, got ") + argv[0].type_name();
      return false;
    }
    auto& items = as_list(argv[0])->items;
    if (items.empty()) {
      *err = "pop() on an empty list";
      return false;
    }
    *result = items.back();
    items.pop_back();
    return true;
  });

  define_native("sqrt", 1, [](int, Value* argv, Value* result, std::string* err) {
    if (!argv[0].is_number()) {
      *err = std::string("sqrt() expects a number, got ") + argv[0].type_name();
      return false;
    }
    *result = Value::number(std::sqrt(argv[0].as_number()));
    return true;
  });

  define_native("floor", 1, [](int, Value* argv, Value* result, std::string* err) {
    if (!argv[0].is_number()) {
      *err = std::string("floor() expects a number, got ") + argv[0].type_name();
      return false;
    }
    *result = Value::number(std::floor(argv[0].as_number()));
    return true;
  });

  define_native("abs", 1, [](int, Value* argv, Value* result, std::string* err) {
    if (!argv[0].is_number()) {
      *err = std::string("abs() expects a number, got ") + argv[0].type_name();
      return false;
    }
    *result = Value::number(std::abs(argv[0].as_number()));
    return true;
  });
}

ObjUpvalue* VM::capture_upvalue(Value* local) {
  // The open-upvalue list is kept sorted by stack address, descending. Two
  // closures capturing the same variable must share one upvalue, or assigning
  // through one would not be visible through the other.
  ObjUpvalue* previous = nullptr;
  ObjUpvalue* current = open_upvalues_;
  while (current != nullptr && current->location > local) {
    previous = current;
    current = current->next_open;
  }
  if (current != nullptr && current->location == local) return current;

  ObjUpvalue* created = gc_.new_upvalue(local);
  created->next_open = current;
  if (previous == nullptr) {
    open_upvalues_ = created;
  } else {
    previous->next_open = created;
  }
  return created;
}

void VM::close_upvalues(Value* last) {
  while (open_upvalues_ != nullptr && open_upvalues_->location >= last) {
    ObjUpvalue* upvalue = open_upvalues_;
    // Copy the value into the upvalue and redirect the pointer at the copy.
    // Skipping this leaves the closure pointing at a stack slot that the next
    // call will reuse.
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    open_upvalues_ = upvalue->next_open;
  }
}

bool VM::call_closure(ObjClosure* closure, int arg_count) {
  if (arg_count != closure->function->arity) {
    runtime_error(std::string("expected ") +
                  std::to_string(closure->function->arity) + " argument" +
                  (closure->function->arity == 1 ? "" : "s") + " but got " +
                  std::to_string(arg_count));
    return false;
  }
  if (frame_count_ == kFramesMax) {
    runtime_error("stack overflow: call depth exceeded " +
                  std::to_string(kFramesMax));
    return false;
  }
  CallFrame& frame = frames_[frame_count_++];
  frame.closure = closure;
  frame.ip = closure->function->chunk.code().data();
  frame.slots = stack_top_ - arg_count - 1;
  if (static_cast<std::size_t>(frame_count_) > stats_.max_frame_depth) {
    stats_.max_frame_depth = static_cast<std::size_t>(frame_count_);
  }
  ++stats_.calls;
  return true;
}

bool VM::call_value(Value callee, int arg_count) {
  if (callee.is_object()) {
    switch (callee.as_object()->type) {
      case ObjType::Closure:
        return call_closure(static_cast<ObjClosure*>(callee.as_object()), arg_count);
      case ObjType::Native: {
        auto* native = static_cast<ObjNative*>(callee.as_object());
        if (native->arity >= 0 && native->arity != arg_count) {
          runtime_error(native->name->chars + "() expects " +
                        std::to_string(native->arity) + " argument" +
                        (native->arity == 1 ? "" : "s") + " but got " +
                        std::to_string(arg_count));
          return false;
        }
        Value result = Value::nil();
        std::string err;
        if (!native->function(arg_count, stack_top_ - arg_count, &result, &err)) {
          runtime_error(err);
          return false;
        }
        stack_top_ -= arg_count + 1;
        push(result);
        return true;
      }
      default:
        break;
    }
  }
  runtime_error(std::string("can only call functions, got ") + callee.type_name());
  return false;
}


namespace {

bool values_less(const Value& a, const Value& b, bool* ok) {
  *ok = true;
  if (a.is_number() && b.is_number()) return a.as_number() < b.as_number();
  if (is_string(a) && is_string(b)) return as_string(a)->chars < as_string(b)->chars;
  *ok = false;
  return false;
}

}  // namespace

// Addition, shared by ADD and its superinstruction.
//
// Factored out precisely because a superinstruction is only correct if it is
// indistinguishable from the sequence it replaces. Two copies of these rules
// would drift, and the drift would show up as a program behaving differently
// with the optimizer on.
bool VM::add_values(const Value& a, const Value& b, Value* out) {
  if (a.is_number() && b.is_number()) {
    *out = Value::number(a.as_number() + b.as_number());
    return true;
  }
  if (is_string(a) && is_string(b)) {
    *out = Value::object(gc_.string(as_string(a)->chars + as_string(b)->chars));
    return true;
  }
  if (is_list(a) && is_list(b)) {
    ObjList* joined = gc_.new_list();
    TempRoot root(gc_, joined);
    joined->items = as_list(a)->items;
    const auto& rhs = as_list(b)->items;
    joined->items.insert(joined->items.end(), rhs.begin(), rhs.end());
    *out = Value::object(joined);
    return true;
  }
  runtime_error(std::string("cannot add ") + a.type_name() + " and " +
                b.type_name());
  return false;
}

// Labels-as-values is a GNU extension, and -Wpedantic is on for everything
// else in this file on purpose. Scoping the suppression to the dispatch loop
// keeps the rest of the translation unit strictly conforming.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

VM::Result VM::execute() {
  CallFrame* frame = &frames_[frame_count_ - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_U16()                                       \
  (frame->ip += 2, static_cast<std::uint16_t>(           \
                       (frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants()[READ_U16()])
#define RUNTIME_ERROR(msg)      \
  do {                          \
    runtime_error(msg);         \
    return Result::RuntimeError;\
  } while (0)

#ifdef LUMEN_COMPUTED_GOTO
  // A full 256-entry table, not one entry per opcode.
  //
  // A switch has `default:`; a jump table has nothing. Indexing a table of
  // OpCount entries with a byte that is out of range jumps to whatever happens
  // to follow it in memory, and the failure is a segmentation fault with no
  // indication of where it came from. Padding the table so every possible byte
  // lands on a trap turns that into a diagnosable error at the cost of about a
  // kilobyte of stack.
  //
  // Not `static`, either. The GNU manual's own caveat is that a table of label
  // addresses is only meaningful inside the function the labels belong to, and
  // a function-scope static initialised from them interacts badly with a
  // compiler that clones or specialises that function.
  void* dispatch_table[256];
  for (std::size_t i = 0; i < 256; ++i) dispatch_table[i] = &&op_Invalid;
  {
    std::size_t next = 0;
#define LUMEN_OP_LABEL(name, text) dispatch_table[next++] = &&op_##name;
    LUMEN_OPCODES(LUMEN_OP_LABEL)
#undef LUMEN_OP_LABEL
    if (next != static_cast<std::size_t>(Op::OpCount)) {
      // The enum, the name table and this table are all generated from the
      // same list, so they cannot drift - but the check costs nothing and the
      // failure it would catch is untraceable.
      RUNTIME_ERROR("internal error: dispatch table does not match the opcode enum");
    }
  }

#define VM_TALLY()                                                          \
  do {                                                                      \
    ++stats_.instructions;                                                  \
    const std::size_t depth = static_cast<std::size_t>(stack_top_ - stack_);\
    if (depth > stats_.max_stack_depth) stats_.max_stack_depth = depth;     \
  } while (0)
#define VM_DISPATCH()                                  \
  do {                                                 \
    if (count_instructions_) VM_TALLY();               \
    goto *dispatch_table[READ_BYTE()];                 \
  } while (0)
#define VM_BEGIN VM_DISPATCH();
#define VM_CASE(name) op_##name:
#define VM_NEXT VM_DISPATCH()
#define VM_END
#else
#define VM_TALLY()                                                          \
  do {                                                                      \
    ++stats_.instructions;                                                  \
    const std::size_t depth = static_cast<std::size_t>(stack_top_ - stack_);\
    if (depth > stats_.max_stack_depth) stats_.max_stack_depth = depth;     \
  } while (0)
#define VM_BEGIN                                       \
  for (;;) {                                           \
    if (count_instructions_) VM_TALLY();               \
    switch (static_cast<Op>(READ_BYTE())) {
#define VM_CASE(name) case Op::name:
#define VM_NEXT break
#define VM_END                                         \
  default:                                             \
    RUNTIME_ERROR("corrupt bytecode: unknown opcode");  \
    }                                                  \
  }
#endif

  VM_BEGIN

  VM_CASE(Constant) {
    push(READ_CONSTANT());
    VM_NEXT;
  }
  VM_CASE(Nil) { push(Value::nil()); VM_NEXT; }
  VM_CASE(True) { push(Value::boolean(true)); VM_NEXT; }
  VM_CASE(False) { push(Value::boolean(false)); VM_NEXT; }
  VM_CASE(Pop) { pop(); VM_NEXT; }

  VM_CASE(GetLocal) {
    const std::uint8_t slot = READ_BYTE();
    push(frame->slots[slot]);
    VM_NEXT;
  }
  VM_CASE(SetLocal) {
    const std::uint8_t slot = READ_BYTE();
    // Peeked, not popped: assignment is an expression and evaluates to the
    // assigned value.
    frame->slots[slot] = peek(0);
    VM_NEXT;
  }
  VM_CASE(GetGlobal) {
    ObjString* name = as_string(READ_CONSTANT());
    const auto it = globals_.find(name);
    if (it == globals_.end()) {
      RUNTIME_ERROR("undefined variable '" + name->chars + "'");
    }
    push(it->second);
    VM_NEXT;
  }
  VM_CASE(DefineGlobal) {
    ObjString* name = as_string(READ_CONSTANT());
    globals_[name] = peek(0);
    pop();
    VM_NEXT;
  }
  VM_CASE(SetGlobal) {
    ObjString* name = as_string(READ_CONSTANT());
    const auto it = globals_.find(name);
    if (it == globals_.end()) {
      // Assigning to a name that was never declared is a typo, not an implicit
      // declaration. Silently creating it is how a misspelling becomes a bug
      // that surfaces somewhere else entirely.
      RUNTIME_ERROR("undefined variable '" + name->chars + "'");
    }
    it->second = peek(0);
    VM_NEXT;
  }
  VM_CASE(GetUpvalue) {
    const std::uint8_t slot = READ_BYTE();
    push(*frame->closure->upvalues[slot]->location);
    VM_NEXT;
  }
  VM_CASE(SetUpvalue) {
    const std::uint8_t slot = READ_BYTE();
    *frame->closure->upvalues[slot]->location = peek(0);
    VM_NEXT;
  }

  VM_CASE(Equal) {
    const Value b = pop();
    const Value a = pop();
    push(Value::boolean(a == b));
    VM_NEXT;
  }
  VM_CASE(NotEqual) {
    const Value b = pop();
    const Value a = pop();
    push(Value::boolean(!(a == b)));
    VM_NEXT;
  }
  VM_CASE(Less) {
    bool ok = false;
    const Value b = pop();
    const Value a = pop();
    const bool r = values_less(a, b, &ok);
    if (!ok) RUNTIME_ERROR(std::string("cannot compare ") + a.type_name() + " and " + b.type_name());
    push(Value::boolean(r));
    VM_NEXT;
  }
  VM_CASE(LessEqual) {
    bool ok = false;
    const Value b = pop();
    const Value a = pop();
    const bool greater = values_less(b, a, &ok);
    if (!ok) RUNTIME_ERROR(std::string("cannot compare ") + a.type_name() + " and " + b.type_name());
    push(Value::boolean(!greater));
    VM_NEXT;
  }
  VM_CASE(Greater) {
    bool ok = false;
    const Value b = pop();
    const Value a = pop();
    const bool r = values_less(b, a, &ok);
    if (!ok) RUNTIME_ERROR(std::string("cannot compare ") + a.type_name() + " and " + b.type_name());
    push(Value::boolean(r));
    VM_NEXT;
  }
  VM_CASE(GreaterEqual) {
    bool ok = false;
    const Value b = pop();
    const Value a = pop();
    const bool less = values_less(a, b, &ok);
    if (!ok) RUNTIME_ERROR(std::string("cannot compare ") + a.type_name() + " and " + b.type_name());
    push(Value::boolean(!less));
    VM_NEXT;
  }

  VM_CASE(Add) {
    const Value b = pop();
    const Value a = pop();
    Value result;
    if (!add_values(a, b, &result)) return Result::RuntimeError;
    push(result);
    VM_NEXT;
  }
  VM_CASE(Subtract) {
    const Value b = pop();
    const Value a = pop();
    if (!a.is_number() || !b.is_number()) {
      RUNTIME_ERROR(std::string("cannot subtract ") + b.type_name() + " from " + a.type_name());
    }
    push(Value::number(a.as_number() - b.as_number()));
    VM_NEXT;
  }
  VM_CASE(Multiply) {
    const Value b = pop();
    const Value a = pop();
    if (!a.is_number() || !b.is_number()) {
      RUNTIME_ERROR(std::string("cannot multiply ") + a.type_name() + " by " + b.type_name());
    }
    push(Value::number(a.as_number() * b.as_number()));
    VM_NEXT;
  }
  VM_CASE(Divide) {
    const Value b = pop();
    const Value a = pop();
    if (!a.is_number() || !b.is_number()) {
      RUNTIME_ERROR(std::string("cannot divide ") + a.type_name() + " by " + b.type_name());
    }
    if (b.as_number() == 0.0) {
      // IEEE would give infinity. A language that silently produces infinity
      // here turns a logic error into a number that propagates through the
      // rest of the computation.
      RUNTIME_ERROR("division by zero");
    }
    push(Value::number(a.as_number() / b.as_number()));
    VM_NEXT;
  }
  VM_CASE(Modulo) {
    const Value b = pop();
    const Value a = pop();
    if (!a.is_number() || !b.is_number()) {
      RUNTIME_ERROR(std::string("cannot take ") + a.type_name() + " modulo " + b.type_name());
    }
    if (b.as_number() == 0.0) RUNTIME_ERROR("modulo by zero");
    push(Value::number(std::fmod(a.as_number(), b.as_number())));
    VM_NEXT;
  }
  VM_CASE(Negate) {
    if (!peek(0).is_number()) {
      RUNTIME_ERROR(std::string("cannot negate ") + peek(0).type_name());
    }
    const Value v = pop();
    push(Value::number(-v.as_number()));
    VM_NEXT;
  }
  VM_CASE(Not) {
    push(Value::boolean(!pop().truthy()));
    VM_NEXT;
  }

  VM_CASE(Print) {
    out_ << pop().to_string() << "\n";
    VM_NEXT;
  }

  VM_CASE(Jump) {
    const std::uint16_t offset = READ_U16();
    frame->ip += offset;
    VM_NEXT;
  }
  VM_CASE(JumpIfFalse) {
    const std::uint16_t offset = READ_U16();
    if (!peek(0).truthy()) frame->ip += offset;
    VM_NEXT;
  }
  VM_CASE(JumpIfTrue) {
    const std::uint16_t offset = READ_U16();
    if (peek(0).truthy()) frame->ip += offset;
    VM_NEXT;
  }
  VM_CASE(PopJumpIfFalse) {
    const std::uint16_t offset = READ_U16();
    if (!pop().truthy()) frame->ip += offset;
    VM_NEXT;
  }
  VM_CASE(Loop) {
    const std::uint16_t offset = READ_U16();
    frame->ip -= offset;
    VM_NEXT;
  }

  VM_CASE(Call) {
    const std::uint8_t arg_count = READ_BYTE();
    if (stack_top_ - stack_ > static_cast<std::ptrdiff_t>(kStackMax - 256)) {
      RUNTIME_ERROR("stack overflow");
    }
    if (!call_value(peek(arg_count), arg_count)) return Result::RuntimeError;
    frame = &frames_[frame_count_ - 1];
    VM_NEXT;
  }
  VM_CASE(Closure) {
    auto* function = static_cast<ObjFunction*>(READ_CONSTANT().as_object());
    ObjClosure* closure = gc_.new_closure(function);
    // Pushed before the upvalues are captured: capture_upvalue allocates, and
    // the closure is otherwise reachable only from this C++ local.
    push(Value::object(closure));
    for (int i = 0; i < function->upvalue_count; ++i) {
      const std::uint8_t is_local = READ_BYTE();
      const std::uint8_t index = READ_BYTE();
      closure->upvalues[static_cast<std::size_t>(i)] =
          is_local ? capture_upvalue(frame->slots + index)
                   : frame->closure->upvalues[index];
    }
    VM_NEXT;
  }
  VM_CASE(CloseUpvalue) {
    close_upvalues(stack_top_ - 1);
    pop();
    VM_NEXT;
  }
  VM_CASE(Return) {
    const Value result = pop();
    close_upvalues(frame->slots);
    --frame_count_;
    if (frame_count_ == 0) {
      pop();
      return Result::Ok;
    }
    stack_top_ = frame->slots;
    push(result);
    frame = &frames_[frame_count_ - 1];
    VM_NEXT;
  }

  VM_CASE(BuildList) {
    const std::uint16_t count = READ_U16();
    ObjList* list = gc_.new_list();
    list->items.assign(stack_top_ - count, stack_top_);
    stack_top_ -= count;
    push(Value::object(list));
    VM_NEXT;
  }
  VM_CASE(IndexGet) {
    const Value index = pop();
    const Value target = pop();
    if (!index.is_number()) {
      RUNTIME_ERROR(std::string("index must be a number, got ") + index.type_name());
    }
    const double raw = index.as_number();
    if (raw != std::floor(raw)) RUNTIME_ERROR("index must be a whole number");
    long long i = static_cast<long long>(raw);

    if (is_list(target)) {
      auto& items = as_list(target)->items;
      // Negative indices count from the end, which is worth having and is
      // exactly where an off-by-one hides, so it is tested both ways.
      if (i < 0) i += static_cast<long long>(items.size());
      if (i < 0 || i >= static_cast<long long>(items.size())) {
        RUNTIME_ERROR("list index " + std::to_string(static_cast<long long>(raw)) +
                      " out of range for length " + std::to_string(items.size()));
      }
      push(items[static_cast<std::size_t>(i)]);
      VM_NEXT;
    }
    if (is_string(target)) {
      const std::string& text = as_string(target)->chars;
      if (i < 0) i += static_cast<long long>(text.size());
      if (i < 0 || i >= static_cast<long long>(text.size())) {
        RUNTIME_ERROR("string index " + std::to_string(static_cast<long long>(raw)) +
                      " out of range for length " + std::to_string(text.size()));
      }
      push(Value::object(gc_.intern(std::string(1, text[static_cast<std::size_t>(i)]))));
      VM_NEXT;
    }
    RUNTIME_ERROR(std::string("cannot index a ") + target.type_name());
  }
  VM_CASE(IndexSet) {
    const Value value = pop();
    const Value index = pop();
    const Value target = pop();
    if (!is_list(target)) {
      RUNTIME_ERROR(std::string("cannot assign into a ") + target.type_name());
    }
    if (!index.is_number()) {
      RUNTIME_ERROR(std::string("index must be a number, got ") + index.type_name());
    }
    auto& items = as_list(target)->items;
    const double raw = index.as_number();
    if (raw != std::floor(raw)) RUNTIME_ERROR("index must be a whole number");
    long long i = static_cast<long long>(raw);
    if (i < 0) i += static_cast<long long>(items.size());
    if (i < 0 || i >= static_cast<long long>(items.size())) {
      RUNTIME_ERROR("list index " + std::to_string(static_cast<long long>(raw)) +
                    " out of range for length " + std::to_string(items.size()));
    }
    items[static_cast<std::size_t>(i)] = value;
    push(value);
    VM_NEXT;
  }

  VM_CASE(AddLocalConst) {
    const std::uint8_t slot = READ_BYTE();
    const Value constant = READ_CONSTANT();
    const Value local = frame->slots[slot];
    // The fast path is the whole point of the superinstruction; everything
    // else defers to the shared implementation so behaviour cannot diverge.
    if (local.is_number() && constant.is_number()) {
      push(Value::number(local.as_number() + constant.as_number()));
      VM_NEXT;
    }
    Value result;
    if (!add_values(local, constant, &result)) return Result::RuntimeError;
    push(result);
    VM_NEXT;
  }
  VM_CASE(IncLocal) {
    const std::uint8_t slot = READ_BYTE();
    const Value local = frame->slots[slot];
    if (!local.is_number()) {
      Value result;
      if (!add_values(local, Value::number(1.0), &result)) return Result::RuntimeError;
      frame->slots[slot] = result;
      VM_NEXT;
    }
    frame->slots[slot] = Value::number(local.as_number() + 1.0);
    VM_NEXT;
  }
  VM_CASE(GetLocalGetLocal) {
    const std::uint8_t a = READ_BYTE();
    const std::uint8_t b = READ_BYTE();
    push(frame->slots[a]);
    push(frame->slots[b]);
    VM_NEXT;
  }

#ifdef LUMEN_COMPUTED_GOTO
op_Invalid:
  RUNTIME_ERROR("corrupt bytecode: opcode " +
                std::to_string(static_cast<unsigned>(frame->ip[-1])) +
                " at offset " +
                std::to_string(frame->ip - 1 -
                               frame->closure->function->chunk.code().data()));
#endif

  VM_END

#undef READ_BYTE
#undef READ_U16
#undef READ_CONSTANT
#undef RUNTIME_ERROR
#undef VM_BEGIN
#undef VM_CASE
#undef VM_NEXT
#undef VM_END
#undef VM_TALLY
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

VM::Result VM::run(ObjFunction* script) {
  if (script == nullptr) return Result::CompileError;
  error_.clear();
  reset_stack();

  // The script is wrapped in a closure so the VM has exactly one calling
  // convention. Pushed before the closure is allocated would be wrong; pushed
  // after keeps it rooted while the first frame is set up.
  TempRoot root(gc_, script);
  ObjClosure* closure = gc_.new_closure(script);
  push(Value::object(closure));
  if (!call_closure(closure, 0)) return Result::RuntimeError;
  return execute();
}

}  // namespace lumen
