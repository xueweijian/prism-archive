// PrismArchive -- PrismWin
// FlyoutSurface: the REAL pixels of the inline-preview flyout (card C48).
//
// C30 shipped the companion as a lifecycle shell: WS_EX_NOACTIVATE,
// WS_EX_NOREDIRECTIONBITMAP, hit-testing and placement -- but no glass. That was
// deliberate (the window has no redirection surface BY DESIGN), and this header
// is the other half: the pixels come from a DirectComposition island, exactly as
// ADR-001 requires.
//
// Two layers, on purpose:
//
//   * the LAYOUT + CONTENT layer (Metrics, Scene, PaintContent) is pure D2D and
//     has no window, no D3D and no composition in it. That is what makes the
//     pixels testable headlessly: Tests/win/test_flyout_paint.cpp renders the
//     SAME PaintContent into a WIC bitmap and asserts colours, baselines and
//     corner coverage. A test that re-implemented the painting would prove
//     nothing about the shipping code.
//
//   * the COMPOSITION layer (InitializeComposition) owns the D3D11 device, the
//     IDCompositionDevice, one rounded-rectangle clip and one content visual
//     carrying a premultiplied composition swapchain. It draws by calling the
//     content layer into that swapchain, so there is exactly one painter.
//
// Contracts this file is written against:
//   * C15 tokens are the ONLY colour source (no literals): the surface is
//     --surface-floating-island -- the same token the main window's floating
//     islands use -- with --border-specular-rim as the rim and
//     --accent-liquid-primary for the hover pill and folder glyphs.
//   * The corner is the C15 SQUIRCLE (|x/a|^n + |y/b|^n = 1, n = 4.2), not a
//     circular arc: theme::SquircleField is the classification the pixel suite
//     samples, so the exponent (and its C15 negative control) stays owned by the
//     token header.
//   * Geometry is DPI-scaled (MetricsForDpi), never a constant: at 96 dpi the
//     metrics reproduce C30's kHeaderHeight/kRowHeight exactly, which is what
//     lets the flyout's hit-test and its pixels share one frame.
//   * Placement is C30's ComputeFlyoutRect -- NOT re-implemented here. This file
//     only feeds it the size the content actually needs (rows -> height, width
//     fixed at 360).
//
// Negative controls (CONTRACTS D3, compiled in -- never a text edit):
//   PRISM_C48_MUTANT_SQUARE_CORNER      the surface is painted as a plain
//                                       rectangle: the corner assertions MUST go
//                                       red.
//   PRISM_C48_MUTANT_FIXED_ROW_HEIGHT   the row pitch ignores DPI: the scaling
//                                       assertions MUST go red.
// Each injection counts its own executions (SquareCornerFills), which is how the
// verdict predicate proves the defect really fired instead of merely compiling.
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
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

// The header is header-only and every consumer needs these import libraries --
// CompanionWindow.hpp includes it, and the C30 targets' link lines (owned by
// another card) have no d3d11/dcomp/d2d1 on them. Asking each CMake target to
// remember is how a link error appears only on the target nobody rebuilt.
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../PrismCore/ThemeTokens.hpp"
#include "CompanionPreviewBridge.hpp"  // C30: ComputeFlyoutRect, kMaxPreviewEntries

namespace prism::win::flyout {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Metrics: the ONE geometry authority for the flyout (paint, hit-test and
// placement all read it). Base values are the 96-dpi design numbers; every one
// of them is scaled by dpi/96 except the width, which the blueprint pins at 360.
// ---------------------------------------------------------------------------
inline constexpr int kWidthDip = 360;  // blueprint ch. 4.1: fixed width
inline constexpr int kMaxRows = static_cast<int>(companion::kMaxPreviewEntries);  // 12
inline constexpr int kBaseHeaderHeight = 24;
inline constexpr int kBaseRowHeight = 18;
inline constexpr int kBaseRadius = 14;      // the main window's island radius
inline constexpr int kBaseFontSize = 13;
inline constexpr int kBaseSmallFontSize = 11;
inline constexpr int kBaseIconSize = 12;
inline constexpr int kBaseTextLeft = 30;    // icon column + gap
inline constexpr int kBaseSizeWidth = 76;

inline int ClampRows(int rows) {
  if (rows < 0) return 0;
  return rows > kMaxRows ? kMaxRows : rows;
}

struct Metrics {
  float scale = 1.0f;
  int width = kWidthDip;  // never scaled: a fixed-width flyout is the design
  int headerHeight = kBaseHeaderHeight;
  int rowHeight = kBaseRowHeight;
  int radius = kBaseRadius;
  int fontSize = kBaseFontSize;
  int smallFontSize = kBaseSmallFontSize;
  int iconSize = kBaseIconSize;
  int textLeft = kBaseTextLeft;
  int sizeWidth = kBaseSizeWidth;

  // rows -> height. The header is always there (it names the archive) and the
  // list is clipped to kMaxRows, so the flyout never grows past 12 rows.
  int HeightForRows(int rows) const { return headerHeight + ClampRows(rows) * rowHeight; }
  int RowTop(int row) const { return headerHeight + row * rowHeight; }
  // Baseline offset inside a row: exactly one font size below the row top. The
  // text layouts are built with UNIFORM line spacing pinned to this value, so
  // the baseline is a property of the metrics -- not of the font's own ascent,
  // which would drift with fallback fonts.
  int Ascent() const { return fontSize; }
  int RowBaseline(int row) const { return RowTop(row) + Ascent(); }
  int SizeColumnLeft() const { return width - sizeWidth; }
};

inline Metrics MetricsForDpi(int dpi) {
  Metrics m;
  if (dpi < 48) dpi = 48;
  if (dpi > 480) dpi = 480;
  m.scale = static_cast<float>(dpi) / 96.0f;
#if defined(PRISM_C48_MUTANT_FIXED_ROW_HEIGHT)
  // MUTATION (c48-flyoutpaint.cmake): the row pitch stops tracking the DPI, so
  // the flyout is the same height at 96 and 192 dpi. The suite's "pitch scales
  // with dpi" and "12 rows are 9 pitches taller than 3" assertions MUST go red.
  m.rowHeight = kBaseRowHeight;
#else
  m.rowHeight = static_cast<int>(std::lround(kBaseRowHeight * m.scale));
#endif
  m.headerHeight = static_cast<int>(std::lround(kBaseHeaderHeight * m.scale));
  m.radius = static_cast<int>(std::lround(kBaseRadius * m.scale));
  m.fontSize = static_cast<int>(std::lround(kBaseFontSize * m.scale));
  m.smallFontSize = static_cast<int>(std::lround(kBaseSmallFontSize * m.scale));
  m.iconSize = static_cast<int>(std::lround(kBaseIconSize * m.scale));
  m.textLeft = static_cast<int>(std::lround(kBaseTextLeft * m.scale));
  m.sizeWidth = static_cast<int>(std::lround(kBaseSizeWidth * m.scale));
  return m;
}

// ---------------------------------------------------------------------------
// Model. A row is what the flyout shows, never how it is painted.
// ---------------------------------------------------------------------------
struct Row {
  std::wstring name;
  uint64_t sizeBytes = 0;
  bool directory = false;
};

struct Scene {
  std::wstring title;     // archive path / kind line (the header)
  std::wstring subtitle;  // "12 items" (secondary, right aligned)
  std::vector<Row> rows;  // clipped to kMaxRows when painted
  prism::theme::Variant variant = prism::theme::Variant::Daylight;
  int dpi = 96;
  int hover = -1;

  Metrics metrics() const { return MetricsForDpi(dpi); }
  int VisibleRows() const { return ClampRows(static_cast<int>(rows.size())); }
  int Height() const { return metrics().HeightForRows(VisibleRows()); }
};

// "1.2 MB" / "840 B" / "folder" -- the size column's text. Table-free and
// deterministic, because the pixel suite measures where this text lands.
inline std::wstring SizeLabel(const Row& row) {
  if (row.directory) return L"folder";
  const uint64_t bytes = row.sizeBytes;
  const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
  int unit = 0;
  double value = static_cast<double>(bytes);
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  wchar_t buffer[32]{};
  if (unit == 0) {
    swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
  } else {
    swprintf_s(buffer, L"%.1f %s", value, units[unit]);
  }
  return std::wstring(buffer);
}

// ---------------------------------------------------------------------------
// Colour. Every value is a C15 token; the alpha is kept because the island is
// composited over the desktop by DirectComposition (premultiplied swapchain).
// ---------------------------------------------------------------------------
inline D2D1_COLOR_F ToD2D(const prism::theme::Rgba& c) {
  D2D1_COLOR_F out;
  out.r = c.r;
  out.g = c.g;
  out.b = c.b;
  out.a = c.a;
  return out;
}

inline prism::theme::Rgba TokenColor(prism::theme::TokenId id,
                                     prism::theme::Variant variant) {
  return prism::theme::Lookup(id, variant).color;
}

// The flyout's surface IS the main window's floating-island token.
inline prism::theme::Rgba SurfaceColor(prism::theme::Variant variant) {
  return TokenColor(prism::theme::TokenId::SurfaceFloatingIsland, variant);
}
// The canvas the island floats over (composite of the two surface tokens). The
// suite paints it as the opaque backdrop before drawing the island.
inline prism::theme::Rgba CanvasColor(prism::theme::Variant variant) {
  return prism::theme::CompositeOver(
      TokenColor(prism::theme::TokenId::SurfaceCanvasBase, variant),
      TokenColor(prism::theme::TokenId::SurfaceBackdropReference, variant));
}
inline prism::theme::Rgba RimColor(prism::theme::Variant variant) {
  return prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant)
      .gradient.from;
}
inline prism::theme::Rgba AccentColor(prism::theme::Variant variant) {
  return prism::theme::Lookup(prism::theme::TokenId::AccentLiquidPrimary, variant)
      .gradient.from;
}
inline prism::theme::Rgba WithAlpha(const prism::theme::Rgba& c, float alpha) {
  return prism::theme::Rgba(c.r, c.g, c.b, alpha);
}

// ---------------------------------------------------------------------------
// D3 evidence: how many times the square-corner injection actually painted. The
// mutant's verdict predicate reads this, so "the injection compiled in" is not
// mistaken for "the injection fired".
// ---------------------------------------------------------------------------
inline int& SquareCornerFills() {
  static int fills = 0;
  return fills;
}

// ---------------------------------------------------------------------------
// Squircle path (G2 continuous corner). The boundary is solved against
// theme::SquircleField, so the exponent -- and its C15 negative control -- stay
// owned by the token header instead of being a second 4.2 literal.
//
// NOTE: GalleryRenderer and DialogChrome each carry their own copy today. This
// one is deliberately standalone: V2 is rewriting GalleryRenderer and the dialog
// card owns DialogChrome, so a shared header cannot land from this card without
// touching both. When one of them lands, these collapse into it.
// ---------------------------------------------------------------------------
inline HRESULT CreateSquirclePath(ID2D1Factory* factory, const D2D1_RECT_F& rect, float radius,
                                  ID2D1PathGeometry** out) {
  if (out == nullptr) return E_POINTER;
  *out = nullptr;
  if (factory == nullptr) return E_POINTER;
  const float w = rect.right - rect.left;
  const float h = rect.bottom - rect.top;
  if (!(w > 0.0f) || !(h > 0.0f)) return E_INVALIDARG;
  const float r = (std::max)(0.0f, (std::min)(radius, (std::min)(w, h) * 0.5f));
  if (!(r > 0.0f)) return E_INVALIDARG;

  ComPtr<ID2D1PathGeometry> path;
  HRESULT hr = factory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
  if (FAILED(hr)) return hr;
  ComPtr<ID2D1GeometrySink> sink;
  hr = path->Open(sink.ReleaseAndGetAddressOf());
  if (FAILED(hr)) return hr;

  constexpr int kCornerSegments = 8;
  constexpr double kPi = 3.14159265358979323846;
  // One point of a corner quadrant: walk the direction (cos t, sin t) out to the
  // root of the squircle field (bisection, 20 steps -- nothing at 60 Hz).
  const auto cornerPoint = [r](float cx, float cy, float dx, float dy, double t) {
    const double ct = std::cos(t);
    const double st = std::sin(t);
    double lo = 0.0;
    double hi = 1.6;
    for (int i = 0; i < 20; ++i) {
      const double mid = (lo + hi) * 0.5;
      if (prism::theme::SquircleField(ct * mid * r, st * mid * r, r, r) <= 0.0) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    const double s = (lo + hi) * 0.5;
    return D2D1::Point2F(cx + dx * static_cast<float>(ct * s * r),
                         cy + dy * static_cast<float>(st * s * r));
  };

  const float left = rect.left;
  const float top = rect.top;
  const float right = rect.right;
  const float bottom = rect.bottom;
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

// Placement: C30 owns the geometry (right-of-click, right-edge flip, clamp).
// This only hands it the size the CONTENT needs -- rows drive the height, the
// width stays 360 -- so there is no second copy of the flip rule.
inline companion::FlyoutRect Placement(POINT cursorScreen, RECT workArea, int rows, int dpi,
                                       int32_t gap = companion::kFlyoutGap) {
  const Metrics m = MetricsForDpi(dpi);
  SIZE size{};
  size.cx = m.width;
  size.cy = m.HeightForRows(ClampRows(rows));
  return companion::ComputeFlyoutRect(cursorScreen, size, workArea, gap);
}

// ---------------------------------------------------------------------------
// FlyoutSurface: the painter (headless-capable) plus the DirectComposition
// island. Initialize() creates only the factories, so a test can render pixels
// without a window; InitializeComposition() adds the D3D/DComp half and is the
// only part that can fail on a machine without composition.
// ---------------------------------------------------------------------------
class FlyoutSurface {
 public:
  // ---- factories (no window, no GPU) -----------------------------------
  HRESULT Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   factory_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(
                                   writeFactory_.ReleaseAndGetAddressOf()));
  }

  ID2D1Factory* Factory() const { return factory_.Get(); }
  IDWriteFactory* TextFactory() const { return writeFactory_.Get(); }

  // ---- per-target resources --------------------------------------------
  // Text formats are built at the metrics' sizes with UNIFORM line spacing
  // pinned to the row pitch and its baseline, so the baseline is a metric and
  // not a font property (fallback fonts must not move the rows).
  HRESULT CreateDeviceResources(ID2D1RenderTarget* target, int dpi) {
    if (target == nullptr) return E_POINTER;
    const Metrics m = MetricsForDpi(dpi);
    dpi_ = dpi;
    const prism::theme::Variant variant = prism::theme::Variant::Daylight;
    HRESULT hr = target->CreateSolidColorBrush(ToD2D(SurfaceColor(variant)),
                                               surfaceBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(ToD2D(RimColor(variant)),
                                       rimBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(ToD2D(AccentColor(variant)),
                                       accentBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(
        ToD2D(TokenColor(prism::theme::TokenId::TextPrimary, variant)),
        primaryBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(
        ToD2D(TokenColor(prism::theme::TokenId::TextSecondary, variant)),
        secondaryBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    titleFormat_.Reset();
    rowFormat_.Reset();
    sizeFormat_.Reset();
    smallFormat_.Reset();
    hr = writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         static_cast<float>(m.fontSize), L"en-US",
                                         titleFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         static_cast<float>(m.fontSize), L"en-US",
                                         rowFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         static_cast<float>(m.smallFontSize), L"en-US",
                                         sizeFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         static_cast<float>(m.smallFontSize), L"en-US",
                                         smallFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    const auto pin = [](IDWriteTextFormat* format, float lineHeight, float baseline) {
      if (format == nullptr) return;
      format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      // UNIFORM: the baseline sits exactly "baseline" below the line top, so
      // the row's baseline is a METRIC and not a property of whichever font
      // DirectWrite ended up using.
      format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight, baseline);
    };
    pin(titleFormat_.Get(), static_cast<float>(m.headerHeight),
        static_cast<float>(m.headerHeight) * 0.62f);
    pin(rowFormat_.Get(), static_cast<float>(m.rowHeight), static_cast<float>(m.Ascent()));
    pin(sizeFormat_.Get(), static_cast<float>(m.rowHeight), static_cast<float>(m.Ascent()));
    pin(smallFormat_.Get(), static_cast<float>(m.rowHeight), static_cast<float>(m.Ascent()));
    if (sizeFormat_) sizeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (smallFormat_) smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    return S_OK;
  }

  // ---- the content layer (what the pixel suite renders) ----------------
  void PaintContent(ID2D1RenderTarget* target, const Scene& scene) {
    if (target == nullptr || !surfaceBrush_ || !rowFormat_) return;
    const Metrics m = scene.metrics();
    const int rows = scene.VisibleRows();
    const float width = static_cast<float>(m.width);
    const float height = static_cast<float>(m.HeightForRows(rows));
    const D2D1_RECT_F body = D2D1::RectF(0.0f, 0.0f, width, height);

    // (1) The island surface: the floating-island token at its own alpha, so
    // DirectComposition blends it over the desktop exactly like the main
    // window's islands.
    surfaceBrush_->SetColor(ToD2D(SurfaceColor(scene.variant)));
#if defined(PRISM_C48_MUTANT_SQUARE_CORNER)
    // MUTATION (c48-flyoutpaint.cmake): the G2 corner becomes a right angle.
    target->FillRectangle(body, surfaceBrush_.Get());
    ++SquareCornerFills();  // D3: proof the injection PAINTED, not just compiled
#else
    FillSquircle(target, body, static_cast<float>(m.radius), surfaceBrush_.Get());
#endif

    // (2) The specular rim: the token's top-edge alpha, one hairline thick.
    if (rimBrush_) {
      rimBrush_->SetColor(ToD2D(RimColor(scene.variant)));
      StrokeSquircle(target, body, static_cast<float>(m.radius), rimBrush_.Get(),
                     (std::max)(1.0f, m.scale));
    }

    // (3) The header: the archive name plus the row count. A hairline separates
    // it from the list -- that line is what makes the header a band instead of
    // a first row.
    if (!scene.title.empty() && titleFormat_) {
      const D2D1_RECT_F header = D2D1::RectF(static_cast<float>(m.textLeft) * 0.5f, 0.0f,
                                             width - 8.0f * m.scale,
                                             static_cast<float>(m.headerHeight));
      primaryBrush_->SetColor(
          ToD2D(TokenColor(prism::theme::TokenId::TextPrimary, scene.variant)));
      target->DrawText(scene.title.c_str(), static_cast<UINT32>(scene.title.size()),
                        titleFormat_.Get(), header, primaryBrush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }
    if (!scene.subtitle.empty() && sizeFormat_ && secondaryBrush_) {
      const D2D1_RECT_F sub = D2D1::RectF(width - static_cast<float>(m.sizeWidth), 0.0f,
                                          width - 8.0f * m.scale,
                                          static_cast<float>(m.headerHeight));
      secondaryBrush_->SetColor(
          ToD2D(TokenColor(prism::theme::TokenId::TextSecondary, scene.variant)));
      target->DrawText(scene.subtitle.c_str(), static_cast<UINT32>(scene.subtitle.size()),
                        sizeFormat_.Get(), sub, secondaryBrush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }
    if (rimBrush_) {
      rimBrush_->SetColor(ToD2D(WithAlpha(RimColor(scene.variant), 0.10f)));
      target->FillRectangle(
          D2D1::RectF(static_cast<float>(m.textLeft) * 0.5f,
                      static_cast<float>(m.headerHeight) - (std::max)(1.0f, m.scale),
                      width - static_cast<float>(m.textLeft) * 0.5f,
                      static_cast<float>(m.headerHeight)),
          rimBrush_.Get());
    }

    // (4) The rows: hover pill, icon, name, size. Every row is a band of exactly
    // m.rowHeight, and its baseline is m.RowBaseline(row) -- the pixel suite
    // measures that.
    for (int i = 0; i < rows; ++i) {
      const Row& row = scene.rows[static_cast<size_t>(i)];
      const float top = static_cast<float>(m.RowTop(i));
      const float bottom = top + static_cast<float>(m.rowHeight);
      if (i == scene.hover && accentBrush_) {
        accentBrush_->SetColor(ToD2D(WithAlpha(AccentColor(scene.variant), 0.18f)));
        FillSquircle(target,
                     D2D1::RectF(4.0f * m.scale, top + 1.0f * m.scale,
                                 width - 4.0f * m.scale, bottom - 1.0f * m.scale),
                     static_cast<float>(m.radius) * 0.5f, accentBrush_.Get());
      }

      // Icon: a squircle tile in the accent (folder) or the secondary text tone
      // (file). Vector, so it stays crisp at every DPI.
      const float icon = static_cast<float>(m.iconSize);
      const float iconLeft = 8.0f * m.scale;
      const float iconTop = top + (static_cast<float>(m.rowHeight) - icon) * 0.5f;
      if (accentBrush_ && secondaryBrush_) {
        ID2D1SolidColorBrush* brush = row.directory ? accentBrush_.Get() : secondaryBrush_.Get();
        brush->SetColor(ToD2D(row.directory
                                  ? AccentColor(scene.variant)
                                  : WithAlpha(TokenColor(prism::theme::TokenId::TextSecondary,
                                                         scene.variant),
                                              0.55f)));
        FillSquircle(target, D2D1::RectF(iconLeft, iconTop, iconLeft + icon, iconTop + icon),
                     icon * 0.28f, brush);
      }

      const float textLeft = static_cast<float>(m.textLeft);
      const float nameRight = static_cast<float>(m.SizeColumnLeft()) - 4.0f * m.scale;
      if (primaryBrush_ && rowFormat_ && nameRight > textLeft) {
        primaryBrush_->SetColor(
            ToD2D(TokenColor(prism::theme::TokenId::TextPrimary, scene.variant)));
        target->DrawText(row.name.c_str(), static_cast<UINT32>(row.name.size()),
                          rowFormat_.Get(), D2D1::RectF(textLeft, top, nameRight, bottom),
                          primaryBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                          DWRITE_MEASURING_MODE_NATURAL);
      }
      if (secondaryBrush_ && sizeFormat_) {
        const std::wstring size = SizeLabel(row);
        secondaryBrush_->SetColor(
            ToD2D(TokenColor(prism::theme::TokenId::TextSecondary, scene.variant)));
        target->DrawText(size.c_str(), static_cast<UINT32>(size.size()), sizeFormat_.Get(),
                          D2D1::RectF(static_cast<float>(m.SizeColumnLeft()), top,
                                      width - 8.0f * m.scale, bottom),
                          secondaryBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                          DWRITE_MEASURING_MODE_NATURAL);
      }
    }
  }

  // ---- the composition layer -------------------------------------------
  // Creates the D3D11 device (hardware, then WARP), the D2D device context, the
  // IDCompositionDevice and the visual tree:
  //
  //     target -> root visual (rounded-rect CLIP) -> content visual (swapchain)
  //
  // The clip is a SUPERSET of the painted squircle on purpose: a circular clip
  // at the same radius would shave the G2 bulge the C29 suite exists to protect.
  bool InitializeComposition(HWND hwnd, int dpi) {
    if (hwnd == nullptr) return false;
    compositionError_ = S_OK;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, d3d_.ReleaseAndGetAddressOf(), nullptr,
                                   nullptr);
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                             d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    }
    if (FAILED(hr)) {
      compositionError_ = hr;
      return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3d_.As(&dxgiDevice))) return false;
    if (!d2dDevice_) {
      ComPtr<ID2D1Factory1> factory1;
      if (FAILED(factory_.As(&factory1))) return false;
      if (FAILED(factory1->CreateDevice(dxgiDevice.Get(),
                                        d2dDevice_.ReleaseAndGetAddressOf()))) {
        return false;
      }
    }
    if (!d2dContext_ && FAILED(d2dDevice_->CreateDeviceContext(
                             D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                             d2dContext_.ReleaseAndGetAddressOf()))) {
      return false;
    }

    if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice),
                                        reinterpret_cast<void**>(
                                            dcomp_.ReleaseAndGetAddressOf())))) {
      return false;
    }
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd, /*topmost=*/TRUE,
                                           target_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (FAILED(dcomp_->CreateVisual(root_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(dcomp_->CreateVisual(content_.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(target_->SetRoot(root_.Get()))) return false;

    const Metrics m = MetricsForDpi(dpi);
    dpi_ = dpi;
    // The rounded-rectangle clip: the visual tree's shape safety net. Radii are
    // circular by DComp's model, so they are inflated past the squircle.
    if (FAILED(dcomp_->CreateRectangleClip(clip_.ReleaseAndGetAddressOf()))) return false;
    const float clipRadius = static_cast<float>(m.radius) * 1.25f;
    clip_->SetLeft(0.0f);
    clip_->SetTop(0.0f);
    clip_->SetRight(static_cast<float>(m.width));
    clip_->SetBottom(static_cast<float>(m.HeightForRows(kMaxRows)));
    clip_->SetTopLeftRadiusX(clipRadius);
    clip_->SetTopLeftRadiusY(clipRadius);
    clip_->SetTopRightRadiusX(clipRadius);
    clip_->SetTopRightRadiusY(clipRadius);
    clip_->SetBottomLeftRadiusX(clipRadius);
    clip_->SetBottomLeftRadiusY(clipRadius);
    clip_->SetBottomRightRadiusX(clipRadius);
    clip_->SetBottomRightRadiusY(clipRadius);
    if (FAILED(root_->SetClip(clip_.Get()))) return false;
    if (FAILED(root_->AddVisual(content_.Get(), FALSE, nullptr))) return false;

    if (!CreateSwapchain(m.width, m.HeightForRows(kMaxRows))) return false;
    if (FAILED(content_->SetContent(swapchain_.Get()))) return false;
    // The brushes/text formats belong to the swapchain's D2D target: without
    // them PaintContent is a no-op, which would look exactly like "the island
    // is there but empty". Paint() re-creates them when the DPI changes.
    if (FAILED(CreateDeviceResources(d2dContext_.Get(), dpi))) return false;
    if (FAILED(dcomp_->Commit())) return false;
    compositionReady_ = true;
    return true;
  }

  bool UsesComposition() const { return compositionReady_; }
  HRESULT CompositionError() const { return compositionError_; }

  // Resizes the island to exactly the rows it shows. Returns false when
  // composition is unavailable (the caller keeps its fallback).
  bool Resize(int width, int height) {
    if (!compositionReady_ || width <= 0 || height <= 0) return false;
    if (width == surfaceWidth_ && height == surfaceHeight_) return true;
    if (!CreateSwapchain(width, height)) return false;
    if (FAILED(content_->SetContent(swapchain_.Get()))) return false;
    if (clip_) {
      clip_->SetRight(static_cast<float>(width));
      clip_->SetBottom(static_cast<float>(height));
    }
    return SUCCEEDED(dcomp_->Commit());
  }

  // Renders one frame into the composition swapchain: clear to transparent (the
  // island's corners must show the desktop), paint the content, present, commit.
  bool Paint(const Scene& scene) {
    if (!compositionReady_ || !d2dContext_ || !d2dBitmap_) return false;
    const Metrics m = scene.metrics();
    if (scene.dpi != dpi_ || !surfaceBrush_ || !rowFormat_) {
      if (FAILED(CreateDeviceResources(d2dContext_.Get(), scene.dpi))) return false;
    }
    if (!Resize(m.width, m.HeightForRows(scene.VisibleRows()))) return false;
    d2dContext_->SetTarget(d2dBitmap_.Get());
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));  // premultiplied transparent
    PaintContent(d2dContext_.Get(), scene);
    if (FAILED(d2dContext_->EndDraw())) return false;
    if (FAILED(swapchain_->Present(1, 0))) return false;
    return SUCCEEDED(dcomp_->Commit());
  }

  // ---- introspection (the suite asserts against these, not internals) ---
  size_t VisualCount() const {
    size_t count = 0;
    if (root_) ++count;
    if (content_) ++count;
    return count;
  }
  bool HasRoundedClip() const { return clip_ != nullptr; }
  float ClipRadius() const {
    return clip_ != nullptr ? static_cast<float>(MetricsForDpi(dpi_).radius) * 1.25f : 0.0f;
  }
  bool SwapchainLive() const { return swapchain_ != nullptr; }
  // The per-target brushes/text formats exist: PaintContent would silently do
  // nothing without them, so the suite asserts this instead of trusting it.
  bool ContentReady() const { return surfaceBrush_ != nullptr && rowFormat_ != nullptr; }
  int SurfaceWidth() const { return surfaceWidth_; }
  int SurfaceHeight() const { return surfaceHeight_; }
  int Dpi() const { return dpi_; }

 private:
  void FillSquircle(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                    ID2D1Brush* brush) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(CreateSquirclePath(factory_.Get(), rect, radius,
                                  geometry.ReleaseAndGetAddressOf()))) {
      // No path: a plain rectangle is still legible, and a failed paint must
      // never abort the frame. The pixel suite's corner probes catch it.
      target->FillRectangle(rect, brush);
      return;
    }
    target->FillGeometry(geometry.Get(), brush);
  }

  void StrokeSquircle(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                      ID2D1Brush* brush, float strokeWidth) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(CreateSquirclePath(factory_.Get(), rect, radius,
                                  geometry.ReleaseAndGetAddressOf()))) {
      return;
    }
    target->DrawGeometry(geometry.Get(), brush, strokeWidth);
  }

  bool CreateSwapchain(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    d2dBitmap_.Reset();
    swapchain_.Reset();
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                  reinterpret_cast<void**>(
                                      factory.ReleaseAndGetAddressOf())))) {
      return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;  // ADR-001
    if (FAILED(factory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr,
                                                      swapchain_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapchain_->GetBuffer(0, __uuidof(IDXGISurface),
                                     reinterpret_cast<void**>(
                                         surface.ReleaseAndGetAddressOf())))) {
      return false;
    }
    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(
            surface.Get(), props, d2dBitmap_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    return true;
  }

  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> writeFactory_;

  // Per-target resources (rebuilt whenever the target or the DPI changes).
  ComPtr<ID2D1SolidColorBrush> surfaceBrush_;
  ComPtr<ID2D1SolidColorBrush> rimBrush_;
  ComPtr<ID2D1SolidColorBrush> accentBrush_;
  ComPtr<ID2D1SolidColorBrush> primaryBrush_;
  ComPtr<ID2D1SolidColorBrush> secondaryBrush_;
  ComPtr<IDWriteTextFormat> titleFormat_;
  ComPtr<IDWriteTextFormat> rowFormat_;
  ComPtr<IDWriteTextFormat> sizeFormat_;
  ComPtr<IDWriteTextFormat> smallFormat_;
  int dpi_ = 96;

  // Composition.
  ComPtr<ID3D11Device> d3d_;
  ComPtr<ID2D1Device> d2dDevice_;
  ComPtr<ID2D1DeviceContext> d2dContext_;
  ComPtr<IDCompositionDevice> dcomp_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> root_;
  ComPtr<IDCompositionVisual> content_;
  ComPtr<IDCompositionRectangleClip> clip_;
  ComPtr<IDXGISwapChain1> swapchain_;
  ComPtr<ID2D1Bitmap1> d2dBitmap_;
  bool compositionReady_ = false;
  HRESULT compositionError_ = S_OK;
  int surfaceWidth_ = 0;
  int surfaceHeight_ = 0;
};

}  // namespace prism::win::flyout
