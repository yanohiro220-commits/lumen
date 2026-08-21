// Benchmarks for the compiler pipeline and the VM.
//
// Each program is run with the optimizer on and off, so the numbers show what
// the optimizer actually buys rather than only that it exists.

#include <algorithm>
#include <chrono>
#include <utility>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "lumen/lumen.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Benchmark {
  const char* name;
  const char* source;
};

double seconds(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

const std::vector<Benchmark>& benchmarks() {
  static const std::vector<Benchmark> list = {
      {"fib(27) recursive",
       "fn fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n"
       "print fib(27);\n"},
      {"loop 3M iterations",
       "let total = 0;\n"
       "for (let i = 0; i < 3000000; i = i + 1) { total = total + i; }\n"
       "print total;\n"},
      {"nested loops 2000x1000",
       "let acc = 0;\n"
       "for (let i = 0; i < 2000; i = i + 1) {\n"
       "  for (let j = 0; j < 1000; j = j + 1) { acc = acc + 1; }\n"
       "}\n"
       "print acc;\n"},
      {"closure counters 200k",
       "fn counter() { let n = 0; return fn() { n = n + 1; return n; }; }\n"
       "let c = counter();\n"
       "let last = 0;\n"
       "for (let i = 0; i < 200000; i = i + 1) { last = c(); }\n"
       "print last;\n"},
      {"list build + sum 500k",
       "let xs = [];\n"
       "for (let i = 0; i < 500000; i = i + 1) { push(xs, i); }\n"
       "let total = 0;\n"
       "for (let i = 0; i < len(xs); i = i + 1) { total = total + xs[i]; }\n"
       "print total;\n"},
      {"string building 50k",
       "let s = \"\";\n"
       "for (let i = 0; i < 50000; i = i + 1) { s = s + \"x\"; }\n"
       "print len(s);\n"},
  };
  return list;
}

}  // namespace

int main() {
  std::printf("%-28s %12s %12s %10s %14s\n", "benchmark", "optimized", "baseline",
              "speedup", "instructions");
  std::printf("%s\n", std::string(80, '-').c_str());

  // Best of three per configuration. A single timed run in a process that is
  // still faulting in pages reports differences that have nothing to do with
  // the code, and the first configuration measured absorbs all of it - which
  // showed up here as the optimizer appearing to make an untouched benchmark
  // 30% slower.
  constexpr int kRepeats = 3;

  for (const auto& b : benchmarks()) {
    // Announced and flushed before the benchmark runs, not after. stdout to a
    // pipe is block buffered, so a crash discards everything since the last
    // flush and the log names nothing at all.
    std::printf("%-28s ", b.name);
    std::fflush(stdout);

    std::ostringstream sink;

    auto measure = [&](lumen::RunOptions opts) {
      double best = 1e18;
      lumen::RunResult last;
      for (int i = 0; i < kRepeats; ++i) {
        const auto t0 = Clock::now();
        last = lumen::run_source(b.source, sink, opts);
        best = std::min(best, seconds(t0));
      }
      return std::pair<double, lumen::RunResult>{best, last};
    };

    lumen::RunOptions fast;
    fast.count_instructions = true;

    lumen::RunOptions slow;
    slow.optimize = false;
    slow.peephole = false;
    slow.count_instructions = true;

    // Untimed warm-up before either configuration is measured.
    lumen::run_source(b.source, sink, lumen::RunOptions{});

    const auto [optimized, r1] = measure(fast);
    const auto [baseline, r2] = measure(slow);

    if (!r1.ok || !r2.ok) {
      std::printf("FAILED: %s\n",
                  r1.ok ? r2.runtime_error.c_str() : r1.runtime_error.c_str());
      std::fflush(stdout);
      continue;
    }
    std::printf("%11.3fs %11.3fs %9.2fx %14llu\n", optimized, baseline,
                baseline / optimized,
                static_cast<unsigned long long>(r1.vm.instructions));
    std::fflush(stdout);
  }

  return 0;
}
