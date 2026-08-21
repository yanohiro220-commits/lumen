#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lumen/ast.hpp"
#include "lumen/gc.hpp"
#include "lumen/object.hpp"
#include "lumen/parser.hpp"

namespace lumen {

struct CompileOptions {
  // The peephole pass rewrites the finished bytecode. Turning it off is what
  // lets the test suite run every program twice and require identical output,
  // which is the only cheap way to be confident a superinstruction is a faithful
  // substitution for the sequence it replaces.
  bool peephole = true;
};

// Walks the AST and emits bytecode, resolving names as it goes.
//
// Name resolution happens here rather than in a separate pass because scoping
// is lexical and the walk is already in source order: a local is in scope
// exactly when it is on the compiler's local stack. Splitting it out would mean
// maintaining the same scope stack twice.
class Compiler {
 public:
  explicit Compiler(GC& gc, CompileOptions options = {})
      : gc_(gc), options_(options) {}

  // Returns the top-level function, or nullptr on error. The result is a
  // function rather than a bare chunk so the VM has exactly one calling
  // convention, including for the script itself.
  ObjFunction* compile(Program& program);

  const std::vector<Diagnostic>& errors() const { return errors_; }
  bool had_error() const { return !errors_.empty(); }

  struct Stats {
    std::uint32_t peephole_rewrites = 0;
    std::uint32_t functions_compiled = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Local {
    std::string name;
    int depth = -1;   // -1 while the initializer is still being compiled
    bool captured = false;
  };

  struct Upvalue {
    std::uint8_t index = 0;
    bool is_local = false;
  };

  struct LoopContext {
    std::size_t continue_target = 0;
    std::vector<std::size_t> break_jumps;
    std::vector<std::size_t> continue_jumps;
    int scope_depth = 0;
    bool continue_is_forward = false;
  };

  struct FunctionState {
    ObjFunction* function = nullptr;
    std::vector<Local> locals;
    std::vector<Upvalue> upvalues;
    int scope_depth = 0;
    std::vector<LoopContext> loops;
    FunctionState* enclosing = nullptr;
  };

  Chunk& chunk() { return current_->function->chunk; }

  void emit(Op op, int line);
  void emit_byte(std::uint8_t b, int line);
  void emit_u16(std::uint16_t v, int line);
  void emit_constant(Value v, int line);
  std::size_t emit_jump(Op op, int line);
  void patch_jump(std::size_t offset);
  void emit_loop(std::size_t target, int line);
  int make_constant(Value v);

  void begin_scope();
  void end_scope(int line);
  void discard_locals_above(int depth, int line);
  void declare_local(const std::string& name, int line);
  void mark_initialized();
  int resolve_local(FunctionState* state, const std::string& name);
  int resolve_upvalue(FunctionState* state, const std::string& name);
  int add_upvalue(FunctionState* state, std::uint8_t index, bool is_local);

  void statement(Stmt* stmt);
  void expression(Expr* expr);
  void block(const std::vector<StmtPtr>& statements, int line);
  void function(const FunctionBody& body, int line);
  void named_variable(const std::string& name, int line, Expr* value);

  void error(int line, const std::string& message);

  // Rewrites the finished chunk in place.
  void peephole(Chunk& c);

  GC& gc_;
  CompileOptions options_;
  FunctionState* current_ = nullptr;
  std::vector<Diagnostic> errors_;
  Stats stats_;
};

}  // namespace lumen
