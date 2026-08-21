#pragma once

#include <cstdint>

#include "lumen/ast.hpp"

namespace lumen {

// AST-level optimizer.
//
// Running before code generation rather than as a peephole pass over bytecode
// is what makes the interesting rewrites possible: dead branch elimination
// needs to delete a whole subtree, and by the time that subtree is a run of
// instructions with jumps into it, removing it safely is much harder than not
// emitting it.
//
// Everything here is a rewrite that cannot change observable behaviour. That
// rules out some tempting ones - see the notes on division and on `x * 0`.
class Optimizer {
 public:
  struct Stats {
    std::uint32_t constants_folded = 0;
    std::uint32_t branches_eliminated = 0;
    std::uint32_t statements_removed = 0;
    std::uint32_t algebraic_simplifications = 0;

    std::uint32_t total() const {
      return constants_folded + branches_eliminated + statements_removed +
             algebraic_simplifications;
    }
  };

  void run(Program& program);
  const Stats& stats() const { return stats_; }

 private:
  void optimize_statements(std::vector<StmtPtr>& statements);
  StmtPtr optimize_stmt(StmtPtr stmt);
  ExprPtr optimize_expr(ExprPtr expr);

  ExprPtr fold_binary(BinaryExpr* node, ExprPtr owner);
  ExprPtr fold_unary(UnaryExpr* node, ExprPtr owner);
  ExprPtr fold_logical(LogicalExpr* node, ExprPtr owner);
  ExprPtr simplify_algebraic(BinaryExpr* node, ExprPtr owner);

  static const Literal* literal_of(const Expr* e);
  static bool is_pure(const Expr* e);

  Stats stats_;
};

}  // namespace lumen
