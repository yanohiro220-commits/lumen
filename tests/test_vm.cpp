#include "helpers.hpp"

using namespace lumen_test;

namespace {

void expect_output(const std::string& source, const std::string& want) {
  const Outcome r = run(source);
  if (!r.ok) {
    ::testing::fail(__FILE__, __LINE__, "program failed: " + r.error);
  }
  CHECK_EQ(r.output, want);
}

void expect_error(const std::string& source, const std::string& fragment) {
  const Outcome r = run(source);
  CHECK(!r.ok);
  if (!contains(r.error, fragment)) {
    ::testing::fail(__FILE__, __LINE__,
                    "error was \"" + r.error + "\", expected it to contain \"" +
                        fragment + "\"");
  }
}

}  // namespace

TEST(VM, Arithmetic) {
  expect_output("print 1 + 2;", "3\n");
  expect_output("print 7 - 10;", "-3\n");
  expect_output("print 6 * 7;", "42\n");
  expect_output("print 10 / 4;", "2.5\n");
  expect_output("print 10 % 3;", "1\n");
  expect_output("print -(3 + 4);", "-7\n");
}

TEST(VM, PrecedenceAndAssociativity) {
  expect_output("print 2 + 3 * 4;", "14\n");
  expect_output("print (2 + 3) * 4;", "20\n");
  // Left associative: 10 - 3 - 2 is 5, not 9.
  expect_output("print 10 - 3 - 2;", "5\n");
  expect_output("print 100 / 10 / 2;", "5\n");
  expect_output("print 2 + 3 < 4 + 5;", "true\n");
}

TEST(VM, NumbersPrintWithoutTrailingZeros) {
  expect_output("print 1;", "1\n");
  expect_output("print 1.0;", "1\n");
  expect_output("print 1.5;", "1.5\n");
  expect_output("print 0 - 0;", "0\n");
}

TEST(VM, Comparisons) {
  expect_output("print 1 < 2;", "true\n");
  expect_output("print 2 <= 2;", "true\n");
  expect_output("print 3 > 4;", "false\n");
  expect_output("print 4 >= 4;", "true\n");
  expect_output("print 1 == 1;", "true\n");
  expect_output("print 1 != 1;", "false\n");
  expect_output("print \"a\" < \"b\";", "true\n");
}

TEST(VM, EqualityAcrossTypes) {
  expect_output("print 1 == \"1\";", "false\n");
  expect_output("print nil == false;", "false\n");
  expect_output("print true == true;", "true\n");
  expect_output("print [1, 2] == [1, 2];", "true\n");
  expect_output("print [1, 2] == [1, 3];", "false\n");
}

TEST(VM, OnlyNilAndFalseAreFalsey) {
  // Zero and the empty string are truthy on purpose; the test pins that
  // decision so it cannot drift.
  expect_output("if (0) { print \"zero is truthy\"; }", "zero is truthy\n");
  expect_output("if (\"\") { print \"empty is truthy\"; }", "empty is truthy\n");
  expect_output("if (nil) { print \"no\"; } else { print \"nil is falsey\"; }",
                "nil is falsey\n");
  expect_output("print !nil;", "true\n");
  expect_output("print !0;", "false\n");
}

TEST(VM, StringOperations) {
  expect_output("print \"ab\" + \"cd\";", "abcd\n");
  expect_output("print len(\"hello\");", "5\n");
  expect_output("print \"hello\"[1];", "e\n");
  expect_output("print \"hello\"[-1];", "o\n");
  expect_output("print \"a\" == \"a\";", "true\n");
}

TEST(VM, LongStringsCompareByContent) {
  // Above the intern threshold strings are not deduplicated, so equality has
  // to fall back to comparing contents rather than pointers.
  expect_output(
      "let a = \"\"; let b = \"\";\n"
      "for (let i = 0; i < 100; i = i + 1) { a = a + \"x\"; b = b + \"x\"; }\n"
      "print a == b;\n"
      "print len(a);\n",
      "true\n100\n");
}

TEST(VM, GlobalsAndLocals) {
  expect_output("let a = 1; { let a = 2; print a; } print a;", "2\n1\n");
  expect_output("let a = 1; a = a + 1; print a;", "2\n");
  expect_output("let a; print a;", "nil\n");
}

TEST(VM, AssignmentIsAnExpression) {
  expect_output("let a = 0; print a = 5;", "5\n");
  expect_output("let a = 0; let b = 0; a = b = 3; print a + b;", "6\n");
}

TEST(VM, ControlFlow) {
  expect_output("if (true) { print 1; } else { print 2; }", "1\n");
  expect_output("if (false) { print 1; } else { print 2; }", "2\n");
  expect_output("let i = 0; while (i < 3) { print i; i = i + 1; }", "0\n1\n2\n");
  expect_output("for (let i = 0; i < 3; i = i + 1) { print i; }", "0\n1\n2\n");
}

TEST(VM, LogicalOperatorsShortCircuit) {
  expect_output("print false and (1 / 0);", "false\n");
  expect_output("print true or (1 / 0);", "true\n");
  // The result is the operand, not a coerced boolean.
  expect_output("print nil or \"fallback\";", "fallback\n");
  expect_output("print 1 and 2;", "2\n");
}

TEST(VM, BreakAndContinue) {
  expect_output("for (let i = 0; i < 10; i = i + 1) { if (i == 3) { break; } print i; }",
                "0\n1\n2\n");
  expect_output(
      "for (let i = 0; i < 5; i = i + 1) { if (i % 2 == 0) { continue; } print i; }",
      "1\n3\n");
  expect_output("let i = 0; while (true) { i = i + 1; if (i > 2) { break; } } print i;",
                "3\n");
}

// `continue` in a for loop has to reach the increment, not the condition.
// Jumping to the condition skips `i = i + 1` and loops forever.
TEST(VM, ContinueRunsTheIncrement) {
  const Outcome r = run(
      "let seen = 0;\n"
      "for (let i = 0; i < 100; i = i + 1) { if (i < 98) { continue; } seen = seen + 1; }\n"
      "print seen;\n");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("2\n"));
}

// Breaking out of a scope has to discard the locals declared in it, or the
// stack grows by one per iteration until it overflows.
TEST(VM, BreakDoesNotLeakStackSlots) {
  const Outcome r = run(
      "let n = 0;\n"
      "for (let i = 0; i < 100000; i = i + 1) {\n"
      "  let a = i; let b = i * 2;\n"
      "  if (b > 4) { n = n + 1; continue; }\n"
      "  n = n + 1;\n"
      "}\n"
      "print n;\n");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("100000\n"));
}

TEST(VM, Functions) {
  expect_output("fn add(a, b) { return a + b; } print add(2, 3);", "5\n");
  expect_output("fn noop() {} print noop();", "nil\n");
  expect_output("fn early(n) { if (n > 0) { return \"pos\"; } return \"non\"; }\n"
                "print early(1); print early(-1);",
                "pos\nnon\n");
}

TEST(VM, Recursion) {
  expect_output("fn fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }\n"
                "print fib(20);",
                "6765\n");
  expect_output("fn fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\n"
                "print fact(10);",
                "3628800\n");
}

TEST(VM, MutualRecursion) {
  expect_output(
      "fn is_even(n) { if (n == 0) { return true; } return is_odd(n - 1); }\n"
      "fn is_odd(n) { if (n == 0) { return false; } return is_even(n - 1); }\n"
      "print is_even(10); print is_odd(7);",
      "true\ntrue\n");
}

TEST(VM, ClosuresCaptureByReference) {
  expect_output(
      "fn counter() { let n = 0; return fn() { n = n + 1; return n; }; }\n"
      "let c = counter();\n"
      "print c(); print c(); print c();",
      "1\n2\n3\n");
}

TEST(VM, ClosuresAreIndependent) {
  expect_output(
      "fn counter() { let n = 0; return fn() { n = n + 1; return n; }; }\n"
      "let a = counter(); let b = counter();\n"
      "a(); a();\n"
      "print a(); print b();",
      "3\n1\n");
}

// Two closures made in the same scope must share one upvalue, or assigning
// through one is invisible to the other.
TEST(VM, ClosuresShareACapturedVariable) {
  expect_output(
      "fn pair() {\n"
      "  let n = 0;\n"
      "  let inc = fn() { n = n + 1; return n; };\n"
      "  let get = fn() { return n; };\n"
      "  inc(); inc();\n"
      "  return get;\n"
      "}\n"
      "print pair()();",
      "2\n");
}

// A variable captured three levels down has to be threaded through every
// intermediate closure as an upvalue of an upvalue.
TEST(VM, TransitiveCapture) {
  expect_output(
      "fn outer() {\n"
      "  let x = \"captured\";\n"
      "  fn middle() {\n"
      "    fn inner() { return x; }\n"
      "    return inner;\n"
      "  }\n"
      "  return middle;\n"
      "}\n"
      "print outer()()();",
      "captured\n");
}

// The captured local is gone from the stack by the time the closure runs, so
// the upvalue must have been closed over a heap copy.
TEST(VM, ClosureOutlivesItsFrame) {
  expect_output(
      "fn make(v) { return fn() { return v; }; }\n"
      "let fns = [];\n"
      "for (let i = 0; i < 3; i = i + 1) { push(fns, make(i)); }\n"
      "print fns[0](); print fns[1](); print fns[2]();",
      "0\n1\n2\n");
}

TEST(VM, Lambdas) {
  expect_output("let f = fn(a, b) { return a * b; }; print f(6, 7);", "42\n");
  expect_output("print (fn(x) { return x + 1; })(41);", "42\n");
}

TEST(VM, HigherOrderFunctions) {
  expect_output(
      "fn apply(f, x) { return f(x); }\n"
      "print apply(fn(v) { return v * 2; }, 21);",
      "42\n");
}

TEST(VM, Lists) {
  expect_output("print [1, 2, 3];", "[1, 2, 3]\n");
  expect_output("print [];", "[]\n");
  expect_output("let xs = [1, 2]; push(xs, 3); print xs;", "[1, 2, 3]\n");
  expect_output("let xs = [1, 2, 3]; print pop(xs); print xs;", "3\n[1, 2]\n");
  expect_output("let xs = [1, 2, 3]; xs[1] = 9; print xs;", "[1, 9, 3]\n");
  expect_output("print [1, 2] + [3];", "[1, 2, 3]\n");
  expect_output("print len([1, 2, 3]);", "3\n");
  expect_output("print [[1, 2], [3]][0][1];", "2\n");
  expect_output("print [1, \"two\", nil, true];", "[1, \"two\", nil, true]\n");
}

TEST(VM, NegativeIndices) {
  expect_output("print [1, 2, 3][-1];", "3\n");
  expect_output("print [1, 2, 3][-3];", "1\n");
  expect_error("print [1, 2, 3][-4];", "out of range");
}

TEST(VM, Builtins) {
  expect_output("print str(42) + \"!\";", "42!\n");
  expect_output("print num(\"3.5\") + 1;", "4.5\n");
  expect_output("print num(\"abc\");", "nil\n");
  // A partial parse is not a number; returning the prefix would silently turn
  // "12abc" into 12.
  expect_output("print num(\"12abc\");", "nil\n");
  expect_output("print type(1); print type(\"s\"); print type([]); print type(nil);",
                "number\nstring\nlist\nnil\n");
  expect_output("print sqrt(16); print floor(3.7); print abs(0 - 5);", "4\n3\n5\n");
}

TEST(VM, RuntimeErrors) {
  expect_error("print 1 / 0;", "division by zero");
  expect_error("print 1 % 0;", "modulo by zero");
  expect_error("print 1 + \"a\";", "cannot add");
  expect_error("print -\"a\";", "cannot negate");
  expect_error("print undefined_name;", "undefined variable");
  expect_error("undefined_name = 1;", "undefined variable");
  expect_error("print [1][5];", "out of range");
  expect_error("print 1();", "can only call");
  expect_error("fn f(a) {} f();", "expected 1 argument but got 0");
  expect_error("fn f(a) {} f(1, 2);", "expected 1 argument but got 2");
  expect_error("print len(1);", "expects a string or list");
}

TEST(VM, RuntimeErrorCarriesAStackTrace) {
  std::ostringstream out;
  lumen::RunResult r = lumen::run_source(
      "fn inner() { return 1 / 0; }\n"
      "fn outer() { return inner(); }\n"
      "print outer();\n",
      out);
  CHECK(!r.ok);
  CHECK(contains(r.runtime_error, "division by zero"));
  CHECK(contains(r.runtime_error, "in inner()"));
  CHECK(contains(r.runtime_error, "in outer()"));
  CHECK(contains(r.runtime_error, "in script"));
}

TEST(VM, DeepRecursionOverflowsCleanly) {
  // A runaway recursion has to produce an error, not a segmentation fault.
  const Outcome r = run("fn f(n) { return f(n + 1); } f(0);");
  CHECK(!r.ok);
  CHECK(contains(r.error, "stack overflow"));
}

TEST(VM, CompileErrors) {
  expect_error("let;", "expected a variable name");
  expect_error("print 1", "expected ';'");
  expect_error("let a = ;", "expected an expression");
  expect_error("{ let a = 1;", "expected '}'");
  expect_error("1 + ;", "expected an expression");
  expect_error("break;", "outside a loop");
  expect_error("continue;", "outside a loop");
  expect_error("return 1;", "cannot return from top-level");
  expect_error("{ let a = 1; let a = 2; }", "already declared");
  expect_error("{ let a = a; }", "in its own initializer");
  expect_error("1 = 2;", "invalid assignment target");
}

TEST(VM, ParserRecoversAndReportsMoreThanOneError) {
  std::ostringstream out;
  lumen::RunResult r = lumen::run_source(
      "let a = ;\nlet b = ;\nlet c = ;\n", out);
  CHECK(!r.ok);
  CHECK_GE(r.errors.size(), std::size_t{2});
}

TEST(VM, InstructionCounting) {
  lumen::RunOptions o;
  o.count_instructions = true;
  const Outcome r = run("for (let i = 0; i < 100; i = i + 1) {}", o);
  CHECK(r.ok);
  CHECK_GT(r.vm.instructions, std::uint64_t{100});
  CHECK_GT(r.vm.max_stack_depth, std::size_t{0});
}
