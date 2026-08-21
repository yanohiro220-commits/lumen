#include "helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace lumen_test;

namespace {

// Every example carries its own expected output in `#=` comments at the top.
// Keeping the expectation next to the program means an example cannot drift
// from what it claims to print - the failure shows up here rather than in
// somebody's terminal.
struct Example {
  std::string name;
  std::string source;
  std::string expected;
};

std::vector<Example> load_examples() {
  std::vector<Example> out;
  const std::filesystem::path dir(LUMEN_EXAMPLES_DIR);
  if (!std::filesystem::exists(dir)) return out;

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".lum") paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    std::ifstream file(path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string source = buffer.str();

    std::string expected;
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.rfind("#= ", 0) == 0) {
        expected += line.substr(3) + "\n";
      } else if (line == "#=") {
        expected += "\n";
      }
    }
    out.push_back(Example{path.filename().string(), source, expected});
  }
  return out;
}

}  // namespace

TEST(Examples, DirectoryIsNotEmpty) {
  const auto examples = load_examples();
  CHECK_GT(examples.size(), std::size_t{0});
}

TEST(Examples, EachProducesItsDeclaredOutput) {
  for (const Example& example : load_examples()) {
    const Outcome r = run(example.source);
    if (!r.ok) {
      ::testing::fail(__FILE__, __LINE__, example.name + " failed: " + r.error);
    }
    if (r.output != example.expected) {
      ::testing::fail(__FILE__, __LINE__,
                      example.name + " printed:\n" + r.output + "expected:\n" +
                          example.expected);
    }
  }
}

// The strongest correctness check in the suite. An optimizer is right exactly
// when it does not change what a program does, so every example is run under
// all four combinations of AST optimization and peephole rewriting and the
// results must be identical.
TEST(Examples, EveryOptimizerConfigurationAgrees) {
  for (const Example& example : load_examples()) {
    bool agreed = false;
    const std::string detail = run_all_configs(example.source, &agreed);
    if (!agreed) {
      ::testing::fail(__FILE__, __LINE__,
                      example.name + ": configurations disagreed: " + detail);
    }
  }
}

TEST(Examples, EachSurvivesGcStress) {
  for (const Example& example : load_examples()) {
    lumen::RunOptions o;
    o.stress_gc = true;
    const Outcome stressed = run(example.source, o);
    const Outcome normal = run(example.source);
    if (stressed.output != normal.output || stressed.ok != normal.ok) {
      ::testing::fail(__FILE__, __LINE__,
                      example.name + " differs under GC stress:\n" +
                          stressed.output + "vs\n" + normal.output);
    }
  }
}

// A grab bag of programs that exercise combinations the examples do not, run
// through the same all-configurations comparison.
TEST(Differential, OptimizerNeverChangesBehaviour) {
  const std::vector<std::string> programs = {
      "print 1 + 2 * 3 - 4 / 2;",
      "let a = 1; { let b = 2; { let c = 3; print a + b + c; } }",
      "for (let i = 0; i < 5; i = i + 1) { for (let j = 0; j < 5; j = j + 1) { if (i == j) { print i * j; } } }",
      "fn f(n) { if (n <= 0) { return 0; } return n + f(n - 1); } print f(50);",
      "let xs = []; for (let i = 0; i < 20; i = i + 1) { push(xs, i * i); } print xs;",
      "let s = \"\"; for (let i = 0; i < 10; i = i + 1) { s = s + str(i); } print s;",
      "print false and undefined_thing;",
      "print true or undefined_thing;",
      "let i = 0; while (i < 10) { i = i + 1; if (i % 2 == 0) { continue; } if (i > 7) { break; } print i; }",
      "fn outer() { let x = 1; fn mid() { fn inner() { return x; } return inner(); } return mid(); } print outer();",
      "print 1 / 0;",
      "print undefined_variable;",
      "print [1,2,3][10];",
      "fn f(a) {} f();",
      "print 2 * 0;",
      "let a = 3; print a * 1 + 0 - 0;",
      "if (1 < 2) { print \"yes\"; } else { print \"no\"; }",
      "print len(\"abc\") + len([1,2]);",
  };
  for (const std::string& program : programs) {
    bool agreed = false;
    const std::string detail = run_all_configs(program, &agreed);
    if (!agreed) {
      ::testing::fail(__FILE__, __LINE__,
                      "configurations disagreed for: " + program + "\n  " + detail);
    }
  }
}
