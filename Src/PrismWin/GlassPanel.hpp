// PrismArchive -- PrismWin
// C77: THE glass panel helper. One surface language for every floating pane
// (breadcrumb island, action island, sidebar base, Quick Look card).
//
// WHAT "LIQUID GLASS" NEEDS THAT A SEMI-TRANSPARENT FILL DOES NOT HAVE
//   1. a FILL that lets the backdrop through (alpha .45-.55, token-driven),
//   2. a TOP SPECULAR RIM: 1 device pixel, much brighter than the fill, which
//      is what reads as a lit edge rather than a painted rectangle,
//   3. a DROP SHADOW: blur 24, offset y +6, peak alpha .18 -- depth, because a
//      panel with no shadow is a rectangle drawn ON the content, not above it,
//   4. a CONTINUOUS corner (radius 14) solved against C15's SquircleField, so
//      the exponent stays owned by the token header.
//
// WHY THREE PRIMITIVES AND NOT ONE
//   The action island's FILL belongs to ActionBarPainter (it is C64's pinned
//   pixel: CompositeOver(SurfaceFloatingIsland, canvas)). C77 owns only its
//   BASE (shadow) and its EDGE (rim). Painting a second fill under it would
//   have changed that pinned composite -- so the helper exposes PaintShadow /
//   PaintFill / PaintRim separately and Paint() composes all three.
//
// Everything here is TOKENS (CONTRACTS A6 / C15): no literal colour, and each
// of the three new tokens has a Daylight, a Midnight AND a HighContrast row.
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

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

#include "../PrismCore/ThemeTokens.hpp"

namespace prism::win::glass {

// The design numbers (DESIGN.md 2.1 / the card's brief). They live next to the
// painter that applies them; the pixel suite pins each one.
inline constexpr float kPanelRadius = 14.0f;       // continuous corner, DIP
inline constexpr float kShadowBlur = 24.0f;        // gaussian radius, DIP
inline constexpr float kShadowOffsetY = 6.0f;      // panel sits 6 px above
// The card's fill window. C124 raised the CEILING from .55 to the surface
// ladder's floating tier (.75): a clamp BELOW the token silently repaints the
// tier, so the password dialog's card measured (29,33,42) while the action
// island beside it measured (35,40,51) for the same token -- "one surface, two
// truths", which is the defect this card exists to remove (C88's "the card's
// pixels come from the theme tokens" is what caught it). The FLOOR still keeps
// a fill translucent enough to be glass; .75 lets a quarter of the backdrop
// through, and C77's clamp assertions are relational to these two constants.
inline constexpr float kFillAlphaMin = 0.45f;
inline constexpr float kFillAlphaMax = 0.75f;
inline constexpr float kRimHeight = 1.0f;          // one device pixel
inline constexpr int kShadowLayers = 8;

// A squircle (|x|^n + |y|^n = 1) closed path, n from theme::SquircleField.
// Duplicated nowhere: every painter in this file goes through this one.
inline HRESULT CreateSquirclePath(ID2D1Factory* factory, const D2D1_RECT_F& rect,
                                  float radius, ID2D1PathGeometry** out) {
  if (factory == nullptr || out == nullptr) return E_POINTER;
  const float w = rect.right - rect.left;
  const float h = rect.bottom - rect.top;
  if (!(w > 0.0f) || !(h > 0.0f)) return E_INVALIDARG;
  const float r = (std::max)(0.0f, (std::min)(radius, (std::min)(w, h) * 0.5f));

  Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
  HRESULT hr = factory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
  if (FAILED(hr)) return hr;
  Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
  hr = path->Open(sink.ReleaseAndGetAddressOf());
  if (FAILED(hr)) return hr;

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

// Which parts of a panel this call paints. A caller that owns one of them
// (the action island's fill) leaves that flag off instead of fighting a second
// fill it cannot see.
struct Parts {
  bool shadow = true;
  bool fill = true;
  bool rim = true;
  bool border = true;
  // The fill's alpha is the token's, optionally scaled (the card's .45-.55
  // window). 1.0 means "whatever C15 says".
  float fillAlphaScale = 1.0f;
  // An EXPLICIT fill for surfaces that are not the floating-island token (the
  // sidebar base is SurfaceFloatingIsland at .94, pinned by the C29 suite).
  bool hasExplicitFill = false;
  prism::theme::Rgba explicitFill{};
};

// What the last Paint() actually did, for the pixel suite (an absence must say
// so -- CONTRACTS D12).
struct Stats {
  int panels = 0;
  int shadows = 0;
  int fills = 0;
  int rims = 0;
};

class PanelPainter {
 public:
  // The painter owns NO device resource: it borrows the host's one recolourable
  // solid brush (the same pattern the sidebar painter uses). That keeps the
  // C75 memory accounting exact -- a new brush here would have been one more
  // device object nobody counted.
  void Bind(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush) {
    brush_ = brush;
    factory_.Reset();
    if (target != nullptr) target->GetFactory(factory_.ReleaseAndGetAddressOf());
  }

  void Unbind() {
    brush_ = nullptr;
    factory_.Reset();
  }

  bool Live() const { return brush_ != nullptr && factory_ != nullptr; }

  // ---- the three primitives ------------------------------------------------
  // 1. the shadow: kShadowLayers nested squircles whose accumulated alpha
  //    follows a linear ramp from the token's peak at the panel edge to 0 at
  //    kShadowBlur. D2D1 on a plain render target has no blur effect, so this
  //    IS the gaussian -- 8 layers, each one a step of the same falloff.
  //
  // The shadow is CLIPPED OUT of the panel's own bounding box. A panel's shadow
  // falls on the backdrop BEHIND it; letting it through the translucent fill
  // would darken the panel interior, and for the action island (whose fill is
  // painted by ActionBarPainter, pinned to CompositeOver(island, canvas) by
  // C64) that would have silently repainted a contract this card does not own.
  // It also means the four corner pixels are EXACTLY the backdrop -- assertion
  // (4) of the card.
  void PaintShadow(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                   prism::theme::Variant variant, Stats* stats = nullptr) {
#if defined(PRISM_C77_MUTANT_NO_SHADOW)
    // C77 MUTANT ACTIVE: no-shadow -- the drop shadow is compiled out, so the
    // suite's "6..10 px below the panel is darker by 3..10" check MUST go red.
    (void)target;
    (void)rect;
    (void)variant;
    (void)stats;
    return;
#endif
    if (!Live()) return;
    const prism::theme::Rgba shadow =
        prism::theme::Lookup(prism::theme::TokenId::ShadowFloatingPanel, variant).color;
    const float peak = shadow.a;
    if (!(peak > 0.0f)) return;
    const D2D1_RECT_F offset =
        D2D1::RectF(rect.left, rect.top + kShadowOffsetY, rect.right,
                    rect.bottom + kShadowOffsetY);
    const float reach = kShadowBlur + kShadowOffsetY;
    const D2D1_RECT_F bands[4] = {
        D2D1::RectF(rect.left - reach, rect.top - reach, rect.right + reach, rect.top),
        D2D1::RectF(rect.left - reach, rect.bottom, rect.right + reach, rect.bottom + reach),
        D2D1::RectF(rect.left - reach, rect.top, rect.left, rect.bottom),
        D2D1::RectF(rect.right, rect.top, rect.right + reach, rect.bottom),
    };
    for (const D2D1_RECT_F& band : bands) {
      if (band.right <= band.left || band.bottom <= band.top) continue;
      target->PushAxisAlignedClip(band, D2D1_ANTIALIAS_MODE_ALIASED);
      // Draw outermost -> innermost. T_k is the alpha a pixel AT layer k's edge
      // must end up with; a_k is what this layer has to add to get there.
      float previous = 0.0f;
      for (int i = 0; i < kShadowLayers; ++i) {
        const float inflate =
            kShadowBlur * (1.0f - static_cast<float>(i) / static_cast<float>(kShadowLayers));
        // A squared falloff, not a linear one: a gaussian's mass sits near the
        // edge, and the card's "3..10 levels darker at 6..10 px out" band is
        // measured there (see the C77 pixel suite).
        const float q = inflate / kShadowBlur;
        const float targetAlpha = peak * (1.0f - q * q);
        const float add = 1.0f - (1.0f - targetAlpha) / (1.0f - previous);
        previous = targetAlpha;
        if (!(add > 0.0f)) continue;
        brush_->SetColor(D2D1::ColorF(shadow.r, shadow.g, shadow.b, add));
        const D2D1_RECT_F layer = D2D1::RectF(offset.left - inflate, offset.top - inflate,
                                              offset.right + inflate, offset.bottom + inflate);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
        if (SUCCEEDED(CreateSquirclePath(factory_.Get(), layer, kPanelRadius + inflate,
                                         path.ReleaseAndGetAddressOf()))) {
          target->FillGeometry(path.Get(), brush_);
        } else {
          target->FillRectangle(layer, brush_);
        }
      }
      target->PopAxisAlignedClip();
    }
    ++shadowPaintCount_;
    if (stats != nullptr) ++stats->shadows;
  }

  // 2. the fill: the glass surface (C15 token, appearance.glassIntensity folded
  //    in by EffectiveGlassSurface) at the card's .45-.55 window.
  void PaintFill(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                 prism::theme::Variant variant, const Parts& parts,
                 Stats* stats = nullptr) {
    if (!Live()) return;
    prism::theme::Rgba fill = parts.hasExplicitFill
                                  ? parts.explicitFill
                                  : prism::theme::EffectiveGlassSurface(variant);
    fill.a = ClampFillAlpha(fill.a * parts.fillAlphaScale);
    brush_->SetColor(D2D1::ColorF(fill.r, fill.g, fill.b, fill.a));
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (SUCCEEDED(CreateSquirclePath(factory_.Get(), rect, kPanelRadius,
                                     path.ReleaseAndGetAddressOf()))) {
      target->FillGeometry(path.Get(), brush_);
    } else {
      target->FillRectangle(rect, brush_);
    }
    ++fillPaintCount_;
    if (stats != nullptr) ++stats->fills;
  }

  // 3. the specular rim: one device pixel along the TOP edge, in the
  //    --border-specular-rim top stop (alpha .85 Daylight / .28 Midnight), then
  //    the hairline border around the rest of the shape.
  void PaintRim(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                prism::theme::Variant variant, const Parts& parts,
                Stats* stats = nullptr) {
    if (!Live()) return;
    const prism::theme::Rgba rim =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant).gradient.from;
    // The top edge is the shape's flat run: [left+radius, right-radius] at
    // y = top. Inset by half a pixel so the 1 px line covers exactly one row.
    const float inset = kPanelRadius;
    const float y = rect.top + 0.5f;
    brush_->SetColor(D2D1::ColorF(rim.r, rim.g, rim.b, rim.a));
    if (rect.right - inset > rect.left + inset) {
      target->FillRectangle(
          D2D1::RectF(rect.left + inset, y - 0.5f, rect.right - inset, y + 0.5f),
          brush_);
    }
    ++rimPaintCount_;
    if (stats != nullptr) ++stats->rims;

    if (!parts.border) return;
    prism::theme::Rgba border = rim;
    border.a = 0.16f;
    brush_->SetColor(D2D1::ColorF(border.r, border.g, border.b, border.a));
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (SUCCEEDED(CreateSquirclePath(factory_.Get(), rect, kPanelRadius,
                                     path.ReleaseAndGetAddressOf()))) {
      target->DrawGeometry(path.Get(), brush_, 1.0f);
    } else {
      target->DrawRectangle(rect, brush_, 1.0f);
    }
  }

  // All three, in the only order that composites correctly.
  void Paint(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
             prism::theme::Variant variant, const Parts& parts,
             Stats* stats = nullptr) {
    if (parts.shadow) PaintShadow(target, rect, variant, stats);
    if (parts.fill) PaintFill(target, rect, variant, parts, stats);
    if (parts.rim) PaintRim(target, rect, variant, parts, stats);
    ++panelPaintCount_;
    if (stats != nullptr) ++stats->panels;
  }

  static float ClampFillAlpha(float alpha) {
    if (!(alpha >= kFillAlphaMin)) return kFillAlphaMin;
    if (alpha > kFillAlphaMax) return kFillAlphaMax;
    return alpha;
  }

  int ShadowPaints() const { return shadowPaintCount_; }
  int FillPaints() const { return fillPaintCount_; }
  int RimPaints() const { return rimPaintCount_; }
  int PanelPaints() const { return panelPaintCount_; }
  void ResetCounters() {
    shadowPaintCount_ = 0;
    fillPaintCount_ = 0;
    rimPaintCount_ = 0;
    panelPaintCount_ = 0;
  }

 private:
  Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
  ID2D1SolidColorBrush* brush_ = nullptr;  // borrowed, not owned
  int shadowPaintCount_ = 0;
  int fillPaintCount_ = 0;
  int rimPaintCount_ = 0;
  int panelPaintCount_ = 0;
};

}  // namespace prism::win::glass
