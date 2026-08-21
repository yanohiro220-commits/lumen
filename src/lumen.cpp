#include "lumen/lumen.hpp"

#include <ostream>

namespace lumen {

RunResult run_source(std::string_view source, std::ostream& out,
                     const RunOptions& options) {
  RunResult result;

  Lexer lexer(source);
  std::vector<Token> tokens = lexer.scan();

  Parser parser(std::move(tokens));
  Program program = parser.parse();
  if (parser.had_error()) {
    result.errors = parser.errors();
    return result;
  }

  if (options.optimize) {
    Optimizer optimizer;
    optimizer.run(program);
    result.optimizer = optimizer.stats();
  }

  VM vm(out);
  vm.gc().set_stress(options.stress_gc);
  vm.set_trace_counts(options.count_instructions);

  CompileOptions compile_options;
  compile_options.peephole = options.peephole;
  Compiler compiler(vm.gc(), compile_options);

  ObjFunction* script = compiler.compile(program);
  result.compiler = compiler.stats();
  if (script == nullptr) {
    result.errors = compiler.errors();
    return result;
  }
  if (options.dump_bytecode) {
    result.disassembly = script->chunk.disassemble("script");
    // Dumping does not run the program. Printing a disassembly *after* the
    // program's own output interleaves the two and makes both harder to read.
    result.status = VM::Result::Ok;
    result.ok = true;
    return result;
  }

  result.status = vm.run(script);
  result.vm = vm.stats();
  result.ok = result.status == VM::Result::Ok;
  if (!result.ok) result.runtime_error = vm.error();
  return result;
}

}  // namespace lumen
