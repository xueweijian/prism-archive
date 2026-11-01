// PrismArchive -- PrismWin
// The gallery FRAME renderer: draws the complete Liquid Glass design scene
// (full-bleed grid, floating breadcrumb island, progress capsule, Quick Look
// card) onto ANY ID2D1RenderTarget.
//
// Why a separate renderer instead of drawing inside the window class:
//  (a) the CI snapshot test renders the SAME code to a WIC bitmap (headless)
//      and ships the PNG as an artifact -- design review without a desktop,
//  (b) the window class adopts this next batch; one renderer, two hosts.
//
// All DECISIONS come from PrismCore (ViewportWindow / ProgressHud /
// QuickLookPlanner); this file only paints what they say.
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

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/GlassIsland.hpp"
#include "../PrismCore/ListLayout.hpp"
#include "../PrismCore/PathSafety.hpp"  // FileNameOf
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/ProgressHud.hpp"
#include "../PrismCore/SidebarModel.hpp"
#include "../PrismCore/ThemeTokens.hpp"
#include "../PrismCore/ThumbStore.hpp"
#include "SidebarView.hpp"  // C12 draw commands -- painted since C27, refined by C29

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

#if defined(PRISM_C27_MUTANT_NO_SIDEBAR_PAINT)
inline constexpr bool kPaintSidebar = false;
#else
inline constexpr bool kPaintSidebar = true;
#endif

class GalleryRenderer {
 public:
  // The design tokens (iOS 26 dark glass language).
  static constexpr D2D1_COLOR_F kBackdrop = {0.043f, 0.055f, 0.075f, 1.0f};   // #0B0E13
  static constexpr D2D1_COLOR_F kTileFill = {1.0f, 1.0f, 1.0f, 0.06f};        // glass pane
  static constexpr D2D1_COLOR_F kTileBorder = {1.0f, 1.0f, 1.0f, 0.12f};
  static constexpr D2D1_COLOR_F kIslandFill = {0.10f, 0.12f, 0.16f, 0.72f};   // floating glass
  static constexpr D2D1_COLOR_F kCardFill = {0.086f, 0.10f, 0.13f, 0.90f};    // Quick Look body
  static constexpr D2D1_COLOR_F kIslandBorder = {1.0f, 1.0f, 1.0f, 0.16f};
  static constexpr D2D1_COLOR_F kAccent = {0.243f, 0.545f, 1.0f, 1.0f};       // #3E8BFF
  static constexpr D2D1_COLOR_F kTextPrimary = {0.910f, 0.925f, 0.945f, 1.0f};
  static constexpr D2D1_COLOR_F kTextSecondary = {0.604f, 0.655f, 0.706f, 1.0f};

  // ------------------------------------------------------------ C29 sidebar
  // The rail is a real COLUMN: when it is shown, list content starts after it.
  // Both numbers below are layout POLICY, not paint, so they live next to the
  // painter that applies them and are pinned by their own unit test.
  static constexpr float kSidebarColumnWidth = 236.0f;    // SidebarView::Metrics::width
  static constexpr float kSidebarCollapseBelow = 960.0f;  // 4 list columns + chrome

  // Negative-control switch (CONTRACTS D3): the rail is never painted, so the
  // suite's "the commands reached pixels" assertions MUST go red.
#if defined(PRISM_C29_MUTANT_NO_SIDEBAR_PAINT)
  static constexpr bool kSidebarPaintDisabled = true;
#else
  static constexpr bool kSidebarPaintDisabled = false;
#endif
  // ... and the width fold inverted (960 px and wider folds, narrower shows).
#if defined(PRISM_C29_MUTANT_COLLAPSE_INVERTED)
  static constexpr bool kSidebarCollapseInverted = true;
#else
  static constexpr bool kSidebarCollapseInverted = false;
#endif

  static constexpr std::string_view ActiveMutation() {
    if (kSidebarPaintDisabled) return "no-sidebar-paint";
    if (kSidebarCollapseInverted) return "collapse-inverted";
    return {};
  }

  // Pure function of the client width: no hysteresis, no hidden state, so the
  // suite pins the threshold exactly (959 -> folded, 960 -> shown). This is the
  // window's auto-fold POLICY; the renderer paints whatever Scene it is handed,
  // because C27's glue suite builds a 900 px scene and expects pixels.
  static bool SidebarFits(float windowWidth) {
    const bool roomEnough = windowWidth >= kSidebarCollapseBelow;
    return kSidebarCollapseInverted ? !roomEnough : roomEnough;
  }
  // Pixels the list content must leave free on the left while the rail shows.
  static float SidebarInset(float windowWidth) {
    return SidebarFits(windowWidth) ? kSidebarColumnWidth : 0.0f;
  }

  struct Scene {
    std::vector<prism::ArchiveItem>* items = nullptr;
    prism::gallery::ViewportWindow* viewport = nullptr;
    prism::ProgressHud::View hud;
    bool quickLookOpen = false;
    std::wstring quickLookTitle;
    std::wstring quickLookMeta;
    std::wstring breadcrumb;

    // Batch 3: real thumbnails from the solid sweep (nullptr -> gradient
    // placeholders), the glass island model (nullptr -> static islands),
    // and the main-window list mode.
    int selectedItem = -1;  // grid-mode selection (accent border)

    prism::gallery::ThumbStore* thumbs = nullptr;
    prism::glass::GlassIslandScene* islands = nullptr;
    // C27: the sidebar is a LIST OF DRAW COMMANDS (C12's Layout). The renderer
    // paints them; it never derives one. nullptr -> no rail (the pre-C27 scene,
    // which the snapshot tool and older tests still build).
    const prism::win::sidebar::Layout* sidebar = nullptr;
    bool listMode = false;
    std::vector<prism::list::SortItem>* listItems = nullptr;
    uint32_t listScroll = 0;
    int selectedRow = -1;
    // C13 columns, persisted by the app (C27): 0/absent -> the module default.
    const prism::list::ColumnLayout* listColumns = nullptr;
    bool hasListColumns = false;
    // C29: the explicit fold (the width fold is GalleryWindow's policy through
    // SidebarFits) and the theme whose design tokens the rail resolves. Both
    // default to the shipping values, so a pre-C29 scene is unchanged.
    bool sidebarCollapsed = false;
    prism::theme::Variant sidebarTheme = prism::theme::Variant::Midnight;
  };

  HRESULT Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   factory_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(writeFactory_.ReleaseAndGetAddressOf()));
  }

  ID2D1Factory* Factory() const { return factory_.Get(); }

  // Per-target brush creation (call once per render target).
  HRESULT CreateDeviceResources(ID2D1RenderTarget* target) {
    HRESULT hr = target->CreateSolidColorBrush(kAccent, accent_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTileFill, tile_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTileBorder, tileBorder_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kIslandFill, island_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kCardFill, card_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kIslandBorder, islandBorder_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTextPrimary, textPrimary_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTextSecondary, textSecondary_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    // Sunset-ish gradient palette for photo placeholders (design stand-ins
    // until the ring-buffer WIC decode pipeline lands).
    const D2D1_COLOR_F stops[][2] = {
        {{0.99f, 0.62f, 0.28f, 0.95f}, {0.86f, 0.26f, 0.36f, 0.95f}},  // sunset
        {{0.24f, 0.66f, 0.78f, 0.95f}, {0.16f, 0.30f, 0.62f, 0.95f}},  // lagoon
        {{0.58f, 0.42f, 0.95f, 0.95f}, {0.26f, 0.20f, 0.55f, 0.95f}},  // dusk
    };
    for (int i = 0; i < 3; ++i) {
      const D2D1_GRADIENT_STOP gs[2] = {{0.0f, stops[i][0]}, {1.0f, stops[i][1]}};
      ComPtr<ID2D1GradientStopCollection> collection;
      hr = target->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2,
                                                D2D1_EXTEND_MODE_CLAMP,
                                                collection.ReleaseAndGetAddressOf());
      if (FAILED(hr)) return hr;
      hr = target->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 200)),
          collection.Get(), thumbs_[i].ReleaseAndGetAddressOf());
      if (FAILED(hr)) return hr;
    }

    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    13.0f, L"en-US", caption_.ReleaseAndGetAddressOf());
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    15.0f, L"en-US", title_.ReleaseAndGetAddressOf());
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    12.0f, L"en-US", small_.ReleaseAndGetAddressOf());
    if (caption_) caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (title_) title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // C29: one recolourable brush for the rail (dozens of tiny runs; a brush per
    // token would churn device state) and two single-line text formats -- a
    // wrapped label would spill into its neighbour and be clipped mid-glyph.
    hr = target->CreateSolidColorBrush(kTextPrimary, sidebarBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    13.0f, L"en-US", sidebarLabel_.ReleaseAndGetAddressOf());
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    11.0f, L"en-US", sidebarSmall_.ReleaseAndGetAddressOf());
    if (sidebarLabel_) {
      sidebarLabel_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      sidebarLabel_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (sidebarSmall_) sidebarSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return S_OK;
  }

  void Draw(ID2D1RenderTarget* target, const Scene& scene) {
    const D2D1_SIZE_F size = target->GetSize();
    target->Clear(kBackdrop);

    // C29: the rail is part of the LAYOUT. While it is shown the list content
    // starts AFTER the column, so folding it returns a full column of row width
    // (asserted in pixels by win.sidebar_paint and the snapshot).
    const float inset =
        (SidebarShown(scene) && SidebarFits(size.width)) ? kSidebarColumnWidth : 0.0f;
    // The grid is only drawn when a viewport AND its items exist: a scene may be
    // a sidebar-only composition (the C27 glue suite and the C29 suites both
    // build one), and DrawGrid dereferences the viewport. The pre-C29 version
    // painted the grid twice in grid mode; the guard makes the call total.
    if (scene.listMode) {
      DrawListMode(target, scene, size, inset);
    } else if (scene.viewport && scene.items) {
      DrawGrid(target, scene, size);
    }
    // NEGATIVE CONTROL (CONTRACTS D3): the rail is never painted, so the suite's
    // "the command list reached pixels" assertion MUST go red.
    if (kPaintSidebar) DrawSidebar(target, scene, size);
    DrawBreadcrumbIsland(target, scene, size);
    DrawProgressCapsule(target, scene, size);
    if (scene.quickLookOpen) DrawQuickLookCard(target, scene, size);
  }

  // -------------------------------------------------------------------------
  // The sidebar rail (C12's commands; painted since C27, refined by C29).
  //
  // Everything below CONSUMES the command list: nothing here decides where a row
  // goes, which row is focused or what the capsule says -- that is
  // SidebarView::Build's job and C12's suite already proves it. What this code
  // adds is the last mile:
  //   * colours come from C15's design tokens (never raw literals),
  //   * the scrolled content is clipped between contentTop and contentBottom,
  //   * the telemetry capsule stays anchored to the viewport bottom,
  //   * corners are the C15 SQUIRCLE (theme::SquircleField), not a circular arc,
  //   * icon ids become vector glyphs, so a snapshot shows the real rail.
  // The counters are the negative control's evidence (CONTRACTS D3): the mutant
  // build never paints, so "the commands reached pixels" MUST go red.
  // -------------------------------------------------------------------------
  static bool SidebarShown(const Scene& scene) {
    if (kSidebarPaintDisabled) return false;
    return scene.sidebar != nullptr && !scene.sidebarCollapsed;
  }

  static D2D1_COLOR_F ToD2d(const prism::theme::Rgba& c) {
    D2D1_COLOR_F out;
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = c.a;
    return out;
  }

  // ColorToken -> concrete RGBA from the token table (C15). The rail's panel is
  // the floating-island surface at near-opaque alpha, and the telemetry capsule
  // is the recessed canvas pane, which is what makes the capsule legible on the
  // panel instead of a same-colour box.
  static prism::theme::Rgba TokenColor(prism::win::sidebar::ColorToken token,
                                       prism::theme::Variant variant) {
    using prism::theme::TokenId;
    const auto color = [variant](TokenId id) {
      return prism::theme::Lookup(id, variant).color;
    };
    const auto liquidFrom = [variant]() {
      return prism::theme::Lookup(TokenId::AccentLiquidPrimary, variant).gradient.from;
    };
    switch (token) {
      case prism::win::sidebar::ColorToken::Panel: {
        prism::theme::Rgba c = color(TokenId::SurfaceFloatingIsland);
        c.a = 0.94f;
        return c;
      }
      case prism::win::sidebar::ColorToken::SectionLabel:
        return color(TokenId::TextSecondary);
      case prism::win::sidebar::ColorToken::PrimaryText:
        return color(TokenId::TextPrimary);
      case prism::win::sidebar::ColorToken::SecondaryText:
        return color(TokenId::TextSecondary);
      case prism::win::sidebar::ColorToken::SelectedBackground: {
        prism::theme::Rgba c = liquidFrom();
        c.a = 0.32f;
        return c;
      }
      case prism::win::sidebar::ColorToken::Accent:
      case prism::win::sidebar::ColorToken::Locked: {
        prism::theme::Rgba c = liquidFrom();
        c.a = 1.0f;
        return c;
      }
      case prism::win::sidebar::ColorToken::Muted: {
        prism::theme::Rgba c = color(TokenId::TextSecondary);
        c.a = 0.62f;
        return c;
      }
    }
    return color(TokenId::TextPrimary);
  }

  // Rounded rectangle whose corners are the C15 SQUIRCLE (|x|^n + |y|^n = 1),
  // not a circular arc: the boundary is solved against theme::SquircleField, so
  // the exponent -- and its negative control -- stay owned by the token header.
  // A hardcoded 4.2 here would be a second, silently divergent design source.
  static HRESULT CreateSquircleRoundedRect(ID2D1RenderTarget* target,
                                           const D2D1_RECT_F& rect, float radius,
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

    // One point of a corner quadrant: direction (cos t, sin t), distance the
    // root of the squircle field along it (bisection, 20 steps, 6 points per
    // corner -- nothing at 60 Hz).
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

  void FillSquircleRoundedRect(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                               float radius, ID2D1Brush* brush) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(CreateSquircleRoundedRect(target, rect, radius,
                                         geometry.ReleaseAndGetAddressOf()))) {
      // No path: a plain rectangle is still legible, and a failed paint must
      // never abort the whole frame.
      target->FillRectangle(rect, brush);
      return;
    }
    target->FillGeometry(geometry.Get(), brush);
  }

  // Icon ids, not bitmaps (C12 contract): tiny vector geometry keeps the
  // snapshot crisp and needs no asset pipeline at 16 px.
  void DrawSidebarGlyph(ID2D1RenderTarget* target, const prism::win::sidebar::Box& box,
                        std::string_view icon, const D2D1_COLOR_F& color) {
    if (!sidebarBrush_ || box.Empty()) return;
    sidebarBrush_->SetColor(color);
    ID2D1Brush* brush = sidebarBrush_.Get();
    const float l = box.left, t = box.top, r = box.right, b = box.bottom;
    const float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
    const float w = r - l, h = b - t;

    if (icon == "chevron-right" || icon == "chevron-down") {
      if (icon == "chevron-right") {
        target->DrawLine(D2D1::Point2F(l + w * 0.32f, t + h * 0.22f),
                         D2D1::Point2F(l + w * 0.62f, cy), brush, 1.6f);
        target->DrawLine(D2D1::Point2F(l + w * 0.62f, cy),
                         D2D1::Point2F(l + w * 0.32f, b - h * 0.22f), brush, 1.6f);
      } else {
        target->DrawLine(D2D1::Point2F(l + w * 0.22f, t + h * 0.34f),
                         D2D1::Point2F(cx, t + h * 0.66f), brush, 1.6f);
        target->DrawLine(D2D1::Point2F(cx, t + h * 0.66f),
                         D2D1::Point2F(r - w * 0.22f, t + h * 0.34f), brush, 1.6f);
      }
      return;
    }
    if (icon == "folder") {
      target->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(l + w * 0.10f, t + h * 0.20f, l + w * 0.54f,
                                        t + h * 0.40f), 1.5f, 1.5f), brush);
      target->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(l + w * 0.10f, t + h * 0.30f, r - w * 0.10f,
                                        b - h * 0.16f), 2.5f, 2.5f), brush);
      return;
    }
    if (icon == "clock") {
      target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w * 0.40f, h * 0.40f),
                          brush, 1.4f);
      target->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx, t + h * 0.24f), brush, 1.4f);
      target->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(r - w * 0.24f, cy), brush, 1.4f);
      return;
    }
    if (icon == "home") {
      target->DrawLine(D2D1::Point2F(l + w * 0.10f, t + h * 0.52f),
                       D2D1::Point2F(cx, t + h * 0.16f), brush, 1.5f);
      target->DrawLine(D2D1::Point2F(cx, t + h * 0.16f),
                       D2D1::Point2F(r - w * 0.10f, t + h * 0.52f), brush, 1.5f);
      target->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(l + w * 0.24f, t + h * 0.48f, r - w * 0.24f,
                                        b - h * 0.12f), 1.5f, 1.5f), brush);
      return;
    }
    if (icon == "download") {
      target->DrawLine(D2D1::Point2F(cx, t + h * 0.14f), D2D1::Point2F(cx, t + h * 0.64f),
                       brush, 1.5f);
      target->DrawLine(D2D1::Point2F(cx - w * 0.22f, t + h * 0.44f),
                       D2D1::Point2F(cx, t + h * 0.66f), brush, 1.5f);
      target->DrawLine(D2D1::Point2F(cx + w * 0.22f, t + h * 0.44f),
                       D2D1::Point2F(cx, t + h * 0.66f), brush, 1.5f);
      target->FillRectangle(
          D2D1::RectF(l + w * 0.18f, b - h * 0.18f, r - w * 0.18f, b - h * 0.10f), brush);
      return;
    }
    if (icon == "star") {
      const D2D1_POINT_2F north = D2D1::Point2F(cx, t + h * 0.10f);
      const D2D1_POINT_2F east = D2D1::Point2F(r - w * 0.10f, cy);
      const D2D1_POINT_2F south = D2D1::Point2F(cx, b - h * 0.10f);
      const D2D1_POINT_2F west = D2D1::Point2F(l + w * 0.10f, cy);
      target->DrawLine(north, east, brush, 1.5f);
      target->DrawLine(east, south, brush, 1.5f);
      target->DrawLine(south, west, brush, 1.5f);
      target->DrawLine(west, north, brush, 1.5f);
      return;
    }
    if (icon == "lock") {
      target->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(l + w * 0.24f, t + h * 0.46f, r - w * 0.24f,
                                        b - h * 0.12f), 2.0f, 2.0f), brush);
      target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, t + h * 0.44f), w * 0.22f,
                                        h * 0.26f), brush, 1.4f);
      return;
    }
    if (icon == "bolt") {
      target->DrawLine(D2D1::Point2F(r - w * 0.26f, t + h * 0.14f),
                       D2D1::Point2F(l + w * 0.32f, cy), brush, 1.6f);
      target->DrawLine(D2D1::Point2F(l + w * 0.32f, cy),
                       D2D1::Point2F(r - w * 0.32f, cy), brush, 1.6f);
      target->DrawLine(D2D1::Point2F(r - w * 0.32f, cy),
                       D2D1::Point2F(l + w * 0.26f, b - h * 0.14f), brush, 1.6f);
      return;
    }
    if (icon == "chart") {
      const float heights[3] = {0.36f, 0.66f, 0.90f};
      for (int i = 0; i < 3; ++i) {
        const float x = l + w * (0.16f + 0.28f * static_cast<float>(i));
        const float top = b - h * 0.14f - (h * 0.68f) * heights[i];
        target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x, top, x + w * 0.18f, b - h * 0.14f), 1.0f,
                              1.0f), brush);
      }
      return;
    }
    if (icon == "empty") {
      target->DrawRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(l + w * 0.18f, t + h * 0.18f, r - w * 0.18f,
                                        b - h * 0.18f), 2.5f, 2.5f), brush, 1.4f);
      return;
    }
    // Unknown id: a token-driven plate, never an invisible row.
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(l + w * 0.20f, t + h * 0.20f, r - w * 0.20f,
                                      b - h * 0.20f), 2.5f, 2.5f), brush);
  }

  static void DrawSidebarText(ID2D1RenderTarget* target, IDWriteTextFormat* format,
                              const std::string& text, const D2D1_RECT_F& box,
                              ID2D1Brush* brush, float nudgeY, float nudgeX = 0.0f) {
    if (!format || text.empty()) return;
    const std::wstring wide = Utf8(text);
    if (wide.empty()) return;
    target->DrawText(wide.c_str(), static_cast<UINT32>(wide.size()), format,
                     D2D1::RectF(box.left + nudgeX, box.top + nudgeY, box.right,
                                 box.bottom + nudgeY),
                     brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  static bool IsAnchoredSidebarCommand(prism::win::sidebar::PrimKind kind) {
    using prism::win::sidebar::PrimKind;
    switch (kind) {
      case PrimKind::Panel:
      case PrimKind::Separator:
      case PrimKind::StatusPill:
      case PrimKind::StatusIcon:
      case PrimKind::StatusText:
      case PrimKind::RatioBar:
        return true;
      default:
        return false;
    }
  }

  void PaintSidebarCommand(ID2D1RenderTarget* target,
                           const prism::win::sidebar::Command& command,
                           prism::theme::Variant variant) {
    using prism::win::sidebar::PrimKind;
    if (!sidebarBrush_) return;
    const D2D1_RECT_F box = D2D1::RectF(command.box.left, command.box.top,
                                        command.box.right, command.box.bottom);
    sidebarBrush_->SetColor(ToD2d(TokenColor(command.color, variant)));
    switch (command.kind) {
      case PrimKind::Panel:
      case PrimKind::RowHit:
        return;  // the panel is painted up front; a hit region paints nothing
      case PrimKind::SectionLabel:
        ++dbgSidebarLabels;
        DrawSidebarText(target, sidebarSmall_.Get(), command.text, box,
                        sidebarBrush_.Get(), 2.0f);
        return;
      case PrimKind::Selection:
        // The focus pill: squircle radius 8 (blueprint 3.2 row highlight).
        FillSquircleRoundedRect(target, box, 8.0f, sidebarBrush_.Get());
        return;
      case PrimKind::Glyph:
      case PrimKind::Chevron:
      case PrimKind::StatusIcon:
        DrawSidebarGlyph(target, command.box, command.icon,
                         ToD2d(TokenColor(command.color, variant)));
        return;
      case PrimKind::Label:
      case PrimKind::EmptyState:
        ++dbgSidebarLabels;
        DrawSidebarText(target, sidebarLabel_.Get(), command.text, box,
                        sidebarBrush_.Get(), 6.0f, 2.0f);
        return;
      case PrimKind::Separator:
        target->FillRectangle(box, sidebarBrush_.Get());
        return;
      case PrimKind::StatusPill: {
        // A RECESSED pane, not a second floating one: the canvas surface over
        // the column's island fill. That is also what makes the capsule's
        // squircle corner legible instead of a same-colour rectangle.
        prism::theme::Rgba pane =
            prism::theme::Lookup(prism::theme::TokenId::SurfaceCanvasBase, variant).color;
        pane.a = 0.95f;
        sidebarBrush_->SetColor(ToD2d(pane));
        FillSquircleRoundedRect(target, box, 14.0f, sidebarBrush_.Get());
        prism::theme::Rgba rim =
            prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant)
                .gradient.from;
        rim.a = 0.16f;
        sidebarBrush_->SetColor(ToD2d(rim));
        ComPtr<ID2D1PathGeometry> outline;
        if (SUCCEEDED(CreateSquircleRoundedRect(target, box, 14.0f,
                                                outline.ReleaseAndGetAddressOf()))) {
          target->DrawGeometry(outline.Get(), sidebarBrush_.Get(), 1.0f);
        }
        return;
      }
      case PrimKind::StatusText:
        ++dbgSidebarLabels;
        DrawSidebarText(target, sidebarSmall_.Get(), command.text, box,
                        sidebarBrush_.Get(), 3.0f, 2.0f);
        return;
      case PrimKind::RatioBar:
        target->FillRoundedRectangle(D2D1::RoundedRect(box, 1.5f, 1.5f),
                                     sidebarBrush_.Get());
        return;
    }
  }

  void DrawSidebar(ID2D1RenderTarget* target, const Scene& scene,
                   const D2D1_SIZE_F& size) {
    if (!SidebarShown(scene)) {
      if (scene.sidebar != nullptr) ++dbgSidebarCollapsed;
      return;
    }
    if (!sidebarBrush_) return;
    const prism::win::sidebar::Layout& layout = *scene.sidebar;
    const prism::theme::Variant variant = scene.sidebarTheme;
    using prism::win::sidebar::ColorToken;
    const float left = layout.panel.left;
    const float right = layout.panel.right;
    const float top = (std::max)(layout.panel.top, 0.0f);
    const float bottom = (std::min)(layout.panel.bottom, size.height);
    if (right <= left || bottom <= top) return;

    // 1. the column itself, plus the specular hairline that separates it from
    //    the content without a hard black line.
    sidebarBrush_->SetColor(ToD2d(TokenColor(ColorToken::Panel, variant)));
    target->FillRectangle(D2D1::RectF(left, top, right, bottom), sidebarBrush_.Get());
    prism::theme::Rgba rim =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant)
            .gradient.from;
    rim.a = 0.22f;
    sidebarBrush_->SetColor(ToD2d(rim));
    target->FillRectangle(D2D1::RectF(right - 1.0f, top, right, bottom),
                          sidebarBrush_.Get());
    ++dbgSidebarPanels;

    // 2. the scrolled content, clipped to [contentTop, contentBottom]. The
    //    layout already CULLED what is fully outside (C12); this clip is what
    //    makes a partially visible row slide under the header and behind the
    //    anchored capsule instead of bleeding over them.
    const D2D1_RECT_F contentClip =
        D2D1::RectF(left, layout.contentTop, right, layout.contentBottom);
    bool clipped = false;
    for (const prism::win::sidebar::Command& command : layout.commands) {
      if (command.kind == prism::win::sidebar::PrimKind::RowHit) continue;  // hit only
      if (command.box.top >= bottom || command.box.bottom <= top) {
        ++dbgSidebarClipped;
        continue;  // outside the client area entirely
      }
      ++dbgSidebarCommands;
      if (!IsAnchoredSidebarCommand(command.kind)) {
        if (command.box.bottom <= contentClip.top ||
            command.box.top >= contentClip.bottom) {
          ++dbgSidebarClipped;
          continue;  // outside the scroll window: nothing to paint
        }
        if (!clipped) {
          target->PushAxisAlignedClip(contentClip, D2D1_ANTIALIAS_MODE_ALIASED);
          clipped = true;
        }
      } else if (clipped) {
        target->PopAxisAlignedClip();
        clipped = false;
      }
      PaintSidebarCommand(target, command, variant);
      ++dbgSidebarPrimitives;
      ++dbgSidebarPainted;
    }
    if (clipped) target->PopAxisAlignedClip();
  }

 private:
  static std::wstring Utf8(std::string_view s) {
    if (s.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), wide);
    return out;
  }

  void DrawGrid(ID2D1RenderTarget* target, const Scene& scene, const D2D1_SIZE_F& size) {
    const auto range = scene.viewport->Visible(2);
    if (range.Empty()) return;
    const int columns = scene.viewport->Columns();
    const float cellW = static_cast<float>(scene.viewport->ColumnWidth());
    const float cellH = static_cast<float>(scene.viewport->CellHeight());
    const float scroll = static_cast<float>(scene.viewport->Scroll());
    constexpr float kInset = 8.0f;
    constexpr float kRadius = 16.0f;

    for (int index = range.first; index <= range.last; ++index) {
      if (index >= static_cast<int>(scene.items->size())) break;
      const auto& item = (*scene.items)[index];
      const int col = index % columns;
      const int row = index / columns;
      const float left = static_cast<float>(col) * cellW + kInset;
      const float top = static_cast<float>(row) * cellH - scroll + kInset;
      const float right = static_cast<float>(col + 1) * cellW - kInset;
      const float bottom = static_cast<float>(row + 1) * cellH - scroll - kInset;
      if (bottom < 0 || top > size.height) continue;

      if (item.isDir) {
        // Folder tile: glass pane + simple vector folder glyph.
        const D2D1_ROUNDED_RECT pane =
            D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), kRadius, kRadius);
        target->FillRoundedRectangle(pane, tile_.Get());
        target->DrawRoundedRectangle(pane, tileBorder_.Get(), 1.0f);
        const float cx = (left + right) / 2.0f;
        const float cy = (top + bottom) / 2.0f - 14.0f;
        D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
            D2D1::RectF(cx - 34.0f, cy - 22.0f, cx + 34.0f, cy + 24.0f), 7.0f, 7.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
        folder = D2D1::RoundedRect(D2D1::RectF(cx - 34.0f, cy - 30.0f, cx + 6.0f, cy - 14.0f),
                                   6.0f, 6.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
      } else if (prism::QuickLookPlanner::KindByName(item.path) == prism::PreviewKind::Image) {
        // Photo tile: REAL thumbnail when the sweep harvested one, the ⚡
        // streaming badge for over-limit blocks, gradient stand-in otherwise.
        // Everything is clipped by a squircle layer: DrawBitmap ignores the
        // rounded rect, so without the mask a wide photo bleeds past the
        // tile's corners (visible immediately in the snapshot).
        const D2D1_RECT_F photoRect = D2D1::RectF(left, top, right, bottom - 30.0f);
        const prism::gallery::DecodedThumb* thumb =
            scene.thumbs ? scene.thumbs->Lookup(item.index) : nullptr;
        if (thumb) ++dbgThumbLookupHits;

        ComPtr<ID2D1PathGeometry> clipGeometry;
        if (SUCCEEDED(CreateSquircleGeometry(target, photoRect, kRadius,
                                             clipGeometry.ReleaseAndGetAddressOf()))) {
          D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters(
              photoRect, clipGeometry.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
          target->PushLayer(layer, nullptr);
        }

        if (thumb) {
          SyncThumbBitmap(target, item.index, *thumb, scene.thumbs->Version());
          const auto it = thumbBitmaps_.find(item.index);
          if (it != thumbBitmaps_.end() && it->second) {
            ++dbgThumbBitmaps;
            const float areaW = right - left;
            const float areaH = bottom - 30.0f - top;
            const float scale = (std::min)(areaW / thumb->width, areaH / thumb->height);
            const float drawW = thumb->width * scale;
            const float drawH = thumb->height * scale;
            const D2D1_RECT_F fit = D2D1::RectF(left + (areaW - drawW) / 2.0f,
                                                top + (areaH - drawH) / 2.0f,
                                                left + (areaW + drawW) / 2.0f,
                                                top + (areaH + drawH) / 2.0f);
            target->FillRectangle(photoRect, tile_.Get());
            ++dbgThumbDraws;
            target->DrawBitmap(it->second.Get(), fit, 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
          }
        } else if (item.size > prism::QuickLookPlanner::kDefaultByteLimit) {
          // Over-budget solid block: elegant "tap to stream" badge.
          thumbs_[index % 3]->SetStartPoint(D2D1::Point2F(left, top));
          thumbs_[index % 3]->SetEndPoint(D2D1::Point2F(left, bottom - 30.0f));
          target->FillRectangle(photoRect, thumbs_[index % 3].Get());
          DrawLabel(target, caption_.Get(), L"\x26A1 \x70B9\x51FB\x6D41\x5F0F\x52A0\x8F7D",
                    D2D1::RectF(left, top + (bottom - top) / 2.0f - 12.0f, right,
                                top + (bottom - top) / 2.0f + 12.0f),
                    textPrimary_.Get());
        } else {
          thumbs_[index % 3]->SetStartPoint(D2D1::Point2F(left, top));
          thumbs_[index % 3]->SetEndPoint(D2D1::Point2F(left, bottom - 30.0f));
          target->FillRectangle(photoRect, thumbs_[index % 3].Get());
        }
        if (clipGeometry) target->PopLayer();

        if (static_cast<int>(item.index) == scene.selectedItem) {
          // Selection ring: 2px accent, squircle-ish corners stay readable.
          islandBorder_->SetColor(kAccent);
          D2D1_ROUNDED_RECT ring =
              D2D1::RoundedRect(D2D1::RectF(left - 1.0f, top - 1.0f, right + 1.0f,
                                            bottom + 1.0f), kRadius, kRadius);
          target->DrawRoundedRectangle(ring, islandBorder_.Get(), 2.0f);
          islandBorder_->SetColor(kIslandBorder);
        }
        // Name plate under the photo (the grid is unusable without labels).
        const D2D1_ROUNDED_RECT plate = D2D1::RoundedRect(
            D2D1::RectF(left, bottom - 28.0f, right, bottom), 10.0f, 10.0f);
        target->FillRoundedRectangle(plate, tile_.Get());
        DrawLabel(target, caption_.Get(), Utf8(FileNameOf(item.path)),
                  D2D1::RectF(left + 8.0f, bottom - 26.0f, right - 8.0f, bottom - 4.0f),
                  textPrimary_.Get());
      } else {
        // Document tile: paper pane + text lines (the doc quick look reads
        // exactly this silhouette).
        D2D1_ROUNDED_RECT paper = D2D1::RoundedRect(
            D2D1::RectF(left, top, right, bottom - 30.0f), kRadius, kRadius);
        target->FillRoundedRectangle(paper, island_.Get());
        target->DrawRoundedRectangle(paper, tileBorder_.Get(), 1.0f);
        const float lineTop = top + 24.0f;
        const float lineW[4] = {0.62f, 0.84f, 0.74f, 0.50f};
        for (int line = 0; line < 4; ++line) {
          const float w = (right - left - 32.0f) * lineW[line];
          D2D1_ROUNDED_RECT textLine = D2D1::RoundedRect(
              D2D1::RectF(left + 16.0f, lineTop + line * 22.0f, left + 16.0f + w,
                          lineTop + line * 22.0f + 7.0f),
              3.5f, 3.5f);
          target->FillRoundedRectangle(textLine, line == 0 ? accent_.Get() : tileBorder_.Get());
        }

        const D2D1_ROUNDED_RECT pane =
            D2D1::RoundedRect(D2D1::RectF(left, bottom - 28.0f, right, bottom), 10.0f, 10.0f);
        target->FillRoundedRectangle(pane, tile_.Get());
        const std::wstring name = Utf8(FileNameOf(item.path));
        DrawLabel(target, caption_.Get(), name, D2D1::RectF(left + 8.0f, bottom - 26.0f,
                                                            right - 8.0f, bottom - 4.0f),
                  textPrimary_.Get());
      }
    }
  }

  // Device-bitmap cache for decoded thumbnails; invalidated exactly when the
  // ThumbStore mutates (Version()), never per frame.
  void SyncThumbBitmap(ID2D1RenderTarget* target, uint32_t itemIndex,
                       const prism::gallery::DecodedThumb& thumb, uint64_t version) {
    if (version != thumbVersion_) {
      thumbBitmaps_.clear();
      thumbVersion_ = version;
    }
    auto& slot = thumbBitmaps_[itemIndex];
    if (slot) return;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(target->CreateBitmap(D2D1::SizeU(thumb.width, thumb.height),
                                    thumb.pixels.data(),
                                    thumb.width * 4, props,
                                    slot.ReleaseAndGetAddressOf()))) {
      slot.Reset();
    }
  }

  // Squircle (G2 continuous corner) clip path for a photo tile. D2D1 has no
  // "clip to rounded rect" on DrawBitmap, so the tile pushes a geometry
  // layer instead; superellipse exponent 4.2 per the design tokens.
  static HRESULT CreateSquircleGeometry(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                                        float radius, ID2D1PathGeometry** out) {
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.ReleaseAndGetAddressOf());
    ComPtr<ID2D1PathGeometry> path;
    HRESULT hr = factory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1GeometrySink> sink;
    hr = path->Open(sink.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float halfW = (rect.right - rect.left) / 2.0f;
    const float halfH = (rect.bottom - rect.top) / 2.0f;
    (void)radius;
    // Superellipse sampled as a polyline; 32 segments per quadrant is well
    // under a pixel of sag at tile sizes and keeps the mask cheap. `radius`
    // only feeds the fallback assumption that the tile is at least as tall
    // as its corner radius (asserted by the caller's geometry).
    constexpr int kSegments = 32;
    constexpr float kExponent = 4.2f;
    constexpr float kPi = 3.14159265358979f;
    const auto pointAt = [&](float t) {
      // Parameterize one quadrant of the superellipse: |x/a|^n + |y/b|^n = 1
      const float c = std::cos(t);
      const float s = std::sin(t);
      const float ex = std::pow(std::fabs(c), 2.0f / kExponent);
      const float ey = std::pow(std::fabs(s), 2.0f / kExponent);
      return D2D1::Point2F(cx + (c >= 0 ? ex : -ex) * halfW,
                           cy + (s >= 0 ? ey : -ey) * halfH);
    };
    sink->BeginFigure(pointAt(0.0f), D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i <= kSegments * 4; ++i) {
      const float t = (2.0f * kPi) * (static_cast<float>(i) / (kSegments * 4));
      sink->AddLine(pointAt(t));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    if (FAILED(hr)) return hr;
    *out = path.Detach();
    return S_OK;
  }

  static D2D1_COLOR_F BarColorOf(prism::list::BarColor color) {
    switch (color) {
      case prism::list::BarColor::Teal: return {0.0f, 0.780f, 0.745f, 1.0f};   // #00C7BE
      case prism::list::BarColor::Blue: return {0.0f, 0.478f, 1.0f, 1.0f};     // #007AFF
      case prism::list::BarColor::Green: return {0.204f, 0.780f, 0.349f, 1.0f}; // #34C759
      case prism::list::BarColor::Amber: return {1.0f, 0.584f, 0.0f, 1.0f};    // #FF9500
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
  }

  // The main-window file list (blueprint ch. 3.2): 名称 | 原始大小 | 压缩后 |
  // 压缩率, 36px virtualized rows, semantic micro-capsule bars, folders first.
  void DrawListMode(ID2D1RenderTarget* target, const Scene& scene,
                    const D2D1_SIZE_F& size, float inset) {
    if (!scene.listItems) return;
    const auto& rows = *scene.listItems;
    // The app persists the user's column geometry (C27); absent means the module
    // default, which is what every pre-C27 call site gets.
    const prism::list::ColumnLayout cols =
        scene.hasListColumns && scene.listColumns != nullptr ? *scene.listColumns
                                                             : prism::list::ColumnLayout{};
    const float headerY = 68.0f;
    const float footerY = size.height - 56.0f;
    const float contentTop = headerY + 36.0f;
    const uint32_t viewportH =
        static_cast<uint32_t>((std::max)(0.0f, footerY - contentTop));

    // Header
    // `inset` is the rail's column (C29): every LEFT-anchored edge moves with
    // it while the right-anchored columns stay put, so no column shifts.
    DrawLabel(target, caption_.Get(), L"\x540D\x79F0 ^", D2D1::RectF(24.0f + inset, headerY,
                                                cols.sizeX(size.width) - 12.0f,
                                                headerY + 26.0f), textSecondary_.Get());
    DrawTrailingLabel(target, L"\x539F\x59CB\x5927\x5C0F", size.width - cols.sizeW - cols.packedW - cols.ratioW + 6,
                      headerY, cols.sizeW - 18, textSecondary_.Get());
    DrawTrailingLabel(target, L"\x538B\x7F29\x540E", size.width - cols.packedW - cols.ratioW + 6,
                      headerY, cols.packedW - 18, textSecondary_.Get());
    DrawTrailingLabel(target, L"\x538B\x7F29\x7387", size.width - cols.ratioW + 6, headerY,
                      cols.ratioW - 18, textSecondary_.Get());

    const auto window =
        prism::list::RowWindow(scene.listScroll, viewportH, static_cast<uint32_t>(rows.size()));
    for (uint32_t i = window.first; i <= window.last && i < rows.size(); ++i) {
      const auto& row = rows[i];
      const float top = contentTop + static_cast<float>(i) * prism::list::kRowHeight -
                        static_cast<float>(scene.listScroll);
      const float bottom = top + prism::list::kRowHeight;
      if (bottom < contentTop || top > footerY) continue;

      if (static_cast<int>(i) == scene.selectedRow) {
        // Selection highlight: G2 squircle R=10, accent fluid fill.
        D2D1_ROUNDED_RECT sel =
            D2D1::RoundedRect(D2D1::RectF(16.0f + inset, top + 3.0f, size.width - 16.0f,
                                          bottom - 3.0f),
                              10.0f, 10.0f);
        D2D1_COLOR_F selColor = kAccent;
        selColor.a = 0.26f;
        accent_->SetColor(selColor);
        target->FillRoundedRectangle(sel, accent_.Get());
        accent_->SetColor(kAccent);
      }

      const std::wstring name = Utf8(row.name);
      if (row.isDir) {
        const D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
            D2D1::RectF(28.0f + inset, top + 9.0f, 60.0f + inset, top + 27.0f), 5.0f, 5.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
        DrawLabel(target, caption_.Get(), name, D2D1::RectF(70.0f + inset, top + 5.0f,
                      cols.sizeX(size.width) - 80.0f, bottom - 4.0f), textPrimary_.Get());
        DrawTrailingLabel(target, L"--", size.width - cols.sizeW - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.sizeW - 18, textSecondary_.Get());
        DrawTrailingLabel(target, L"--", size.width - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.packedW - 18, textSecondary_.Get());
      } else {
        DrawLabel(target, caption_.Get(), name, D2D1::RectF(44.0f + inset, top + 5.0f,
                      cols.sizeX(size.width) - 52.0f, bottom - 4.0f), textPrimary_.Get());
        const std::wstring sizeText = Utf8(prism::list::FormatBytes(row.size));
        const std::wstring packedText = Utf8(prism::list::FormatBytes(row.packedSize));
        DrawTrailingLabel(target, sizeText, size.width - cols.sizeW - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.sizeW - 18, textPrimary_.Get());
        DrawTrailingLabel(target, packedText, size.width - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.packedW - 18, textPrimary_.Get());
      }

      // Semantic micro-capsule ratio bar (blueprint ch. 3.2.3).
      if (const auto bar = prism::list::RatioBar(row)) {
        const float trackX = size.width - cols.ratioW + 8.0f;
        const float trackW = cols.ratioW - 64.0f;
        const float cy = top + prism::list::kRowHeight / 2.0f;
        D2D1_ROUNDED_RECT track =
            D2D1::RoundedRect(D2D1::RectF(trackX, cy - 3.0f, trackX + trackW, cy + 3.0f),
                              3.0f, 3.0f);
        tileBorder_->SetColor(kTileBorder);
        target->FillRoundedRectangle(track, tileBorder_.Get());
        D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
            D2D1::RectF(trackX, cy - 3.0f, trackX + trackW * bar->percent / 100.0f, cy + 3.0f),
            3.0f, 3.0f);
        tileBorder_->SetColor(BarColorOf(bar->color));
        target->FillRoundedRectangle(fill, tileBorder_.Get());
        tileBorder_->SetColor(kTileBorder);
        wchar_t pct[8];
        swprintf_s(pct, L"%d%%", bar->percent);
        DrawTrailingLabel(target, pct, size.width - cols.ratioW + 8.0f + trackW + 6.0f, top + 6.0f,
                          40, textSecondary_.Get());
      }
    }
  }

  void DrawTrailingLabel(ID2D1RenderTarget* target, const std::wstring& text, float x,
                         float y, float w, ID2D1Brush* brush) {
    if (!caption_ || text.empty()) return;
    caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawLabel(target, caption_.Get(), text, D2D1::RectF(x, y, x + w, y + 24.0f), brush);
    caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }

  void DrawBreadcrumbIsland(ID2D1RenderTarget* target, const Scene& scene,
                            const D2D1_SIZE_F& size) {
    float width = size.width * 0.62f;
    float left = (size.width - width) / 2.0f;
    float top = 16.0f;
    float height = 44.0f;
    if (scene.islands) {
      // The island model owns the geometry (blueprint ch. 3.2); the renderer
      // only paints what the scene computed.
      const auto r = scene.islands->RectOf(prism::glass::IslandId::Nav);
      left = static_cast<float>(r.x);
      top = static_cast<float>(r.y);
      width = static_cast<float>(r.w);
      height = static_cast<float>(r.h);
    }
    const D2D1_ROUNDED_RECT pill =
        D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height),
                          height / 2.0f, height / 2.0f);
    target->FillRoundedRectangle(pill, island_.Get());
    target->DrawRoundedRectangle(pill, islandBorder_.Get(), 1.0f);
    DrawLabel(target, title_.Get(), scene.breadcrumb,
              D2D1::RectF(left + 24.0f, top + 10.0f, left + width - 24.0f, top + 34.0f),
              textPrimary_.Get());
    if (scene.islands) {
      // Pointer-tracked specular rim (blueprint ch. 3.1 Layer 5): a bright
      // segment flows along the top edge toward the cursor. In the real
      // pipeline this comes from the HLSL shader; the software snapshot
      // approximates it from the SAME cbuffer params (pointer xy).
      float cb[20];
      scene.islands->WriteCbuffer(prism::glass::IslandId::Nav,
                                  static_cast<uint32_t>(size.width),
                                  static_cast<uint32_t>(size.height), cb);
      const float px = (std::min)((std::max)(cb[4], left + 70.0f), left + width - 70.0f);
      D2D1_COLOR_F rim = {1.0f, 1.0f, 1.0f, 0.62f};
      islandBorder_->SetColor(rim);
      D2D1_ROUNDED_RECT glow = D2D1::RoundedRect(
          D2D1::RectF(px - 62.0f, top + 1.0f, px + 62.0f, top + 3.0f), 1.0f, 1.0f);
      target->FillRoundedRectangle(glow, islandBorder_.Get());
      islandBorder_->SetColor(kIslandBorder);
    }
  }

  void DrawProgressCapsule(ID2D1RenderTarget* target, const Scene& scene,
                           const D2D1_SIZE_F& size) {
    if (scene.hud.phase == prism::HudPhase::Hidden) return;
    const float width = 260.0f, height = 46.0f;
    const float left = (size.width - width) / 2.0f;
    const float top = size.height - height - 22.0f;
    const D2D1_ROUNDED_RECT pill =
        D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height), height / 2.0f,
                          height / 2.0f);
    target->FillRoundedRectangle(pill, island_.Get());
    target->DrawRoundedRectangle(pill, islandBorder_.Get(), 1.0f);

    // Progress track + fill (the HUD model owns the percent; -1 == spin).
    const float trackInset = 10.0f;
    const float trackW = width - 2.0f * trackInset - 56.0f;
    D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
        D2D1::RectF(left + trackInset, top + height / 2.0f - 3.0f,
                    left + trackInset + trackW, top + height / 2.0f + 3.0f),
        3.0f, 3.0f);
    target->FillRoundedRectangle(track, tileBorder_.Get());
    if (scene.hud.percent >= 0) {
      const float fillW = trackW * scene.hud.percent / 100.0f;
      if (fillW > 1.0f) {
        D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
            D2D1::RectF(track.rect.left, track.rect.top, track.rect.left + fillW,
                        track.rect.bottom),
            3.0f, 3.0f);
        target->FillRoundedRectangle(fill, accent_.Get());
      }
    }
    const std::wstring label =
        scene.hud.percent >= 0 ? std::to_wstring(scene.hud.percent) + L"%" : L"...";
    DrawLabel(target, caption_.Get(), label,
              D2D1::RectF(left + trackInset + trackW + 8.0f, top + 13.0f,
                          left + width - trackInset, top + height - 13.0f),
              textPrimary_.Get());
  }

  void DrawQuickLookCard(ID2D1RenderTarget* target, const Scene& scene,
                         const D2D1_SIZE_F& size) {
    const float width = 380.0f, height = 500.0f;
    const float left = size.width - width - 48.0f;
    const float top = 96.0f;
    const D2D1_ROUNDED_RECT card =
        D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height), 24.0f, 24.0f);
    target->FillRoundedRectangle(card, card_.Get());
    target->DrawRoundedRectangle(card, islandBorder_.Get(), 1.2f);

    // Preview area: gradient stand-in for the decoded texture.
    thumbs_[1]->SetStartPoint(D2D1::Point2F(left, top));
    thumbs_[1]->SetEndPoint(D2D1::Point2F(left, top + height - 110.0f));
    D2D1_ROUNDED_RECT preview = D2D1::RoundedRect(
        D2D1::RectF(left + 14.0f, top + 14.0f, left + width - 14.0f, top + height - 110.0f),
        16.0f, 16.0f);
    target->FillRoundedRectangle(preview, thumbs_[1].Get());

    DrawLabel(target, title_.Get(), scene.quickLookTitle,
              D2D1::RectF(left + 18.0f, top + height - 98.0f, left + width - 18.0f,
                          top + height - 70.0f), textPrimary_.Get());
    DrawLabel(target, small_.Get(), scene.quickLookMeta,
              D2D1::RectF(left + 18.0f, top + height - 64.0f, left + width - 18.0f,
                          top + height - 42.0f), textSecondary_.Get());
    DrawLabel(target, small_.Get(), L"Space close  \u00B7  Esc close  \u00B7  arrows browse",
              D2D1::RectF(left + 18.0f, top + height - 36.0f, left + width - 18.0f,
                          top + height - 12.0f), textSecondary_.Get());
  }

  static void DrawLabel(ID2D1RenderTarget* target, IDWriteTextFormat* format,
                        const std::wstring& text, const D2D1_RECT_F& layout,
                        ID2D1Brush* brush) {
    if (!format || text.empty()) return;
    target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, layout,
                     brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<ID2D1SolidColorBrush> accent_, tile_, tileBorder_, island_, card_, islandBorder_;
  ComPtr<ID2D1SolidColorBrush> textPrimary_, textSecondary_;
  // C29 sidebar: one recolourable brush + the two single-line text formats.
  ComPtr<ID2D1SolidColorBrush> sidebarBrush_;
  ComPtr<IDWriteTextFormat> sidebarLabel_, sidebarSmall_;
  ComPtr<ID2D1LinearGradientBrush> thumbs_[3];
  std::unordered_map<uint32_t, ComPtr<ID2D1Bitmap>> thumbBitmaps_;
  uint64_t thumbVersion_ = 0;

 public:
  // Debug counters (printed by the snapshot; drop when the pipeline is
  // proven -- they cost nothing).
  int dbgThumbLookupHits = 0;
  int dbgThumbBitmaps = 0;
  int dbgThumbDraws = 0;
  // C27: proof that the sidebar command list reached pixels. dbgSidebarCommands
  // counts every non-hit command considered, dbgSidebarPrimitives the ones
  // actually painted, dbgSidebarLabels the text runs, dbgSidebarPanels the
  // frames that painted a rail at all.
  int dbgSidebarCommands = 0;
  int dbgSidebarPrimitives = 0;
  int dbgSidebarLabels = 0;
  int dbgSidebarPanels = 0;
  // C29 additions: primitives that reached the device, commands culled by the
  // scroll window, and how often the rail was folded away.
  int dbgSidebarPainted = 0;
  int dbgSidebarClipped = 0;
  int dbgSidebarCollapsed = 0;

 private:
  ComPtr<IDWriteTextFormat> caption_, title_, small_;
};

}  // namespace prism::win::gallery
