#pragma once

// A ~120 line test runner. The project has no third-party dependencies, and
// pulling in a framework to get TEST/CHECK plus a summary line would be the
// only reason to add one.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
  const char* suite;
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) {
    registry().push_back(TestCase{suite, name, fn});
  }
};

struct Failure : std::exception {
  std::string message;
  explicit Failure(std::string m) : message(std::move(m)) {}
  const char* what() const noexcept override { return message.c_str(); }
};

inline std::string loc(const char* file, int line) {
  return std::string(file) + ":" + std::to_string(line);
}

inline void fail(const char* file, int line, const std::string& what) {
  throw Failure(loc(file, line) + ": " + what);
}

// Renders a value for a failure message. std::to_string alone would not
// compile for the string and enum comparisons the tests do, and a CHECK_EQ
// that prints nothing useful is a CHECK.
inline std::string to_display(const std::string& s) { return "\"" + s + "\""; }
inline std::string to_display(const char* s) {
  return s ? "\"" + std::string(s) + "\"" : std::string("null");
}
inline std::string to_display(bool b) { return b ? "true" : "false"; }

template <typename T>
std::string to_display(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

inline bool near(double a, double b, double tol) {
  if (std::isnan(a) || std::isnan(b)) return false;
  return std::abs(a - b) <= tol;
}

int run_all(const char* filter);

}  // namespace testing

#define LU_CONCAT_INNER(a, b) a##b
#define LU_CONCAT(a, b) LU_CONCAT_INNER(a, b)

#define TEST(suite_name, test_name)                                        \
  static void LU_CONCAT(lu_test_, __LINE__)();                             \
  static ::testing::Registrar LU_CONCAT(lu_reg_, __LINE__)(                \
      #suite_name, #test_name, &LU_CONCAT(lu_test_, __LINE__));            \
  static void LU_CONCAT(lu_test_, __LINE__)()

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")");  \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto lu_a = (a);                                                       \
    auto lu_b = (b);                                                       \
    if (!(lu_a == lu_b)) {                                                 \
      ::testing::fail(__FILE__, __LINE__,                                  \
                      "CHECK_EQ(" #a ", " #b ") -> " +                     \
                          ::testing::to_display(lu_a) + " != " +           \
                          ::testing::to_display(lu_b));                    \
    }                                                                      \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                              \
  do {                                                                     \
    const double lu_a = (a);                                               \
    const double lu_b = (b);                                               \
    if (!::testing::near(lu_a, lu_b, (tol))) {                             \
      ::testing::fail(__FILE__, __LINE__,                                  \
                      "CHECK_NEAR(" #a ", " #b ") -> " +                   \
                          std::to_string(lu_a) + " vs " +                  \
                          std::to_string(lu_b));                           \
    }                                                                      \
  } while (0)

#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))

#define CHECK_THROWS(expr)                                                 \
  do {                                                                     \
    bool lu_threw = false;                                                 \
    try {                                                                  \
      (void)(expr);                                                        \
    } catch (const ::testing::Failure&) {                                  \
      throw;                                                               \
    } catch (...) {                                                        \
      lu_threw = true;                                                     \
    }                                                                      \
    if (!lu_threw)                                                         \
      ::testing::fail(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")");      \
  } while (0)
