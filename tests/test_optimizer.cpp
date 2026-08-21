#include "helpers.hpp"

using namespace lumen_test;

namespace {

std::string dump(const std::string& source, bool optimize, bool peephole) {
  lumen::RunOptions o;
  o.optimize = optimize;
  o.peephole = peephole;
  return disassemble(source, o);
}

}  // namespace

TEST(Optimizer, FoldsConstantArithmetic) {
  const Outcome r = run("print 2 * 3 + 4;");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("10\n"));
  CHECK_GE(r.optimizer.constants_folded, std::uint32_t{2});

  const std::string code = dump("let x = 2 * 3 + 4;", true, false);
  // The whole expression collapses to one constant, so no arithmetic remains.
  CHECK(!contains(code, "MULTIPLY"));
  CHECK(!contains(code, "ADD"));
  CHECK(contains(code, "(10)"));
}

TEST(Optimizer, FoldsUnaryOperators) {
  CHECK_EQ(run("print -(2 + 3);").output, std::string("-5\n"));
  CHECK_EQ(run("print !true;").output, std::string("false\n"));
  CHECK_EQ(run("print !!nil;").output, std::string("false\n"));
  const std::string code = dump("let x = -(2 + 3);", true, false);
  CHECK(!contains(code, "NEGATE"));
}

TEST(Optimizer, FoldsComparisonsAndEquality) {
  CHECK_EQ(run("print 1 < 2;").output, std::string("true\n"));
  CHECK_EQ(run("print \"a\" == \"a\";").output, std::string("true\n"));
  CHECK_EQ(run("print 1 == \"1\";").output, std::string("false\n"));
  const std::string code = dump("let x = 1 < 2;", true, false);
  CHECK(!contains(code, "LESS"));
}

TEST(Optimizer, FoldsStringConcatenation) {
  const std::string code = dump("let s = \"hello\" + \" \" + \"world\";", true, false);
  CHECK(!contains(code, "ADD"));
  CHECK(contains(code, "hello world"));
}

// Dividing by zero is a run-time error here, so folding it at compile time
// would replace an error with a constant and the two paths would disagree.
TEST(Optimizer, DoesNotFoldDivisionByZero) {
  const std::string code = dump("let x = 1 / 0;", true, false);
  CHECK(contains(code, "DIVIDE"));
  const Outcome r = run("print 1 / 0;");
  CHECK(!r.ok);
  CHECK(contains(r.error, "division by zero"));
}

TEST(Optimizer, EliminatesConstantBranches) {
  const std::string code = dump("if (false) { print \"dead\"; } print \"live\";", true, false);
  CHECK(!contains(code, "dead"));
  CHECK(contains(code, "live"));

  const std::string kept = dump("if (true) { print \"kept\"; }", true, false);
  CHECK(contains(kept, "kept"));
  CHECK(!contains(kept, "JUMP_IF"));
}

TEST(Optimizer, EliminatesWhileFalse) {
  const std::string code = dump("while (false) { print \"never\"; } print \"after\";", true, false);
  CHECK(!contains(code, "never"));
  CHECK(!contains(code, "LOOP"));
}

TEST(Optimizer, KeepsWhileTrue) {
  // An infinite loop is intentional and must survive.
  const std::string code = dump("while (true) { print 1; }", true, false);
  CHECK(contains(code, "LOOP"));
}

TEST(Optimizer, RemovesUnreachableStatements) {
  const Outcome r = run(
      "fn f() { return 1; print \"unreachable\"; print \"also\"; }\n"
      "print f();");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("1\n"));
  CHECK_GE(r.optimizer.statements_removed, std::uint32_t{2});
}

TEST(Optimizer, FoldsShortCircuitOperators) {
  CHECK_EQ(run("print false and 1;").output, std::string("false\n"));
  CHECK_EQ(run("print true or 1;").output, std::string("true\n"));
  CHECK_EQ(run("print true and 7;").output, std::string("7\n"));
  CHECK_EQ(run("print nil or 7;").output, std::string("7\n"));
  const std::string code = dump("let x = false and expensive();", true, false);
  CHECK(!contains(code, "CALL"));
}

TEST(Optimizer, AppliesAlgebraicIdentities) {
  const std::string code = dump("let a = 1; let b = a * 1 + 0;", true, false);
  CHECK(!contains(code, "MULTIPLY"));
  CHECK(!contains(code, "ADD"));

  const Outcome r = run("let a = 5; print a * 1; print a + 0; print a - 0; print a / 1;");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("5\n5\n5\n5\n"));
  CHECK_GE(r.optimizer.algebraic_simplifications, std::uint32_t{4});
}

// x * 0 -> 0 is not a valid rewrite. It is wrong for NaN and infinity, and it
// would also skip the type error the VM raises when x is not a number.
TEST(Optimizer, DoesNotFoldMultiplyByZero) {
  const std::string code = dump("let a = 1; let b = a * 0;", true, false);
  CHECK(contains(code, "MULTIPLY"));
  const Outcome r = run("let a = \"s\"; print a * 0;");
  CHECK(!r.ok);
  CHECK(contains(r.error, "cannot multiply"));
}

// Deleting a subtree is only safe when evaluating it has no effect. A call
// might print or assign, so it always counts as impure.
TEST(Optimizer, PreservesSideEffects) {
  const Outcome r = run(
      "fn effect() { print \"ran\"; return 3; }\n"
      "let x = effect() * 1;\n"
      "print x;");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("ran\n3\n"));
}

TEST(Optimizer, OptimizesInsideFunctionBodies) {
  const std::string code = dump("fn f() { return 2 + 3; } print f();", true, false);
  const Outcome r = run("fn f() { return 2 + 3; } print f();");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("5\n"));
  CHECK_GE(r.optimizer.constants_folded, std::uint32_t{1});
  (void)code;
}

TEST(Peephole, FusesIncrementIntoOneInstruction) {
  const std::string with = dump("for (let i = 0; i < 3; i = i + 1) {}", true, true);
  const std::string without = dump("for (let i = 0; i < 3; i = i + 1) {}", true, false);
  CHECK(contains(with, "INC_LOCAL"));
  CHECK(!contains(without, "INC_LOCAL"));
  CHECK_LT(with.size(), without.size());
}

TEST(Peephole, FusesLocalPlusConstant) {
  const std::string code = dump("fn f(a) { return a + 10; } print f(1);", true, true);
  const Outcome r = run("fn f(a) { return a + 10; } print f(1);");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("11\n"));
  (void)code;
}

TEST(Peephole, FusesAdjacentLocalReads) {
  const Outcome r = run("fn f(a, b) { return a + b; } print f(20, 22);");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("42\n"));
  CHECK_GE(r.compiler.peephole_rewrites, std::uint32_t{1});
}

// Rewriting shortens the code, so every jump has to be re-encoded against the
// new layout. A jump left pointing at its old offset lands in the middle of an
// instruction and the program silently does something else.
TEST(Peephole, JumpTargetsSurviveTheRewrite) {
  const Outcome r = run(
      "let total = 0;\n"
      "for (let i = 0; i < 20; i = i + 1) {\n"
      "  if (i % 3 == 0) { continue; }\n"
      "  if (i > 15) { break; }\n"
      "  total = total + i;\n"
      "}\n"
      "print total;\n");
  CHECK(r.ok);
  // 1..15 excluding multiples of 3: 1+2+4+5+7+8+10+11+13+14 = 75
  CHECK_EQ(r.output, std::string("75\n"));
}

// An instruction that a jump lands on cannot be folded into a
// superinstruction, or the jump arrives in the middle of an operand.
TEST(Peephole, DoesNotFoldAcrossAJumpTarget) {
  const Outcome r = run(
      "let i = 0;\n"
      "let sum = 0;\n"
      "while (i < 5) {\n"
      "  sum = sum + i;\n"
      "  i = i + 1;\n"
      "}\n"
      "print sum;\n");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("10\n"));
}

TEST(Peephole, SuperinstructionsMatchTheirSlowPath) {
  // INC_LOCAL replaces `local = local + 1`, so it has to raise the same error
  // when the local is not a number.
  const Outcome fused = run("let s = \"a\"; s = s + 1;", lumen::RunOptions{});
  lumen::RunOptions no_peephole;
  no_peephole.peephole = false;
  const Outcome plain = run("let s = \"a\"; s = s + 1;", no_peephole);
  CHECK_EQ(fused.ok, plain.ok);
  CHECK_EQ(fused.error, plain.error);
}

TEST(Peephole, IncLocalOnAStringConcatenates) {
  const Outcome r = run(
      "fn f() { let s = \"a\"; s = s + \"b\"; return s; }\n"
      "print f();");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("ab\n"));
}

TEST(Optimizer, DisabledLeavesCodeAlone) {
  lumen::RunOptions o;
  o.optimize = false;
  o.peephole = false;
  const Outcome r = run("print 2 + 3;", o);
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("5\n"));
  CHECK_EQ(r.optimizer.total(), std::uint32_t{0});
  CHECK_EQ(r.compiler.peephole_rewrites, std::uint32_t{0});
}
