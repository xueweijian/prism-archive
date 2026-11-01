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
#include "../PrismCore/WindowLayout.hpp"  // V1: the ONE geometry authority
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
  // V1 (P0-2): the colour of a LOADING skeleton card. It may only ever appear
  // when an item is classified as a tile without pixels -- which the rule in
  // WindowLayout forbids. The snapshot suite calibrates its "no skeleton"
  // detector on this exact colour (Scene::debugForceSkeleton), so an absent hit
  // means the rule held, not that the detector was blind.
  static constexpr D2D1_COLOR_F kSkeletonHighlight = {1.0f, 0.584f, 0.0f, 1.0f};  // #FF9500
  static constexpr D2D1_COLOR_F kSkeletonShimmer = {1.0f, 0.843f, 0.400f, 1.0f};  // #FFD766

  // ------------------------------------------------------------ C29 sidebar
  // The rail is a real COLUMN: when it is shown, list content starts after it.
  // Both numbers below are layout POLICY, not paint, so they live next to the
  // painter that applies them and are pinned by their own unit test.
  // V1: the column width is DESIGN.md's 220, and it is the SAME constant the
  // layout uses -- one number, one source. (C29 shipped 236 before DESIGN.md
  // existed; the two pixels suites were updated with this constant, not a
  // literal, so the next change is a one-line edit.)
  static constexpr float kSidebarColumnWidth =
      static_cast<float>(prism::layout::kSidebarWidth);
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
    // V1: the window hands in the SAME layout it hit-tests with. When absent
    // (snapshot / bench scenes), the renderer computes it from the target size.
    const prism::layout::Layout* layout = nullptr;
    // TEST SEAM (not a feature): forces the tile path for every cell so the
    // snapshot suite can prove its "skeleton colour" detector actually fires.
    bool debugForceSkeleton = false;
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
    // V1: ONE layout for the whole frame -- bands, grid metrics, list body and
    // chrome. A component that wants a rectangle asks this object.
    // C29: the rail is a COLUMN, so the layout reserves it only while the rail
    // is actually painted AND the view is the list. The grid keeps its
    // full-bleed geometry (that is the C29 contract win.tree_nav depends on).
    const bool railColumn = SidebarShown(scene) && scene.listMode;
    const prism::layout::Layout layout =
        scene.layout != nullptr
            ? *scene.layout
            : prism::layout::Compute(static_cast<int>(size.width),
                                     static_cast<int>(size.height), railColumn,
                                     scene.listMode);
    target->Clear(kBackdrop);
    DrawBands(target, layout);

    // The content is the ONLY region an item may paint into. One clip here
    // replaces the per-tile guesswork that let cards slide under the rail.
    target->PushAxisAlignedClip(ToRectF(layout.contentInner),
                                D2D1_ANTIALIAS_MODE_ALIASED);
    if (scene.listMode) {
      DrawListMode(target, scene, layout);
    } else if (scene.items != nullptr) {
      // A scene may be a sidebar-only composition (the C27 glue suite and the
      // C29 suites both build one): those carry no items. DrawGrid tolerates a
      // missing viewport (scroll 0) because V1 scenes are laid out by rect.
      DrawGrid(target, scene, layout);
    }
    target->PopAxisAlignedClip();

    // NEGATIVE CONTROL (CONTRACTS D3): the rail is never painted, so the suite's
    // "the command list reached pixels" assertion MUST go red.
    if (kPaintSidebar) DrawSidebar(target, scene, size);
    DrawBreadcrumbIsland(target, scene, layout);
    DrawProgressCapsule(target, scene, layout);
    if (scene.quickLookOpen) DrawQuickLookCard(target, scene, layout);
  }

  static D2D1_RECT_F ToRectF(const prism::layout::Rect& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                       static_cast<float>(r.right), static_cast<float>(r.bottom));
  }

  // The five bands, painted from the layout (DESIGN.md §2). This is what makes
  // the structure visible in a snapshot instead of an undifferentiated canvas.
  void DrawBands(ID2D1RenderTarget* target, const prism::layout::Layout& layout) {
    if (layout.titleBar.Empty() || layout.statusBar.Empty()) return;
    D2D1_COLOR_F title = kBackdrop;
    title.r += 0.012f;
    title.g += 0.014f;
    title.b += 0.018f;
    island_->SetColor(title);
    target->FillRectangle(ToRectF(layout.titleBar), island_.Get());

    D2D1_COLOR_F actions = kBackdrop;
    actions.r += 0.020f;
    actions.g += 0.024f;
    actions.b += 0.030f;
    island_->SetColor(actions);
    target->FillRectangle(ToRectF(layout.actionIsland), island_.Get());

    D2D1_COLOR_F status = kBackdrop;
    status.r += 0.016f;
    status.g += 0.018f;
    status.b += 0.022f;
    island_->SetColor(status);
    target->FillRectangle(ToRectF(layout.statusBar), island_.Get());

    // Hairlines between the bands (the 1 px edge of the glass).
    islandBorder_->SetColor(kIslandBorder);
    target->FillRectangle(D2D1::RectF(0.0f, static_cast<float>(layout.actionIsland.bottom),
                                      static_cast<float>(layout.statusBar.right),
                                      static_cast<float>(layout.actionIsland.bottom) + 1.0f),
                          islandBorder_.Get());
    target->FillRectangle(D2D1::RectF(0.0f, static_cast<float>(layout.statusBar.top),
                                      static_cast<float>(layout.statusBar.right),
                                      static_cast<float>(layout.statusBar.top) + 1.0f),
                          islandBorder_.Get());
    island_->SetColor(kIslandFill);
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

  // The grid: every rectangle comes from WindowLayout. The renderer decides
  // NOTHING about position, and it never paints a loading skeleton for an item
  // the classification rule did not hand it as a tile (P0-2).
  void DrawGrid(ID2D1RenderTarget* target, const Scene& scene,
                const prism::layout::Layout& layout) {
    if (!scene.items) return;
    const int count = static_cast<int>(scene.items->size());
    const int rows = layout.columns > 0 ? (count + layout.columns - 1) / layout.columns : 0;
    const int maxScroll =
        (std::max)(0, rows * layout.cellHeight - layout.contentInner.Height());
    const int scroll = (std::min)(
        scene.viewport ? static_cast<int>(scene.viewport->Scroll()) : 0, maxScroll);
    const std::vector<prism::layout::Cell> cells =
        prism::layout::VisibleCells(layout, count, scroll);
    for (const prism::layout::Cell& cell : cells) {
      if (cell.index < 0 || cell.index >= count) continue;
      const prism::ArchiveItem& item = (*scene.items)[static_cast<size_t>(cell.index)];
      const prism::PreviewKind kind = prism::QuickLookPlanner::KindByName(item.path);
      // Availability is a FACT read from the store, not a guess from the name.
      const bool hasThumb = scene.thumbs != nullptr &&
                            prism::layout::ThumbnailCapable(kind) &&
                            scene.thumbs->Contains(item.index);
      const prism::layout::CellKind cellKind =
          scene.debugForceSkeleton
              ? prism::layout::CellKind::Tile
              : prism::layout::CellKindFor(item.isDir, kind, hasThumb);
      if (cellKind == prism::layout::CellKind::Row) {
        DrawFallbackRow(target, scene, cell, item);
      } else if (item.isDir) {
        DrawFolderTile(target, scene, cell.rect, item);
      } else {
        DrawThumbTile(target, scene, cell.rect, item, hasThumb);
      }
    }
  }

  static D2D1_RECT_F CellRectF(const prism::layout::Rect& cell) { return ToRectF(cell); }

  // P0-2: a cell with no thumbnail is a 32 px ROW (icon + name + size), never
  // a skeleton card. The row is anchored to the top of the cell.
  void DrawFallbackRow(ID2D1RenderTarget* target, const Scene& scene,
                       const prism::layout::Cell& cell, const prism::ArchiveItem& item) {
    ++dbgRowCells;
    const D2D1_RECT_F band = ToRectF(prism::layout::FallbackRowRect(cell.rect));
    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(band, 10.0f, 10.0f);
    target->FillRoundedRectangle(pill, tile_.Get());
    target->DrawRoundedRectangle(pill, tileBorder_.Get(), 1.0f);
    // Type glyph: a 16 px token-coloured mark (vector icons are a later pass).
    const float iconTop = band.top + 8.0f;
    const float iconBottom = band.bottom - 8.0f;
    islandBorder_->SetColor(prism::QuickLookPlanner::KindByName(item.path) ==
                                    prism::PreviewKind::Image
                                ? kAccent
                                : kTextSecondary);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(band.left + 8.0f, iconTop, band.left + 24.0f,
                                      iconBottom),
                          3.0f, 3.0f),
        islandBorder_.Get());
    islandBorder_->SetColor(kIslandBorder);
    // A narrow grid cell only has room for the name; the size column is drawn
    // when the cell is wide enough to hold both without clipping the name.
    const float width = band.right - band.left;
    const bool showSize = width >= 220.0f;
    const float nameRight = showSize ? band.right - 104.0f : band.right - 8.0f;
    DrawLabel(target, caption_.Get(), Utf8(FileNameOf(item.path)),
              D2D1::RectF(band.left + 32.0f, band.top + 7.0f, nameRight, band.bottom - 6.0f),
              textPrimary_.Get());
    if (showSize) {
      DrawTrailingLabel(target, Utf8(prism::list::FormatBytes(item.size)),
                        band.right - 100.0f, band.top + 7.0f, 92.0f, textSecondary_.Get());
    }
    DrawSelectionRing(target, scene, prism::layout::FallbackRowRect(cell.rect), item.index,
                      10.0f);
  }

  void DrawFolderTile(ID2D1RenderTarget* target, const Scene& scene,
                      const prism::layout::Rect& cell, const prism::ArchiveItem& item) {
    ++dbgTileCells;
    constexpr float kRadius = 16.0f;
    const D2D1_RECT_F rect = CellRectF(cell);
    const D2D1_ROUNDED_RECT pane = D2D1::RoundedRect(rect, kRadius, kRadius);
    target->FillRoundedRectangle(pane, tile_.Get());
    target->DrawRoundedRectangle(pane, tileBorder_.Get(), 1.0f);
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f - 14.0f;
    D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
        D2D1::RectF(cx - 34.0f, cy - 22.0f, cx + 34.0f, cy + 24.0f), 7.0f, 7.0f);
    target->FillRoundedRectangle(folder, accent_.Get());
    folder = D2D1::RoundedRect(D2D1::RectF(cx - 34.0f, cy - 30.0f, cx + 6.0f, cy - 14.0f),
                               6.0f, 6.0f);
    target->FillRoundedRectangle(folder, accent_.Get());
    DrawNamePlate(target, cell, item);
    DrawSelectionRing(target, scene, cell, item.index);
  }

  void DrawThumbTile(ID2D1RenderTarget* target, const Scene& scene,
                     const prism::layout::Rect& cell, const prism::ArchiveItem& item,
                     bool hasThumb) {
    ++dbgTileCells;
    constexpr float kRadius = 16.0f;
    const D2D1_RECT_F rect = CellRectF(cell);
    const D2D1_RECT_F photoRect =
        D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom - 30.0f);
    const prism::gallery::DecodedThumb* thumb =
        (hasThumb && scene.thumbs) ? scene.thumbs->Lookup(item.index) : nullptr;
    if (thumb) ++dbgThumbLookupHits;
    if (!thumb) {
      DrawSkeletonTile(target, photoRect);
      DrawNamePlate(target, cell, item);
      DrawSelectionRing(target, scene, cell, item.index);
      return;
    }
    ComPtr<ID2D1PathGeometry> clipGeometry;
    if (SUCCEEDED(CreateSquircleGeometry(target, photoRect, kRadius,
                                         clipGeometry.ReleaseAndGetAddressOf()))) {
      D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters(
          photoRect, clipGeometry.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      target->PushLayer(layer, nullptr);
    }
    SyncThumbBitmap(target, item.index, *thumb, scene.thumbs->Version());
    const auto it = thumbBitmaps_.find(item.index);
    if (it != thumbBitmaps_.end() && it->second) {
      ++dbgThumbBitmaps;
      const float areaW = photoRect.right - photoRect.left;
      const float areaH = photoRect.bottom - photoRect.top;
      const float scale = (std::min)(areaW / thumb->width, areaH / thumb->height);
      const float drawW = thumb->width * scale;
      const float drawH = thumb->height * scale;
      const D2D1_RECT_F fit = D2D1::RectF(
          photoRect.left + (areaW - drawW) / 2.0f, photoRect.top + (areaH - drawH) / 2.0f,
          photoRect.left + (areaW + drawW) / 2.0f, photoRect.top + (areaH + drawH) / 2.0f);
      target->FillRectangle(photoRect, tile_.Get());
      ++dbgThumbDraws;
      target->DrawBitmap(it->second.Get(), fit, 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (clipGeometry) target->PopLayer();
    DrawNamePlate(target, cell, item);
    DrawSelectionRing(target, scene, cell, item.index);
  }

  // The ONLY place kSkeletonHighlight is painted. Reaching it means the
  // classification rule failed to hand this cell a thumbnail -- the P0-2 bug.
  void DrawSkeletonTile(ID2D1RenderTarget* target, const D2D1_RECT_F& rect) {
    ++dbgSkeletonCells;
    island_->SetColor(kSkeletonHighlight);
    target->FillRectangle(rect, island_.Get());
    island_->SetColor(kSkeletonShimmer);
    const float cy = (rect.top + rect.bottom) / 2.0f;
    target->FillRectangle(
        D2D1::RectF(rect.left + 12.0f, cy - 4.0f, rect.right - 12.0f, cy + 4.0f),
        island_.Get());
    island_->SetColor(kIslandFill);
  }

  void DrawNamePlate(ID2D1RenderTarget* target, const prism::layout::Rect& cell,
                     const prism::ArchiveItem& item) {
    const D2D1_RECT_F rect = CellRectF(cell);
    const D2D1_ROUNDED_RECT plate = D2D1::RoundedRect(
        D2D1::RectF(rect.left, rect.bottom - 28.0f, rect.right, rect.bottom), 10.0f, 10.0f);
    target->FillRoundedRectangle(plate, tile_.Get());
    DrawLabel(target, caption_.Get(), Utf8(FileNameOf(item.path)),
              D2D1::RectF(rect.left + 8.0f, rect.bottom - 26.0f, rect.right - 8.0f,
                          rect.bottom - 4.0f),
              textPrimary_.Get());
  }

  void DrawSelectionRing(ID2D1RenderTarget* target, const Scene& scene,
                         const prism::layout::Rect& cell, uint32_t itemIndex,
                         float radius = 16.0f) {
    if (scene.selectedItem < 0) return;
    if (static_cast<int>(itemIndex) != scene.selectedItem) return;
    const D2D1_RECT_F rect = CellRectF(cell);
    islandBorder_->SetColor(kAccent);
    target->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(rect.left - 1.0f, rect.top - 1.0f, rect.right + 1.0f,
                                      rect.bottom + 1.0f),
                          radius, radius),
        islandBorder_.Get(), 2.0f);
    islandBorder_->SetColor(kIslandBorder);
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
  // 压缩率, 32 px virtualized rows from WindowLayout, semantic micro-capsule
  // bars, folders first. Every rectangle is anchored to the layout's list body,
  // which is already inset by the rail (C29's column, V1's geometry).
  void DrawListMode(ID2D1RenderTarget* target, const Scene& scene,
                    const prism::layout::Layout& layout) {
    if (!scene.listItems) return;
    const auto& rows = *scene.listItems;
    // The app persists the user's column geometry (C27); absent means the module
    // default, which is what every pre-C27 call site gets.
    const prism::list::ColumnLayout cols =
        scene.hasListColumns && scene.listColumns != nullptr ? *scene.listColumns
                                                             : prism::list::ColumnLayout{};
    const prism::layout::Rect header = prism::layout::ListHeaderRect(layout);
    const prism::layout::Rect body = prism::layout::ListBodyRect(layout);
    const int scroll = static_cast<int>(scene.listScroll);
    const float right = static_cast<float>(body.right);
    const float nameRight = right - cols.sizeW - cols.packedW - cols.ratioW - 12.0f;
    const float sizeX = right - cols.sizeW - cols.packedW - cols.ratioW + 6.0f;
    const float packedX = right - cols.packedW - cols.ratioW + 6.0f;
    const float ratioX = right - cols.ratioW + 6.0f;
    const float textTop = static_cast<float>(header.top) + 4.0f;

    // Header
    DrawLabel(target, caption_.Get(), L"\x540D\x79F0 ^",
              D2D1::RectF(static_cast<float>(header.left) + prism::layout::kListIndent,
                          textTop, nameRight, textTop + 24.0f),
              textSecondary_.Get());
    DrawTrailingLabel(target, L"\x539F\x59CB\x5927\x5C0F", sizeX, textTop, cols.sizeW - 18,
                      textSecondary_.Get());
    DrawTrailingLabel(target, L"\x538B\x7F29\x540E", packedX, textTop, cols.packedW - 18,
                      textSecondary_.Get());
    DrawTrailingLabel(target, L"\x538B\x7F29\x7387", ratioX, textTop, cols.ratioW - 18,
                      textSecondary_.Get());

    const prism::layout::RowRange window = prism::layout::VisibleListRows(
        layout, static_cast<int>(rows.size()), scroll);
    for (int i = window.first; i <= window.last; ++i) {
      const auto& row = rows[static_cast<size_t>(i)];
      const prism::layout::Rect rr = prism::layout::ListRowRect(layout, i, scroll);
      const float top = static_cast<float>(rr.top);
      const float bottom = static_cast<float>(rr.bottom);
      if (bottom <= static_cast<float>(body.top) ||
          top >= static_cast<float>(body.bottom)) {
        continue;
      }
      const D2D1_RECT_F rowRect = ToRectF(rr);
      if (i == scene.selectedRow) {
        // Selection highlight: G2 squircle R=10, accent fluid fill.
        D2D1_ROUNDED_RECT sel = D2D1::RoundedRect(
            D2D1::RectF(rowRect.left + 8.0f, top + 2.0f, rowRect.right - 8.0f, bottom - 2.0f),
            10.0f, 10.0f);
        D2D1_COLOR_F selColor = kAccent;
        selColor.a = 0.26f;
        accent_->SetColor(selColor);
        target->FillRoundedRectangle(sel, accent_.Get());
        accent_->SetColor(kAccent);
      }

      const std::wstring name = Utf8(row.name);
      const float nameLeft = rowRect.left + prism::layout::kListIndent;
      if (row.isDir) {
        const D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
            D2D1::RectF(nameLeft + 4.0f, top + 9.0f, nameLeft + 20.0f, top + 27.0f), 5.0f,
            5.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
        DrawLabel(target, caption_.Get(), name,
                  D2D1::RectF(nameLeft + 28.0f, top + 5.0f, nameRight, bottom - 4.0f),
                  textPrimary_.Get());
        DrawTrailingLabel(target, L"--", sizeX, top + 6.0f, cols.sizeW - 18,
                          textSecondary_.Get());
        DrawTrailingLabel(target, L"--", packedX, top + 6.0f, cols.packedW - 18,
                          textSecondary_.Get());
      } else {
        DrawLabel(target, caption_.Get(), name,
                  D2D1::RectF(nameLeft, top + 5.0f, nameRight, bottom - 4.0f),
                  textPrimary_.Get());
        DrawTrailingLabel(target, Utf8(prism::list::FormatBytes(row.size)), sizeX,
                          top + 6.0f, cols.sizeW - 18, textPrimary_.Get());
        DrawTrailingLabel(target, Utf8(prism::list::FormatBytes(row.packedSize)), packedX,
                          top + 6.0f, cols.packedW - 18, textPrimary_.Get());
      }

      // Semantic micro-capsule ratio bar (blueprint ch. 3.2.3).
      if (const auto bar = prism::list::RatioBar(row)) {
        const float trackX = right - cols.ratioW + 8.0f;
        const float trackW = cols.ratioW - 64.0f;
        const float cy = top + prism::layout::kListRowHeight / 2.0f;
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
        DrawTrailingLabel(target, pct, right - cols.ratioW + 8.0f + trackW + 6.0f,
                          top + 6.0f, 40, textSecondary_.Get());
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

  // The breadcrumb pill: geometry from the action band (DESIGN.md §2), so it
  // can never overlap the title bar, the status bar or a content card.
  void DrawBreadcrumbIsland(ID2D1RenderTarget* target, const Scene& scene,
                            const prism::layout::Layout& layout) {
    const prism::layout::Rect pillRect = prism::layout::NavPillRect(layout);
    if (pillRect.Empty()) return;
    const D2D1_RECT_F pill = ToRectF(pillRect);
    const float height = static_cast<float>(pillRect.Height());
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(pill, height / 2.0f, height / 2.0f);
    target->FillRoundedRectangle(shape, island_.Get());
    target->DrawRoundedRectangle(shape, islandBorder_.Get(), 1.0f);
    DrawLabel(target, title_.Get(), scene.breadcrumb,
              ToRectF(prism::layout::NavTextRect(layout)), textPrimary_.Get());
    if (scene.islands) {
      // Pointer-tracked specular rim (blueprint ch. 3.1 Layer 5): a bright
      // segment flows along the top edge toward the cursor. In the real
      // pipeline this comes from the HLSL shader; the software snapshot
      // approximates it from the SAME cbuffer params (pointer xy).
      float cb[20];
      scene.islands->WriteCbuffer(prism::glass::IslandId::Nav,
                                  static_cast<uint32_t>(layout.windowW),
                                  static_cast<uint32_t>(layout.windowH), cb);
      const float px = (std::min)((std::max)(cb[4], pill.left + 70.0f), pill.right - 70.0f);
      D2D1_COLOR_F rim = {1.0f, 1.0f, 1.0f, 0.62f};
      islandBorder_->SetColor(rim);
      D2D1_ROUNDED_RECT glow = D2D1::RoundedRect(
          D2D1::RectF(px - 62.0f, pill.top + 1.0f, px + 62.0f, pill.top + 3.0f), 1.0f, 1.0f);
      target->FillRoundedRectangle(glow, islandBorder_.Get());
      islandBorder_->SetColor(kIslandBorder);
    }
  }

  // Progress capsule: bottom-centre of the CONTENT. It used to sit at
  // size.height - 68, crossing into the 28 px status bar.
  void DrawProgressCapsule(ID2D1RenderTarget* target, const Scene& scene,
                           const prism::layout::Layout& layout) {
    if (scene.hud.phase == prism::HudPhase::Hidden) return;
    const prism::layout::Rect capsuleRect = prism::layout::ProgressCapsuleRect(layout);
    if (capsuleRect.Empty()) return;
    const D2D1_RECT_F pill = ToRectF(capsuleRect);
    const float width = static_cast<float>(capsuleRect.Width());
    const float height = static_cast<float>(capsuleRect.Height());
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(pill, height / 2.0f, height / 2.0f);
    target->FillRoundedRectangle(shape, island_.Get());
    target->DrawRoundedRectangle(shape, islandBorder_.Get(), 1.0f);

    // Progress track + fill (the HUD model owns the percent; -1 == spin).
    const float trackInset = 10.0f;
    const float trackW = width - 2.0f * trackInset - 56.0f;
    const float cy = (pill.top + pill.bottom) / 2.0f;
    D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
        D2D1::RectF(pill.left + trackInset, cy - 3.0f, pill.left + trackInset + trackW,
                    cy + 3.0f),
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
              D2D1::RectF(pill.left + trackInset + trackW + 8.0f, pill.top + 13.0f,
                          pill.right - trackInset, pill.bottom - 13.0f),
              textPrimary_.Get());
  }

  void DrawQuickLookCard(ID2D1RenderTarget* target, const Scene& scene,
                         const prism::layout::Layout& layout) {
    const prism::layout::Rect cardRect = prism::layout::QuickLookCardRect(layout);
    if (cardRect.Empty()) return;
    const D2D1_RECT_F card = ToRectF(cardRect);
    const float height = static_cast<float>(cardRect.Height());
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(card, 24.0f, 24.0f);
    target->FillRoundedRectangle(shape, card_.Get());
    target->DrawRoundedRectangle(shape, islandBorder_.Get(), 1.2f);

    // Preview area: gradient stand-in for the decoded texture.
    thumbs_[1]->SetStartPoint(D2D1::Point2F(card.left, card.top));
    thumbs_[1]->SetEndPoint(D2D1::Point2F(card.left, card.top + height - 110.0f));
    D2D1_ROUNDED_RECT preview = D2D1::RoundedRect(
        D2D1::RectF(card.left + 14.0f, card.top + 14.0f, card.right - 14.0f,
                    card.top + height - 110.0f),
        16.0f, 16.0f);
    target->FillRoundedRectangle(preview, thumbs_[1].Get());

    DrawLabel(target, title_.Get(), scene.quickLookTitle,
              D2D1::RectF(card.left + 18.0f, card.top + height - 98.0f, card.right - 18.0f,
                          card.top + height - 70.0f),
              textPrimary_.Get());
    DrawLabel(target, small_.Get(), scene.quickLookMeta,
              D2D1::RectF(card.left + 18.0f, card.top + height - 64.0f, card.right - 18.0f,
                          card.top + height - 42.0f),
              textSecondary_.Get());
    DrawLabel(target, small_.Get(), L"Space close  \u00B7  Esc close  \u00B7  arrows browse",
              D2D1::RectF(card.left + 18.0f, card.top + height - 36.0f, card.right - 18.0f,
                          card.top + height - 12.0f),
              textSecondary_.Get());
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
  // V1: proof that the classification rule reached pixels. dbgRowCells counts
  // 32 px fallback rows (P0-2), dbgTileCells the real tiles, and
  // dbgSkeletonCells MUST stay 0 in a correct build -- the snapshot suite
  // calibrates its skeleton-colour detector on the debug seam.
  int dbgTileCells = 0;
  int dbgRowCells = 0;
  int dbgSkeletonCells = 0;
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
