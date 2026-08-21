#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "lumen/lumen.hpp"
#include "test_framework.hpp"

namespace lumen_test {

struct Outcome {
  std::string output;
  std::string error;      // first compile diagnostic, or the runtime error
  bool ok = false;
  lumen::Optimizer::Stats optimizer;
  lumen::Compiler::Stats compiler;
  lumen::VM::Stats vm;
};

inline Outcome run(const std::string& source, lumen::RunOptions options = {}) {
  std::ostringstream out;
  lumen::RunResult result = lumen::run_source(source, out, options);
  Outcome outcome;
  outcome.output = out.str();
  outcome.ok = result.ok;
  outcome.optimizer = result.optimizer;
  outcome.compiler = result.compiler;
  outcome.vm = result.vm;
  if (!result.errors.empty()) {
    outcome.error = result.errors.front().message;
  } else if (!result.runtime_error.empty()) {
    // Only the first line; the rest is a stack trace whose line numbers would
    // make the tests brittle.
    outcome.error = result.runtime_error.substr(0, result.runtime_error.find('\n'));
  }
  return outcome;
}

// Runs a program through every configuration and requires identical output.
//
// This is the strongest single test in the suite. An optimizer is correct
// exactly when it does not change what a program does, and comparing the four
// combinations of AST optimization and peephole rewriting checks that directly
// rather than checking that some particular rewrite happened.
inline std::string run_all_configs(const std::string& source, bool* agreed) {
  const std::vector<std::pair<bool, bool>> configs = {
      {true, true}, {true, false}, {false, true}, {false, false}};
  std::string first;
  *agreed = true;
  for (std::size_t i = 0; i < configs.size(); ++i) {
    lumen::RunOptions o;
    o.optimize = configs[i].first;
    o.peephole = configs[i].second;
    const Outcome outcome = run(source, o);
    const std::string combined = outcome.output + "|" + outcome.error;
    if (i == 0) {
      first = combined;
    } else if (combined != first) {
      *agreed = false;
      return combined + "  !=  " + first;
    }
  }
  return first;
}

inline std::string disassemble(const std::string& source,
                               lumen::RunOptions options = {}) {
  options.dump_bytecode = true;
  std::ostringstream out;
  lumen::RunResult result = lumen::run_source(source, out, options);
  // An empty listing would make every "this instruction is absent" assertion
  // pass for the wrong reason, which is exactly how a whole group of these
  // tests was silently vacuous until the opcode names changed.
  if (result.disassembly.empty()) return "<no disassembly: " + source + ">";
  return result.disassembly;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

inline int count_occurrences(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return 0;
  int n = 0;
  for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    ++n;
  }
  return n;
}

}  // namespace lumen_test
