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
//   * C100: the silhouettes are CURVES (ID2D1PathGeometry + AddBezier/AddArc,
//     stroked through ONE round-cap ID2D1StrokeStyle), and the band carries NO
//     mnemonic letter any more -- the "single-letter psoriasis" is gone from the
//     device. Alt+<letter> never depended on that glyph: the window claims the
//     chord in WM_SYSKEYDOWN and asks the MODEL (Model::PressMnemonic), and the
//     UIA node publishes shortcut = "Alt+O" for assistive tech. Both counts
//     below (MnemonicGlyphsDrawn / IconCurveCount) are what the C100 suite
//     asserts, and the two mutant arms put each defect back;
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
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "../PrismCore/ActionBarModel.hpp"
#include "../PrismCore/I18n.hpp"  // C74: the eight band keys live in the tables
#include "../PrismCore/ThemeTokens.hpp"
#include "Typography.hpp"  // C103: the ONE font stack + TextFormat entry (BLUEPRINT:170)

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
  // C100: ONE ink width for all eight silhouettes (the blueprint's 1.8-2.0 px
  // band). Eight icons at eight widths is how a band starts to look assembled
  // from different families; the suite asserts this exact number.
  static constexpr float kGlyphStroke = 1.8f;

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
      prism::win::typography::CreateTextFormat(
          writeFactory_.Get(), 11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
          L"en-US", labelFormat_.ReleaseAndGetAddressOf());
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
#if defined(PRISM_C100_MUTANT_MNEMONIC)
    // MUTANT ACTIVE (card C100, arm mnemonic): the 10 px corner letter is back.
    // Its text format exists again because the glyph that "needs" it exists
    // again -- the C100 suite's MnemonicGlyphsDrawn() assertion MUST go red.
    if (!mnemonicFormat_) {
      prism::win::typography::CreateTextFormat(
          writeFactory_.Get(), 10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
          L"en-US", mnemonicFormat_.ReleaseAndGetAddressOf());
      if (mnemonicFormat_) {
        mnemonicFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        mnemonicFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        mnemonicFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      }
    }
#endif
    // C100: ONE stroke style for all eight glyphs -- round caps on both ends and
    // on a dash, round joins. A curve that ends in a flat shaved rectangle reads
    // as a cut wire at 24 px; the round cap is what makes "圆头" true. The suite
    // reads the caps back off the DEVICE object (GetStartCap/GetDashCap), so a
    // style that silently degraded to butt caps cannot pass.
    if (!strokeStyle_) {
      ComPtr<ID2D1Factory> factory;
      target->GetFactory(factory.ReleaseAndGetAddressOf());
      if (factory) {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        factory->CreateStrokeStyle(props, nullptr, 0,
                                   strokeStyle_.ReleaseAndGetAddressOf());
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
    // C100: both counts describe the BAND THIS CALL PAINTS, not "everything
    // since launch". The suite asks two questions -- "does the painted band
    // carry a mnemonic letter" and "is this icon a curve drawing" -- and a
    // cumulative counter would answer neither.
    dbgMnemonicGlyphs = 0;
    iconCurves_.fill(0);

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
  // C100: the mnemonic letters this band painted last frame. MUST be 0 in the
  // shipping build -- the correction the card asks for is "no letter on the
  // device", and a counter is the only way to say that without eyeballing a PNG.
  int dbgMnemonicGlyphs = 0;

  // ---- C100 evidence: the four numbers/edges the suite reads back ----------
  // 1. how many Alt+<letter> glyphs the LAST Draw put on the device (0 shipping).
  int MnemonicGlyphsDrawn() const { return dbgMnemonicGlyphs; }
  // 2. curve primitives (AddBezier + AddArc) the last paint of one icon used.
  //    An icon that regressed to DrawLine calls reports 0 and goes red.
  int IconCurveCount(int actionIndex) const {
    if (actionIndex < 0 || actionIndex >= prism::actionbar::kActionCount) return 0;
    return iconCurves_[static_cast<std::size_t>(actionIndex)];
  }
  // 3. the stroke style the glyphs are drawn with, read off the DEVICE object
  //    (D2D1_CAP_STYLE_ROUND). D3: query it, never trust the comment.
  bool StrokeStyleBound() const { return strokeStyle_ != nullptr; }
  D2D1_CAP_STYLE StrokeStartCap() const {
    return strokeStyle_ ? strokeStyle_->GetStartCap() : D2D1_CAP_STYLE_FLAT;
  }
  D2D1_CAP_STYLE StrokeEndCap() const {
    return strokeStyle_ ? strokeStyle_->GetEndCap() : D2D1_CAP_STYLE_FLAT;
  }
  D2D1_CAP_STYLE StrokeDashCap() const {
    return strokeStyle_ ? strokeStyle_->GetDashCap() : D2D1_CAP_STYLE_FLAT;
  }
  D2D1_LINE_JOIN StrokeJoin() const {
    return strokeStyle_ ? strokeStyle_->GetLineJoin() : D2D1_LINE_JOIN_MITER;
  }
  // 4. the glyph ink width, so "1.8-2.0 px stroke" is a number, not a claim.
  static constexpr float GlyphStrokeWidth() { return kGlyphStroke; }

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
  // C78: the seam is a hairline at a measured 8%. C91 moved it from the
  // specular RIM to --divider-subtle: the rim is WHITE in all three variants, and
  // once the daylight theme actually reached the island (a near-white surface)
  // a white 8% seam measured ZERO levels -- the group separator disappeared in
  // Day. --divider-subtle is the token that exists for exactly this: dark in
  // Daylight, white in Midnight, white at .40 in HighContrast. The ActionBar's
  // own alpha still decides the weight, so Midnight paints the same 8% white
  // pixel as before and the C78 band (0.06..0.10) still holds.
  static D2D1_COLOR_F SeparatorColor(prism::theme::Variant variant) {
    prism::theme::Rgba c =
        prism::theme::Lookup(prism::theme::TokenId::DividerSubtle, variant).color;
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
    if (FAILED(prism::win::typography::CreateTextLayout(
            writeFactory_.Get(), label.c_str(), static_cast<UINT32>(label.size()),
            labelFormat_.Get(), static_cast<float>(boxWidth),
            static_cast<float>(prism::actionbar::kLabelHeight),
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
    // V4: both boxes come from the MODEL, and the press sink moves both by the
    // SAME dy -- moving one would reopen the misalignment the screenshot showed.
    const prism::actionbar::Rect iconRect(prism::actionbar::IconBoxRect(button.rect));
    const prism::actionbar::Rect labelRect(prism::actionbar::LabelBoxRect(button.rect));
    const D2D1_RECT_F iconBox =
        D2D1::RectF(static_cast<float>(iconRect.left), static_cast<float>(iconRect.top) + dy,
                    static_cast<float>(iconRect.right),
                    static_cast<float>(iconRect.bottom) + dy);
    SetBrush(ToD2d(Token(prism::theme::TokenId::TextPrimary, lastVariant_), alpha));
    const int actionIndex = prism::actionbar::IndexOf(button.id);
    PaintGlyph(target, spec.icon, iconBox, actionIndex);
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
    // C100: the gate used to be "arrow && mnemonicFormat_" -- the chevron was
    // hanging off a text format that only the deleted corner letter needed.
    if (arrow) {
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

    // C100: the corner letter is GONE ("单字母牛皮癣"). It was never the
    // shortcut's entry point -- the window claims Alt+<letter> in WM_SYSKEYDOWN
    // and asks the MODEL (Model::PressMnemonic -> FromMnemonic), and the UIA
    // button already publishes shortcut = "Alt+O". So deleting the glyph deletes
    // a decoration, not a feature. The mutant below puts the letter back, which
    // is what makes MnemonicGlyphsDrawn() == 0 a real assertion instead of a
    // statement about the source code.
#if defined(PRISM_C100_MUTANT_MNEMONIC)
    const D2D1_RECT_F r = RectF(button.rect);
    if (mnemonicFormat_ && spec.mnemonic != 0) {
      const wchar_t letter[2] = {static_cast<wchar_t>(spec.mnemonic), L'\0'};
      SetBrush(ToD2d(Token(prism::theme::TokenId::TextSecondary, lastVariant_),
                     enabled ? 0.90f : kDisabledAlpha * 0.90f));
      target->DrawText(letter, 1, mnemonicFormat_.Get(),
                       D2D1::RectF(r.right - 18.0f, r.top + 5.0f + dy, r.right - 5.0f,
                                   r.top + 17.0f + dy),
                       brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      ++dbgMnemonicGlyphs;
    }
#endif
  }

  // ---- C100: one path builder for all eight silhouettes -------------------
  // A silhouette is a set of figures stroked through the ONE round-cap style.
  // The builder counts its curve primitives (AddBezier / AddArc) because "this
  // icon is a curve drawing" has to be a NUMBER a test can read: the C100 suite
  // asserts >= 2 per icon. What it replaces is a stack of 23 DrawLine calls with
  // zero curves, whose worst member was the compress glyph -- four straight
  // lines from the corners to the middle, i.e. a big X.
  struct GlyphPath {
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    int curves = 0;
    bool figureOpen = false;
  };

  static bool BeginGlyph(ID2D1RenderTarget* target, GlyphPath* glyph) {
    if (target == nullptr || glyph == nullptr) return false;
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.ReleaseAndGetAddressOf());
    if (!factory) return false;
    if (FAILED(factory->CreatePathGeometry(glyph->geometry.ReleaseAndGetAddressOf()))) {
      return false;
    }
    return SUCCEEDED(glyph->geometry->Open(glyph->sink.ReleaseAndGetAddressOf()));
  }

  static void GlyphBegin(GlyphPath& g, D2D1_POINT_2F p) {
    if (!g.sink) return;
    if (g.figureOpen) {
      g.sink->EndFigure(D2D1_FIGURE_END_OPEN);
      g.figureOpen = false;
    }
    g.sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
    g.figureOpen = true;
  }

  static void GlyphLine(GlyphPath& g, D2D1_POINT_2F p) {
    if (!g.sink || !g.figureOpen) return;
    g.sink->AddLine(p);
  }

  // A cubic -- the workhorse of a round corner and of an arch.
  static void GlyphCurve(GlyphPath& g, D2D1_POINT_2F c1, D2D1_POINT_2F c2, D2D1_POINT_2F end) {
    if (!g.sink || !g.figureOpen) return;
    g.sink->AddBezier(D2D1::BezierSegment(c1, c2, end));
    ++g.curves;
  }

  // An elliptical arc -- a circle is two of these, so a "round" icon is round
  // in the geometry, not because an ellipse primitive was substituted in.
  static void GlyphArc(GlyphPath& g, D2D1_POINT_2F end, D2D1_SIZE_F radius,
                       D2D1_SWEEP_DIRECTION sweep) {
    if (!g.sink || !g.figureOpen) return;
    g.sink->AddArc(D2D1::ArcSegment(end, radius, 0.0f, sweep, D2D1_ARC_SIZE_SMALL));
    ++g.curves;
  }

  // One stroke, one style, one width: DrawGeometry (never DrawLine/DrawEllipse),
  // so the round caps of the shared stroke style reach every end in the set.
  void StrokeGlyph(ID2D1RenderTarget* target, GlyphPath& g, ID2D1Brush* brush, float width) {
    if (!g.sink) return;
    if (g.figureOpen) {
      g.sink->EndFigure(D2D1_FIGURE_END_OPEN);
      g.figureOpen = false;
    }
    if (FAILED(g.sink->Close())) return;
    target->DrawGeometry(g.geometry.Get(), brush, width, strokeStyle_.Get());
    g.sink.Reset();
  }

  // ---- eight self-drawn silhouettes, all inside the 24 px box.
  // C100: every one is built from curves (>= 2 primitives) and stroked at ONE
  // width with round caps. The shapes are written in NORMALISED box coordinates
  // so a change to the icon box moves the whole family together instead of only
  // the icons whose numbers happened to be pixels.
  void PaintGlyph(ID2D1RenderTarget* target, std::string_view icon,
                  const D2D1_RECT_F& box, int actionIndex) {
    ID2D1Brush* brush = brush_.Get();
    const float l = box.left, t = box.top, r = box.right, b = box.bottom;
    const float w = r - l, h = b - t;
    const auto P = [&](float nx, float ny) {
      return D2D1::Point2F(l + w * nx, t + h * ny);
    };

    GlyphPath g;
    if (!BeginGlyph(target, &g)) return;

    if (icon == "open") {
      // A rounded folder whose lid is lifted ajar: the lid is an arch, the body
      // is a tab plus a tray with two rounded bottom corners.
      GlyphBegin(g, P(0.12f, 0.44f));
      GlyphLine(g, P(0.38f, 0.44f));
      GlyphLine(g, P(0.46f, 0.54f));
      GlyphLine(g, P(0.86f, 0.54f));
      GlyphLine(g, P(0.86f, 0.80f));
      GlyphCurve(g, P(0.86f, 0.835f), P(0.835f, 0.86f), P(0.80f, 0.86f));
      GlyphLine(g, P(0.20f, 0.86f));
      GlyphCurve(g, P(0.165f, 0.86f), P(0.14f, 0.835f), P(0.14f, 0.80f));
      GlyphLine(g, P(0.14f, 0.44f));
      GlyphBegin(g, P(0.10f, 0.42f));
      GlyphCurve(g, P(0.34f, 0.24f), P(0.64f, 0.22f), P(0.92f, 0.32f));
    } else if (icon == "extract") {
      // A box the content drops into, under a rounded arrow head.
      GlyphBegin(g, P(0.12f, 0.56f));
      GlyphLine(g, P(0.12f, 0.74f));
      GlyphCurve(g, P(0.12f, 0.82f), P(0.16f, 0.86f), P(0.24f, 0.86f));
      GlyphLine(g, P(0.76f, 0.86f));
      GlyphCurve(g, P(0.84f, 0.86f), P(0.88f, 0.82f), P(0.88f, 0.74f));
      GlyphLine(g, P(0.88f, 0.56f));
      GlyphBegin(g, P(0.50f, 0.12f));
      GlyphLine(g, P(0.50f, 0.44f));
      GlyphBegin(g, P(0.34f, 0.30f));
      GlyphCurve(g, P(0.40f, 0.36f), P(0.46f, 0.42f), P(0.50f, 0.46f));
      GlyphCurve(g, P(0.54f, 0.42f), P(0.60f, 0.36f), P(0.66f, 0.30f));
    } else if (icon == "compress") {
#if defined(PRISM_C100_MUTANT_LINE_COMPRESS)
      // MUTANT ACTIVE (card C100, arm line_compress): the pre-fix glyph, four
      // straight lines from the corners into the middle -- which is a big X,
      // not a packed box. The suite's "the centre pixel is background" and
      // "no radial stroke in any quadrant" assertions MUST go red.
      const float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
      target->DrawLine(D2D1::Point2F(l + w * 0.14f, t + h * 0.20f),
                       D2D1::Point2F(cx - w * 0.06f, cy), brush, kGlyphStroke);
      target->DrawLine(D2D1::Point2F(r - w * 0.14f, t + h * 0.20f),
                       D2D1::Point2F(cx + w * 0.06f, cy), brush, kGlyphStroke);
      target->DrawLine(D2D1::Point2F(l + w * 0.14f, b - h * 0.20f),
                       D2D1::Point2F(cx - w * 0.06f, cy), brush, kGlyphStroke);
      target->DrawLine(D2D1::Point2F(r - w * 0.14f, b - h * 0.20f),
                       D2D1::Point2F(cx + w * 0.06f, cy), brush, kGlyphStroke);
#else
      // A PACKED BOX: a rounded carton with two open flaps above it and two
      // straps down the face. Nothing crosses the middle -- the straps sit a
      // quarter of the box in from each edge, and the carton's lid is a whole
      // quarter ABOVE the icon centre, so the centre of the glyph stays paper.
      // That clearance is the difference between a parcel and a cross, and the
      // suite measures it in pixels (centre pixel + a radial-run sweep).
      GlyphBegin(g, P(0.28f, 0.26f));
      GlyphLine(g, P(0.72f, 0.26f));
      GlyphCurve(g, P(0.79f, 0.26f), P(0.84f, 0.31f), P(0.84f, 0.38f));
      GlyphLine(g, P(0.84f, 0.80f));
      GlyphCurve(g, P(0.84f, 0.87f), P(0.79f, 0.92f), P(0.72f, 0.92f));
      GlyphLine(g, P(0.28f, 0.92f));
      GlyphCurve(g, P(0.21f, 0.92f), P(0.16f, 0.87f), P(0.16f, 0.80f));
      GlyphLine(g, P(0.16f, 0.38f));
      GlyphCurve(g, P(0.16f, 0.31f), P(0.21f, 0.26f), P(0.28f, 0.26f));
      // the two open flaps, angled out like a lid that was folded back
      GlyphBegin(g, P(0.16f, 0.26f));
      GlyphLine(g, P(0.34f, 0.16f));
      GlyphBegin(g, P(0.84f, 0.26f));
      GlyphLine(g, P(0.66f, 0.16f));
      // the two straps, from the lid seam to the foot
      GlyphBegin(g, P(0.26f, 0.27f));
      GlyphLine(g, P(0.26f, 0.92f));
      GlyphBegin(g, P(0.74f, 0.27f));
      GlyphLine(g, P(0.74f, 0.92f));
#endif
    } else if (icon == "add") {
      // The round-headed plus: two arms, round caps from the shared style. The
      // arms are cubics with a half-pixel bow so the family keeps ONE primitive
      // vocabulary (lines for straight runs, cubics for everything else) -- a
      // call-add glyph that is two bare DrawLine calls is the defect this ends.
      GlyphBegin(g, P(0.50f, 0.16f));
      GlyphCurve(g, P(0.494f, 0.40f), P(0.506f, 0.62f), P(0.50f, 0.84f));
      GlyphBegin(g, P(0.16f, 0.50f));
      GlyphCurve(g, P(0.40f, 0.494f), P(0.62f, 0.506f), P(0.84f, 0.50f));
    } else if (icon == "delete") {
      // A lidded bin: lid bar, arched handle, tapered can with a rounded foot,
      // two ribs.
      GlyphBegin(g, P(0.14f, 0.30f));
      GlyphLine(g, P(0.86f, 0.30f));
      GlyphBegin(g, P(0.38f, 0.30f));
      GlyphLine(g, P(0.38f, 0.22f));
      GlyphCurve(g, P(0.38f, 0.20f), P(0.39f, 0.19f), P(0.41f, 0.19f));
      GlyphLine(g, P(0.59f, 0.19f));
      GlyphCurve(g, P(0.61f, 0.19f), P(0.62f, 0.20f), P(0.62f, 0.22f));
      GlyphLine(g, P(0.62f, 0.30f));
      GlyphBegin(g, P(0.20f, 0.30f));
      GlyphLine(g, P(0.26f, 0.80f));
      GlyphCurve(g, P(0.265f, 0.855f), P(0.30f, 0.88f), P(0.36f, 0.88f));
      GlyphLine(g, P(0.64f, 0.88f));
      GlyphCurve(g, P(0.70f, 0.88f), P(0.735f, 0.855f), P(0.74f, 0.80f));
      GlyphLine(g, P(0.80f, 0.30f));
      GlyphBegin(g, P(0.42f, 0.50f));
      GlyphLine(g, P(0.42f, 0.72f));
      GlyphBegin(g, P(0.58f, 0.50f));
      GlyphLine(g, P(0.58f, 0.72f));
    } else if (icon == "test") {
      // A shield with a swept check inside it: "the archive is sound".
      GlyphBegin(g, P(0.50f, 0.10f));
      GlyphLine(g, P(0.82f, 0.22f));
      GlyphLine(g, P(0.82f, 0.50f));
      GlyphCurve(g, P(0.82f, 0.68f), P(0.68f, 0.80f), P(0.50f, 0.88f));
      GlyphCurve(g, P(0.32f, 0.80f), P(0.18f, 0.68f), P(0.18f, 0.50f));
      GlyphLine(g, P(0.18f, 0.22f));
      GlyphLine(g, P(0.50f, 0.10f));
      GlyphBegin(g, P(0.35f, 0.47f));
      GlyphCurve(g, P(0.40f, 0.52f), P(0.44f, 0.56f), P(0.47f, 0.60f));
      GlyphCurve(g, P(0.53f, 0.52f), P(0.60f, 0.43f), P(0.67f, 0.36f));
    } else if (icon == "view") {
      // An eye made of four cubics, with a filled pupil sitting on the axis.
      const float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
      GlyphBegin(g, P(0.10f, 0.50f));
      GlyphCurve(g, P(0.24f, 0.28f), P(0.40f, 0.24f), P(0.50f, 0.24f));
      GlyphCurve(g, P(0.60f, 0.24f), P(0.76f, 0.28f), P(0.90f, 0.50f));
      GlyphCurve(g, P(0.76f, 0.72f), P(0.60f, 0.76f), P(0.50f, 0.76f));
      GlyphCurve(g, P(0.40f, 0.76f), P(0.24f, 0.72f), P(0.10f, 0.50f));
      target->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(cx, cy), w * 0.11f, h * 0.11f), brush);
    } else if (icon == "search") {
      // A magnifier: a circle of TWO arcs (not an ellipse primitive) on a 45
      // degree handle, so the lens and the handle share one ink.
      const float lx = l + w * 0.44f, ly = t + h * 0.44f, rad = w * 0.28f;
      GlyphBegin(g, D2D1::Point2F(lx - rad, ly));
      GlyphArc(g, D2D1::Point2F(lx + rad, ly), D2D1::SizeF(rad, rad),
               D2D1_SWEEP_DIRECTION_CLOCKWISE);
      GlyphArc(g, D2D1::Point2F(lx - rad, ly), D2D1::SizeF(rad, rad),
               D2D1_SWEEP_DIRECTION_CLOCKWISE);
      GlyphBegin(g, P(0.64f, 0.64f));
      GlyphLine(g, P(0.86f, 0.86f));
    }

    StrokeGlyph(target, g, brush, kGlyphStroke);
    iconCurves_[actionIndex] = g.curves;
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
  // C100: ONE text format -- the label. The corner letter's 10 px semi-bold
  // format is gone with the letter, so a band that still allocates two formats
  // is a band that still has the defect (the suite asserts this is 1).
  int TextFormatCount() const {
#if defined(PRISM_C100_MUTANT_MNEMONIC)
    return (labelFormat_ ? 1 : 0) + (mnemonicFormat_ ? 1 : 0);
#else
    return labelFormat_ ? 1 : 0;
#endif
  }

  // C75: drop everything bound to a render target (and the DWrite factory this
  // painter holds) so a minimised window returns it. CreateDeviceResources
  // rebuilds it; the band's layout lives in PrismCore and never moves.
  void ReleaseDeviceResources() {
    brush_.Reset();
    writeFactory_.Reset();
    labelFormat_.Reset();
#if defined(PRISM_C100_MUTANT_MNEMONIC)
    mnemonicFormat_.Reset();
#endif
    // C100: the stroke style is a device object too (it came from the target's
    // factory); a minimised window must be able to return it.
    strokeStyle_.Reset();
  }

 private:
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<IDWriteTextFormat> labelFormat_;
#if defined(PRISM_C100_MUTANT_MNEMONIC)
  // MUTANT ACTIVE (arm mnemonic) only: the deleted letter's text format.
  ComPtr<IDWriteTextFormat> mnemonicFormat_;
#endif
  // C100: the one stroke style every silhouette is drawn through.
  ComPtr<ID2D1StrokeStyle> strokeStyle_;
  // C100: per-action curve primitive count of the LAST Draw (see accessors).
  std::array<int, static_cast<std::size_t>(prism::actionbar::kActionCount)> iconCurves_{};
  prism::theme::Variant lastVariant_ = prism::theme::Variant::Midnight;
};

}  // namespace prism::win::actionbar
