// PrismArchive -- PrismWin
// C109: numeric-column typography in ONE place so the renderer and the
// assertion measure the SAME thing.
//
// WHY A MONOSPACED FAMILY (and not just tnum):
//   BLUEPRINT.md requires OpenType tnum for size/ratio/offset columns. On the
//   shipping Segoe UI family the DIGITS are already tabular (1/8/0 all advance
//   7.008 px) and tnum changes nothing -- but '.' advances only 2.818 px.
//   Tabular figures equalize FIGURES, not punctuation, so "6.3 KB" and
//   "11.2 KB" still put their decimal point on different pixels. The card's
//   assertion is explicit: 1 / 8 / 0 / '.' must agree within 0.5 px. Only a
//   monospaced numeric column satisfies that (the brief allows a mono family).
//   Consolas is deterministic: it ships with every Windows since Vista and
//   every glyph advances 7.147 px at 13 px, so '.' is a fixed slot and the
//   right edge is exact. We still set tnum on the layout (design mandate);
//   the negative control removes both halves of the fix.
//
// PRISM_C109_MUTANT_PROPORTIONAL restores the shipped behaviour exactly:
// proportional Segoe UI with no typography, so 1/8/0/. diverge and the
// suite's advance assertion goes red.
#pragma once

#include <dwrite.h>

namespace prism::win::listtype {

#if defined(PRISM_C109_MUTANT_PROPORTIONAL)
inline constexpr bool kTabularNumbers = false;
inline constexpr const wchar_t* kNumericFamily = L"Segoe UI";
#else
inline constexpr bool kTabularNumbers = true;
inline constexpr const wchar_t* kNumericFamily = L"Consolas";
#endif

inline constexpr float kNumericFontSize = 13.0f;

// 13 px, single line, trailing alignment: a cell is [left, right] and its ink
// ends on the column baseline `right`.
inline HRESULT CreateNumericFormat(IDWriteFactory* factory, IDWriteTextFormat** out) {
  if (factory == nullptr || out == nullptr) return E_POINTER;
  *out = nullptr;
  HRESULT hr = factory->CreateTextFormat(
      kNumericFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, kNumericFontSize, L"en-US", out);
  if (FAILED(hr)) return hr;
  (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
  (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  return S_OK;
}

// OpenType tnum on the run. Best-effort: a face without the feature ignores it,
// and the family above is what actually guarantees equal advances.
inline void ApplyTabularFigures(IDWriteFactory* factory, IDWriteTextLayout* layout,
                                UINT32 length) {
#if defined(PRISM_C109_MUTANT_PROPORTIONAL)
  (void)factory;
  (void)layout;
  (void)length;
#else
  if (!kTabularNumbers || factory == nullptr || layout == nullptr || length == 0) return;
  IDWriteTypography* typography = nullptr;
  if (FAILED(factory->CreateTypography(&typography))) return;
  DWRITE_FONT_FEATURE feature{};
  feature.nameTag = DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES;
  feature.parameter = 1;
  if (SUCCEEDED(typography->AddFontFeature(feature))) {
    const DWRITE_TEXT_RANGE range{0, length};
    layout->SetTypography(typography, range);
  }
  typography->Release();
#endif
}

inline HRESULT CreateNumericLayout(IDWriteFactory* factory, IDWriteTextFormat* format,
                                   const wchar_t* text, UINT32 length, float maxWidth,
                                   float maxHeight, IDWriteTextLayout** out) {
  if (factory == nullptr || format == nullptr || text == nullptr || out == nullptr) {
    return E_POINTER;
  }
  *out = nullptr;
  HRESULT hr = factory->CreateTextLayout(text, length, format, maxWidth, maxHeight, out);
  if (FAILED(hr)) return hr;
  ApplyTabularFigures(factory, *out, length);
  return S_OK;
}

}  // namespace prism::win::listtype
