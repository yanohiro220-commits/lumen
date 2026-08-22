#include "lumen/optimizer.hpp"

#include <cmath>

namespace lumen {

const Literal* Optimizer::literal_of(const Expr* e) {
  if (!e || e->kind != Expr::Kind::Literal) return nullptr;
  return &static_cast<const LiteralExpr*>(e)->value;
}

// Whether evaluating the expression can be skipped without losing an effect.
//
// This gates every rewrite that deletes a subtree. A call might print or
// assign, so it is never pure; an assignment obviously is not. Being
// conservative here costs a missed optimisation, while being wrong deletes the
// user's side effects.
bool Optimizer::is_pure(const Expr* e) {
  if (!e) return true;
  switch (e->kind) {
    case Expr::Kind::Literal:
    case Expr::Kind::Variable:
    case Expr::Kind::Lambda:
      return true;
    case Expr::Kind::Unary:
      return is_pure(static_cast<const UnaryExpr*>(e)->operand.get());
    case Expr::Kind::Binary: {
      const auto* b = static_cast<const BinaryExpr*>(e);
      return is_pure(b->left.get()) && is_pure(b->right.get());
    }
    case Expr::Kind::Logical: {
      const auto* l = static_cast<const LogicalExpr*>(e);
      return is_pure(l->left.get()) && is_pure(l->right.get());
    }
    case Expr::Kind::ListLiteral: {
      const auto* l = static_cast<const ListExpr*>(e);
      for (const auto& item : l->items) {
        if (!is_pure(item.get())) return false;
      }
      return true;
    }
    case Expr::Kind::Index:
      // An index can fault - out of range is a run-time error - and a fault is
      // observable, so an index expression is never safe to delete.
      return false;
    default:
      return false;
  }
}

void Optimizer::run(Program& program) { optimize_statements(program.statements); }

void Optimizer::optimize_statements(std::vector<StmtPtr>& statements) {
  std::vector<StmtPtr> out;
  out.reserve(statements.size());

  bool unreachable = false;
  std::uint32_t dropped = 0;

  for (auto& stmt : statements) {
    if (unreachable) {
      // Everything after an unconditional jump out of the block is dead.
      ++dropped;
      continue;
    }
    StmtPtr optimized = optimize_stmt(std::move(stmt));
    if (!optimized) continue;

    const bool terminates = optimized->kind == Stmt::Kind::Return ||
                            optimized->kind == Stmt::Kind::Break ||
                            optimized->kind == Stmt::Kind::Continue;
    out.push_back(std::move(optimized));
    if (terminates) unreachable = true;
  }

  stats_.statements_removed += dropped;
  statements = std::move(out);
}

StmtPtr Optimizer::optimize_stmt(StmtPtr stmt) {
  if (!stmt) return nullptr;

  switch (stmt->kind) {
    case Stmt::Kind::Expression: {
      auto* s = static_cast<ExpressionStmt*>(stmt.get());
      s->expr = optimize_expr(std::move(s->expr));
      // A pure expression evaluated for its value and then discarded does
      // nothing at all.
      if (is_pure(s->expr.get()) && s->expr->kind == Expr::Kind::Literal) {
        ++stats_.statements_removed;
        return nullptr;
      }
      return stmt;
    }
    case Stmt::Kind::Print: {
      auto* s = static_cast<PrintStmt*>(stmt.get());
      s->expr = optimize_expr(std::move(s->expr));
      return stmt;
    }
    case Stmt::Kind::Let: {
      auto* s = static_cast<LetStmt*>(stmt.get());
      if (s->initializer) s->initializer = optimize_expr(std::move(s->initializer));
      return stmt;
    }
    case Stmt::Kind::Block: {
      auto* s = static_cast<BlockStmt*>(stmt.get());
      optimize_statements(s->statements);
      return stmt;
    }
    case Stmt::Kind::If: {
      auto* s = static_cast<IfStmt*>(stmt.get());
      s->condition = optimize_expr(std::move(s->condition));
      s->then_branch = optimize_stmt(std::move(s->then_branch));
      s->else_branch = optimize_stmt(std::move(s->else_branch));

      if (const Literal* lit = literal_of(s->condition.get())) {
        ++stats_.branches_eliminated;
        StmtPtr taken = lit->truthy() ? std::move(s->then_branch)
                                      : std::move(s->else_branch);
        if (!taken) return nullptr;
        return taken;
      }
      return stmt;
    }
    case Stmt::Kind::While: {
      auto* s = static_cast<WhileStmt*>(stmt.get());
      s->condition = optimize_expr(std::move(s->condition));
      s->body = optimize_stmt(std::move(s->body));
      if (const Literal* lit = literal_of(s->condition.get())) {
        if (!lit->truthy()) {
          // `while (false)` never runs. The reverse - `while (true)` - is left
          // alone; it is an intentional infinite loop, and the code generator
          // already drops the condition test for it.
          ++stats_.branches_eliminated;
          return nullptr;
        }
      }
      return stmt;
    }
    case Stmt::Kind::For: {
      auto* s = static_cast<ForStmt*>(stmt.get());
      s->initializer = optimize_stmt(std::move(s->initializer));
      if (s->condition) s->condition = optimize_expr(std::move(s->condition));
      if (s->increment) s->increment = optimize_expr(std::move(s->increment));
      s->body = optimize_stmt(std::move(s->body));
      return stmt;
    }
    case Stmt::Kind::Return: {
      auto* s = static_cast<ReturnStmt*>(stmt.get());
      if (s->value) s->value = optimize_expr(std::move(s->value));
      return stmt;
    }
    case Stmt::Kind::Function: {
      auto* s = static_cast<FunctionStmt*>(stmt.get());
      optimize_statements(s->body->body);
      return stmt;
    }
    default:
      return stmt;
  }
}

ExprPtr Optimizer::optimize_expr(ExprPtr expr) {
  if (!expr) return nullptr;

  switch (expr->kind) {
    case Expr::Kind::Unary: {
      auto* e = static_cast<UnaryExpr*>(expr.get());
      e->operand = optimize_expr(std::move(e->operand));
      return fold_unary(std::move(expr));
    }
    case Expr::Kind::Binary: {
      auto* e = static_cast<BinaryExpr*>(expr.get());
      e->left = optimize_expr(std::move(e->left));
      e->right = optimize_expr(std::move(e->right));
      ExprPtr folded = fold_binary(std::move(expr));
      if (folded->kind != Expr::Kind::Binary) return folded;
      return simplify_algebraic(std::move(folded));
    }
    case Expr::Kind::Logical: {
      auto* e = static_cast<LogicalExpr*>(expr.get());
      e->left = optimize_expr(std::move(e->left));
      e->right = optimize_expr(std::move(e->right));
      return fold_logical(std::move(expr));
    }
    case Expr::Kind::Assign: {
      auto* e = static_cast<AssignExpr*>(expr.get());
      e->value = optimize_expr(std::move(e->value));
      return expr;
    }
    case Expr::Kind::Call: {
      auto* e = static_cast<CallExpr*>(expr.get());
      e->callee = optimize_expr(std::move(e->callee));
      for (auto& arg : e->args) arg = optimize_expr(std::move(arg));
      return expr;
    }
    case Expr::Kind::ListLiteral: {
      auto* e = static_cast<ListExpr*>(expr.get());
      for (auto& item : e->items) item = optimize_expr(std::move(item));
      return expr;
    }
    case Expr::Kind::Index: {
      auto* e = static_cast<IndexExpr*>(expr.get());
      e->target = optimize_expr(std::move(e->target));
      e->index = optimize_expr(std::move(e->index));
      return expr;
    }
    case Expr::Kind::IndexAssign: {
      auto* e = static_cast<IndexAssignExpr*>(expr.get());
      e->target = optimize_expr(std::move(e->target));
      e->index = optimize_expr(std::move(e->index));
      e->value = optimize_expr(std::move(e->value));
      return expr;
    }
    case Expr::Kind::Lambda: {
      auto* e = static_cast<LambdaExpr*>(expr.get());
      optimize_statements(e->body->body);
      return expr;
    }
    default:
      return expr;
  }
}

ExprPtr Optimizer::fold_unary(ExprPtr owner) {
  auto* node = static_cast<UnaryExpr*>(owner.get());
  const Literal* operand = literal_of(node->operand.get());
  if (!operand) return owner;

  if (node->op == TokenType::Bang) {
    ++stats_.constants_folded;
    return std::make_unique<LiteralExpr>(Literal::of(!operand->truthy()), node->line);
  }
  if (node->op == TokenType::Minus && operand->type == Literal::Type::Number) {
    ++stats_.constants_folded;
    return std::make_unique<LiteralExpr>(Literal::of(-operand->number), node->line);
  }
  return owner;
}

ExprPtr Optimizer::fold_binary(ExprPtr owner) {
  auto* node = static_cast<BinaryExpr*>(owner.get());
  const Literal* l = literal_of(node->left.get());
  const Literal* r = literal_of(node->right.get());
  if (!l || !r) return owner;

  const int line = node->line;
  auto fold = [&](Literal result) -> ExprPtr {
    ++stats_.constants_folded;
    return std::make_unique<LiteralExpr>(std::move(result), line);
  };

  // String concatenation is the one non-numeric fold worth doing: building a
  // message out of literal pieces is common, and it costs nothing at run time
  // once folded.
  if (node->op == TokenType::Plus && l->type == Literal::Type::String &&
      r->type == Literal::Type::String) {
    return fold(Literal::of(l->string + r->string));
  }

  if (node->op == TokenType::EqualEqual || node->op == TokenType::BangEqual) {
    if (l->type != r->type) {
      return fold(Literal::of(node->op == TokenType::BangEqual));
    }
    bool equal = false;
    switch (l->type) {
      case Literal::Type::Nil: equal = true; break;
      case Literal::Type::Bool: equal = l->boolean == r->boolean; break;
      case Literal::Type::Number: equal = l->number == r->number; break;
      case Literal::Type::String: equal = l->string == r->string; break;
    }
    return fold(Literal::of(node->op == TokenType::EqualEqual ? equal : !equal));
  }

  if (l->type != Literal::Type::Number || r->type != Literal::Type::Number) {
    return owner;
  }
  const double a = l->number;
  const double b = r->number;

  switch (node->op) {
    case TokenType::Plus: return fold(Literal::of(a + b));
    case TokenType::Minus: return fold(Literal::of(a - b));
    case TokenType::Star: return fold(Literal::of(a * b));
    case TokenType::Slash:
      // Division and modulo by zero are left for the VM. They are defined -
      // IEEE gives inf and NaN - but folding them here would bake the result
      // into a constant and lose the run-time error the VM raises, so the two
      // paths would disagree.
      if (b == 0.0) return owner;
      return fold(Literal::of(a / b));
    case TokenType::Percent:
      if (b == 0.0) return owner;
      return fold(Literal::of(std::fmod(a, b)));
    case TokenType::Less: return fold(Literal::of(a < b));
    case TokenType::LessEqual: return fold(Literal::of(a <= b));
    case TokenType::Greater: return fold(Literal::of(a > b));
    case TokenType::GreaterEqual: return fold(Literal::of(a >= b));
    default: return owner;
  }
}

ExprPtr Optimizer::simplify_algebraic(ExprPtr owner) {
  auto* node = static_cast<BinaryExpr*>(owner.get());
  const Literal* l = literal_of(node->left.get());
  const Literal* r = literal_of(node->right.get());

  auto is_number = [](const Literal* lit, double v) {
    return lit && lit->type == Literal::Type::Number && lit->number == v;
  };

  // x + 0, 0 + x, x - 0, x * 1, 1 * x, x / 1 all reduce to x.
  //
  // Notably absent: x * 0 -> 0. That identity does not hold in IEEE
  // arithmetic - NaN * 0 is NaN and inf * 0 is NaN - and here it would also
  // skip the type error the VM raises when x is a string.
  switch (node->op) {
    case TokenType::Plus:
      if (is_number(r, 0.0) && is_pure(node->left.get())) {
        ++stats_.algebraic_simplifications;
        return std::move(node->left);
      }
      if (is_number(l, 0.0) && is_pure(node->right.get())) {
        ++stats_.algebraic_simplifications;
        return std::move(node->right);
      }
      return owner;
    case TokenType::Minus:
      if (is_number(r, 0.0) && is_pure(node->left.get())) {
        ++stats_.algebraic_simplifications;
        return std::move(node->left);
      }
      return owner;
    case TokenType::Star:
      if (is_number(r, 1.0) && is_pure(node->left.get())) {
        ++stats_.algebraic_simplifications;
        return std::move(node->left);
      }
      if (is_number(l, 1.0) && is_pure(node->right.get())) {
        ++stats_.algebraic_simplifications;
        return std::move(node->right);
      }
      return owner;
    case TokenType::Slash:
      if (is_number(r, 1.0) && is_pure(node->left.get())) {
        ++stats_.algebraic_simplifications;
        return std::move(node->left);
      }
      return owner;
    default:
      return owner;
  }
}

ExprPtr Optimizer::fold_logical(ExprPtr owner) {
  auto* node = static_cast<LogicalExpr*>(owner.get());
  const Literal* l = literal_of(node->left.get());
  if (!l) return owner;

  // Short circuiting makes these safe even when the other side is impure: the
  // language guarantees the right operand is not evaluated when the left one
  // decides the result.
  if (node->op == TokenType::And) {
    ++stats_.branches_eliminated;
    if (!l->truthy()) {
      return std::make_unique<LiteralExpr>(*l, node->line);
    }
    return std::move(node->right);
  }
  ++stats_.branches_eliminated;
  if (l->truthy()) {
    return std::make_unique<LiteralExpr>(*l, node->line);
  }
  return std::move(node->right);
}

}  // namespace lumen
