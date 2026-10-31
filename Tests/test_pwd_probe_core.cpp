// PrismCore unit tests for the password probe slice (batch B3):
//   * BuildDiagnostics -- exact message format, both found/miss shapes,
//     per-source breakdown, redaction (content-level, CONTRACTS D2);
//   * the Probe executor -- because the header is COM-free by contract, its
//     MUTATION SELF-CERTIFICATION also runs here on Linux (CONTRACTS D3/D5):
//     mutation #1 removes the RedactSecrets filter on untrusted exception
//     text, mutation #2 removes the stop-on-hit break; each must turn a
//     pinned assertion in THIS file red, verified in the PR report.
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "PasswordRing.hpp"  // the executor lives here (COM-free)
#include "PasswordRing.hpp"
#include "mini_test.hpp"

namespace {

using prism::pwd::Candidate;
using prism::pwd::Source;
using AttemptPlan = prism::pwd::PasswordRing::AttemptPlan;
using prism::pwd::Probe;

Candidate C(const std::string& text, Source s) {
  Candidate c;
  c.text = text;
  c.source = s;
  return c;
}

// 12 candidates, mixed sources: 5 filename hints, 3 vault, 4 user input.
AttemptPlan MixedPlan(int concurrency) {
  AttemptPlan plan;
  plan.concurrency = concurrency;
  for (int i = 0; i < 5; ++i)
    plan.ordered.push_back(C("hint_pkg_" + std::to_string(i), Source::FilenameHint));
  for (int i = 0; i < 3; ++i)
    plan.ordered.push_back(C("vault_pw_" + std::to_string(i), Source::VaultHistory));
  for (int i = 0; i < 4; ++i)
    plan.ordered.push_back(C("typed_" + std::to_string(i), Source::UserInput));
  return plan;
}

bool ContainsAny(const std::string& haystack, const std::vector<std::string>& needles) {
  for (const std::string& n : needles) {
    if (haystack.find(n) != std::string::npos) return true;
  }
  return false;
}

std::vector<std::string> AllCandidates(const AttemptPlan& plan) {
  std::vector<std::string> out;
  for (const Candidate& c : plan.ordered) out.push_back(c.text);
  return out;
}

// ---- BuildDiagnostics (pure decision core) --------------------------------

PRISM_TEST("diag.miss_format_pins_source_breakdown") {
  const AttemptPlan plan = MixedPlan(1);
  // Prefix of 5 = the five filename hints: the breakdown names sources in
  // ordinal order and omits zero counts. Byte-exact on purpose (CONTRACTS C4
  // spirit: deterministic output, or failure reports mean nothing).
  PRISM_CHECK_EQ(prism::pwd::BuildDiagnostics(plan, 5, false, 1234).diagnostics,
                 std::string("已尝试 5/12 个候选（来源：文件名嗅探 5），"
                             "未命中（用时 1234 ms）"));
}

PRISM_TEST("diag.hit_format_differs_from_miss") {
  const AttemptPlan plan = MixedPlan(1);
  const std::string hit =
      prism::pwd::BuildDiagnostics(plan, 4, true, 88).diagnostics;
  PRISM_CHECK_EQ(hit, std::string("已尝试 4/12 个候选（来源：文件名嗅探 4），"
                                  "命中（用时 88 ms）"));
  // The two shapes must be mutually exclusive: a hit report never says
  // "未命中" (that is the kind of contradiction a UI cannot survive).
  PRISM_CHECK(hit.find("未命中") == std::string::npos);
  const std::string miss =
      prism::pwd::BuildDiagnostics(plan, 12, false, 88).diagnostics;
  PRISM_CHECK(miss.find("命中") != std::string::npos);  // contains 未命中
  PRISM_CHECK_EQ(miss, std::string("已尝试 12/12 个候选（来源：文件名嗅探 5、"
                                   "密码库 3、用户输入 4），未命中（用时 88 ms）"));
}

PRISM_TEST("diag_redaction_no_candidate_text_nor_long_fragments") {
  AttemptPlan plan;
  plan.concurrency = 1;
  plan.ordered.push_back(C("S3cr3t_Zone_42", Source::FilenameHint));
  plan.ordered.push_back(C("hunter2024!", Source::VaultHistory));
  plan.ordered.push_back(C("pin#998877aa", Source::UserInput));
  // attemptsMade larger than the plan must clamp, not read out of bounds.
  const std::string diag =
      prism::pwd::BuildDiagnostics(plan, 99, false, 3).diagnostics;
  // Content-level (D2): full candidates AND >=4-char fragments are asserted
  // absent, because RedactSecrets promises partial-leak protection.
  PRISM_CHECK(!ContainsAny(diag, {"S3cr3t_Zone_42", "hunter2024!", "pin#998877aa"}));
  PRISM_CHECK(!ContainsAny(diag, {"S3cr", "3t_Z", "unte", "r2024", "9988", "#998"}));
  PRISM_CHECK_EQ(diag, std::string("已尝试 3/3 个候选（来源：文件名嗅探 1、密码库 1、"
                                   "用户输入 1），未命中（用时 3 ms）"));
}

PRISM_TEST("diag_display_order_is_source_ordinal_not_plan_order") {
  AttemptPlan plan;
  plan.concurrency = 1;
  plan.ordered.push_back(C("typed_0", Source::UserInput));
  plan.ordered.push_back(C("hint_0", Source::FilenameHint));
  const std::string diag =
      prism::pwd::BuildDiagnostics(plan, 2, false, 0).diagnostics;
  // FilenameHint (ordinal 0) is displayed first even though the plan lists
  // the user input first; untouched sources never appear.
  PRISM_CHECK_EQ(diag, std::string("已尝试 2/2 个候选（来源：文件名嗅探 1、用户输入 1），"
                                   "未命中（用时 0 ms）"));
  PRISM_CHECK(!ContainsAny(diag, {"密码库", "压缩包注释", "旁车文本"}));
}

PRISM_TEST("diag_concurrent_plan_consistent_and_empty_plan_explicit") {
  const AttemptPlan wide = MixedPlan(4);
  // A concurrent executor only reports the completion COUNT; the pure
  // formatter stays byte-identical to the sequential shape.
  PRISM_CHECK_EQ(prism::pwd::BuildDiagnostics(wide, 12, false, 7).diagnostics,
                 std::string("已尝试 12/12 个候选（来源：文件名嗅探 5、密码库 3、"
                             "用户输入 4），未命中（用时 7 ms）"));
  const AttemptPlan none;
  PRISM_CHECK_EQ(prism::pwd::BuildDiagnostics(none, 0, false, 0).diagnostics,
                 std::string("无可用候选密码，未执行探测（用时 0 ms）"));
}

// ---- Probe executor (COM-free; mutation vehicle) ---------------------------

PRISM_TEST("probe_sequential_stops_on_hit_counts_and_progress") {
  AttemptPlan plan;
  plan.concurrency = 1;
  const char* kWords[] = {"wrong01", "wrong02", "wrong03", "Right04",
                          "wrong05", "wrong06", "wrong07", "wrong08"};
  for (const char* w : kWords) plan.ordered.push_back(C(w, Source::UserInput));

  std::vector<int> done;
  const prism::pwd::ProbeOutcome out =
      Probe(plan, [](const std::string& pw) { return pw == "Right04"; },
            [&](int d, int) { done.push_back(d); });

  PRISM_CHECK(out.found);
  PRISM_CHECK_EQ(out.password, std::string("Right04"));
  PRISM_CHECK_EQ(out.attemptsMade, 4);  // 3 misses + the hit; 5.. never tried
  PRISM_CHECK_EQ(done.size(), size_t(4));
  for (size_t i = 0; i < done.size(); ++i) PRISM_CHECK_EQ(done[i], static_cast<int>(i) + 1);
  PRISM_CHECK(out.elapsedMs >= 0);
  PRISM_CHECK(out.diagnostics.find("4/8") != std::string::npos);
  // MUTATION #2 VEHICLE: keep-attempting-after-hit makes attemptsMade 8.
}

PRISM_TEST("probe_exhaustion_reports_every_candidate_without_leak") {
  AttemptPlan plan;
  plan.concurrency = 1;
  for (int i = 1; i <= 12; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "bad%06d", i);
    plan.ordered.push_back(C(buf, Source::UserInput));
  }
  int events = 0;
  const prism::pwd::ProbeOutcome out =
      Probe(plan, [](const std::string&) { return false; },
            [&events](int d, int total) {
              if (d <= total) ++events;
            });
  PRISM_CHECK(!out.found);
  PRISM_CHECK(out.password.empty());
  PRISM_CHECK_EQ(out.attemptsMade, 12);  // AES silent budget, all consumed
  PRISM_CHECK_EQ(events, 12);
  PRISM_CHECK(out.diagnostics.find("12/12") != std::string::npos);
  PRISM_CHECK(!ContainsAny(out.diagnostics, AllCandidates(plan)));
}

PRISM_TEST("probe_concurrent_pool_counts_every_candidate_once") {
  const AttemptPlan plan = MixedPlan(4);
  std::mutex lock;
  std::vector<std::string> tried;
  const prism::pwd::ProbeOutcome out =
      Probe(plan,
            [&](const std::string& pw) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
              std::lock_guard<std::mutex> g(lock);
              tried.push_back(pw);
              return false;
            },
            {});
  PRISM_CHECK(!out.found);
  PRISM_CHECK_EQ(out.attemptsMade, 12);
  PRISM_CHECK_EQ(tried.size(), size_t(12));
  // Each candidate exactly once: a candidate tried twice means double-counted
  // progress, one never tried means the ring has a hole.
  for (const std::string& c : AllCandidates(plan)) {
    int seen = 0;
    for (const std::string& t : tried) {
      if (t == c) ++seen;
    }
    PRISM_CHECK_EQ(seen, 1);
  }
  PRISM_CHECK(!ContainsAny(out.diagnostics, AllCandidates(plan)));
}

PRISM_TEST("probe_redacts_untrusted_exception_text") {
  AttemptPlan plan;
  plan.concurrency = 2;
  plan.ordered.push_back(C("alpha_secret_11", Source::UserInput));
  plan.ordered.push_back(C("V3rySecret_LongPin_9137", Source::UserInput));
  plan.ordered.push_back(C("gamma_secret_33", Source::UserInput));
  plan.ordered.push_back(C("delta_secret_44", Source::UserInput));
  // Every candidate's failure text embeds the candidate itself -- the shape
  // real 7z callback errors take. MUTATION #1 VEHICLE: dropping the
  // RedactSecrets call around failureNotes makes this exact case red.
  const prism::pwd::ProbeOutcome out =
      Probe(plan,
            [](const std::string& pw) {
              throw std::runtime_error("CryptoGetTextPassword rejected <" + pw + ">");
              return false;
            },
            {});
  PRISM_CHECK(!out.found);
  PRISM_CHECK_EQ(out.attemptsMade, 4);
  PRISM_CHECK(out.diagnostics.find("探测异常") != std::string::npos);
  PRISM_CHECK(out.diagnostics.find("[redacted]") != std::string::npos);
  PRISM_CHECK(!ContainsAny(out.diagnostics, AllCandidates(plan)));
  PRISM_CHECK(!ContainsAny(out.diagnostics,
                           {"V3rySecret", "LongPin", "alpha_secret", "gamma_secret",
                            "delta_secret", "9137"}));
}

PRISM_TEST("probe_concurrent_hit_terminates_and_reports_monotonic") {
  AttemptPlan plan;
  plan.concurrency = 4;
  const char* kWords[] = {"slow01", "slow02", "slow03", "slow04", "Right05",
                          "slow06", "slow07", "slow08", "slow09", "slow10",
                          "slow11", "slow12", "slow13", "slow14", "slow15",
                          "slow16"};
  for (const char* w : kWords) plan.ordered.push_back(C(w, Source::UserInput));

  std::mutex lock;
  int lastDone = 0;
  const prism::pwd::ProbeOutcome out =
      Probe(plan,
            [](const std::string& pw) {
              if (pw == "Right05") return true;  // instant hit
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
              return false;
            },
            [&](int done, int total) {
              std::lock_guard<std::mutex> g(lock);
              // Monotonicity is the contract an observable counter must keep
              // even from a concurrent pool.
              if (done <= lastDone || done > total) PRISM_CHECK(false);
              lastDone = done;
            });
  PRISM_CHECK(out.found);
  PRISM_CHECK_EQ(out.password, std::string("Right05"));
  // The hit stops NEW attempts; in-flight workers finish what they already
  // claimed, so the count sits between the hit position and the full sweep.
  PRISM_CHECK(out.attemptsMade >= 5);
  PRISM_CHECK(out.attemptsMade <= 16);
  PRISM_CHECK_EQ(lastDone, out.attemptsMade);
  PRISM_CHECK(!ContainsAny(out.diagnostics, AllCandidates(plan)));
}

}  // namespace

PRISM_TEST_MAIN()
