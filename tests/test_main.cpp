#include "test_framework.hpp"

#include <cstring>

namespace testing {

int run_all(const char* filter) {
  int passed = 0;
  std::vector<std::string> failures;
  for (const auto& t : registry()) {
    if (filter && *filter && !std::strstr(t.suite, filter) &&
        !std::strstr(t.name, filter)) {
      continue;
    }
    try {
      t.fn();
      ++passed;
      std::printf("  \033[32mPASS\033[0m %s.%s\n", t.suite, t.name);
    } catch (const Failure& f) {
      failures.push_back(std::string(t.suite) + "." + t.name + "\n      " +
                         f.what());
      std::printf("  \033[31mFAIL\033[0m %s.%s\n        %s\n", t.suite, t.name,
                  f.what());
    } catch (const std::exception& e) {
      failures.push_back(std::string(t.suite) + "." + t.name +
                         "\n      unexpected exception: " + e.what());
      std::printf("  \033[31mFAIL\033[0m %s.%s\n        threw: %s\n", t.suite,
                  t.name, e.what());
    }
  }
  std::printf("\n%d passed, %zu failed\n", passed, failures.size());
  return failures.empty() ? 0 : 1;
}

}  // namespace testing

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  return testing::run_all(filter);
}
