// PrismArchive -- PrismWin
// ActionBar: the PAINTER of the action band (DESIGN.md §2/§3/§4).
//
// The model owns the table, the rules and the geometry (PrismCore's
// ActionBarModel.hpp); this file owns the last mile, and nothing else:
//
//   * the island is the C15 surface token, NOT a coloured ribbon: hover lifts
//     the local fill by a token-derived white wash (+4%), it never turns blue --
//     Bandizip's blue toolbar is exactly what the blueprint forbids copying;
//   * C78: the band has a GROUP RHYTHM. Four groups (Archive / Modify /
//     Inspect / Find) are separated by a 1 px token-white seam at alpha 0.08;
//     the gap across a seam is 12 px, inside a group 4 px;
//   * every corner is a SQUIRCLE (theme::SquircleField, n=4.2, radius 20 for
//     the island and 10 for a button), not a circular arc;
//   * pressing sinks the CONTENT by one pixel (the fill stays put), so the
//     click reads as a physical press instead of a colour flash;
//   * C78: a dropdown action shows a small chevron under its label, and ONLY
//     while it is hovered, pressed or open -- no permanent menu affordance;
//   * a disabled action is painted at 38% of its ink -- dim enough to read as
//     unavailable, visible enough to teach where it will appear. Its LABEL is
//     TextSecondary, so the word stays below 3:1 contrast (a "disabled" word
//     that still passes AA is a lie about being clickable);
//   * icons are vector glyphs drawn from scratch (no Bandizip pixels, no
//     bitmap pipeline): eight silhouettes, one recolourable brush;
//   * the dropdown panel is the same glass, hanging below its button.
//
// The counters at the bottom are the negative control's evidence (CONTRACTS
// D3): PRISM_V2_MUTANT_NO_ACTION_PAINT paints nothing at all, so the suite's
// "the band reached pixels" assertions MUST go red.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "../PrismCore/ActionBarModel.hpp"
#include "../PrismCore/I18n.hpp"  // C74: the eight band keys live in the tables
#include "../PrismCore/ThemeTokens.hpp"

namespace prism::win::actionbar {

using Microsoft::WRL::ComPtr;

// C74: the Win layer resolves the band's KEYS against the active table. A
// missing key (or a null translator) keeps the model's built-in word, so a
// table-less build still paints labels instead of empty buttons. The size is
// kActionCount by construction, which is exactly what Model::SetLabels accepts.
inline std::array<std::string, static_cast<std::size_t>(prism::actionbar::kActionCount)>
LabelsFromTranslator(const prism::i18n::Translator* translator) {
  std::array<std::string, static_cast<std::size_t>(prism::actionbar::kActionCount)> labels;
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    labels[static_cast<std::size_t>(i)] = prism::actionbar::DefaultLabels()[static_cast<std::size_t>(i)];
  }
  if (translator == nullptr) return labels;
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    const std::string_view key = prism::actionbar::kActions[static_cast<std::size_t>(i)].key;
    if (translator->Has(key)) {
      labels[static_cast<std::size_t>(i)] = translator->Translate(key);
    }
  }
  return labels;
}

#if defined(PRISM_V2_MUTANT_NO_ACTION_PAINT)
inline constexpr bool kPaintActions = false;
#else
inline constexpr bool kPaintActions = true;
#endif

inline constexpr std::string_view PaintMutation() {
  return kPaintActions ? std::string_view{} : std::string_view{"no-action-paint"};
}

class ActionBarPainter {
 public:
  // The disabled ink ratio. One number, used by every primitive, so "disabled
  // looks disabled" cannot drift between the icon and its label. C78 moved it
  // 0.40 -> 0.38 (two-fifths of the ink, just under) and keeps the label
  // on TextSecondary; the contrast assertion measures < 3:1 on the DEVICE.
#if defined(PRISM_C78_MUTANT_DISABLED_BRIGHT)
  static constexpr float kDisabledAlpha = 0.70f;
#else
  static constexpr float kDisabledAlpha = 0.38f;
#endif
  // C78: focus ring geometry. 2 px of the accent token, inset 2 px so it never
  // touches the squircle edge or clips the icon.
  static constexpr float kFocusRingWidth = 2.0f;
  static constexpr float kFocusRingInset = 2.0f;
  static constexpr float kFocusRingRadius = 10.0f;

  HRESULT CreateDeviceResources(ID2D1RenderTarget* target) {
    if (!writeFactory_) {
      HRESULT hr = DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(writeFactory_.ReleaseAndGetAddressOf()));
      if (FAILED(hr)) return hr;
    }
    if (!brush_) {
      HRESULT hr = target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                                 brush_.ReleaseAndGetAddressOf());
      if (FAILED(hr)) return hr;
    }
    if (!labelFormat_) {
      writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      11.0f, L"en-US", labelFormat_.ReleaseAndGetAddressOf());
      if (labelFormat_) {
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // V4: a label that does not fit ends in "…", never mid-glyph. The sign
        // and the trimming options are DWrite state, so the suite can QUERY them
        // back (LabelEllipsisConfigured) instead of trusting this comment.
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(
                labelFormat_.Get(), sign.ReleaseAndGetAddressOf()))) {
          labelFormat_->SetTrimming(&trimming, sign.Get());
        }
      }
    }
    if (!mnemonicFormat_) {
      writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      10.0f, L"en-US", mnemonicFormat_.ReleaseAndGetAddressOf());
      if (mnemonicFormat_) {
        mnemonicFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        mnemonicFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        mnemonicFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      }
    }
    return S_OK;
  }

  // D3: whether the painter drops the last button (true only in the mutant
  // build, so the verdict's predicate can prove the injection compiled in).
  static constexpr bool PaintSkipsLastButton() {
#if defined(PRISM_C67_MUTANT_SKIP_LAST_PAINT)
    return true;
#else
    return false;
#endif
  }

  // hoverIndex / pressedIndex are button indices (-1 == nothing under/held).
  // They are PAINT inputs only: which button is legal is the model's decision.
  void Draw(ID2D1RenderTarget* target, const prism::actionbar::Layout& layout,
            const prism::actionbar::Model& model, int hoverIndex, int pressedIndex,
            prism::theme::Variant variant) {
    if (!kPaintActions) return;
    if (!brush_ || !target) return;
    if (layout.island.Empty() || layout.buttons.empty()) return;

    const D2D1_SIZE_F size = target->GetSize();
    lastVariant_ = variant;

    // 1. the island: floating glass, specular hairline, squircle corners.
    const D2D1_RECT_F island = RectF(layout.island);
    const D2D1_COLOR_F islandFill = IslandFill(variant);
    fillSquircle(target, island, 20.0f, islandFill);
    SetBrush(BorderColor(variant));
    // The hairline is drawn INSIDE the island (path deflated by half a stroke):
    // a centred 1 px stroke would spill half a pixel above the band and break
    // "everything lives inside layout.action" for one antialiased row.
    strokeSquircle(target, D2D1::RectF(island.left + 0.5f, island.top + 0.5f,
                                       island.right - 0.5f, island.bottom - 0.5f),
                   19.5f, 1.0f);
    ++dbgIslands;

    // 2. C78: the Group seams, painted on the island BEFORE any button so a
    // hover wash can never cover a hairline (the seam belongs to the band, not
    // to the button under the pointer).
    for (const prism::actionbar::GroupSeparator& separator : layout.separators) {
#if defined(PRISM_C78_MUTANT_NO_GROUP_SEPARATOR)
      (void)separator;
      break;
#else
      fillRect(target, RectF(separator.rect), SeparatorColor(variant));
      ++dbgSeparators;
#endif
    }

    // 3. per-button state fill + focus ring, then the content.
    for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
#if defined(PRISM_C67_MUTANT_SKIP_LAST_PAINT)
      // MUTANT ACTIVE (card C67, arm skip_last_paint): the last button is
      // never painted, i.e. the "geometry says eight, the screen shows seven"
      // half-band the C66 rule forbids. Banner + verdict live in
      // Tests/win/test_c67_gestures.cpp next to the same macro.
      if (i + 1 == layout.buttons.size()) break;
#endif
      const prism::actionbar::Button& button = layout.buttons[i];
      const int index = static_cast<int>(i);
      const bool enabled = model.Enabled(button.id);
      const bool hovered = enabled && index == hoverIndex;
      const bool pressed = enabled && index == pressedIndex;
      const D2D1_RECT_F rect = RectF(button.rect);

      if (hovered || pressed) {
        // The lift: a token-white wash INSIDE the button, never a hue change.
        // C78: a ~4% wash. A literal 4% measures 9 luminance levels over the
        // Midnight island (above the 3-8 band this card pins), so the shipping
        // value is 3.3% = 7.0 levels; the assertion measures the rendered delta.
#if defined(PRISM_C78_MUTANT_STRONG_HOVER)
        const float alpha = pressed ? 0.24f : 0.18f;
#else
        const float alpha = pressed ? 0.07f : 0.033f;
#endif
        fillSquircle(target, D2D1::RectF(rect.left + 2.0f, rect.top + 2.0f,
                                         rect.right - 2.0f, rect.bottom - 2.0f),
                     10.0f, Wash(variant, alpha));
        // Depth: one specular hairline along the TOP inner edge of the raised
        // face (token white at 10%). Without it the hover fill is a flat grey
        // rectangle; with it the button reads as a small glass plate.
        SetBrush(RimHighlight(variant));
        target->DrawLine(D2D1::Point2F(rect.left + 12.0f, rect.top + 3.5f),
                         D2D1::Point2F(rect.right - 12.0f, rect.top + 3.5f), brush_.Get(),
                         1.0f);
        ++dbgHighlights;
        if (pressed) ++dbgPressed;
        if (hovered) ++dbgHover;
      }
      ++dbgButtons;

      if (model.FocusIndex() == index) {
#if !defined(PRISM_C78_MUTANT_NO_FOCUS_RING)
        // C78: 2 px accent ring. Drawn even when the button is disabled -- the
        // ring says "the keyboard is here", not "this will run".
        SetBrush(Accent(variant, 0.90f));
        strokeSquircle(target,
                       D2D1::RectF(rect.left + kFocusRingInset, rect.top + kFocusRingInset,
                                   rect.right - kFocusRingInset, rect.bottom - kFocusRingInset),
                       kFocusRingRadius, kFocusRingWidth);
        ++dbgFocusRings;
#endif
      }

      // Pressed: content sinks 1 px; the island and the fill do not move.
      const float dy = pressed ? 1.0f : 0.0f;
      // C78: the chevron is part of the CONTENT, so it sinks with the glyph and
      // the word on press. It is shown only for a menu action that is hovered,
      // held, or has its panel open -- there is no permanent arrow.
      const bool arrow = buttonHasDropdown(button.id) &&
                         (hovered || pressed ||
                          (model.DropdownOpen() && model.DropdownOwner() == button.id));
      PaintButtonContent(target, button, model, enabled, dy, arrow);
      if (!enabled) ++dbgDisabled;
    }

    // 4. the dropdown hangs BELOW the band and above the content.
    if (model.DropdownOpen() && enabledDropdown(model, layout, hoverIndex)) {
      const int ownerIndex = OwnerIndex(layout, model.DropdownOwner());
      if (ownerIndex >= 0) DrawDropdown(target, layout, model, ownerIndex, variant, size);
    }
  }

  // Debug counters (the negative control's evidence; see the header comment).
  int dbgIslands = 0;
  int dbgButtons = 0;
  int dbgIcons = 0;
  int dbgLabels = 0;
  int dbgDisabled = 0;
  int dbgHover = 0;
  int dbgPressed = 0;
  int dbgFocusRings = 0;
  int dbgSeparators = 0;
  int dbgArrows = 0;
  int dbgHighlights = 0;
  int dbgDropdowns = 0;
  int dbgDropdownRows = 0;
  // V4: labels that needed the ellipsis. MUST be 0 for the shipping table (the
  // assertion is "no truncated label"), and non-zero only in a squeeze.
  int dbgLabelsClipped = 0;

  // V4: proves the ellipsis is wired into the text format (D3-style: query the
  // device state, do not trust a comment). Requires CreateDeviceResources.
  bool LabelEllipsisConfigured() const {
    if (!labelFormat_) return false;
    DWRITE_TRIMMING trimming{};
    IDWriteInlineObject* sign = nullptr;
    const HRESULT hr = labelFormat_->GetTrimming(&trimming, &sign);
    bool ok = false;
    if (SUCCEEDED(hr)) {
      // CHARACTER granularity + a real ellipsis sign object: the only way a
      // too-long label can be cut is with "…" at the end.
      ok = trimming.granularity == DWRITE_TRIMMING_GRANULARITY_CHARACTER &&
           prism::actionbar::kLabelEllipsis == 0x2026 && sign != nullptr;
    }
    if (sign != nullptr) sign->Release();
    return ok;
  }

 private:
  static D2D1_RECT_F RectF(const prism::actionbar::Rect& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                       static_cast<float>(r.right), static_cast<float>(r.bottom));
  }

  void SetBrush(const D2D1_COLOR_F& color) { brush_->SetColor(color); }

  // ---- tokens (C15). Never a raw literal: a second colour source is exactly
  // how the band would drift away from the rest of the glass.
  static prism::theme::Rgba Token(prism::theme::TokenId id,
                                  prism::theme::Variant variant) {
    return prism::theme::Lookup(id, variant).color;
  }
  static D2D1_COLOR_F ToD2d(prism::theme::Rgba c, float alphaScale = 1.0f) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a * alphaScale);
  }
  static D2D1_COLOR_F IslandFill(prism::theme::Variant variant) {
    // C64: the TOKEN's own alpha (.52 Midnight / .58 Daylight). This used to
    // override it with .72, which is why the island looked like a lit gray
    // ribbon against the canvas instead of glass. A pixel assertion in the C64
    // suite compares this fill with CompositeOver(SurfaceFloatingIsland, canvas).
    // C71: and the token's alpha now carries appearance.glassIntensity, through
    // the same EffectiveGlassSurface() helper the shader and the content painter use.
    return ToD2d(prism::theme::EffectiveGlassSurface(variant));
  }
  static D2D1_COLOR_F BorderColor(prism::theme::Variant variant) {
    prism::theme::Rgba c =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant).gradient.from;
    c.a = 0.16f;
    return ToD2d(c);
  }
  static D2D1_COLOR_F Accent(prism::theme::Variant variant, float alpha) {
    // C71: the focus ring follows appearance.accentColor when the user saved
    // one; otherwise this is the theme's --accent-liquid-primary token.
    prism::theme::Rgba c = prism::theme::EffectiveAccent(variant);
    c.a = alpha;
    return ToD2d(c);
  }
  // C78: the seam is the specular-rim token at a measured 8%. Using the token
  // (not ColorF(1,1,1)) keeps HighContrast honest: a theme that redefines the
  // rim redefines the seam.
  static D2D1_COLOR_F SeparatorColor(prism::theme::Variant variant) {
    prism::theme::Rgba c =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant).gradient.from;
    c.a = prism::actionbar::kGroupSeparatorAlpha;
    return ToD2d(c);
  }
  // C78: the 1 px top rim of a raised face, from the same specular token.
  static D2D1_COLOR_F RimHighlight(prism::theme::Variant variant) {
    prism::theme::Rgba c =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant).gradient.from;
    c.a = 0.10f;
    return ToD2d(c);
  }
  // The hover/press wash is achromatic on purpose (blue ribbon forbidden) and
  // token-derived: the specular rim is white in all three shipping variants.
  static D2D1_COLOR_F Wash(prism::theme::Variant variant, float alpha) {
    prism::theme::Rgba c =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant).gradient.from;
    c.a = alpha;
    return ToD2d(c);
  }

  // ---- squircle geometry (theme::SquircleField owns the exponent)
  HRESULT CreateSquirclePath(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                             ID2D1PathGeometry** out) {
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.ReleaseAndGetAddressOf());
    ComPtr<ID2D1PathGeometry> path;
    HRESULT hr = factory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1GeometrySink> sink;
    hr = path->Open(sink.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (!(w > 0.0f) || !(h > 0.0f)) {
      sink->Close();
      return E_INVALIDARG;
    }
    const float r = (std::max)(0.0f, (std::min)(radius, (std::min)(w, h) * 0.5f));
    constexpr int kCornerSegments = 6;
    constexpr double kPi = 3.14159265358979323846;
    const auto cornerPoint = [&](float cx, float cy, float dx, float dy, double t) {
      const double ct = std::cos(t);
      const double st = std::sin(t);
      double lo = 0.0, hi = 1.6;
      for (int i = 0; i < 20 && r > 0.0f; ++i) {
        const double mid = (lo + hi) * 0.5;
        if (prism::theme::SquircleField(ct * mid * r, st * mid * r, r, r) <= 0.0) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      const double s = r > 0.0f ? (lo + hi) * 0.5 : 0.0;
      return D2D1::Point2F(cx + dx * static_cast<float>(ct * s * r),
                           cy + dy * static_cast<float>(st * s * r));
    };
    const float left = rect.left, top = rect.top, right = rect.right, bottom = rect.bottom;
    sink->BeginFigure(D2D1::Point2F(left + r, top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(right - r, top));
    for (int i = 1; i <= kCornerSegments; ++i) {
      const double t = (kPi * 0.5) * (1.0 - static_cast<double>(i) / kCornerSegments);
      sink->AddLine(cornerPoint(right - r, top + r, 1.0f, -1.0f, t));
    }
    sink->AddLine(D2D1::Point2F(right, bottom - r));
    for (int i = 1; i <= kCornerSegments; ++i) {
      const double t = (kPi * 0.5) * (static_cast<double>(i) / kCornerSegments);
      sink->AddLine(cornerPoint(right - r, bottom - r, 1.0f, 1.0f, t));
    }
    sink->AddLine(D2D1::Point2F(left + r, bottom));
    for (int i = 1; i <= kCornerSegments; ++i) {
      const double t = (kPi * 0.5) * (1.0 - static_cast<double>(i) / kCornerSegments);
      sink->AddLine(cornerPoint(left + r, bottom - r, -1.0f, 1.0f, t));
    }
    sink->AddLine(D2D1::Point2F(left, top + r));
    for (int i = 1; i <= kCornerSegments; ++i) {
      const double t = (kPi * 0.5) * (static_cast<double>(i) / kCornerSegments);
      sink->AddLine(cornerPoint(left + r, top + r, -1.0f, -1.0f, t));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    if (FAILED(hr)) return hr;
    *out = path.Detach();
    return S_OK;
  }

  // A crisp 1 px column: FillRectangle on integer bounds, no antialiasing, so
  // the rendered alpha is exactly the token alpha (the pixel assertion recovers
  // it as (seam - fill) / (white - fill)).
  void fillRect(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                const D2D1_COLOR_F& color) {
    SetBrush(color);
    target->FillRectangle(rect, brush_.Get());
  }

  void fillSquircle(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                    const D2D1_COLOR_F& color) {
    ComPtr<ID2D1PathGeometry> geometry;
    SetBrush(color);
    if (FAILED(CreateSquirclePath(target, rect, radius,
                                  geometry.ReleaseAndGetAddressOf()))) {
      target->FillRectangle(rect, brush_.Get());
      return;
    }
    target->FillGeometry(geometry.Get(), brush_.Get());
  }

  void strokeSquircle(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                      float width) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(CreateSquirclePath(target, rect, radius,
                                  geometry.ReleaseAndGetAddressOf()))) {
      target->DrawRectangle(rect, brush_.Get(), width);
      return;
    }
    target->DrawGeometry(geometry.Get(), brush_.Get(), width);
  }

  // Does the label need more than its box? Measured with the SAME format the
  // painter uses, so a shipping string that would be ellipsized is counted
  // (dbgLabelsClipped) instead of silently truncated.
  bool LabelOverflows(const std::wstring& label, int boxWidth) {
    if (!labelFormat_ || !writeFactory_ || boxWidth <= 0) return false;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(writeFactory_->CreateTextLayout(
            label.c_str(), static_cast<UINT32>(label.size()), labelFormat_.Get(),
            static_cast<float>(boxWidth), static_cast<float>(prism::actionbar::kLabelHeight),
            layout.ReleaseAndGetAddressOf()))) {
      return false;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return false;
    return metrics.widthIncludingTrailingWhitespace >
           static_cast<float>(boxWidth) + 0.5f;
  }

  static bool buttonHasDropdown(prism::actionbar::ActionId id) {
    const int index = prism::actionbar::IndexOf(id);
    return index >= 0 &&
           prism::actionbar::SpecOf(index).hasDropdown;
  }

  // ---- one button's icon + label + mnemonic + chevron, all offset by dy.
  void PaintButtonContent(ID2D1RenderTarget* target, const prism::actionbar::Button& button,
                          const prism::actionbar::Model& model, bool enabled, float dy,
                          bool arrow) {
    const prism::actionbar::Spec& spec = prism::actionbar::SpecOf(
        (std::max)(0, prism::actionbar::IndexOf(button.id)));
    const float alpha = enabled ? 1.0f : kDisabledAlpha;
    const D2D1_RECT_F r = RectF(button.rect);
    // V4: both boxes come from the MODEL, and the press sink moves both by the
    // SAME dy -- moving one would reopen the misalignment the screenshot showed.
    const prism::actionbar::Rect iconRect(prism::actionbar::IconBoxRect(button.rect));
    const prism::actionbar::Rect labelRect(prism::actionbar::LabelBoxRect(button.rect));
    const D2D1_RECT_F iconBox =
        D2D1::RectF(static_cast<float>(iconRect.left), static_cast<float>(iconRect.top) + dy,
                    static_cast<float>(iconRect.right),
                    static_cast<float>(iconRect.bottom) + dy);
    SetBrush(ToD2d(Token(prism::theme::TokenId::TextPrimary, lastVariant_), alpha));
    PaintGlyph(target, spec.icon, iconBox);
    ++dbgIcons;

    // C74: the word comes from the caller-injected table (or the built-in
    // default), never from the compile-time table -- that is what makes a
    // language switch reach this pixel.
    const std::wstring label = Utf8(model.LabelOf(button.id));
    if (!label.empty() && labelFormat_) {
      // C78: a disabled LABEL is TextSecondary at 38%, not TextPrimary at 38%.
      // Measured on the device that reading is < 3:1: a disabled word must not
      // still pass AA, or the band promises an action it will not run.
      SetBrush(ToD2d(Token(enabled ? prism::theme::TokenId::TextPrimary
                                   : prism::theme::TokenId::TextSecondary,
                           lastVariant_),
                     alpha));
      const D2D1_RECT_F labelBox =
          D2D1::RectF(static_cast<float>(labelRect.left),
                      static_cast<float>(labelRect.top) + dy,
                      static_cast<float>(labelRect.right),
                      static_cast<float>(labelRect.bottom) + dy);
      // Ellipsis, not CLIP: the format carries the U+2026 trimming sign.
      target->DrawText(label.c_str(), static_cast<UINT32>(label.size()), labelFormat_.Get(),
                       labelBox, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
      ++dbgLabels;
      if (LabelOverflows(label, labelRect.Width())) ++dbgLabelsClipped;
    }
    // C78: the dropdown chevron. 8 x 4 px, directly under the label, two
    // 1.4 px strokes; token-coloured TextSecondary so it is quieter than the
    // word and never becomes a second icon. Only the caller's arrow flag (menu
    // action + hover/press/open) reaches this branch.
#if !defined(PRISM_C78_MUTANT_NO_ARROW)
    if (arrow && mnemonicFormat_) {
      const prism::actionbar::Rect arrowRect(prism::actionbar::ArrowBoxRect(button.rect));
      const float ax = (arrowRect.left + arrowRect.right) * 0.5f;
      const float top = static_cast<float>(arrowRect.top) + dy;
      const float bottom = static_cast<float>(arrowRect.bottom) + dy;
      SetBrush(ToD2d(Token(prism::theme::TokenId::TextSecondary, lastVariant_),
                     enabled ? 0.95f : kDisabledAlpha * 0.95f));
      target->DrawLine(D2D1::Point2F(ax - 3.0f, top + 0.5f),
                       D2D1::Point2F(ax, bottom - 0.5f), brush_.Get(), 1.4f);
      target->DrawLine(D2D1::Point2F(ax, bottom - 0.5f),
                       D2D1::Point2F(ax + 3.0f, top + 0.5f), brush_.Get(), 1.4f);
      ++dbgArrows;
    }
#endif

    // The mnemonic: a quiet 10 px letter in the corner. It is the only visible
    // promise that Alt+<letter> works, and it disappears when disabled.
    if (mnemonicFormat_ && spec.mnemonic != 0) {
      const wchar_t letter[2] = {static_cast<wchar_t>(spec.mnemonic), L'\0'};
      // C64: 0.60 alpha measured 3.2:1 against the island (below WCAG 4.5:1),
      // which is why the letter read as a detached speck. 0.90 measures ~5.5:1
      // and is still visibly quieter than the label's TextPrimary.
      SetBrush(ToD2d(Token(prism::theme::TokenId::TextSecondary, lastVariant_),
                     enabled ? 0.90f : kDisabledAlpha * 0.90f));
      target->DrawText(letter, 1, mnemonicFormat_.Get(),
                       D2D1::RectF(r.right - 18.0f, r.top + 5.0f + dy, r.right - 5.0f,
                                   r.top + 17.0f + dy),
                       brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  // ---- eight self-drawn silhouettes, all inside the 24 px box.
  void PaintGlyph(ID2D1RenderTarget* target, std::string_view icon,
                  const D2D1_RECT_F& box) {
    ID2D1Brush* brush = brush_.Get();
    const float l = box.left, t = box.top, r = box.right, b = box.bottom;
    const float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
    const float w = r - l, h = b - t;
    constexpr float kStroke = 1.8f;  // 24 px box: one more fifth of a pixel

    if (icon == "open") {
      // Folder outline with an arrow leaving it: "open the archive".
      target->DrawLine(D2D1::Point2F(l + w * 0.10f, t + h * 0.34f),
                       D2D1::Point2F(l + w * 0.40f, t + h * 0.34f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.40f, t + h * 0.34f),
                       D2D1::Point2F(l + w * 0.52f, t + h * 0.46f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.52f, t + h * 0.46f),
                       D2D1::Point2F(r - w * 0.12f, t + h * 0.46f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(r - w * 0.12f, t + h * 0.46f),
                       D2D1::Point2F(r - w * 0.12f, b - h * 0.14f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(r - w * 0.12f, b - h * 0.14f),
                       D2D1::Point2F(l + w * 0.12f, b - h * 0.14f), brush, kStroke);
    } else if (icon == "extract") {
      // A tray the content drops into.
      target->DrawLine(D2D1::Point2F(cx, t + h * 0.12f), D2D1::Point2F(cx, t + h * 0.58f),
                       brush, kStroke);
      target->DrawLine(D2D1::Point2F(cx - w * 0.22f, t + h * 0.38f),
                       D2D1::Point2F(cx, t + h * 0.60f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(cx + w * 0.22f, t + h * 0.38f),
                       D2D1::Point2F(cx, t + h * 0.60f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.14f, b - h * 0.16f),
                       D2D1::Point2F(r - w * 0.14f, b - h * 0.16f), brush, kStroke);
    } else if (icon == "compress") {
      // Two arrows meeting in the middle: the inverse of extract.
      target->DrawLine(D2D1::Point2F(l + w * 0.14f, t + h * 0.20f),
                       D2D1::Point2F(cx - w * 0.06f, cy), brush, kStroke);
      target->DrawLine(D2D1::Point2F(r - w * 0.14f, t + h * 0.20f),
                       D2D1::Point2F(cx + w * 0.06f, cy), brush, kStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.14f, b - h * 0.20f),
                       D2D1::Point2F(cx - w * 0.06f, cy), brush, kStroke);
      target->DrawLine(D2D1::Point2F(r - w * 0.14f, b - h * 0.20f),
                       D2D1::Point2F(cx + w * 0.06f, cy), brush, kStroke);
    } else if (icon == "add") {
      target->DrawLine(D2D1::Point2F(cx, t + h * 0.16f), D2D1::Point2F(cx, b - h * 0.16f),
                       brush, kStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.16f, cy), D2D1::Point2F(r - w * 0.16f, cy),
                       brush, kStroke);
    } else if (icon == "delete") {
      target->DrawLine(D2D1::Point2F(l + w * 0.14f, t + h * 0.26f),
                       D2D1::Point2F(r - w * 0.14f, t + h * 0.26f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(cx - w * 0.12f, t + h * 0.14f),
                       D2D1::Point2F(cx + w * 0.12f, t + h * 0.14f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.22f, t + h * 0.26f),
                       D2D1::Point2F(l + w * 0.28f, b - h * 0.12f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(r - w * 0.22f, t + h * 0.26f),
                       D2D1::Point2F(r - w * 0.28f, b - h * 0.12f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.28f, b - h * 0.12f),
                       D2D1::Point2F(r - w * 0.28f, b - h * 0.12f), brush, kStroke);
    } else if (icon == "test") {
      // Check: "the archive is sound".
      target->DrawLine(D2D1::Point2F(l + w * 0.16f, t + h * 0.54f),
                       D2D1::Point2F(cx - w * 0.02f, b - h * 0.24f), brush, kStroke);
      target->DrawLine(D2D1::Point2F(cx - w * 0.02f, b - h * 0.24f),
                       D2D1::Point2F(r - w * 0.14f, t + h * 0.24f), brush, kStroke);
    } else if (icon == "view") {
      // Eye: outline + pupil.
      target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w * 0.42f, h * 0.26f), brush,
                          kStroke);
      target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w * 0.10f, h * 0.10f), brush);
    } else if (icon == "search") {
      target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - w * 0.06f, cy - h * 0.06f),
                                        w * 0.26f, h * 0.26f),
                          brush, kStroke);
      target->DrawLine(D2D1::Point2F(cx + w * 0.14f, cy + h * 0.14f),
                       D2D1::Point2F(r - w * 0.12f, b - h * 0.12f), brush, kStroke);
    }
  }

  static int OwnerIndex(const prism::actionbar::Layout& layout,
                        prism::actionbar::ActionId id) {
    for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
      if (layout.buttons[i].id == id) return static_cast<int>(i);
    }
    return -1;
  }

  static bool enabledDropdown(const prism::actionbar::Model& model,
                              const prism::actionbar::Layout& layout, int) {
    return OwnerIndex(layout, model.DropdownOwner()) >= 0;
  }

  void DrawDropdown(ID2D1RenderTarget* target, const prism::actionbar::Layout& layout,
                    const prism::actionbar::Model& model, int ownerIndex,
                    prism::theme::Variant variant, const D2D1_SIZE_F& size) {
    prism::actionbar::Rect panelRect = prism::actionbar::DropdownRect(layout, ownerIndex);
    if (panelRect.Empty()) return;
    // Keep the panel on screen: it hangs into the content, so a narrow window
    // would let it run off the right edge.
    if (panelRect.right > static_cast<int>(size.width)) {
      const int width = panelRect.Width();
      panelRect.left = (std::max)(0, static_cast<int>(size.width) - width - 4);
      panelRect.right = panelRect.left + width;
    }
    const D2D1_RECT_F panel = RectF(panelRect);
    fillSquircle(target, panel, 14.0f, IslandFill(variant));
    SetBrush(BorderColor(variant));
    strokeSquircle(target, panel, 14.0f, 1.0f);
    ++dbgDropdowns;

    const std::span<const prism::actionbar::DropdownItem> items =
        prism::actionbar::DropdownItems(model.DropdownOwner());
    for (std::size_t i = 0; i < items.size(); ++i) {
      const float top = panel.top + static_cast<float>(prism::actionbar::kDropdownPad) +
                        static_cast<float>(i) * prism::actionbar::kDropdownRowHeight;
      const D2D1_RECT_F row =
          D2D1::RectF(panel.left + 4.0f, top, panel.right - 4.0f,
                      top + prism::actionbar::kDropdownRowHeight);
      if (static_cast<int>(i) == model.DropdownIndex()) {
        fillSquircle(target, row, 8.0f, Wash(variant, 0.12f));
      }
      const std::wstring label = Utf8(items[i].label);
      if (!label.empty() && labelFormat_) {
        SetBrush(ToD2d(Token(static_cast<int>(i) == model.DropdownIndex()
                                ? prism::theme::TokenId::TextPrimary
                                : prism::theme::TokenId::TextSecondary,
                            variant)));
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        target->DrawText(label.c_str(), static_cast<UINT32>(label.size()), labelFormat_.Get(),
                         D2D1::RectF(row.left + 10.0f, row.top, row.right - 6.0f, row.bottom),
                         brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        ++dbgDropdownRows;
      }
    }
  }

  static std::wstring Utf8(std::string_view s) {
    if (s.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), wide);
    return out;
  }

 public:
  int BrushCount() const { return brush_ ? 1 : 0; }
  int TextFormatCount() const {
    return (labelFormat_ ? 1 : 0) + (mnemonicFormat_ ? 1 : 0);
  }

  // C75: drop everything bound to a render target (and the DWrite factory this
  // painter holds) so a minimised window returns it. CreateDeviceResources
  // rebuilds it; the band's layout lives in PrismCore and never moves.
  void ReleaseDeviceResources() {
    brush_.Reset();
    writeFactory_.Reset();
    labelFormat_.Reset();
    mnemonicFormat_.Reset();
  }

 private:
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<IDWriteTextFormat> labelFormat_, mnemonicFormat_;
  prism::theme::Variant lastVariant_ = prism::theme::Variant::Midnight;
};

}  // namespace prism::win::actionbar
