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

  // Each of these takes ownership and re-derives the typed node inside.
  //
  // They used to take (node, owner) as two arguments, and the caller built the
  // node from the very pointer it was moving from in the same call. Argument
  // evaluation order is unspecified in C++, so whether the node was computed
  // before or after the move was up to the compiler - Clang happened to do it
  // before, GCC after, and GCC got a null. Taking only the owner makes that
  // mistake impossible to write.
  ExprPtr fold_binary(ExprPtr owner);
  ExprPtr fold_unary(ExprPtr owner);
  ExprPtr fold_logical(ExprPtr owner);
  ExprPtr simplify_algebraic(ExprPtr owner);

  static const Literal* literal_of(const Expr* e);
  static bool is_pure(const Expr* e);

  Stats stats_;
};

}  // namespace lumen
