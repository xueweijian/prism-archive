// CompressionRatio.hpp -- C81/A3: THE single source of truth for every
// 压缩率/压缩比 surface (list cell, sidebar info card, status band).
//
// Before C81 the three surfaces computed three different numbers from three
// different formulas for the SAME archive (docs/UI-AUDIT.md A3: the row said
// "100%", the info card "压缩比 126.7%", the status band "压缩率未知" -- one
// 15-byte file, one frame). Every surface now renders the string THIS
// function returns for the same byte pair; nothing else formats a ratio.
//
// Rules (integer-exact, round-half-up at the one display decimal):
//   * originalBytes == 0 || compressedBytes == 0 -> "未知"
//     An empty file, a directory, or an archive whose engine answered
//     nothing has NO ratio; a fabricated "0.0%" or "100%" is a lie.
//   * compressedBytes >  originalBytes           -> ">100% (P.P%)"
//     The entry EXPANDED. The ">100%" marker is the honest direction and
//     the real value is preserved after it -- never clamped to 100%.
//   * compressedBytes == originalBytes           -> "100.0%"
//   * otherwise                                  -> "P.P%"
//
// Header-only PrismCore: no <windows.h>; self-contained includes (B2);
// pure function -- same inputs, same string, no state (R2).
#ifndef PRISM_COMPRESSION_RATIO_HPP_
#define PRISM_COMPRESSION_RATIO_HPP_

#include <cstdint>
#include <string>

namespace prism {

inline std::string CompressionRatioView(std::uint64_t originalBytes,
                                        std::uint64_t compressedBytes) {
  if (originalBytes == 0 || compressedBytes == 0) return "未知";
  // permille = round(compressed * 1000 / original), half up. Exact while
  // compressed * 1000 fits in 64 bits (archives under ~18 EB -- the display
  // never gets there); past that long double keeps the string honest.
  std::uint64_t permille;
  if (compressedBytes <= ~std::uint64_t{0} / 1000u) {
    permille = (compressedBytes * 1000u + originalBytes / 2u) / originalBytes;
  } else {
    const long double exact = static_cast<long double>(compressedBytes) *
                              1000.0L / static_cast<long double>(originalBytes);
    permille = static_cast<std::uint64_t>(exact + 0.5L);
  }
  std::string text;
  if (compressedBytes > originalBytes) text = ">100% (";
  text += std::to_string(permille / 10u);
  text += '.';
  text += static_cast<char>('0' + static_cast<int>(permille % 10u));
  text += '%';
  if (compressedBytes > originalBytes) text += ')';
  return text;
}

}  // namespace prism

#endif  // PRISM_COMPRESSION_RATIO_HPP_
