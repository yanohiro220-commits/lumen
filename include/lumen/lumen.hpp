#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "lumen/compiler.hpp"
#include "lumen/lexer.hpp"
#include "lumen/optimizer.hpp"
#include "lumen/parser.hpp"
#include "lumen/vm.hpp"

namespace lumen {

struct RunOptions {
  bool optimize = true;   // AST-level constant folding and dead code removal
  bool peephole = true;   // bytecode superinstructions
  bool stress_gc = false; // collect on every allocation
  bool dump_bytecode = false;
  bool count_instructions = false;
};

struct RunResult {
  bool ok = false;
  VM::Result status = VM::Result::CompileError;
  std::vector<Diagnostic> errors;
  std::string runtime_error;
  std::string disassembly;
  Optimizer::Stats optimizer;
  Compiler::Stats compiler;
  VM::Stats vm;
};

// Compiles and runs `source`, writing program output to `out`.
//
// The whole pipeline behind one call, because every caller - the CLI, the
// REPL, the test suite - wants the same five stages in the same order, and a
// caller that assembles them by hand is a caller that can get the order wrong.
RunResult run_source(std::string_view source, std::ostream& out,
                     const RunOptions& options = {});

}  // namespace lumen
