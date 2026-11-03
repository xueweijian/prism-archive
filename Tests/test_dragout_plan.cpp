// Card C67 -- the drag-out gate as DATA (no COM, no Explorer).
//
// The rule under test (one sentence): hovering is not a drop. A data object may
// answer "yes, I have CF_HDROP" as often as a target asks, and MUST NOT extract
// until a target actually asks for the payload -- and then only once.
//
// NEGATIVE CONTROL (D3/D27): PRISM_C67_MUTANT_EAGER_MATERIALIZE makes Offer()
// extract too. This binary then fails on "no extraction on hover" and prints
// "C67 MUTATION CAUGHT: eager_materialize" AT the first failure, so a test
// suite that stopped noticing the difference can never look green.
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../Src/PrismCore/DragOutPlan.hpp"
#include "mini_test.hpp"

namespace {

using prism::dragout::CheckPathList;
using prism::dragout::MaterializeGate;
using prism::dragout::Stage;

// D3 predicate: did the injected defect actually fire? Asked of a FRESH gate,
// so a red for an unrelated reason can never be reported as CAUGHT.
[[maybe_unused]] bool EagerInjectionFired() {
  int extractions = 0;
  MaterializeGate gate([&extractions]() {
    ++extractions;
    return true;
  });
  (void)gate.Offer();
  return extractions > 0;
}

}  // namespace

PRISM_TEST("dragout.hover_offers_without_extracting") {
  int extractions = 0;
  MaterializeGate gate([&extractions]() {
    ++extractions;
    return true;
  });
  // Explorer probes the object on DragEnter/DragOver: many times, never a drop.
  for (int i = 0; i < 5; ++i) PRISM_CHECK(gate.Offer());
  PRISM_CHECK_EQ(extractions, 0);              // the whole point of the card
  PRISM_CHECK_EQ(gate.extractions(), static_cast<std::size_t>(0));
  PRISM_CHECK(gate.stage() != Stage::Ready);
  PRISM_CHECK(!gate.ready());
}

PRISM_TEST("dragout.demand_extracts_exactly_once") {
  int extractions = 0;
  MaterializeGate gate([&extractions]() {
    ++extractions;
    return true;
  });
  PRISM_CHECK(gate.Offer());
  PRISM_CHECK(gate.Demand());
  PRISM_CHECK_EQ(extractions, 1);
  PRISM_CHECK(gate.ready());
  PRISM_CHECK_EQ(gate.stage(), Stage::Ready);
  // A target may ask twice (sizing pass, then the copy).
  PRISM_CHECK(gate.Demand());
  PRISM_CHECK_EQ(extractions, 1);
  PRISM_CHECK_EQ(gate.extractions(), static_cast<std::size_t>(1));
}

PRISM_TEST("dragout.failure_is_reported_not_hidden") {
  MaterializeGate gate([]() { return false; });
  PRISM_CHECK(!gate.Demand());
  PRISM_CHECK(!gate.ready());
  PRISM_CHECK_EQ(gate.stage(), Stage::Failed);
  // A gate with no extractor can still offer (it cannot know yet), but demanding
  // must fail instead of handing out an empty HDROP.
  MaterializeGate empty;
  PRISM_CHECK(!empty.Offer());  // cannot offer what it can never produce
  PRISM_CHECK(!empty.Demand());
  PRISM_CHECK_EQ(empty.stage(), Stage::Failed);
}

PRISM_TEST("dragout.temp_dir_names_are_unique_per_process_and_drag") {
  const std::wstring first = prism::dragout::TempDirLeaf(4242u, 1u);
  const std::wstring second = prism::dragout::TempDirLeaf(4242u, 2u);
  PRISM_CHECK(first != second);
  PRISM_CHECK(first.find(L"4242") != std::wstring::npos);
  PRISM_CHECK(first.rfind(L"prism_dragout_p", 0) == 0);
}

PRISM_TEST("dragout.path_list_must_be_absolute") {
  const auto ok = CheckPathList({L"C:\\temp\\a.txt", L"D:\\x\\b.txt"});
  PRISM_CHECK(ok.ok());
  PRISM_CHECK_EQ(ok.count, static_cast<std::size_t>(2));
  const auto relative = CheckPathList({L"a.txt", L"C:\\temp\\b.txt"});
  PRISM_CHECK(!relative.ok());
  PRISM_CHECK_EQ(relative.relative, static_cast<std::size_t>(1));
  const auto empty = CheckPathList({L"", L"C:\\temp\\b.txt"});
  PRISM_CHECK(!empty.ok());
  PRISM_CHECK_EQ(empty.empty, static_cast<std::size_t>(1));
  const auto none = CheckPathList({});
  PRISM_CHECK(!none.ok());  // an HDROP with no entries is a failure, not a copy
}

int main(int argc, char** argv) {
#if defined(PRISM_C67_MUTANT_EAGER_MATERIALIZE)
  // D28: the banner sits next to the guard the mutation matrix looks for.
  std::cout << "C67 MUTANT ACTIVE: hovering materialises the drag payload" << std::endl;
  prism::test::SetMutationVerdict("C67 MUTATION CAUGHT: eager_materialize",
                                  [] { return EagerInjectionFired(); });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL DRAG-OUT-GATE CHECKS PASSED" << std::endl;
  return 0;
}
