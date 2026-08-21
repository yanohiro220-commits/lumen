#include "lumen/compiler.hpp"

#include <algorithm>
#include <cassert>

namespace lumen {

void Compiler::error(int line, const std::string& message) {
  errors_.push_back(Diagnostic{line, 0, message});
}

void Compiler::emit(Op op, int line) { chunk().write(op, line); }
void Compiler::emit_byte(std::uint8_t b, int line) { chunk().write_byte(b, line); }
void Compiler::emit_u16(std::uint16_t v, int line) { chunk().write_u16(v, line); }

int Compiler::make_constant(Value v) {
  const int index = chunk().add_constant(v);
  if (index > 0xffff) {
    error(0, "too many constants in one function");
    return 0;
  }
  return index;
}

void Compiler::emit_constant(Value v, int line) {
  emit(Op::Constant, line);
  emit_u16(static_cast<std::uint16_t>(make_constant(v)), line);
}

std::size_t Compiler::emit_jump(Op op, int line) {
  emit(op, line);
  emit_u16(0xffff, line);
  return chunk().size() - 2;
}

void Compiler::patch_jump(std::size_t offset) {
  const std::size_t jump = chunk().size() - offset - 2;
  if (jump > 0xffff) {
    error(0, "jump target is too far away");
    return;
  }
  chunk().patch_u16(offset, static_cast<std::uint16_t>(jump));
}

void Compiler::emit_loop(std::size_t target, int line) {
  emit(Op::Loop, line);
  const std::size_t offset = chunk().size() + 2 - target;
  if (offset > 0xffff) {
    error(line, "loop body is too large");
    return;
  }
  emit_u16(static_cast<std::uint16_t>(offset), line);
}

void Compiler::begin_scope() { ++current_->scope_depth; }

// Emits the pops for every local above `depth` without removing them from the
// compiler's list. Used by `break` and `continue`, which leave a scope by
// jumping over the pops that end_scope would have emitted.
void Compiler::discard_locals_above(int depth, int line) {
  for (auto it = current_->locals.rbegin(); it != current_->locals.rend(); ++it) {
    if (it->depth <= depth) break;
    emit(it->captured ? Op::CloseUpvalue : Op::Pop, line);
  }
}

void Compiler::end_scope(int line) {
  --current_->scope_depth;
  while (!current_->locals.empty() &&
         current_->locals.back().depth > current_->scope_depth) {
    // A captured local cannot simply be popped: a closure still points at its
    // stack slot, so the value has to be moved to the heap first.
    emit(current_->locals.back().captured ? Op::CloseUpvalue : Op::Pop, line);
    current_->locals.pop_back();
  }
}

void Compiler::declare_local(const std::string& name, int line) {
  if (current_->scope_depth == 0) return;  // globals are not tracked here
  for (auto it = current_->locals.rbegin(); it != current_->locals.rend(); ++it) {
    if (it->depth != -1 && it->depth < current_->scope_depth) break;
    if (it->name == name) {
      error(line, "'" + name + "' is already declared in this scope");
      return;
    }
  }
  if (current_->locals.size() >= 256) {
    error(line, "too many local variables in one function");
    return;
  }
  current_->locals.push_back(Local{name, -1, false});
}

void Compiler::mark_initialized() {
  if (current_->scope_depth == 0 || current_->locals.empty()) return;
  current_->locals.back().depth = current_->scope_depth;
}

int Compiler::resolve_local(FunctionState* state, const std::string& name) {
  for (int i = static_cast<int>(state->locals.size()) - 1; i >= 0; --i) {
    if (state->locals[static_cast<std::size_t>(i)].name == name) {
      if (state->locals[static_cast<std::size_t>(i)].depth == -1) {
        // `let x = x;` where the inner x is this same declaration. Reading it
        // would observe an uninitialized slot.
        return -2;
      }
      return i;
    }
  }
  return -1;
}

int Compiler::add_upvalue(FunctionState* state, std::uint8_t index, bool is_local) {
  for (std::size_t i = 0; i < state->upvalues.size(); ++i) {
    if (state->upvalues[i].index == index && state->upvalues[i].is_local == is_local) {
      return static_cast<int>(i);
    }
  }
  if (state->upvalues.size() >= 256) {
    error(0, "too many closure variables in one function");
    return 0;
  }
  state->upvalues.push_back(Upvalue{index, is_local});
  state->function->upvalue_count = static_cast<int>(state->upvalues.size());
  return static_cast<int>(state->upvalues.size() - 1);
}

// Walks outward through enclosing functions, adding an upvalue at every level.
//
// The recursion is what makes transitive capture work: a variable used three
// functions deep is captured as a local by the function that owns it, then
// threaded through each intermediate closure as an upvalue-of-an-upvalue. A
// flat implementation that only looks one level out silently fails on exactly
// that case.
int Compiler::resolve_upvalue(FunctionState* state, const std::string& name) {
  if (state->enclosing == nullptr) return -1;

  const int local = resolve_local(state->enclosing, name);
  if (local >= 0) {
    state->enclosing->locals[static_cast<std::size_t>(local)].captured = true;
    return add_upvalue(state, static_cast<std::uint8_t>(local), true);
  }
  const int upvalue = resolve_upvalue(state->enclosing, name);
  if (upvalue >= 0) {
    return add_upvalue(state, static_cast<std::uint8_t>(upvalue), false);
  }
  return -1;
}

void Compiler::named_variable(const std::string& name, int line, Expr* value) {
  Op get_op = Op::GetGlobal;
  Op set_op = Op::SetGlobal;
  int arg = resolve_local(current_, name);
  bool wide = false;

  if (arg == -2) {
    error(line, "cannot read '" + name + "' in its own initializer");
    arg = 0;
  }
  if (arg >= 0) {
    get_op = Op::GetLocal;
    set_op = Op::SetLocal;
  } else {
    arg = resolve_upvalue(current_, name);
    if (arg >= 0) {
      get_op = Op::GetUpvalue;
      set_op = Op::SetUpvalue;
    } else {
      arg = make_constant(Value::object(gc_.intern(name)));
      wide = true;
    }
  }

  if (value != nullptr) {
    expression(value);
    emit(set_op, line);
  } else {
    emit(get_op, line);
  }
  if (wide) {
    emit_u16(static_cast<std::uint16_t>(arg), line);
  } else {
    emit_byte(static_cast<std::uint8_t>(arg), line);
  }
}

void Compiler::expression(Expr* expr) {
  const int line = expr->line;
  switch (expr->kind) {
    case Expr::Kind::Literal: {
      const Literal& lit = static_cast<LiteralExpr*>(expr)->value;
      switch (lit.type) {
        case Literal::Type::Nil: emit(Op::Nil, line); break;
        case Literal::Type::Bool:
          emit(lit.boolean ? Op::True : Op::False, line);
          break;
        case Literal::Type::Number:
          emit_constant(Value::number(lit.number), line);
          break;
        case Literal::Type::String:
          emit_constant(Value::object(gc_.intern(lit.string)), line);
          break;
      }
      return;
    }
    case Expr::Kind::Variable:
      named_variable(static_cast<VariableExpr*>(expr)->name, line, nullptr);
      return;
    case Expr::Kind::Assign: {
      auto* e = static_cast<AssignExpr*>(expr);
      named_variable(e->name, line, e->value.get());
      return;
    }
    case Expr::Kind::Unary: {
      auto* e = static_cast<UnaryExpr*>(expr);
      expression(e->operand.get());
      emit(e->op == TokenType::Minus ? Op::Negate : Op::Not, line);
      return;
    }
    case Expr::Kind::Binary: {
      auto* e = static_cast<BinaryExpr*>(expr);
      expression(e->left.get());
      expression(e->right.get());
      switch (e->op) {
        case TokenType::Plus: emit(Op::Add, line); break;
        case TokenType::Minus: emit(Op::Subtract, line); break;
        case TokenType::Star: emit(Op::Multiply, line); break;
        case TokenType::Slash: emit(Op::Divide, line); break;
        case TokenType::Percent: emit(Op::Modulo, line); break;
        case TokenType::EqualEqual: emit(Op::Equal, line); break;
        case TokenType::BangEqual: emit(Op::NotEqual, line); break;
        case TokenType::Less: emit(Op::Less, line); break;
        case TokenType::LessEqual: emit(Op::LessEqual, line); break;
        case TokenType::Greater: emit(Op::Greater, line); break;
        case TokenType::GreaterEqual: emit(Op::GreaterEqual, line); break;
        default: error(line, "unsupported binary operator"); break;
      }
      return;
    }
    case Expr::Kind::Logical: {
      auto* e = static_cast<LogicalExpr*>(expr);
      expression(e->left.get());
      // The condition is peeked, not popped: when the jump is taken the left
      // operand *is* the result of the whole expression.
      const std::size_t jump =
          emit_jump(e->op == TokenType::And ? Op::JumpIfFalse : Op::JumpIfTrue, line);
      emit(Op::Pop, line);
      expression(e->right.get());
      patch_jump(jump);
      return;
    }
    case Expr::Kind::Call: {
      auto* e = static_cast<CallExpr*>(expr);
      expression(e->callee.get());
      for (const auto& arg : e->args) expression(arg.get());
      emit(Op::Call, line);
      emit_byte(static_cast<std::uint8_t>(e->args.size()), line);
      return;
    }
    case Expr::Kind::ListLiteral: {
      auto* e = static_cast<ListExpr*>(expr);
      for (const auto& item : e->items) expression(item.get());
      emit(Op::BuildList, line);
      emit_u16(static_cast<std::uint16_t>(e->items.size()), line);
      return;
    }
    case Expr::Kind::Index: {
      auto* e = static_cast<IndexExpr*>(expr);
      expression(e->target.get());
      expression(e->index.get());
      emit(Op::IndexGet, line);
      return;
    }
    case Expr::Kind::IndexAssign: {
      auto* e = static_cast<IndexAssignExpr*>(expr);
      expression(e->target.get());
      expression(e->index.get());
      expression(e->value.get());
      emit(Op::IndexSet, line);
      return;
    }
    case Expr::Kind::Lambda: {
      auto* e = static_cast<LambdaExpr*>(expr);
      function(*e->body, line);
      return;
    }
  }
}

void Compiler::block(const std::vector<StmtPtr>& statements, int line) {
  begin_scope();
  for (const auto& s : statements) statement(s.get());
  end_scope(line);
}

void Compiler::function(const FunctionBody& body, int line) {
  FunctionState state;
  state.enclosing = current_;
  state.function = gc_.new_function();

  // Rooted immediately, before anything else allocates.
  //
  // The function is reachable only from this C++ local until it lands in the
  // enclosing chunk's constant pool. Interning the name one line earlier - the
  // obvious order - allocates, which can trigger a collection that frees the
  // function that is about to be written to. Under a normal heap that window
  // is almost never hit; under GC stress it is hit every time.
  TempRoot root(gc_, state.function);

  state.function->arity = static_cast<int>(body.params.size());
  state.function->name = gc_.intern(body.name);

  // Slot zero holds the closure itself, which is how `Call` finds the frame
  // base. Giving it an unusable name keeps user code from resolving to it.
  state.locals.push_back(Local{"", 0, false});

  current_ = &state;
  begin_scope();
  for (const auto& param : body.params) {
    declare_local(param, line);
    mark_initialized();
  }
  for (const auto& s : body.body) statement(s.get());

  // Implicit `return nil` for a function that falls off the end.
  emit(Op::Nil, line);
  emit(Op::Return, line);

  if (options_.peephole) peephole(state.function->chunk);
  ++stats_.functions_compiled;

  ObjFunction* compiled = state.function;
  current_ = state.enclosing;

  emit(Op::Closure, line);
  emit_u16(static_cast<std::uint16_t>(make_constant(Value::object(compiled))), line);
  for (const auto& upvalue : state.upvalues) {
    emit_byte(upvalue.is_local ? 1 : 0, line);
    emit_byte(upvalue.index, line);
  }
}

void Compiler::statement(Stmt* stmt) {
  if (stmt == nullptr) return;
  const int line = stmt->line;

  switch (stmt->kind) {
    case Stmt::Kind::Expression: {
      expression(static_cast<ExpressionStmt*>(stmt)->expr.get());
      emit(Op::Pop, line);
      return;
    }
    case Stmt::Kind::Print: {
      expression(static_cast<PrintStmt*>(stmt)->expr.get());
      emit(Op::Print, line);
      return;
    }
    case Stmt::Kind::Let: {
      auto* s = static_cast<LetStmt*>(stmt);
      declare_local(s->name, line);
      if (s->initializer) {
        expression(s->initializer.get());
      } else {
        emit(Op::Nil, line);
      }
      if (current_->scope_depth > 0) {
        mark_initialized();
        // A local needs no store: it already sits in its own stack slot.
        return;
      }
      emit(Op::DefineGlobal, line);
      emit_u16(static_cast<std::uint16_t>(
                   make_constant(Value::object(gc_.intern(s->name)))),
               line);
      return;
    }
    case Stmt::Kind::Block:
      block(static_cast<BlockStmt*>(stmt)->statements, line);
      return;
    case Stmt::Kind::If: {
      auto* s = static_cast<IfStmt*>(stmt);
      expression(s->condition.get());
      const std::size_t else_jump = emit_jump(Op::PopJumpIfFalse, line);
      statement(s->then_branch.get());
      if (s->else_branch) {
        const std::size_t end_jump = emit_jump(Op::Jump, line);
        patch_jump(else_jump);
        statement(s->else_branch.get());
        patch_jump(end_jump);
      } else {
        patch_jump(else_jump);
      }
      return;
    }
    case Stmt::Kind::While: {
      auto* s = static_cast<WhileStmt*>(stmt);
      const std::size_t loop_start = chunk().size();

      LoopContext loop;
      loop.continue_target = loop_start;
      loop.scope_depth = current_->scope_depth;
      current_->loops.push_back(loop);

      expression(s->condition.get());
      const std::size_t exit_jump = emit_jump(Op::PopJumpIfFalse, line);
      statement(s->body.get());
      emit_loop(loop_start, line);
      patch_jump(exit_jump);

      for (std::size_t jump : current_->loops.back().break_jumps) patch_jump(jump);
      current_->loops.pop_back();
      return;
    }
    case Stmt::Kind::For: {
      auto* s = static_cast<ForStmt*>(stmt);
      begin_scope();
      if (s->initializer) statement(s->initializer.get());

      const std::size_t loop_start = chunk().size();
      std::size_t exit_jump = SIZE_MAX;
      if (s->condition) {
        expression(s->condition.get());
        exit_jump = emit_jump(Op::PopJumpIfFalse, line);
      }

      LoopContext loop;
      loop.scope_depth = current_->scope_depth;
      // `continue` must reach the increment, not the condition, so its target
      // is not known until the increment has been emitted. Jumps are collected
      // and patched afterwards.
      loop.continue_is_forward = true;
      current_->loops.push_back(loop);

      statement(s->body.get());

      // `continue` lands here, at the increment, not at the condition.
      for (std::size_t jump : current_->loops.back().continue_jumps) {
        patch_jump(jump);
      }

      if (s->increment) {
        expression(s->increment.get());
        emit(Op::Pop, line);
      }
      emit_loop(loop_start, line);
      if (exit_jump != SIZE_MAX) patch_jump(exit_jump);

      for (std::size_t jump : current_->loops.back().break_jumps) patch_jump(jump);
      current_->loops.pop_back();
      end_scope(line);
      return;
    }
    case Stmt::Kind::Return: {
      auto* s = static_cast<ReturnStmt*>(stmt);
      if (current_->enclosing == nullptr) {
        error(line, "cannot return from top-level code");
        return;
      }
      if (s->value) {
        expression(s->value.get());
      } else {
        emit(Op::Nil, line);
      }
      emit(Op::Return, line);
      return;
    }
    case Stmt::Kind::Function: {
      auto* s = static_cast<FunctionStmt*>(stmt);
      declare_local(s->body->name, line);
      // Marked initialized before the body is compiled, so a function can
      // refer to itself and recurse.
      mark_initialized();
      function(*s->body, line);
      if (current_->scope_depth == 0) {
        emit(Op::DefineGlobal, line);
        emit_u16(static_cast<std::uint16_t>(
                     make_constant(Value::object(gc_.intern(s->body->name)))),
                 line);
      }
      return;
    }
    case Stmt::Kind::Break: {
      if (current_->loops.empty()) {
        error(line, "'break' outside a loop");
        return;
      }
      LoopContext& loop = current_->loops.back();
      // Locals declared inside the loop have to be discarded before jumping
      // out. Jumping over the scope's own pops would leave them on the stack,
      // and a `break` inside a loop that runs many times would grow the stack
      // without bound. They stay in the compiler's local list, because the
      // (unreachable) code after the break is still compiled in their scope.
      discard_locals_above(loop.scope_depth, line);
      loop.break_jumps.push_back(emit_jump(Op::Jump, line));
      return;
    }
    case Stmt::Kind::Continue: {
      if (current_->loops.empty()) {
        error(line, "'continue' outside a loop");
        return;
      }
      LoopContext& loop = current_->loops.back();
      discard_locals_above(loop.scope_depth, line);
      if (loop.continue_is_forward) {
        loop.continue_jumps.push_back(emit_jump(Op::Jump, line));
      } else {
        emit_loop(loop.continue_target, line);
      }
      return;
    }
  }
}


namespace {

// Byte length of the instruction at `offset`, including operands.
//
// Closure is the only variable-length instruction: it carries two bytes per
// upvalue after its operand, and getting this wrong desynchronises the whole
// decode, so it is computed from the function object in the constant pool
// rather than guessed.
std::size_t instruction_length(const Chunk& c, std::size_t offset) {
  const Op op = static_cast<Op>(c.at(offset));
  switch (op) {
    case Op::Constant:
    case Op::GetGlobal:
    case Op::DefineGlobal:
    case Op::SetGlobal:
    case Op::Jump:
    case Op::JumpIfFalse:
    case Op::JumpIfTrue:
    case Op::PopJumpIfFalse:
    case Op::Loop:
    case Op::BuildList:
    case Op::GetLocalGetLocal:
      return 3;
    case Op::GetLocal:
    case Op::SetLocal:
    case Op::GetUpvalue:
    case Op::SetUpvalue:
    case Op::Call:
    case Op::IncLocal:
      return 2;
    case Op::AddLocalConst:
      return 4;
    case Op::Closure: {
      const std::uint16_t idx = c.u16_at(offset + 1);
      int upvalues = 0;
      if (idx < c.constants().size() && c.constants()[idx].is_object()) {
        const Obj* o = c.constants()[idx].as_object();
        if (o->type == ObjType::Function) {
          upvalues = static_cast<const ObjFunction*>(o)->upvalue_count;
        }
      }
      return 3 + static_cast<std::size_t>(upvalues) * 2;
    }
    default:
      return 1;
  }
}

bool is_jump(Op op) {
  return op == Op::Jump || op == Op::JumpIfFalse || op == Op::JumpIfTrue ||
         op == Op::PopJumpIfFalse || op == Op::Loop;
}

}  // namespace

// Peephole pass over the finished bytecode.
//
// Three things make this safe, and skipping any of them produces a program that
// runs and is subtly wrong rather than one that crashes:
//
//  1. No pattern may start in the middle of another instruction, so the chunk
//     is decoded instruction by instruction rather than scanned for byte
//     sequences.
//  2. No instruction that is the target of a jump may be folded *into* a
//     superinstruction. Folding it would make the jump land inside an operand.
//  3. Every jump has to be re-encoded, because the rewrite moves everything
//     after it. The old target offset is remembered and mapped through to the
//     new layout after the rewrite, rather than being adjusted in place.
void Compiler::peephole(Chunk& c) {
  if (c.size() == 0) return;

  const std::vector<std::uint8_t> old_code = c.code();
  const std::vector<int> old_lines = c.expand_lines();

  // Pass 1: instruction boundaries and jump targets.
  std::vector<std::size_t> starts;
  std::vector<bool> is_target(old_code.size() + 1, false);
  for (std::size_t i = 0; i < old_code.size();) {
    starts.push_back(i);
    const Op op = static_cast<Op>(old_code[i]);
    const std::size_t len = instruction_length(c, i);
    if (is_jump(op)) {
      const std::size_t operand = static_cast<std::size_t>(
          (old_code[i + 1] << 8) | old_code[i + 2]);
      const std::size_t target =
          op == Op::Loop ? (i + 3) - operand : (i + 3) + operand;
      if (target <= old_code.size()) is_target[target] = true;
    }
    i += len;
  }

  // Pass 2: rewrite.
  std::vector<std::uint8_t> out;
  std::vector<int> out_lines;
  std::vector<std::size_t> old_to_new(old_code.size() + 1, 0);
  struct PendingJump {
    std::size_t operand_offset;  // in the new code
    std::size_t old_target;
    bool backward;
  };
  std::vector<PendingJump> jumps;
  out.reserve(old_code.size());

  auto emit_raw = [&](std::uint8_t byte, int line) {
    out.push_back(byte);
    out_lines.push_back(line);
  };

  auto instruction_at = [&](std::size_t idx) -> Op {
    return static_cast<Op>(old_code[idx]);
  };

  for (std::size_t i = 0; i < old_code.size();) {
    old_to_new[i] = out.size();
    const Op op = instruction_at(i);
    const std::size_t len = instruction_length(c, i);
    const int line = old_lines[i];

    // i += 1 case: GET_LOCAL a, CONSTANT 1, ADD, SET_LOCAL a, POP  ->  INC_LOCAL a
    if (op == Op::GetLocal) {
      const std::size_t i2 = i + 2;
      if (i2 + 3 <= old_code.size() && !is_target[i2] &&
          instruction_at(i2) == Op::Constant) {
        const std::uint16_t k = static_cast<std::uint16_t>(
            (old_code[i2 + 1] << 8) | old_code[i2 + 2]);
        const std::size_t i3 = i2 + 3;
        const bool const_is_one = k < c.constants().size() &&
                                  c.constants()[k].is_number() &&
                                  c.constants()[k].as_number() == 1.0;

        if (i3 < old_code.size() && !is_target[i3] &&
            instruction_at(i3) == Op::Add) {
          const std::size_t i4 = i3 + 1;
          if (const_is_one && i4 + 2 < old_code.size() && !is_target[i4] &&
              instruction_at(i4) == Op::SetLocal &&
              old_code[i4 + 1] == old_code[i + 1] && !is_target[i4 + 2] &&
              instruction_at(i4 + 2) == Op::Pop) {
            emit_raw(static_cast<std::uint8_t>(Op::IncLocal), line);
            emit_raw(old_code[i + 1], line);
            ++stats_.peephole_rewrites;
            i = i4 + 3;
            continue;
          }
          emit_raw(static_cast<std::uint8_t>(Op::AddLocalConst), line);
          emit_raw(old_code[i + 1], line);
          emit_raw(old_code[i2 + 1], line);
          emit_raw(old_code[i2 + 2], line);
          ++stats_.peephole_rewrites;
          i = i3 + 1;
          continue;
        }
      }
      if (i2 + 2 <= old_code.size() && !is_target[i2] &&
          instruction_at(i2) == Op::GetLocal) {
        emit_raw(static_cast<std::uint8_t>(Op::GetLocalGetLocal), line);
        emit_raw(old_code[i + 1], line);
        emit_raw(old_code[i2 + 1], line);
        ++stats_.peephole_rewrites;
        i = i2 + 2;
        continue;
      }
    }

    if (is_jump(op)) {
      const std::size_t operand = static_cast<std::size_t>(
          (old_code[i + 1] << 8) | old_code[i + 2]);
      const bool backward = op == Op::Loop;
      const std::size_t target = backward ? (i + 3) - operand : (i + 3) + operand;
      emit_raw(old_code[i], line);
      jumps.push_back(PendingJump{out.size(), target, backward});
      emit_raw(0, line);
      emit_raw(0, line);
      i += len;
      continue;
    }

    for (std::size_t k = 0; k < len; ++k) emit_raw(old_code[i + k], old_lines[i + k]);
    i += len;
  }
  old_to_new[old_code.size()] = out.size();

  // Pass 3: re-encode jumps against the new layout.
  for (const PendingJump& jump : jumps) {
    const std::size_t new_target = old_to_new[jump.old_target];
    const std::size_t after = jump.operand_offset + 2;
    const std::size_t delta = jump.backward ? after - new_target : new_target - after;
    out[jump.operand_offset] = static_cast<std::uint8_t>(delta >> 8);
    out[jump.operand_offset + 1] = static_cast<std::uint8_t>(delta & 0xff);
  }

  c.code() = std::move(out);
  c.set_lines(std::move(out_lines));
}

ObjFunction* Compiler::compile(Program& program) {
  FunctionState state;
  state.function = gc_.new_function();
  TempRoot root(gc_, state.function);
  state.function->name = nullptr;  // the script itself
  state.locals.push_back(Local{"", 0, false});

  current_ = &state;
  for (const auto& s : program.statements) statement(s.get());
  emit(Op::Nil, 0);
  emit(Op::Return, 0);

  if (options_.peephole) peephole(state.function->chunk);
  ++stats_.functions_compiled;
  current_ = nullptr;

  return had_error() ? nullptr : state.function;
}

}  // namespace lumen
