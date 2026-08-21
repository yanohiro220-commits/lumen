#include "helpers.hpp"

#include "lumen/gc.hpp"

using namespace lumen;
using namespace lumen_test;

namespace {

// Every program in this file is run twice: normally, and with a collection
// forced on every single allocation. Stress mode is where collector bugs
// actually surface - a missing root is harmless until a collection lands in
// the one window where the object is only reachable from a C++ local.
Outcome run_stressed(const std::string& source) {
  RunOptions o;
  o.stress_gc = true;
  return run(source, o);
}

void expect_same_under_stress(const std::string& source) {
  const Outcome normal = run(source);
  const Outcome stressed = run_stressed(source);
  CHECK_EQ(normal.ok, stressed.ok);
  CHECK_EQ(normal.output, stressed.output);
  CHECK_EQ(normal.error, stressed.error);
}

}  // namespace

TEST(GC, InterningReturnsTheSamePointer) {
  GC gc;
  ObjString* a = gc.intern("hello");
  ObjString* b = gc.intern("hello");
  ObjString* c = gc.intern("world");
  CHECK(a == b);
  CHECK(a != c);
  CHECK_EQ(a->chars, std::string("hello"));
}

TEST(GC, LongStringsAreNotInterned) {
  GC gc;
  const std::string long_text(GC::kInternThreshold + 1, 'x');
  ObjString* a = gc.string(long_text);
  ObjString* b = gc.string(long_text);
  // Not the same object, so equality has to compare contents.
  CHECK(a != b);
  CHECK_EQ(a->chars, b->chars);
  CHECK(Value::object(a) == Value::object(b));
}

TEST(GC, ShortStringsGoThroughTheInternTable) {
  GC gc;
  const std::string short_text(GC::kInternThreshold, 'y');
  CHECK(gc.string(short_text) == gc.string(short_text));
}

TEST(GC, HashIsComputedLazilyAndIsStable) {
  GC gc;
  ObjString* s = gc.new_string("some text");
  const std::uint32_t first = s->hash();
  CHECK_EQ(s->hash(), first);
  CHECK_EQ(hash_string("some text"), first);
}

TEST(GC, CollectsUnreachableObjects) {
  GC gc;
  gc.set_root_marker([](GC&) {});  // nothing is a root
  for (int i = 0; i < 100; ++i) gc.new_list();
  CHECK_GE(gc.live_objects(), std::size_t{100});
  gc.collect();
  CHECK_EQ(gc.live_objects(), std::size_t{0});
  CHECK_GE(gc.stats().objects_freed, std::size_t{100});
}

TEST(GC, KeepsReachableObjects) {
  GC gc;
  ObjList* kept = gc.new_list();
  gc.set_root_marker([kept](GC& g) { g.mark_object(kept); });
  for (int i = 0; i < 50; ++i) gc.new_list();
  gc.collect();
  CHECK_EQ(gc.live_objects(), std::size_t{1});
}

TEST(GC, TracesThroughListContents) {
  GC gc;
  ObjList* root = gc.new_list();
  ObjList* nested = gc.new_list();
  root->items.push_back(Value::object(nested));
  gc.set_root_marker([root](GC& g) { g.mark_object(root); });
  for (int i = 0; i < 20; ++i) gc.new_list();
  gc.collect();
  CHECK_EQ(gc.live_objects(), std::size_t{2});
}

TEST(GC, TempRootProtectsAnObject) {
  GC gc;
  gc.set_root_marker([](GC&) {});
  ObjList* protectee = gc.new_list();
  {
    TempRoot root(gc, protectee);
    gc.collect();
    CHECK_EQ(gc.live_objects(), std::size_t{1});
  }
  gc.collect();
  CHECK_EQ(gc.live_objects(), std::size_t{0});
}

TEST(GC, InternTableIsWeak) {
  GC gc;
  gc.set_root_marker([](GC&) {});
  gc.intern("temporary");
  CHECK_GE(gc.live_objects(), std::size_t{1});
  gc.collect();
  // If the table held strong references nothing would ever be freed, and the
  // heap would grow with every distinct string a program ever mentions.
  CHECK_EQ(gc.live_objects(), std::size_t{0});
}

TEST(GC, HandlesDeeplyNestedStructuresWithoutRecursing) {
  // The mark phase uses an explicit worklist. A recursive marker would blow
  // the C++ stack here, in the middle of a half-marked heap.
  GC gc;
  // The root marker has to be installed before anything is allocated: this
  // loop allocates past the collection threshold, and a collection with no
  // roots registered frees the chain being built out from under the loop.
  ObjList* head = nullptr;
  gc.set_root_marker([&head](GC& g) {
    if (head) g.mark_object(head);
  });

  head = gc.new_list();
  ObjList* current = head;
  for (int i = 0; i < 50000; ++i) {
    ObjList* next = gc.new_list();
    current->items.push_back(Value::object(next));
    current = next;
  }
  gc.collect();
  CHECK_EQ(gc.live_objects(), std::size_t{50001});
}

TEST(GC, StressModeAgreesWithNormalMode) {
  expect_same_under_stress("print 1 + 2;");
  expect_same_under_stress("let xs = [1, 2, 3]; push(xs, 4); print xs;");
  expect_same_under_stress("fn fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print fib(12);");
  expect_same_under_stress(
      "fn counter() { let n = 0; return fn() { n = n + 1; return n; }; }\n"
      "let c = counter(); c(); c(); print c();");
  expect_same_under_stress("print \"a\" + \"b\" + \"c\";");
  expect_same_under_stress("print 1 + \"a\";");
}

// A closure allocated while its upvalues are still being captured is reachable
// only from the VM stack. Losing it there is the classic collector bug.
TEST(GC, ClosureSurvivesCollectionDuringCapture) {
  const Outcome r = run_stressed(
      "fn make(a, b) { return fn() { return a + b; }; }\n"
      "let fns = [];\n"
      "for (let i = 0; i < 20; i = i + 1) { push(fns, make(i, i)); }\n"
      "let total = 0;\n"
      "for (let i = 0; i < 20; i = i + 1) { total = total + fns[i](); }\n"
      "print total;\n");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("380\n"));
}

// Compilation allocates functions and interned strings, and a nested function
// is reachable only from a C++ local until it reaches the enclosing constant
// pool.
TEST(GC, CompilationSurvivesCollection) {
  const Outcome r = run_stressed(
      "fn a() { fn b() { fn c() { return \"deep\"; } return c(); } return b(); }\n"
      "print a();\n");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("deep\n"));
}

TEST(GC, GlobalsAreRoots) {
  const Outcome r = run_stressed(
      "let kept = [1, 2, 3];\n"
      "for (let i = 0; i < 100; i = i + 1) { let junk = [i, i, i]; }\n"
      "print kept;\n");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("[1, 2, 3]\n"));
}

TEST(GC, ReclaimsMemoryInALongRunningLoop) {
  // Allocating in a loop without retaining anything must not grow the heap
  // without bound.
  const Outcome r = run(
      "let last = nil;\n"
      "for (let i = 0; i < 200000; i = i + 1) { last = [i, i + 1, i + 2]; }\n"
      "print last;\n");
  CHECK(r.ok);
  CHECK_EQ(r.output, std::string("[199999, 200000, 200001]\n"));
}
