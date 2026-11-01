// PrismArchive -- PrismWin
// ActionBar: the PAINTER of the action band (DESIGN.md §2/§3/§4).
//
// The model owns the table, the rules and the geometry (PrismCore's
// ActionBarModel.hpp); this file owns the last mile, and nothing else:
//
//   * the island is the C15 surface token, NOT a coloured ribbon: hover lifts
//     the local fill by a white wash (+10%), it never turns blue -- Bandizip's
//     blue toolbar is exactly what the blueprint forbids copying;
//   * every corner is a SQUIRCLE (theme::SquircleField, n=4.2, radius 20 for
//     the island and 10 for a button), not a circular arc;
//   * pressing sinks the CONTENT by one pixel (the fill stays put), so the
//     click reads as a physical press instead of a colour flash;
//   * a disabled action is painted at 40% of its ink -- dim enough to read as
//     unavailable, visible enough to teach where it will appear;
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
#include "../PrismCore/ThemeTokens.hpp"

namespace prism::win::actionbar {

using Microsoft::WRL::ComPtr;

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
  // looks disabled" cannot drift between the icon and its label.
  static constexpr float kDisabledAlpha = 0.40f;

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

    // 2. per-button state fill + focus ring, then the content.
    for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
      const prism::actionbar::Button& button = layout.buttons[i];
      const int index = static_cast<int>(i);
      const bool enabled = model.Enabled(button.id);
      const bool hovered = enabled && index == hoverIndex;
      const bool pressed = enabled && index == pressedIndex;
      const D2D1_RECT_F rect = RectF(button.rect);

      if (hovered || pressed) {
        // The lift: a white wash INSIDE the button, never a hue change. The
        // press adds 60% more wash; the 1 px sink below is what sells it.
        const float alpha = pressed ? 0.16f : 0.10f;
        fillSquircle(target, D2D1::RectF(rect.left + 2.0f, rect.top + 2.0f,
                                         rect.right - 2.0f, rect.bottom - 2.0f),
                     10.0f, Wash(alpha));
        if (pressed) ++dbgPressed;
        if (hovered) ++dbgHover;
      }
      ++dbgButtons;

      if (model.FocusIndex() == index) {
        SetBrush(Accent(variant, 0.90f));
        strokeSquircle(target, D2D1::RectF(rect.left + 2.0f, rect.top + 2.0f,
                                          rect.right - 2.0f, rect.bottom - 2.0f),
                       9.0f, 1.5f);
        ++dbgFocusRings;
      }

      // Pressed: content sinks 1 px; the island and the fill do not move.
      const float dy = pressed ? 1.0f : 0.0f;
      PaintButtonContent(target, button, model, enabled, dy);
      if (!enabled) ++dbgDisabled;
    }

    // 3. the dropdown hangs BELOW the band and above the content.
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
    prism::theme::Rgba c = Token(prism::theme::TokenId::SurfaceFloatingIsland, variant);
    c.a = 0.72f;  // the renderer's floating-island alpha
    return ToD2d(c);
  }
  static D2D1_COLOR_F BorderColor(prism::theme::Variant variant) {
    prism::theme::Rgba c =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant).gradient.from;
    c.a = 0.16f;
    return ToD2d(c);
  }
  static D2D1_COLOR_F Accent(prism::theme::Variant variant, float alpha) {
    prism::theme::Rgba c =
        prism::theme::Lookup(prism::theme::TokenId::AccentLiquidPrimary, variant).gradient.from;
    c.a = alpha;
    return ToD2d(c);
  }
  // The hover/press wash is achromatic on purpose (blue ribbon forbidden).
  static D2D1_COLOR_F Wash(float alpha) { return D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha); }

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

  // ---- one button's icon + label + mnemonic, all offset by dy (the sink).
  void PaintButtonContent(ID2D1RenderTarget* target, const prism::actionbar::Button& button,
                          const prism::actionbar::Model& model, bool enabled, float dy) {
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

    const std::wstring label = Utf8(spec.label);
    if (!label.empty() && labelFormat_) {
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
    // The mnemonic: a quiet 10 px letter in the corner. It is the only visible
    // promise that Alt+<letter> works, and it disappears when disabled.
    if (mnemonicFormat_ && spec.mnemonic != 0) {
      const wchar_t letter[2] = {static_cast<wchar_t>(spec.mnemonic), L'\0'};
      SetBrush(ToD2d(Token(prism::theme::TokenId::TextSecondary, lastVariant_),
                     enabled ? 0.60f : kDisabledAlpha * 0.60f));
      target->DrawText(letter, 1, mnemonicFormat_.Get(),
                       D2D1::RectF(r.right - 18.0f, r.top + 5.0f + dy, r.right - 5.0f,
                                   r.top + 17.0f + dy),
                       brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  // ---- eight self-drawn silhouettes, all inside the 20 px box.
  void PaintGlyph(ID2D1RenderTarget* target, std::string_view icon,
                  const D2D1_RECT_F& box) {
    ID2D1Brush* brush = brush_.Get();
    const float l = box.left, t = box.top, r = box.right, b = box.bottom;
    const float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
    const float w = r - l, h = b - t;
    constexpr float kStroke = 1.6f;

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
        fillSquircle(target, row, 8.0f, Wash(0.12f));
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

  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<IDWriteTextFormat> labelFormat_, mnemonicFormat_;
  prism::theme::Variant lastVariant_ = prism::theme::Variant::Midnight;
};

}  // namespace prism::win::actionbar
