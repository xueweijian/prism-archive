// PrismArchive -- dependency-free test harness.
//
// Deliberately not GoogleTest: PrismCore builds must stay fetch-free so CI
// (and a phone sandbox) can compile and run the suite in seconds.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
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

// --- Mutation verdicts (CONTRACTS D27) -------------------------------------
//
// A mutation build's verdict must be printed at the FIRST assertion failure and
// the process must leave immediately. Printing it after the suite returns is a
// race teardown wins: the mutant keeps running into code that assumes its
// premise, dies there, and CTest reports "required regular expression not
// found" while the FAIL lines above were correct (C34 no-archive-switch, C25
// shared_reader, C35, C39 -- 2026-11-02).
//
// The prefix is supplied by the caller: every card already pins its own marker
// ("C34 MUTATION CAUGHT: no_archive_switch", ...), so the harness must not
// invent one. Exit 0 on purpose: the CTest entries decide with
// PASS_REGULAR_EXPRESSION (D26), and a non-zero code would lose to the same
// teardown crash this mechanism exists to survive.
//
// NOTE (C48): this hook landed with C44 and was reverted with it in ce6cf26.
// Card C48's contract requires D27, so the hook alone is re-landed here --
// unarmed behaviour is byte-identical, and no other caller is touched. If C44
// re-lands its own version, this block is the one to keep (it carries the D3
// predicate; the reverted shape could only prove the injection COMPILED).
struct MutationVerdictState {
  bool armed = false;
  std::string prefix;
  // Empty = "the injection compiled in" is the only proof available (the
  // single-argument arming). When set, CAUGHT is printed only if this says the
  // injected defect really fired -- CONTRACTS D3: a mutant build can go red for
  // an unrelated reason, and that run must stay RED without a false CAUGHT.
  std::function<bool()> injectionActive;
  std::atomic<bool> printed{false};
};

inline MutationVerdictState& MutationVerdict() {
  static MutationVerdictState state;
  return state;
}

// Arm the verdict with the caller's complete marker prefix plus the runtime
// proof that the injected defect fired. nullptr disarms.
inline void SetMutationVerdict(const char* prefix, std::function<bool()> injectionActive) {
  MutationVerdictState& state = MutationVerdict();
  state.prefix = prefix != nullptr ? prefix : "";
  state.injectionActive = std::move(injectionActive);
  state.printed.store(false, std::memory_order_relaxed);
  state.armed = prefix != nullptr;
}

// Arm without a runtime predicate: the build-time mutation flag is the proof.
inline void SetMutationVerdict(const char* prefix) {
  SetMutationVerdict(prefix, std::function<bool()>());
}

inline void ClearMutationVerdict() {
  MutationVerdictState& state = MutationVerdict();
  state.armed = false;
  state.prefix.clear();
  state.injectionActive = nullptr;
  state.printed.store(false, std::memory_order_relaxed);
}

inline bool MutationVerdictArmed() { return MutationVerdict().armed; }

inline void ReportFailure(const char* file, int line, const std::string& what) {
  ++FailureCount();
  std::cout << "    FAIL  " << file << ":" << line << "  " << what << std::endl;
  MutationVerdictState& verdict = MutationVerdict();
  if (!verdict.armed) return;
  // CONTRACTS D3: red does not by itself mean CAUGHT. When the caller supplied
  // a predicate, only a failure that provably came from the injected defect may
  // produce the verdict; otherwise fall through to the ordinary failure path
  // (record and continue, exit 1 at the end) without ever printing CAUGHT.
  if (verdict.injectionActive && !verdict.injectionActive()) return;
  // Only the first failure prints: a worker thread failing at the same time
  // must not append a second marker (or half a line) to a CAUGHT run.
  if (verdict.printed.exchange(true, std::memory_order_acq_rel)) return;
  std::cout << verdict.prefix << " (first failure at " << file << ":" << line
            << "; exiting 0 now so teardown cannot swallow the verdict)" << std::endl;
  // _Exit, not exit: static destructors and atexit handlers ARE the teardown
  // that ate the verdict. _Exit does not flush, so flush both layers first.
  std::cout.flush();
  std::fflush(nullptr);
  std::_Exit(0);
}

template <class A, class B>
bool CheckEq(const A& a, const B& b, const char* exprA, const char* exprB, const char* file,
             int line) {
  ++CheckCount();
  if (!(a == b)) {
    std::ostringstream os;
    os << "expected (" << exprA << ") == (" << exprB << ")   got '" << a << "' vs '" << b << "'";
    ReportFailure(file, line, os.str());
    return false;
  }
  return true;
}

inline bool Check(bool cond, const char* expr, const char* file, int line) {
  ++CheckCount();
  if (!cond) ReportFailure(file, line, std::string("expected true: ") + expr);
  return cond;
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
            << (Registry().size() - static_cast<size_t>(failedCases)) << "/" << Registry().size()
            << " cases passed" << std::endl;
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
