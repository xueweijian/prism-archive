// C81/A3: CompressionRatioView -- THE single source of truth for every
// 压缩率/压缩比 surface (list cell, sidebar info card, status band).
//
// UI-AUDIT A3's defect: the same 15 B file rendered as "100%" (list row,
// clamped), "压缩比 126.7%" (card, permille) and "压缩率未知" (status band,
// never fed) in ONE frame. These tests pin the CONTRACT of the one function
// all three surfaces now call; Tests/win/test_c81_a2a3.cpp pins that the
// three surfaces actually render it.
//
// Pure PrismCore: compiles clean under g++ -Wall -Wextra -Wpedantic -Werror
// (CONTRACTS D31) on the Linux lane; no <windows.h>.
#include "CompressionRatio.hpp"
#include "ListLayout.hpp"
#include "mini_test.hpp"

#include <cstdint>
#include <string>

namespace {

using prism::list::SortItem;

PRISM_TEST("c81.ratio.unknown: either side zero -> 未知") {
  PRISM_CHECK_EQ(prism::CompressionRatioView(0, 0), std::string("未知"));
  PRISM_CHECK_EQ(prism::CompressionRatioView(0, 19), std::string("未知"));
  PRISM_CHECK_EQ(prism::CompressionRatioView(15, 0), std::string("未知"));
}

PRISM_TEST("c81.ratio.shrink: one decimal, half-up") {
  PRISM_CHECK_EQ(prism::CompressionRatioView(15, 15), std::string("100.0%"));
  PRISM_CHECK_EQ(prism::CompressionRatioView(10000, 4280), std::string("42.8%"));
  PRISM_CHECK_EQ(prism::CompressionRatioView(8, 1), std::string("12.5%"));
  PRISM_CHECK_EQ(prism::CompressionRatioView(3, 2), std::string("66.7%"));
  PRISM_CHECK_EQ(prism::CompressionRatioView(1000, 429), std::string("42.9%"));
}

PRISM_TEST("c81.ratio.growth: the real value survives after >100%") {
  // The audit's 15 B file: 19 packed bytes = 126.7%. Never "100%" again.
  PRISM_CHECK_EQ(prism::CompressionRatioView(15, 19), std::string(">100% (126.7%)"));
  PRISM_CHECK(prism::CompressionRatioView(15, 19).find(">100%") != std::string::npos);
  PRISM_CHECK_EQ(prism::CompressionRatioView(10, 20), std::string(">100% (200.0%)"));
  PRISM_CHECK_EQ(prism::CompressionRatioView(7, 8), std::string(">100% (114.3%)"));
}

PRISM_TEST("c81.ratio.pure: same bytes, same string (R2)") {
  for (int i = 0; i < 32; ++i) {
    PRISM_CHECK_EQ(prism::CompressionRatioView(15, 19),
                   prism::CompressionRatioView(15, 19));
  }
}

PRISM_TEST("c81.ratio.cell: the list cell calls the same function") {
  SortItem row;
  row.name = "one.txt";
  row.size = 15;
  row.packedSize = 19;
  PRISM_CHECK_EQ(prism::list::RatioCellText(row),
                 prism::CompressionRatioView(15, 19));
  // Expanded: the capsule bar must NOT render (the text-only honest form).
  PRISM_CHECK_EQ(prism::list::RatioBarVisible(row), false);
  row.packedSize = 6;
  PRISM_CHECK_EQ(prism::list::RatioBarVisible(row), true);
  row.isDir = true;
  PRISM_CHECK_EQ(prism::list::RatioBarVisible(row), false);
  PRISM_CHECK_EQ(prism::list::RatioCellText(row),
                 prism::CompressionRatioView(0, 0));  // a dir has no ratio
}

PRISM_TEST("c81.ratio.cell: capsule bar keeps its blueprint shape") {
  SortItem shrink;
  shrink.name = "docs.zip";
  shrink.size = 10000;
  shrink.packedSize = 4280;
  const auto bar = prism::list::RatioBar(shrink);
  PRISM_CHECK(bar.has_value());
  if (bar.has_value()) {
    PRISM_CHECK_EQ(bar->percent, static_cast<std::uint8_t>(42));
  }
}

}  // namespace

PRISM_TEST_MAIN()
