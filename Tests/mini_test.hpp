// PrismArchive -- dependency-free test harness.
//
// Deliberately not GoogleTest: PrismCore builds must stay fetch-free so CI
// (and a phone sandbox) can compile and run the suite in seconds.
#pragma once

#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace prism::test {

using TestFn = void (*)();

struct Case {
  std::string name;
  TestFn fn;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> registry;
  return registry;
}

inline int& FailureCount() {
  static int failures = 0;
  return failures;
}

inline int& CheckCount() {
  static int checks = 0;
  return checks;
}

struct Registrar {
  Registrar(const char* name, TestFn fn) { Registry().push_back({name, fn}); }
};

inline void ReportFailure(const char* file, int line, const std::string& what) {
  ++FailureCount();
  std::cout << "    FAIL  " << file << ":" << line << "  " << what << std::endl;
}

template <class A, class B>
void CheckEq(const A& a, const B& b, const char* exprA, const char* exprB, const char* file,
             int line) {
  ++CheckCount();
  if (!(a == b)) {
    std::ostringstream os;
    os << "expected (" << exprA << ") == (" << exprB << ")   got '" << a << "' vs '" << b << "'";
    ReportFailure(file, line, os.str());
  }
}

inline void Check(bool cond, const char* expr, const char* file, int line) {
  ++CheckCount();
  if (!cond) ReportFailure(file, line, std::string("expected true: ") + expr);
}

inline int RunAll(int argc, char** argv) {
  const std::string suite = (argc > 0 && argv[0]) ? argv[0] : "prism-tests";
  std::cout << "=== " << suite << " ===" << std::endl;
  int failedCases = 0;
  for (const Case& c : Registry()) {
    const int before = FailureCount();
    std::cout << "  [ RUN ] " << c.name << std::endl;
    c.fn();
    if (FailureCount() != before) {
      ++failedCases;
      std::cout << "  [FAILED] " << c.name << std::endl;
    } else {
      std::cout << "  [ OK  ] " << c.name << std::endl;
    }
  }
  std::cout << "  " << CheckCount() << " checks, " << FailureCount() << " failures, "
            << Registry().size() << " cases" << std::endl;
  return FailureCount() == 0 ? 0 : 1;
}

}  // namespace prism::test

#define PRISM_CONCAT_INNER(a, b) a##b
#define PRISM_CONCAT(a, b) PRISM_CONCAT_INNER(a, b)

#define PRISM_TEST(name)                                                     \
  static void PRISM_CONCAT(prism_test_fn_, __LINE__)();                      \
  static ::prism::test::Registrar PRISM_CONCAT(prism_test_reg_, __LINE__)(   \
      name, &PRISM_CONCAT(prism_test_fn_, __LINE__));                        \
  static void PRISM_CONCAT(prism_test_fn_, __LINE__)()

#define PRISM_CHECK(expr) ::prism::test::Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define PRISM_CHECK_EQ(a, b) \
  ::prism::test::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

#define PRISM_TEST_MAIN()                                     \
  int main(int argc, char** argv) {                           \
    return ::prism::test::RunAll(argc, argv);                 \
  }
