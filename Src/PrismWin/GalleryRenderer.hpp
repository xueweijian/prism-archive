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
#include <dwrite_1.h>  // C79: IDWriteTextLayout1::SetCharacterSpacing (heading tracking)

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "../PrismCore/ActionBarModel.hpp"  // C-V2: the action table + band geometry
#include "../PrismCore/ChromeTheme.hpp"    // C91: ThemeSurfaceColor -- the ONE theme entry
#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/GlassIsland.hpp"
#include "../PrismCore/ListLayout.hpp"
#include "../PrismCore/PathSafety.hpp"  // FileNameOf
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/ProgressHud.hpp"
#include "../PrismCore/SidebarModel.hpp"
#include "../PrismCore/ThemeTokens.hpp"
#include "TabBar.hpp"  // C60: the tab strip (geometry + pixels)
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismCore/WindowLayout.hpp"  // V1: the ONE geometry authority
#include "ActionBar.hpp"    // C-V2: the action-band painter (DESIGN.md §2)
#include "GlassPanel.hpp"   // C77: the ONE glass-panel helper (fill/rim/shadow)
#include "SidebarView.hpp"  // C12 draw commands -- painted since C27, refined by C29

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------- C111
// The folder tile's negative controls (CONTRACTS D3). Each switch lives beside
// the painter it mutates, and each armed branch increments a counter so a CTest
// "MUTATION CAUGHT" verdict can be armed on the injection firing rather than on
// a colour of red that might have come from somewhere else (D27).
inline int& MutantTwoRectsHits() {
  static int hits = 0;
  return hits;
}
inline int& MutantNoHighlightHits() {
  static int hits = 0;
  return hits;
}

#if defined(PRISM_C27_MUTANT_NO_SIDEBAR_PAINT)
inline constexpr bool kPaintSidebar = false;
#else
inline constexpr bool kPaintSidebar = true;
#endif

class GalleryRenderer {
 public:
  // The design tokens (iOS 26 dark glass language).
  //
  // C91: THERE IS NO kBackdrop ANY MORE. The canvas used to be this literal
  // (#0B0E13, ~11,14,19), which is why the day/night diff of the whole window
  // moved 7.96 % of its pixels -- all of them in the title bar. The canvas, the
  // action band and the list header now resolve from the theme through
  // prism::chrome::CanvasColor / CanvasPresentedColor / ThemeSurfaceColor, the
  // ONE entry point in PrismCore. A literal here would be a second authority.
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
  // C93: the capsule under the crumb the pointer is over. White at 14% reads as
  // "hover" on both themes without inventing a token the theme does not own.
  static constexpr D2D1_COLOR_F kCrumbHover = {1.0f, 1.0f, 1.0f, 0.14f};
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
    // C91: WHICH theme every surface in this frame paints. This is the scene's
    // ONLY theme input: the sidebar, the tab strip, the action island, the glass
    // panels, the canvas, the status band and the list all read it, so a window
    // that sets one field can no longer ship a frame where four surfaces follow
    // the theme and the loot behind them does not.
    prism::theme::Variant theme = prism::theme::Variant::Midnight;
    std::vector<prism::ArchiveItem>* items = nullptr;
    prism::gallery::ViewportWindow* viewport = nullptr;
    prism::ProgressHud::View hud;
    bool quickLookOpen = false;
    std::wstring quickLookTitle;
    std::wstring quickLookMeta;
    // C87: WHICH catalog entry the card previews. The renderer looks its decoded
    // pixels up in 'thumbs' -- the card is not allowed to be a colour stand-in
    // when the archive already holds the image.
    int quickLookItem = -1;
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
    // SidebarFits). Defaults to the shipping value, so a pre-C29 scene is
    // unchanged.
    bool sidebarCollapsed = false;
    // V1: the window hands in the SAME layout it hit-tests with. When absent
    // (snapshot / bench scenes), the renderer computes it from the target size.
    const prism::layout::Layout* layout = nullptr;
    // TEST SEAM (not a feature): forces the tile path for every cell so the
    // snapshot suite can prove its "skeleton colour" detector actually fires.
    bool debugForceSkeleton = false;

    // V4: the status band's readout (encryption / method / dictionary / ratio).
    // Defaults to the "unknown" state, which is what an archive the engine has
    // not reported on yet honestly shows -- never an empty 28 px strip.
    prism::layout::StatusInfo status;

    // C-V2: the action band. nullptr -> no band at all (every pre-V2 scene, so
    // the older suites and the snapshot keep their exact framing). The band
    // rectangle itself comes from V1's layout (actionIsland); the model only
    // decides how that row is shared with the breadcrumb pill.
    const prism::actionbar::Model* actions = nullptr;
    int actionHover = -1;    // button index under the pointer (-1: none)
    int actionPressed = -1;  // button index held down (-1: none)

    // C93: the crumb under the POINTER (kNoLevel -> nothing). The rect is the
    // window's own measured span for that crumb, so the highlight can never be
    // wider than the word it makes pressable -- "hit area covers the whole bar"
    // is the defect this field exists to prevent.
    int crumbHoverLevel = prism::tree::kNoLevel;
    prism::layout::Rect crumbHoverRect;

    // C60: the tab strip. nullptr -> no strip at all (every pre-C60 scene, and
    // the "no archive is open" state, where the band is hidden and the action
    // island sits directly under the title). The band's rectangle comes from
    // V1's layout (Layout::tabBar); this view only says what to paint in it.
    const prism::win::tabbar::View* tabs = nullptr;

    // ------------------------------------------------------------- C77 glass
    // The BACKDROP LAYER: the current items rendered at 1/8 resolution, blurred
    // and desaturated, painted under everything. It is what makes a translucent
    // panel read as glass -- a semi-transparent fill over a flat colour is just
    // grey, which is exactly what the user reported.
    //
    // OFF by default, like every other optional layer in this renderer (a null
    // action model paints no band; a null sidebar paints no rail): a pre-C77
    // scene keeps its exact framing, and the window turns both on from the
    // appearance setting, which defaults ON.
    bool backdropRefraction = false;
    // The panel language (fill + top specular rim + drop shadow + squircle 14)
    // for the breadcrumb island, the action island's base, the rail's base and
    // the Quick Look card. Same opt-in rule as above.
    bool glassPanels = false;
    // A caller-side bump for "the items changed". The renderer ALSO fingerprints
    // the catalog (count + first/middle/last path), so a rename rebuilds even
    // when nobody bumps this; the field exists so a test can force the change.
    uint64_t contentRevision = 0;
  };

  HRESULT Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   factory_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(writeFactory_.ReleaseAndGetAddressOf()));
  }

  ID2D1Factory* Factory() const { return factory_.Get(); }

  // C105: the info card's two type sizes as the REAL DirectWrite objects. The
  // geometry suite reads GetFontSize() off these -- 10 (label) / 12 (value) --
  // so "the label is 10 px and the value is 12 px" is measured on the format
  // the painter actually draws with, not on a copy of the number.
  IDWriteTextFormat* CardLabelFormat() const { return sidebarCardLabel_.Get(); }
  IDWriteTextFormat* CardValueFormat() const { return sidebarStatus_.Get(); }

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
    // C79: the status capsule's readout is 12 px secondary text (the group
    // headings reuse sidebarSmall_, the existing 11 px semibold format).
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    12.0f, L"en-US", sidebarStatus_.ReleaseAndGetAddressOf());
    if (sidebarStatus_) {
      sidebarStatus_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      sidebarStatus_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // C105: the info card's LABEL line. 10 px, regular weight, secondary role --
    // the line ABOVE a value, per plan 3.2's "上行 10px 灰色小字". It is its own
    // IDWriteTextFormat (not a size passed at DrawText time) so a suite can read
    // the REAL format back through CardLabelFormat() and pin the size.
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    10.0f, L"en-US",
                                    sidebarCardLabel_.ReleaseAndGetAddressOf());
    if (sidebarCardLabel_) {
      sidebarCardLabel_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      sidebarCardLabel_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // V4/C79: the status band's readout format (12 px, one line, leading,
    // centred in its 20 px inner band so the six segments share one baseline).
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    12.0f, L"en-US", statusFormat_.ReleaseAndGetAddressOf());
    if (statusFormat_) {
      statusFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      statusFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      statusFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // C-V2: the band's own brush/text formats. A caller that never builds an
    // action-bar scene pays one brush and two text formats.
    hr = actionBar_.CreateDeviceResources(target);
    if (FAILED(hr)) return hr;
    // C60: the tab strip's one brush + two formats (same shape as the rail and
    // the band: recoloured per token, never one brush per colour).
    if (!tabPainter_.CreateDeviceResources(target, writeFactory_.Get())) return E_FAIL;
    // C77: the glass helper BORROWS the rail's recolourable brush, so the device
    // accounting C75 pins does not grow by one object nobody counted.
    glassPanel_.Bind(target, sidebarBrush_.Get());
    return S_OK;
  }

  void Draw(ID2D1RenderTarget* target, const Scene& scene) {
    const D2D1_SIZE_F size = target->GetSize();
    // C91: ONE theme resolution per frame. Everything below -- the clear, the
    // bands, the status strip, the tab strip, the rail, the action band, the
    // list, the floating islands -- reads this local theme and canvasColor_,
    // never a variant field of its own. That is what makes "the theme reached
    // the content" a property of this function instead of a promise from the
    // caller.
    const prism::theme::Variant theme = ThemeOf(scene);
    canvasColor_ = CanvasOf(scene, theme);
    // C71: one resolution per frame; every method below that resets a brush
    // restores THESE values, not the pre-C71 constants.
    accentColor_ = AccentOf(theme);
    islandColor_ = IslandOf(theme);
    accent_->SetColor(accentColor_);
    island_->SetColor(islandColor_);
    // C91: the text roles and the rail brush are recoloured EVERY frame. They
    // used to be built once from two literals, so even a scene that knew the
    // variant painted midnight text on a daylight canvas.
    ApplyThemeBrushes(theme);
    // V1: ONE layout for the whole frame -- bands, grid metrics, list body and
    // chrome. A component that wants a rectangle asks this object.
    // V4: the rail is CHROME (DESIGN.md §2 lists it as a band beside the
    // content, §3 lists it with its own four states), so it reserves width in
    // BOTH views. C29's "rail only in list mode" rule is the policy this card
    // reverses: the grid is not a different window, and a rail that disappears
    // when the user presses Ctrl+2 is a layout jump, not a mode.
    const bool railColumn = SidebarShown(scene);
    const prism::layout::Layout layout =
        scene.layout != nullptr
            ? *scene.layout
            : prism::layout::Compute(static_cast<int>(size.width),
                                     static_cast<int>(size.height), railColumn,
                                     scene.listMode);
    target->Clear(canvasColor_);
    // C77: the refraction layer sits under EVERYTHING -- bands, content, chrome.
    // It is built from the content only (the items), never from the chrome, so
    // a hover or a focus pill cannot make the wallpaper shimmer.
    if (scene.backdropRefraction) DrawBackdrop(target, scene, layout);
    DrawBands(target, layout);
    // C60: the tab strip is a BAND of the layout (DESIGN.md §2.1), painted with
    // the bands and before the content, so the content's clip never covers it
    // and a tab can never be drawn over a tile. A null view or a hidden band
    // paints nothing at all -- case 7 of the suite measures the pixels.
    if (scene.tabs != nullptr) {
      // C91: hand the strip the SAME canvas this frame cleared to -- the strip
      // repaints its band over the refraction layer, and a translucent token
      // there would composite the canvas twice (2-3 levels darker than the
      // content beside it, a visible seam).
      const prism::theme::Rgba canvas(canvasColor_.r, canvasColor_.g, canvasColor_.b,
                                      canvasColor_.a);
      tabPainter_.Paint(target, layout, *scene.tabs, theme, canvas);
    }
    // V4: the 28 px band carries its four segments; an empty strip was symptom
    // #4 of the screenshot.
    DrawStatusBar(target, scene, layout);

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

    // C77: the rail's BASE -- the drop shadow of the column, painted under it.
    // The column's own fill belongs to DrawSidebar (C29/C79) and is not touched
    // here: only the depth layer is added, so the rail's pinned surface colour
    // is still exactly what the C29 suite measured.
    if (scene.glassPanels && SidebarShown(scene)) {
      const prism::win::sidebar::Layout& rail = *scene.sidebar;
      const D2D1_RECT_F railBox =
          D2D1::RectF(rail.panel.left, (std::max)(rail.panel.top, 0.0f), rail.panel.right,
                      (std::min)(rail.panel.bottom, size.height));
      if (railBox.right > railBox.left && railBox.bottom > railBox.top) {
        glassPanel_.PaintShadow(target, railBox, scene.theme, &glassStats_);
      }
    }
    // NEGATIVE CONTROL (CONTRACTS D3): the rail is never painted, so the suite's
    // "the command list reached pixels" assertion MUST go red.
    if (kPaintSidebar) DrawSidebar(target, scene, size);
    // C-V2: the action band, between the content and the floating chrome: its
    // dropdown panel hangs over the grid, and the pill is painted after it.
    DrawActionBar(target, scene, layout);
    DrawBreadcrumbIsland(target, scene, layout);
    DrawProgressCapsule(target, scene, layout);
    if (scene.quickLookOpen) DrawQuickLookCard(target, scene, layout);
  }

  // ------------------------------------------------------------------- C77
  // THE BACKDROP LAYER (DESIGN.md 2.1, "the content is the wallpaper").
  //
  // The items are re-rendered into a 1/8-resolution compatible bitmap, blurred
  // with a 15-tap separable gaussian (sigma 3.5 mini px = 28 px at 1:1), then
  // composited at the ghost token's strength and washed with the veil token,
  // which is what cuts its chroma to ~0.35. Only THEN does the chrome paint.
  //
  // WHY 1/8 AND NOT A FULL-SIZE BLUR: at 1/8 a 28 px blur is a 3.5 px blur --
  // 30 DrawBitmap calls on a 200x125 surface instead of a full-window pass, and
  // the upscale back to 1:1 is free (the bilinear tap IS the last blur step).
  //
  // REBUILD POLICY: only on a SIZE or CONTENT change. Scrolling does not
  // rebuild (a backdrop that chased the scroll would be a full-window blur per
  // frame); the ghost is a still of the content, which is what a refraction
  // layer is. dbgBackdropRebuilds / dbgBackdropDraws are the evidence.
  struct BackdropKey {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t count = 0;
    const void* items = nullptr;
    uint64_t fingerprint = 0;
    uint64_t revision = 0;
    bool listMode = false;
    bool operator==(const BackdropKey& other) const {
      return width == other.width && height == other.height && count == other.count &&
             items == other.items && fingerprint == other.fingerprint &&
             revision == other.revision && listMode == other.listMode;
    }
  };

  // Count + first/middle/last path: enough to notice a rename or a navigation
  // without hashing a 100k-entry catalog on every frame. A caller that knows
  // better bumps Scene::contentRevision.
  static uint64_t FingerprintOf(const Scene& scene) {
    if (scene.items == nullptr || scene.items->empty()) return 0;
    const auto hashOne = [](uint64_t seed, const std::string& path) {
      uint64_t h = seed ^ 1469598103934665603ull;
      for (char c : path) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
      }
      return h;
    };
    const std::vector<prism::ArchiveItem>& items = *scene.items;
    uint64_t h = 1469598103934665603ull;
    const size_t picks[3] = {0, items.size() / 2, items.size() - 1};
    for (size_t index : picks) h = hashOne(h, items[index].path);
    return h;
  }

  static BackdropKey MakeBackdropKey(const Scene& scene, const D2D1_SIZE_F& size) {
    BackdropKey key;
    key.width = static_cast<uint32_t>(size.width);
    key.height = static_cast<uint32_t>(size.height);
    key.count = scene.items != nullptr ? scene.items->size() : 0u;
    key.items = scene.items;
    key.fingerprint = FingerprintOf(scene);
    key.revision = scene.contentRevision;
    key.listMode = scene.listMode;
    return key;
  }

  void DrawBackdrop(ID2D1RenderTarget* target, const Scene& scene,
                    const prism::layout::Layout& layout) {
#if defined(PRISM_C77_MUTANT_NO_BACKDROP)
    // C77 MUTANT ACTIVE: no-backdrop -- the layer is compiled out, so the
    // suite's "the backdrop differs from the flat colour" check MUST go red.
    return;
#endif
    const D2D1_SIZE_F size = target->GetSize();
    if (!(size.width >= 8.0f) || !(size.height >= 8.0f)) return;
    const BackdropKey key = MakeBackdropKey(scene, size);
    if (!backdropBitmap_ || !(key == backdropKey_)) {
      if (!BuildBackdrop(target, scene, layout, key)) return;  // fall back to flat
    }
    if (!backdropBitmap_) return;
    const prism::theme::Rgba ghost =
        prism::theme::Lookup(prism::theme::TokenId::SurfaceBackdropGhost, scene.theme).color;
    const prism::theme::Rgba veil =
        prism::theme::Lookup(prism::theme::TokenId::SurfaceBackdropVeil, scene.theme).color;
    target->DrawBitmap(backdropBitmap_.Get(), D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                       ghost.a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (sidebarBrush_ && veil.a > 0.0f) {
      sidebarBrush_->SetColor(ToD2d(veil));
      target->FillRectangle(D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                            sidebarBrush_.Get());
    }
    ++dbgBackdropDraws;
  }

  bool BuildBackdrop(ID2D1RenderTarget* target, const Scene& scene,
                     const prism::layout::Layout& layout, const BackdropKey& key) {
    const D2D1_SIZE_F size = target->GetSize();
    constexpr float kDownsample = 8.0f;
    const UINT32 miniW = static_cast<UINT32>((std::max)(1.0f, std::floor(size.width / kDownsample)));
    const UINT32 miniH =
        static_cast<UINT32>((std::max)(1.0f, std::floor(size.height / kDownsample)));
    const D2D1_PIXEL_FORMAT format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_PREMULTIPLIED);
    const D2D1_SIZE_F miniSize = D2D1::SizeF(static_cast<float>(miniW), static_cast<float>(miniH));
    const D2D1_SIZE_U miniPixels = D2D1::SizeU(miniW, miniH);

    const auto makeTarget = [&](ID2D1BitmapRenderTarget** out) {
      return target->CreateCompatibleRenderTarget(&miniSize, &miniPixels, &format,
                                                  D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, out);
    };

    // 1. the content, at 1/8. The counters are saved and restored: this pass is
    //    not the frame's paint, and inflating dbgTileCells here would have made
    //    the C29/C36 pixel assertions read a number no frame ever produced.
    ComPtr<ID2D1BitmapRenderTarget> content;
    if (FAILED(makeTarget(content.ReleaseAndGetAddressOf()))) return false;
    content->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    content->BeginDraw();
    content->Clear(D2D1::ColorF(0, 0, 0, 0));
    content->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / kDownsample, 1.0f / kDownsample));
    const int savedTiles = dbgTileCells;
    const int savedRows = dbgRowCells;
    const int savedThumbDraws = dbgThumbDraws;
    if (scene.listMode) {
      DrawListMode(content.Get(), scene, layout, /*backdropPass=*/true);
    } else if (scene.items != nullptr) {
      DrawGrid(content.Get(), scene, layout);
    }
    dbgTileCells = savedTiles;
    dbgRowCells = savedRows;
    dbgThumbDraws = savedThumbDraws;
    content->SetTransform(D2D1::Matrix3x2F::Identity());
    if (FAILED(content->EndDraw())) return false;
    ComPtr<ID2D1Bitmap> stage;
    if (FAILED(content->GetBitmap(stage.ReleaseAndGetAddressOf()))) return false;

    // 2. the blur: two separable passes, 15 taps each (sigma = 28 px at 1:1).
    constexpr float kBlurRadiusPx = 28.0f;
    constexpr int kTaps = 7;  // +-7 mini px, sigma 3.5
    const float sigma = kBlurRadiusPx / kDownsample * 0.5f;
    float weights[2 * kTaps + 1];
    float total = 0.0f;
    for (int d = -kTaps; d <= kTaps; ++d) {
      const float w = std::exp(-static_cast<float>(d * d) / (2.0f * sigma * sigma));
      weights[d + kTaps] = w;
      total += w;
    }
    // "over" accumulation realizes a weighted average if each tap's alpha is
    // g_i / (1 - sum of the earlier taps): the visible weights then ARE g_i.
    float alphas[2 * kTaps + 1];
    float consumed = 0.0f;
    for (int i = 0; i < 2 * kTaps + 1; ++i) {
      const float g = weights[i] / total;
      alphas[i] = consumed < 0.999f ? g / (1.0f - consumed) : 0.0f;
      consumed += g;
    }
    const auto blurPass = [&](ID2D1Bitmap* source, bool horizontal,
                              ComPtr<ID2D1Bitmap>* out) {
      ComPtr<ID2D1BitmapRenderTarget> pass;
      if (FAILED(makeTarget(pass.ReleaseAndGetAddressOf()))) return false;
      pass->BeginDraw();
      pass->Clear(D2D1::ColorF(0, 0, 0, 0));
      for (int i = 0; i < 2 * kTaps + 1; ++i) {
        if (!(alphas[i] > 0.0f)) continue;
        const float offset = static_cast<float>(i - kTaps);
        const D2D1_RECT_F dest =
            horizontal ? D2D1::RectF(offset, 0.0f, offset + miniSize.width, miniSize.height)
                       : D2D1::RectF(0.0f, offset, miniSize.width, offset + miniSize.height);
        pass->DrawBitmap(source, dest, alphas[i], D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      }
      if (FAILED(pass->EndDraw())) return false;
      return SUCCEEDED(pass->GetBitmap(out->ReleaseAndGetAddressOf()));
    };
    ComPtr<ID2D1Bitmap> horizontal;
    if (!blurPass(stage.Get(), true, &horizontal)) return false;
    ComPtr<ID2D1Bitmap> vertical;
    if (!blurPass(horizontal.Get(), false, &vertical)) return false;

    backdropBitmap_ = vertical;
    backdropKey_ = key;
    ++dbgBackdropRebuilds;
    return true;
  }

  static D2D1_RECT_F ToRectF(const prism::layout::Rect& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                       static_cast<float>(r.right), static_cast<float>(r.bottom));
  }

  // The five bands, painted from the layout (DESIGN.md §2). This is what makes
  // the structure visible in a snapshot instead of an undifferentiated canvas.
  //
  // The TITLE band deliberately stays the canvas backdrop: it is reserved space
  // (the window's caption sits above the client area), not a second surface.
  // Tinting it would move the colour of pixel (0,0), which is the "the canvas
  // actually reached the target" probe the frame benchmark measures -- a
  // structural band must not break an unrelated contract.
  void DrawBands(ID2D1RenderTarget* target, const prism::layout::Layout& layout) {
    if (layout.titleBar.Empty() || layout.statusBar.Empty()) return;

    // C64: the action band is CANVAS, not a tinted ribbon. It used to be filled
    // with a hand-lightened backdrop (kBackdrop + .020/.024/.030); combined with
    // ActionBarPainter's hardcoded .72 island alpha that made the island about
    // 19 levels brighter than the canvas instead of the token's ~13. The island
    // itself is painted by ActionBarPainter (SurfaceFloatingIsland); everything
    // outside its squircle must read as the canvas it floats on.
    //
    // C91: that canvas is now THE theme's canvas, and the same one the target was
    // cleared to. Filling the band with a literal here would have left a 92 %
    // untheme-able rectangle around the island -- the defect this card measures.
    island_->SetColor(canvasColor_);
    target->FillRectangle(ToRectF(layout.actionIsland), island_.Get());

    // C91: the status strip is the canvas + the card's documented lift, resolved
    // from the theme. The lift is a constant RATIO of the band's own brightness
    // (0.016/0.018/0.022 on the dark canvas); on an opaque high-contrast canvas
    // it lands on (4,5,6), which is the value the pixel suite pins.
    D2D1_COLOR_F status = canvasColor_;
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
    island_->SetColor(islandColor_);
  }

  // C105: the air between a segment's label and its value, in DIPs. It is a
  // token (not an inline 6) because the value run's x is derived from it.
  static constexpr float kStatusValueGap = 6.0f;

  // The advance width of one run in its own format. Used for the status band's
  // label/value split and by nothing else -- a second measuring path would be a
  // second opinion about where the value starts.
  float MeasureRunWidth(const std::wstring& text, IDWriteTextFormat* format,
                        float maxWidth) const {
    if (!writeFactory_ || format == nullptr || text.empty() || maxWidth <= 0.0f) {
      return 0.0f;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(writeFactory_->CreateTextLayout(text.c_str(),
                                               static_cast<UINT32>(text.size()), format,
                                               maxWidth, 40.0f,
                                               layout.ReleaseAndGetAddressOf()))) {
      return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return metrics.widthIncludingTrailingWhitespace;
  }

  // V4: the status band (DESIGN.md §2 "状态条 28", §3 four states).
  //
  // Four segments, left to right: encryption | method | dictionary | ratio. The
  // rectangles and the text come from prism::layout::StatusSegments, so the
  // suite can assert "each state paints ink inside its own segment" without
  // duplicating a pixel table here. Nothing is centred by hand: the format is
  // paragraph-centred inside the segment, which is why the four labels share a
  // baseline no matter which state they are in.
  void DrawStatusBar(ID2D1RenderTarget* target, const Scene& scene,
                     const prism::layout::Layout& layout) {
    if (!statusFormat_ || layout.statusBar.Empty()) return;
    // C79: the counts are the scene's, unless the caller supplied them. Deriving
    // them HERE keeps GalleryWindow's contract unchanged while still letting the
    // snapshot and every list-mode frame show real numbers.
    prism::layout::StatusInfo info = scene.status;
    if (info.itemCount < 0) {
      if (scene.listItems != nullptr) {
        info.itemCount = static_cast<int>(scene.listItems->size());
      } else if (scene.items != nullptr) {
        info.itemCount = static_cast<int>(scene.items->size());
      }
    }
    if (info.selectedCount < 0) {
      info.selectedCount = scene.listMode ? (scene.selectedRow >= 0 ? 1 : 0)
                                          : (scene.selectedItem >= 0 ? 1 : 0);
    }
    const std::vector<prism::layout::StatusSegment> segments =
        prism::layout::StatusSegments(layout, info);
    for (const prism::layout::StatusSegment& segment : segments) {
      const D2D1_RECT_F rect = ToRectF(segment.rect);
      if (segment.id == prism::layout::StatusId::Divider) {
        // C79: a 1 px alpha-.08 rule at the midpoint of the empty middle. It is
        // a token, so HighContrast can raise it without a second code path.
        island_->SetColor(ToD2d(
            prism::theme::Lookup(prism::theme::TokenId::DividerSubtle, scene.theme)
                .color));
        target->FillRectangle(rect, island_.Get());
        island_->SetColor(islandColor_);
        continue;
      }
      float textLeft = rect.left;
      if (segment.id == prism::layout::StatusId::Encryption) {
        // A 6 px state dot: accent while encrypted, secondary while not, hollow
        // while unknown. Colour is never the ONLY signal (the text says it too).
        const D2D1_COLOR_F dot =
            info.crypto == prism::layout::CryptoState::Encrypted
                ? accentColor_
                : (info.crypto == prism::layout::CryptoState::Unencrypted
                       ? kTextSecondary
                       : kIslandBorder);
        const float cy = (rect.top + rect.bottom) * 0.5f;
        island_->SetColor(dot);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + 3.0f, cy), 3.0f, 3.0f),
                            island_.Get());
        island_->SetColor(islandColor_);
        textLeft = rect.left + 12.0f;
      }
      // C86: the label is spelled ONCE (prism::layout::StatusSegmentLabel), so
      // the band and the accessibility tree cannot read differently.
      const std::string label = prism::layout::StatusSegmentLabel(segment);
      if (label.empty()) continue;
      // C105 (plan 3.2): TWO TONES in one segment -- the leading word in the
      // secondary role, its value in the primary role. The split is the one
      // PrismCore spells (StatusSegmentValueText), so the painted value and the
      // announced one are the same string.
      const std::string labelText(segment.text);
      const std::string valueText = prism::layout::StatusSegmentValueText(segment);
      const std::wstring wideLabel = Utf8(labelText);
      target->DrawText(wideLabel.c_str(), static_cast<UINT32>(wideLabel.size()),
                       statusFormat_.Get(),
                       D2D1::RectF(textLeft, rect.top, rect.right, rect.bottom),
                       textSecondary_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      ++dbgStatusSegments;
      if (!valueText.empty()) {
        // The value starts after the MEASURED label, not at a guessed offset:
        // the label is localised ("压缩率" / "RATIO"), so its width is not a
        // constant this painter may assume.
        const float labelWidth =
            MeasureRunWidth(wideLabel, statusFormat_.Get(), rect.right - textLeft);
        const float valueLeft = textLeft + labelWidth + kStatusValueGap;
        const std::wstring wideValue = Utf8(valueText);
        if (valueLeft + 2.0f < rect.right && !wideValue.empty()) {
          target->DrawText(wideValue.c_str(), static_cast<UINT32>(wideValue.size()),
                           statusFormat_.Get(),
                           D2D1::RectF(valueLeft, rect.top, rect.right, rect.bottom),
                           textPrimary_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
          ++dbgStatusValues;
          dbgLastStatusValueLeft = valueLeft;
        }
      }
    }
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

  // C71: the two LIVE colours. appearance.accentColor / glassIntensity are
  // resolved once per frame (the theme runtime is process-global) instead of
  // the old hardcoded kAccent / kIslandFill literals.
  static D2D1_COLOR_F AccentOf(prism::theme::Variant variant) {
    // The pre-C71 kAccent brush was OPAQUE; the token's own alpha is a material
    // role used by DialogChrome, not by these solid fills/rings. Keep the alpha
    // contract identical so this change cannot repaint anything by accident.
    prism::theme::Rgba accent = prism::theme::EffectiveAccent(variant);
    accent.a = 1.0f;
    return ToD2d(accent);
  }
  // C91: the island fill (breadcrumb pill, progress capsule) is the FLOATING
  // ISLAND token, composited over THIS theme's canvas by the one PrismCore
  // entry. The old body was "a literal, and (void)variant" -- which is why the
  // breadcrumb island stayed (25,29,37) in all three themes.
  D2D1_COLOR_F IslandOf(prism::theme::Variant variant) const {
#if defined(PRISM_C91_MUTANT_ISLAND_LITERAL)
    // NEGATIVE CONTROL (CONTRACTS D3): the pre-C91 literal, variant ignored.
    ++dbgMutantIsland;
    D2D1_COLOR_F fill = kIslandFill;
    double alpha = 0.72 * prism::theme::GlassIntensityScale();
    fill.a = static_cast<float>(alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha));
    return fill;
#else
    // The canvas THIS frame cleared to, not a per-theme assumption: an island
    // painted over the veiled canvas and an island painted over the flat one are
    // different pixels, and only the frame knows which it is.
    const prism::theme::Rgba canvas(canvasColor_.r, canvasColor_.g, canvasColor_.b,
                                    canvasColor_.a);
    return ToD2d(prism::chrome::ThemeSurfaceColor(
        prism::theme::TokenId::SurfaceFloatingIsland, variant, canvas));
#endif
  }

  // ---------------------------------------------------------------- C91 theme
  // C91: the frame's theme. The mutation arm is here, in ONE place, so every
  // "the theme quietly went back to Midnight" control below really is the
  // shipped defect and not a lookalike.
  static prism::theme::Variant ThemeOf(const Scene& scene) {
#if defined(PRISM_C91_MUTANT_SCENE_MIDNIGHT)
    ++dbgMutantTheme;
    return prism::theme::Variant::Midnight;
#else
    return scene.theme;
#endif
  }

  // The canvas colour the target is cleared to. With the C77 backdrop layer ON
  // (the shipping default) the canvas pixel at rest is the VEILED composite;
  // with it off it is the flat one. The band fills read this same value, so the
  // action band can never be a rectangle of a slightly different canvas.
  static D2D1_COLOR_F CanvasOf(const Scene& scene, prism::theme::Variant theme) {
    return ToD2d(scene.backdropRefraction ? prism::chrome::CanvasPresentedColor(theme)
                                         : prism::chrome::CanvasColor(theme));
  }

  // C91: the text roles, recoloured EVERY frame. A brush built once from a
  // literal is the second half of "the theme did not reach the content": the
  // surfaces would change while every glyph stayed midnight.
  void ApplyThemeBrushes(prism::theme::Variant theme) {
#if defined(PRISM_C91_MUTANT_BRUSHES_ONCE)
    ++dbgMutantBrushes;  // the injection ran and left the brushes as built
#else
    if (textPrimary_) {
      textPrimary_->SetColor(ToD2d(prism::chrome::ThemeSurfaceColor(
          prism::theme::TokenId::TextPrimary, theme)));
    }
    if (textSecondary_) {
      textSecondary_->SetColor(ToD2d(prism::chrome::ThemeSurfaceColor(
          prism::theme::TokenId::TextSecondary, theme)));
    }
    if (sidebarBrush_) {
      sidebarBrush_->SetColor(ToD2d(prism::chrome::ThemeSurfaceColor(
          prism::theme::TokenId::TextPrimary, theme)));
    }
    // The Quick Look sheet's body (the no-glass path; the glass path resolves
    // its own tokens). It was kCardFill, a midnight literal.
    if (card_) {
      card_->SetColor(ToD2d(prism::chrome::ThemeSurfaceColor(
          prism::theme::TokenId::SurfaceFloatingIsland, theme)));
    }
#endif
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
      case prism::win::sidebar::ColorToken::SelectedBackground:
        // C79: the wash is a C15 token now (accent @14% in Daylight, @16% in
        // Midnight, @24% in HighContrast), not a hardcoded .32 that made the
        // selected row heavier than the design's 12-18% band.
        return color(TokenId::SelectionTint);
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
    // C105 (plan 3.2 "图标使用细线矢量替代原先的字符符号"): the info card's four
    // glyphs are STROKED outlines -- no filled plates, no character symbols.
    // They are shared with nothing else in the rail (the tree uses folder /
    // star / clock / chevron), so this is the card's whole icon language.
    if (icon == "lock") {
      target->DrawRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(l + w * 0.24f, t + h * 0.46f, r - w * 0.24f,
                                        b - h * 0.12f), 2.0f, 2.0f), brush, 1.3f);
      target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, t + h * 0.44f), w * 0.20f,
                                        h * 0.24f), brush, 1.3f);
      target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, t + h * 0.72f), 1.0f, 1.0f), brush);
      return;
    }
    if (icon == "bolt") {
      target->DrawLine(D2D1::Point2F(r - w * 0.26f, t + h * 0.14f),
                       D2D1::Point2F(l + w * 0.32f, cy), brush, 1.4f);
      target->DrawLine(D2D1::Point2F(l + w * 0.32f, cy),
                       D2D1::Point2F(r - w * 0.32f, cy), brush, 1.4f);
      target->DrawLine(D2D1::Point2F(r - w * 0.32f, cy),
                       D2D1::Point2F(l + w * 0.26f, b - h * 0.14f), brush, 1.4f);
      return;
    }
    if (icon == "drum") {
      // The DICTIONARY row: an archive drum in outline (lid ellipse, two walls,
      // a base and one band) -- the same 1.3 px stroke as the lock.
      target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, t + h * 0.28f), w * 0.30f,
                                        h * 0.16f), brush, 1.3f);
      target->DrawLine(D2D1::Point2F(cx - w * 0.30f, t + h * 0.28f),
                       D2D1::Point2F(cx - w * 0.30f, b - h * 0.26f), brush, 1.3f);
      target->DrawLine(D2D1::Point2F(cx + w * 0.30f, t + h * 0.28f),
                       D2D1::Point2F(cx + w * 0.30f, b - h * 0.26f), brush, 1.3f);
      target->DrawLine(D2D1::Point2F(cx - w * 0.30f, b - h * 0.26f),
                       D2D1::Point2F(cx + w * 0.30f, b - h * 0.26f), brush, 1.3f);
      target->DrawLine(D2D1::Point2F(cx - w * 0.30f, t + h * 0.54f),
                       D2D1::Point2F(cx + w * 0.30f, t + h * 0.54f), brush, 1.0f);
      return;
    }
    if (icon == "chart") {
      const float heights[3] = {0.36f, 0.66f, 0.90f};
      for (int i = 0; i < 3; ++i) {
        const float x = l + w * (0.16f + 0.28f * static_cast<float>(i));
        const float top = b - h * 0.14f - (h * 0.68f) * heights[i];
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x, top, x + w * 0.18f, b - h * 0.14f), 1.0f,
                              1.0f), brush, 1.2f);
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
      case PrimKind::StatusLabel:
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
      case PrimKind::SectionLabel: {
        ++dbgSidebarLabels;
        // C79: 11 px semibold + the command's tracking. DWrite exposes letter
        // spacing on the LAYOUT (not the format), so a heading is the one run
        // that gets its own layout; em units = DIPs / 11 px.
        const std::wstring wide = Utf8(command.text);
        ComPtr<IDWriteTextLayout> layout;
        if (sidebarSmall_ && !wide.empty() &&
            SUCCEEDED(writeFactory_->CreateTextLayout(
                wide.c_str(), static_cast<UINT32>(wide.size()), sidebarSmall_.Get(),
                box.right - box.left, box.bottom - box.top,
                layout.ReleaseAndGetAddressOf()))) {
          ComPtr<IDWriteTextLayout1> tracked;
          if (SUCCEEDED(layout.As(&tracked))) {
            tracked->SetCharacterSpacing(
                command.letterSpacing / 11.0f, 0.0f, 0.0f,
                DWRITE_TEXT_RANGE{0, static_cast<UINT32>(wide.size())});
          }
          target->DrawTextLayout(D2D1::Point2F(box.left, box.top + 2.0f), layout.Get(),
                                 sidebarBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
      }
      case PrimKind::Selection:
        // The focus pill: squircle radius 8 (blueprint 3.2 row highlight).
        FillSquircleRoundedRect(target, box, 8.0f, sidebarBrush_.Get());
        return;
      case PrimKind::SelectionBar:
        // C79: the 3 px accent rule. A squircle is the wrong shape for a 3 px
        // bar, so this is a plain rounded rect whose radius is half its width.
        target->FillRoundedRectangle(
            D2D1::RoundedRect(box, 1.5f, 1.5f), sidebarBrush_.Get());
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
      case PrimKind::StatusLabel:
        ++dbgSidebarLabels;
        // C105: the card's field label -- 10 px, secondary role, the line above
        // the value. Its box is the 12 px line the layout reserved for it, so no
        // vertical nudge is applied: the format centres the glyphs in it.
        DrawSidebarText(target, sidebarCardLabel_.Get(), command.text, box,
                        sidebarBrush_.Get(), 0.0f, 0.0f);
        return;
      case PrimKind::StatusText:
        ++dbgSidebarLabels;
        // C105: the card's VALUE line -- 12 px in the PRIMARY role (it used to
        // be secondary, which is what made the card read as one grey block).
        DrawSidebarText(target, sidebarStatus_.Get(), command.text, box,
                        sidebarBrush_.Get(), 0.0f, 0.0f);
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
    const prism::theme::Variant variant = scene.theme;
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
    if (count <= 0) return;
    // V4: the grid is a FLOW (WindowLayout::FlowCells). This layer only reads
    // the two FACTS the classification needs -- the item type and whether the
    // ThumbStore really holds pixels -- and hands them to the layout, which owns
    // every rectangle. A tile-less item therefore paints a 32 px BAND across the
    // content, not a 220 px pill inside a cell.
    std::vector<prism::layout::FlowItem> flow;
    flow.reserve(static_cast<size_t>(count));
    for (const prism::ArchiveItem& item : *scene.items) {
      prism::layout::FlowItem entry;
      entry.isDir = item.isDir;
      entry.kind = prism::QuickLookPlanner::KindByName(item.path);
      // Availability is a FACT read from the store, not a guess from the name.
      entry.thumbnailAvailable = scene.thumbs != nullptr &&
                                 prism::layout::ThumbnailCapable(entry.kind) &&
                                 scene.thumbs->Contains(item.index);
      if (scene.debugForceSkeleton) {
        // TEST SEAM: force the tile path (a photo with no pixels -> skeleton).
        // The flow classifies from these fields, so "available" is what makes
        // it a tile; the painter is then told there are no pixels (below), which
        // is exactly the state the skeleton colour exists for.
        entry.isDir = false;
        entry.kind = prism::PreviewKind::Image;
        entry.thumbnailAvailable = true;
      }
      flow.push_back(entry);
    }
    const int maxScroll = prism::layout::FlowMaxScroll(layout, flow);
    const int scroll = (std::min)(
        (std::max)(0, scene.viewport ? static_cast<int>(scene.viewport->Scroll()) : 0),
        maxScroll);
    const std::vector<prism::layout::FlowCell> cells =
        prism::layout::VisibleFlowCells(layout, flow, scroll);
    for (const prism::layout::FlowCell& cell : cells) {
      if (cell.index < 0 || cell.index >= count) continue;
      const prism::ArchiveItem& item = (*scene.items)[static_cast<size_t>(cell.index)];
      if (cell.kind == prism::layout::CellKind::Row) {
        DrawFallbackRow(target, scene, cell, item);
      } else if (item.isDir) {
        DrawFolderTile(target, scene, cell.rect, item);
      } else {
        DrawThumbTile(target, scene, cell.rect, item,
                      !scene.debugForceSkeleton &&
                          flow[static_cast<size_t>(cell.index)].thumbnailAvailable);
      }
    }
  }

  static D2D1_RECT_F CellRectF(const prism::layout::Rect& cell) { return ToRectF(cell); }

  // P0-2: a cell with no thumbnail is a 32 px ROW (icon + name + size), never
  // a skeleton card. V4: the row IS the band the layout handed in -- full
  // content width, 32 px tall, contiguous with its neighbours.
  void DrawFallbackRow(ID2D1RenderTarget* target, const Scene& scene,
                       const prism::layout::FlowCell& cell, const prism::ArchiveItem& item) {
    ++dbgRowCells;
    const D2D1_RECT_F band = ToRectF(cell.rect);
    const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(band, 10.0f, 10.0f);
    target->FillRoundedRectangle(pill, tile_.Get());
    target->DrawRoundedRectangle(pill, tileBorder_.Get(), 1.0f);
    // Type glyph: a 16 px token-coloured mark (vector icons are a later pass).
    const float iconTop = band.top + 8.0f;
    const float iconBottom = band.bottom - 8.0f;
    islandBorder_->SetColor(prism::QuickLookPlanner::KindByName(item.path) ==
                                    prism::PreviewKind::Image
                                ? accentColor_
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
    DrawSelectionRing(target, scene, cell.rect, item.index, 10.0f);
  }

  // C111: a cubic-bezier rounded rectangle. The control-point ratio is above
  // the circular 0.5523 on purpose: the corner leaves the straight edge more
  // gradually, which is the continuous-corner ("squircle") read of the macOS
  // folder instead of four arcs welded to four lines.
  static HRESULT CreateSquirclePath(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                                    float radius, ID2D1PathGeometry** out) {
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.ReleaseAndGetAddressOf());
    if (!factory) return E_FAIL;
    ComPtr<ID2D1PathGeometry> path;
    HRESULT hr = factory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1GeometrySink> sink;
    hr = path->Open(sink.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    const float halfMin =
        (std::min)(rect.right - rect.left, rect.bottom - rect.top) / 2.0f;
    const float r = (std::max)(0.0f, (std::min)(radius, halfMin));
    constexpr float k = 0.62f;  // > 0.5523: continuous corner, no visible joint
    const float l = rect.left;
    const float t = rect.top;
    const float rt = rect.right;
    const float b = rect.bottom;
    sink->BeginFigure(D2D1::Point2F(l + r, t), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(rt - r, t));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(rt - r + k * r, t), D2D1::Point2F(rt, t + r - k * r),
        D2D1::Point2F(rt, t + r)));
    sink->AddLine(D2D1::Point2F(rt, b - r));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(rt, b - r + k * r), D2D1::Point2F(rt - r + k * r, b),
        D2D1::Point2F(rt - r, b)));
    sink->AddLine(D2D1::Point2F(l + r, b));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(l + r - k * r, b), D2D1::Point2F(l, b - r + k * r),
        D2D1::Point2F(l, b - r)));
    sink->AddLine(D2D1::Point2F(l, t + r));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(l, t + r - k * r), D2D1::Point2F(l + r - k * r, t),
        D2D1::Point2F(l + r, t)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    if (FAILED(hr)) return hr;
    *out = path.Detach();
    return S_OK;
  }

  static D2D1_COLOR_F MixColor(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to,
                               float amount) {
    const float t = (std::max)(0.0f, (std::min)(1.0f, amount));
    D2D1_COLOR_F out;
    out.r = from.r + (to.r - from.r) * t;
    out.g = from.g + (to.g - from.g) * t;
    out.b = from.b + (to.b - from.b) * t;
    out.a = from.a + (to.a - from.a) * t;
    return out;
  }

  // C111: the folder glyph, in TWO layers instead of two rectangles.
  //   * a back tab: narrower than the face, slightly higher, a darker accent;
  //   * a front face: full width, a 1 px rim one step brighter than its fill,
  //     and a specular row along its top edge.
  // Every dimension is a fraction of the CELL (56 % wide, 1.22:1), so the glyph
  // scales with the tile; every colour is derived from the frame's accent token
  // (the live AccentLiquidPrimary), the theme's TextPrimary and the frame's own
  // island surface -- no colour literal is written here.
  void DrawFolderGlyph(ID2D1RenderTarget* target, const Scene& scene,
                       const D2D1_RECT_F& cellRect) {
    const prism::theme::Variant theme = ThemeOf(scene);
    const float cellW = cellRect.right - cellRect.left;
    const float usableBottom = cellRect.bottom - 28.0f;  // DrawNamePlate's top
    const float cx = (cellRect.left + cellRect.right) * 0.5f;
    const float cy = (cellRect.top + usableBottom) * 0.5f - 2.0f;

    const float iconW = cellW * 0.56f;
    const float iconH = iconW / 1.22f;
    // Snap the outline to whole pixels: the specular row must be a real row,
    // not a half-covered anti-aliased smear (B2 measures exactly that row).
    D2D1_RECT_F box;
    box.left = std::floor(cx - iconW * 0.5f + 0.5f);
    box.right = std::floor(cx + iconW * 0.5f + 0.5f);
    box.top = std::floor(cy - iconH * 0.5f + 0.5f);
    box.bottom = std::floor(cy + iconH * 0.5f + 0.5f);
    const float w = box.right - box.left;
    const float h = box.bottom - box.top;
    const float frontTop = box.top + h * 0.08f;  // the tab reaches slightly higher
    const float radius = h * 0.12f;

    const D2D1_COLOR_F light = ToD2d(prism::chrome::ThemeSurfaceColor(
        prism::theme::TokenId::TextPrimary, theme));
    const D2D1_COLOR_F base = accentColor_;
    D2D1_COLOR_F frontFill = MixColor(base, islandColor_, 0.16f);
    frontFill = MixColor(frontFill, light, 0.10f);
    frontFill.a = 1.0f;
    // The back layer is the same accent, darker: 62 % of each channel. Scaling
    // a token is a material operation, not a new colour authority.
    const D2D1_COLOR_F tabFill = D2D1::ColorF(
        base.r * 0.62f, base.g * 0.62f, base.b * 0.62f, 1.0f);
    const D2D1_COLOR_F frontRim = MixColor(frontFill, light, 0.42f);
    const D2D1_COLOR_F frontSheen = MixColor(frontFill, light, 0.85f);

    // Back tab: narrower (5 % in from the left, 68 % of the width), darker, and
    // its lower half hidden behind the face.
    const D2D1_RECT_F tab = D2D1::RectF(box.left + w * 0.05f, box.top,
                                        box.left + w * 0.68f, frontTop + h * 0.22f);
    ComPtr<ID2D1PathGeometry> tabPath;
    if (SUCCEEDED(CreateSquirclePath(target, tab, radius * 0.8f,
                                     tabPath.ReleaseAndGetAddressOf()))) {
      island_->SetColor(tabFill);
      target->FillGeometry(tabPath.Get(), island_.Get());
    }
#if defined(PRISM_C111_MUTANT_NO_HIGHLIGHT)
    // NEGATIVE CONTROL (D3): both 1 px specular rows are gone. B2 must go red.
    ++MutantNoHighlightHits();
#else
    // 1 px specular on the tab's top edge, inset by the corner radius.
    island_->SetColor(light);
    target->FillRectangle(D2D1::RectF(tab.left + radius * 0.8f, box.top,
                                      tab.right - radius * 0.8f, box.top + 1.0f),
                          island_.Get());
#endif

    // Front face: full width, rim one step brighter than the fill, and a second
    // specular row inside its top edge.
    const D2D1_RECT_F face = D2D1::RectF(box.left, frontTop, box.right, box.bottom);
    ComPtr<ID2D1PathGeometry> facePath;
    if (SUCCEEDED(CreateSquirclePath(target, face, radius,
                                     facePath.ReleaseAndGetAddressOf()))) {
      island_->SetColor(frontFill);
      target->FillGeometry(facePath.Get(), island_.Get());
      island_->SetColor(frontRim);
      target->DrawGeometry(facePath.Get(), island_.Get(), 1.0f);
    }
#if !defined(PRISM_C111_MUTANT_NO_HIGHLIGHT)
    island_->SetColor(frontSheen);
    target->FillRectangle(D2D1::RectF(face.left + radius, face.top + 0.5f,
                                      face.right - radius, face.top + 1.5f),
                          island_.Get());
#endif
    island_->SetColor(islandColor_);
    accent_->SetColor(accentColor_);
  }

  void DrawFolderTile(ID2D1RenderTarget* target, const Scene& scene,
                      const prism::layout::Rect& cell, const prism::ArchiveItem& item) {
    ++dbgTileCells;
    constexpr float kRadius = 16.0f;
    const D2D1_RECT_F rect = CellRectF(cell);
    const D2D1_ROUNDED_RECT pane = D2D1::RoundedRect(rect, kRadius, kRadius);
    target->FillRoundedRectangle(pane, tile_.Get());
    target->DrawRoundedRectangle(pane, tileBorder_.Get(), 1.0f);
#if defined(PRISM_C111_MUTANT_TWO_RECTS)
    // NEGATIVE CONTROL (D3): the pre-C111 glyph, unchanged -- two solid rounded
    // rectangles in one accent, no rim, no specular. B1 must go red.
    ++MutantTwoRectsHits();
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f - 14.0f;
    D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
        D2D1::RectF(cx - 34.0f, cy - 22.0f, cx + 34.0f, cy + 24.0f), 7.0f, 7.0f);
    target->FillRoundedRectangle(folder, accent_.Get());
    folder = D2D1::RoundedRect(D2D1::RectF(cx - 34.0f, cy - 30.0f, cx + 6.0f, cy - 14.0f),
                               6.0f, 6.0f);
    target->FillRoundedRectangle(folder, accent_.Get());
#else
    DrawFolderGlyph(target, scene, rect);
#endif
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
    island_->SetColor(islandColor_);
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
    islandBorder_->SetColor(accentColor_);
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
                    const prism::layout::Layout& layout,
                    bool backdropPass = false) {
    if (!scene.listItems) return;
    // C101: this frame's bars replace the previous frame's.
    ratioBarCount_ = 0;
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

    // C90: GLYPHS ARE NOT A REFRACTION SOURCE. At 1/8 a 13 px run of near-white
    // text rasterizes into a solid mini row (measured source coverage 0.81
    // against 0.09 of real ink); 4 mini px of row pitch plus sigma 1.75 fuses
    // those rows into one band, and the veil leaves 35% of it -- the white
    // rectangles over the name and ratio columns. The header was excluded for
    // exactly this reason (audit A2); the rows were missed. Folder icons, the
    // ratio capsule and the selection tint STAY: they are the content a
    // refraction layer is supposed to carry, and they keep C77-1's "the layer
    // exists" measurable.
#if defined(PRISM_C77_MUTANT_TEXT_IN_BACKDROP)
    // C90 MUTANT ACTIVE: text-in-backdrop -- the shipped behaviour, restored.
    // The counter is what the suite's verdict is armed on (D3): the injection
    // provably ran before CAUGHT may be printed.
    const bool drawGlyphs = true;
    if (backdropPass) ++dbgBackdropGlyphPasses;
#else
    const bool drawGlyphs = !backdropPass;
#endif
    // C90 (follow-up): the same rule covers the row's STATE. A 32 px selection
    // pill is 4 mini px of solid accent at 1/8 -- the blur turns it into a
    // full-width band that outlives the selection (the cache key does not carry
    // it) and reads as a rounded panel smeared down the list. Draw()'s own
    // contract says the ghost is built "from the content only, never from the
    // chrome, so a hover or a focus pill cannot make the wallpaper shimmer";
    // the selected row is that pill. Icons and the ratio capsule stay: they
    // are content and they are what C77-1 measures.
#if defined(PRISM_C77_MUTANT_STATE_IN_BACKDROP)
    // C90 MUTANT ACTIVE: state-in-backdrop -- the shipped tint, restored.
    const bool drawRowState = true;
    if (backdropPass) ++dbgBackdropStatePasses;
#else
    const bool drawRowState = !backdropPass;
#endif

    // Header -- C81/A2: FLAT text on the canvas + a 1 px rule under it. No
    // panel, no ring. The band is repainted canvas-flat first: the C77
    // backdrop is a blurred still of the CONTENT, and its bleed into the
    // header strip (plus the crisp labels drawn over it) is exactly the
    // "灰底面板 + 原始大小重影" of audit A2. The backdrop PASS never draws the
    // header at all, so the blurred still cannot contain header text either.
    if (!backdropPass) {
      island_->SetColor(canvasColor_);
      target->FillRectangle(ToRectF(header), island_.Get());
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
      // The one 1 px separator (DESIGN: flat text + a rule, nothing else).
      island_->SetColor(ToD2d(prism::theme::Lookup(prism::theme::TokenId::DividerSubtle,
                                                   scene.theme)
                                  .color));
      target->FillRectangle(D2D1::RectF(static_cast<float>(header.left),
                                        static_cast<float>(header.bottom) - 1.0f,
                                        static_cast<float>(header.right),
                                        static_cast<float>(header.bottom)),
                            island_.Get());
#if defined(PRISM_C81_MUTANT_HEADER_SELECTED)
      // C81 MUTANT ACTIVE: header_selected -- the audit's original sin: the
      // header band gets the SELECTED-ROW vocabulary on top (rounded accent
      // panel + 2 px accent ring). Drawn LAST so the flat fill cannot hide
      // it; the pixel assertions MUST go red.
      const D2D1_ROUNDED_RECT mutantPanel = D2D1::RoundedRect(
          D2D1::RectF(static_cast<float>(header.left) + 8.0f,
                      static_cast<float>(header.top) + 2.0f,
                      static_cast<float>(header.right) - 8.0f,
                      static_cast<float>(header.bottom) - 2.0f),
          10.0f, 10.0f);
      D2D1_COLOR_F mutantFill = accentColor_;
      mutantFill.a = 0.26f;
      accent_->SetColor(mutantFill);
      target->FillRoundedRectangle(mutantPanel, accent_.Get());
      accent_->SetColor(accentColor_);
      target->DrawRoundedRectangle(mutantPanel, accent_.Get(), 2.0f);
#endif
      island_->SetColor(islandColor_);
    }

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
      if (drawRowState && i == scene.selectedRow) {
        // Selection highlight: G2 squircle R=10, accent fluid fill.
        D2D1_ROUNDED_RECT sel = D2D1::RoundedRect(
            D2D1::RectF(rowRect.left + 8.0f, top + 2.0f, rowRect.right - 8.0f, bottom - 2.0f),
            10.0f, 10.0f);
        D2D1_COLOR_F selColor = accentColor_;
        selColor.a = 0.26f;
        accent_->SetColor(selColor);
        target->FillRoundedRectangle(sel, accent_.Get());
        accent_->SetColor(accentColor_);
      }

      const std::wstring name = Utf8(row.name);
      const float nameLeft = rowRect.left + prism::layout::kListIndent;
      if (row.isDir) {
        const D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
            D2D1::RectF(nameLeft + 4.0f, top + 9.0f, nameLeft + 20.0f, top + 27.0f), 5.0f,
            5.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
        if (drawGlyphs) {
          DrawLabel(target, caption_.Get(), name,
                    D2D1::RectF(nameLeft + 28.0f, top + 5.0f, nameRight, bottom - 4.0f),
                    textPrimary_.Get());
          DrawTrailingLabel(target, L"--", sizeX, top + 6.0f, cols.sizeW - 18,
                            textSecondary_.Get());
          DrawTrailingLabel(target, L"--", packedX, top + 6.0f, cols.packedW - 18,
                            textSecondary_.Get());
        }
      } else {
        if (drawGlyphs) {
          DrawLabel(target, caption_.Get(), name,
                    D2D1::RectF(nameLeft, top + 5.0f, nameRight, bottom - 4.0f),
                    textPrimary_.Get());
          DrawTrailingLabel(target, Utf8(prism::list::FormatBytes(row.size)), sizeX,
                            top + 6.0f, cols.sizeW - 18, textPrimary_.Get());
          DrawTrailingLabel(target, Utf8(prism::list::FormatBytes(row.packedSize)), packedX,
                            top + 6.0f, cols.packedW - 18, textPrimary_.Get());
        }
      }

      // 压缩率 cell -- C81/A3: THE canonical view string, shared with the
      // info card and the status band. The micro-capsule bar (blueprint ch.
      // 3.2.3) stays decorative and only shows for a real shrink; an expanded
      // (">100% (...)") or unknown row renders the text across the column so
      // the longer honest string fits.
      if (prism::list::RatioBarVisible(row)) {
        const auto bar = prism::list::RatioBar(row);
        const float trackX = right - cols.ratioW + 8.0f;
        // C101 (ui-redesign-plan §2.2): the bar is a READOUT, not a rule. The
        // shipped one was 6 px of full-alpha #007AFF across the entire 130 px
        // ratio column -- the user's "刺眼荧光绿厚条". This one is 4 px tall with
        // a 2 px radius, at most 56 px wide (it never spans the column), and its
        // hue comes from the MEASURED ratio through PrismCore's RatioBarTint,
        // where the mute (alpha 0.55 + desaturation) is explained and pinned.
        float barWidth = (std::min)(prism::list::kRatioBarMaxWidth,
                                    static_cast<float>(cols.ratioW) - 64.0f);
        float halfH = prism::list::kRatioBarHeight * 0.5f;
        float radius = prism::list::kRatioBarRadius;
        const prism::list::Rgb tint = prism::list::RatioBarTint(bar->percent);
        D2D1_COLOR_F fillColor =
            D2D1::ColorF(tint.r, tint.g, tint.b, prism::list::kRatioBarAlpha);
#if defined(PRISM_C101_MUTANT_LOUD)
        // C101 MUTANT ACTIVE: the shipped bar is restored -- 6 px, radius 3,
        // alpha 1.0, the full ratio column, the file-kind colour. The suite's
        // geometry, alpha and pixel assertions MUST go red on this arm.
        barWidth = static_cast<float>(cols.ratioW) - 64.0f;
        halfH = 3.0f;
        radius = 3.0f;
        fillColor = BarColorOf(bar->color);
        ++dbgMutantRatioBars;
#endif
        const float cy = top + prism::layout::kListRowHeight / 2.0f;
        const D2D1_ROUNDED_RECT track =
            D2D1::RoundedRect(D2D1::RectF(trackX, cy - halfH, trackX + barWidth, cy + halfH),
                              radius, radius);
        tileBorder_->SetColor(kTileBorder);
        target->FillRoundedRectangle(track, tileBorder_.Get());
        const D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
            D2D1::RectF(trackX, cy - halfH, trackX + barWidth * bar->percent / 100.0f, cy + halfH),
            radius, radius);
        tileBorder_->SetColor(fillColor);
        target->FillRoundedRectangle(fill, tileBorder_.Get());
        // C101: the read-back seam. The colour is taken from the brush AFTER
        // SetColor, so the suite measures what reached the device rather than
        // what the caller intended.
        // The backdrop pass paints the same bars into a 1/8 target; the
        // read-back is about the frame the user sees, so only that pass records.
        if (!backdropPass) {
          RecordRatioBar(track.rect, fill.rect, radius, tileBorder_->GetColor(), bar->percent);
        }
        tileBorder_->SetColor(kTileBorder);
        if (drawGlyphs) {
          const std::wstring pct = Utf8(prism::list::RatioCellText(row));
          DrawTrailingLabel(target, pct, right - cols.ratioW + 8.0f + barWidth + 6.0f,
                            top + 6.0f, 40, textSecondary_.Get());
        }
      } else if (drawGlyphs) {
        DrawTrailingLabel(target, Utf8(prism::list::RatioCellText(row)),
                          right - cols.ratioW + 8.0f, top + 6.0f, cols.ratioW - 16,
                          textSecondary_.Get());
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

  // C-V2: the band itself. The model decides how V1's actionIsland is shared
  // with the breadcrumb pill; this only hands those numbers to the painter, so
  // a click (model Hit) and a pixel (button rect) cannot disagree.
  void DrawActionBar(ID2D1RenderTarget* target, const Scene& scene,
                     const prism::layout::Layout& layout) {
    if (!scene.actions) return;
    const prism::actionbar::Layout band = prism::actionbar::Compute(layout);
    // C67: the painter's counters are CUMULATIVE for its own lifetime, so
    // "+= actionBar_.dbgButtons" counted every earlier frame again: a band that
    // drew 8 buttons once and nothing afterwards reported 16 after two paints.
    // Measured here -- the "8 painted while the geometry said 0" false alarm
    // that this card's band test hit first. Only the DELTA of this frame may be
    // accumulated.
    const int beforeButtons = actionBar_.dbgButtons;
    const int beforeIcons = actionBar_.dbgIcons;
    const int beforeLabels = actionBar_.dbgLabels;
    const int beforeIslands = actionBar_.dbgIslands;
    const int beforeClipped = actionBar_.dbgLabelsClipped;
    const int beforeDisabled = actionBar_.dbgDisabled;
    const int beforeHover = actionBar_.dbgHover;
    const int beforePressed = actionBar_.dbgPressed;
    const int beforeDropdowns = actionBar_.dbgDropdowns;
    // C77: the island's BASE and EDGE only. Its FILL stays ActionBarPainter's
    // (C64 pins it to CompositeOver(SurfaceFloatingIsland, canvas), and a second
    // fill under it would have repainted that contract from the outside).
    if (scene.glassPanels && !band.island.Empty()) {
      glassPanel_.PaintShadow(target, ToRectF(band.island), scene.theme, &glassStats_);
    }
    actionBar_.Draw(target, band, *scene.actions, scene.actionHover, scene.actionPressed,
                    scene.theme);
    if (scene.glassPanels && !band.island.Empty()) {
      prism::win::glass::Parts parts;
      parts.shadow = false;
      parts.fill = false;
      parts.border = false;
      glassPanel_.PaintRim(target, ToRectF(band.island), scene.theme, parts, &glassStats_);
    }
    dbgActionButtons += actionBar_.dbgButtons - beforeButtons;
    dbgActionIcons += actionBar_.dbgIcons - beforeIcons;
    dbgActionLabels += actionBar_.dbgLabels - beforeLabels;
    dbgActionIslands += actionBar_.dbgIslands - beforeIslands;
    dbgActionClippedLabels += actionBar_.dbgLabelsClipped - beforeClipped;
    dbgActionDisabled += actionBar_.dbgDisabled - beforeDisabled;
    dbgActionHover += actionBar_.dbgHover - beforeHover;
    dbgActionPressed += actionBar_.dbgPressed - beforePressed;
    dbgActionDropdowns += actionBar_.dbgDropdowns - beforeDropdowns;
  }

  // The breadcrumb pill: geometry from the action band (DESIGN.md §2), so it
  // can never overlap the title bar, the status bar or a content card.
  void DrawBreadcrumbIsland(ID2D1RenderTarget* target, const Scene& scene,
                            const prism::layout::Layout& layout) {
    // C-V2: while an action band is present the pill is the SPLIT one -- the
    // toolbar may have taken width back, and the window hit-tests the same
    // rectangle (GalleryWindow::WindowLayoutForClient -> Compute -> navPill).
    // Without a band this is exactly V1's rect, so every pre-V2 scene (and the
    // snapshot) keeps its framing to the pixel.
    const prism::actionbar::Layout band =
        scene.actions != nullptr ? prism::actionbar::Compute(layout)
                                 : prism::actionbar::Layout{};
    const prism::layout::Rect pillRect =
        scene.actions != nullptr ? band.navPill : prism::layout::NavPillRect(layout);
    if (pillRect.Empty()) return;
    const prism::layout::Rect textRect =
        scene.actions != nullptr ? prism::actionbar::NavTextRect(band)
                                 : prism::layout::NavTextRect(layout);
    const D2D1_RECT_F pill = ToRectF(pillRect);
    const float height = static_cast<float>(pillRect.Height());
    if (scene.glassPanels) {
      // C77: fill + top specular rim + drop shadow + squircle 14, in one call.
      prism::win::glass::Parts parts;
      glassPanel_.Paint(target, pill, scene.theme, parts, &glassStats_);
    } else {
      const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(pill, height / 2.0f, height / 2.0f);
      target->FillRoundedRectangle(shape, island_.Get());
      target->DrawRoundedRectangle(shape, islandBorder_.Get(), 1.0f);
    }
    (void)height;
    // C93: the hover capsule goes UNDER the words. It is the crumb's own span,
    // painted through the same rounded-rect vocabulary as the pill.
    if (scene.crumbHoverRect.Width() > 0 && scene.crumbHoverRect.Height() > 0) {
      const D2D1_ROUNDED_RECT hover =
          D2D1::RoundedRect(ToRectF(scene.crumbHoverRect), 8.0f, 8.0f);
      islandBorder_->SetColor(kCrumbHover);
      target->FillRoundedRectangle(hover, islandBorder_.Get());
      islandBorder_->SetColor(kIslandBorder);
    }
    DrawLabel(target, title_.Get(), scene.breadcrumb, ToRectF(textRect), textPrimary_.Get());
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
    if (scene.glassPanels) {
      // C77: the Quick Look sheet is a glass panel like the rest of the chrome.
      prism::win::glass::Parts parts;
      glassPanel_.Paint(target, card, scene.theme, parts, &glassStats_);
    } else {
      const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(card, 24.0f, 24.0f);
      target->FillRoundedRectangle(shape, card_.Get());
      target->DrawRoundedRectangle(shape, islandBorder_.Get(), 1.2f);
    }

    // Preview area: the REAL decoded pixels when the sweep has them (C87).
    // The gradient is the honest "no pixels yet" state, never a stand-in for an
    // image the archive already holds -- that was the reported "pure blue card".
    thumbs_[1]->SetStartPoint(D2D1::Point2F(card.left, card.top));
    thumbs_[1]->SetEndPoint(D2D1::Point2F(card.left, card.top + height - 110.0f));
    const prism::layout::Rect previewRect = prism::layout::QuickLookPreviewRect(layout);
    const D2D1_ROUNDED_RECT preview = D2D1::RoundedRect(ToRectF(previewRect), 16.0f, 16.0f);
    target->FillRoundedRectangle(preview, thumbs_[1].Get());
#if defined(PRISM_C87_MUTANT_PREVIEW_PLACEHOLDER)
    // MUTANT (preview_placeholder): the card keeps the gradient even though the
    // decoded pixels are in the store -- exactly the reported defect.
    const prism::gallery::DecodedThumb* previewThumb = nullptr;
#else
    const prism::gallery::DecodedThumb* previewThumb =
        (scene.thumbs != nullptr && scene.quickLookItem >= 0)
            ? scene.thumbs->Lookup(static_cast<uint32_t>(scene.quickLookItem))
            : nullptr;
#endif
    if (previewThumb != nullptr && previewThumb->width > 0 && previewThumb->height > 0) {
      const uint32_t itemIndex = static_cast<uint32_t>(scene.quickLookItem);
      SyncThumbBitmap(target, itemIndex, *previewThumb, scene.thumbs->Version());
      const auto bitmap = thumbBitmaps_.find(itemIndex);
      if (bitmap != thumbBitmaps_.end() && bitmap->second) {
        ++dbgQuickLookThumbDraws;
        const prism::layout::Rect fitRect =
            prism::layout::FitContain(previewRect, static_cast<int>(previewThumb->width),
                                      static_cast<int>(previewThumb->height));
        const D2D1_RECT_F fit = ToRectF(fitRect);
        target->FillRoundedRectangle(preview, tile_.Get());
        ComPtr<ID2D1PathGeometry> clip;
        if (SUCCEEDED(CreateSquircleGeometry(target, preview.rect, 16.0f,
                                             clip.ReleaseAndGetAddressOf()))) {
          D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters(
              preview.rect, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
          target->PushLayer(layer, nullptr);
        }
        target->DrawBitmap(bitmap->second.Get(), fit, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (clip) target->PopLayer();
      }
    }

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

 public:
  // ------------------------------------------------------------------ C75
  // What the last CreateDeviceResources() bound to the target, and how to let
  // it go. A minimised window cannot use any of it; the whole set is rebuilt by
  // CreateDeviceResources() inside the restore Resize, before the next paint.
  // Counts are derived from the live COM pointers (C33's rule: accounting from
  // what is held, never from a parallel counter that can drift).
  int BrushCount() const {
    int n = 0;
    if (accent_) ++n;
    if (tile_) ++n;
    if (tileBorder_) ++n;
    if (island_) ++n;
    if (card_) ++n;
    if (islandBorder_) ++n;
    if (textPrimary_) ++n;
    if (textSecondary_) ++n;
    if (sidebarBrush_) ++n;
    for (const auto& gradient : thumbs_) {
      if (gradient) ++n;
    }
    return n + actionBar_.BrushCount() + tabPainter_.BrushCount();
  }
  int TextFormatCount() const {
    int n = 0;
    if (caption_) ++n;
    if (title_) ++n;
    if (small_) ++n;
    if (sidebarLabel_) ++n;
    if (sidebarSmall_) ++n;
    if (sidebarStatus_) ++n;
    if (sidebarCardLabel_) ++n;
    if (statusFormat_) ++n;
    return n + actionBar_.TextFormatCount() + tabPainter_.TextFormatCount();
  }
  size_t ThumbBitmapCount() const { return thumbBitmaps_.size(); }
  size_t ThumbBitmapBytes() const {
    size_t bytes = 0;
    for (const auto& entry : thumbBitmaps_) {
      if (!entry.second) continue;
      const D2D1_SIZE_U size = entry.second->GetPixelSize();
      bytes += static_cast<size_t>(size.width) * size.height * 4u;
    }
    return bytes;
  }
  bool DeviceResourcesLive() const { return accent_ != nullptr; }
  // C100: the band painter itself, so a suite can read its device-state facts
  // (MnemonicGlyphsDrawn / IconCurveCount / the stroke style's caps) after a
  // REAL Draw through the real renderer. Exposing the painter beats duplicating
  // its geometry in a test: the numbers then describe the shipping pixels.
  const prism::win::actionbar::ActionBarPainter& ActionBand() const { return actionBar_; }
  // Drops every target-bound object (brushes, text formats, gradient stops, the
  // device-bitmap cache of decoded thumbnails). The D2D/DWrite FACTORIES are
  // kept: they are small, shared, and re-creating them costs more than they hold.
  void ReleaseDeviceResources() {
    accent_.Reset();
    tile_.Reset();
    tileBorder_.Reset();
    island_.Reset();
    card_.Reset();
    islandBorder_.Reset();
    textPrimary_.Reset();
    textSecondary_.Reset();
    sidebarBrush_.Reset();
    caption_.Reset();
    title_.Reset();
    small_.Reset();
    sidebarLabel_.Reset();
    sidebarSmall_.Reset();
    sidebarStatus_.Reset();
    sidebarCardLabel_.Reset();
    statusFormat_.Reset();
    for (auto& gradient : thumbs_) gradient.Reset();
    thumbBitmaps_.clear();
    thumbVersion_ = 0;
    // C77: the cached backdrop is a device object too; a minimised window must
    // let it go with everything else (C75's whole point).
    glassPanel_.Unbind();
    backdropBitmap_.Reset();
    backdropKey_ = BackdropKey{};
    actionBar_.ReleaseDeviceResources();
    tabPainter_.ReleaseDeviceResources();
  }

 private:
  // C101: the read-back seam's storage, refreshed by every frame that paints a
  // list. Kept tiny on purpose: 64 bars is more than a 1080p row window holds.
  void RecordRatioBar(const D2D1_RECT_F& track, const D2D1_RECT_F& fill, float radius,
                      const D2D1_COLOR_F& color, uint32_t percent) {
    if (ratioBarCount_ >= kMaxRatioBarPaints) return;
    RatioBarPaint& entry = ratioBars_[ratioBarCount_++];
    entry.track = track;
    entry.fill = fill;
    entry.radius = radius;
    entry.color = color;
    entry.percent = percent;
  }
  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> writeFactory_;
  // C71: the live colours this frame resolved (see Draw).
  D2D1_COLOR_F accentColor_{0.243f, 0.545f, 1.0f, 1.0f};
  D2D1_COLOR_F islandColor_{0.10f, 0.12f, 0.16f, 0.72f};
  // C91: the canvas this frame cleared to -- the SAME colour the action band and
  // the list header refill themselves with (see DrawBands / DrawListMode).
  D2D1_COLOR_F canvasColor_{0.043f, 0.055f, 0.075f, 1.0f};

  ComPtr<ID2D1SolidColorBrush> accent_, tile_, tileBorder_, island_, card_, islandBorder_;
  ComPtr<ID2D1SolidColorBrush> textPrimary_, textSecondary_;
  // C29 sidebar: one recolourable brush + the two single-line text formats.
  ComPtr<ID2D1SolidColorBrush> sidebarBrush_;
  ComPtr<IDWriteTextFormat> sidebarLabel_, sidebarSmall_;
  // C79: the capsule readout (12 px); headings reuse sidebarSmall_. C105: it is
  // the card's VALUE line (primary role), and sidebarCardLabel_ is the 10 px
  // label line above it.
  ComPtr<IDWriteTextFormat> sidebarStatus_;
  ComPtr<IDWriteTextFormat> sidebarCardLabel_;
  ComPtr<IDWriteTextFormat> statusFormat_;
  // C-V2: the action band's painter (own brush + text formats).
  prism::win::actionbar::ActionBarPainter actionBar_;
  // C60: the tab strip's painter (own brush + two text formats).
  prism::win::tabbar::Painter tabPainter_;
  ComPtr<ID2D1LinearGradientBrush> thumbs_[3];
  std::unordered_map<uint32_t, ComPtr<ID2D1Bitmap>> thumbBitmaps_;
  uint64_t thumbVersion_ = 0;
  // C77: the cached refraction layer (1/8 size, already blurred) and its key.
  ComPtr<ID2D1Bitmap> backdropBitmap_;
  BackdropKey backdropKey_{};
  // C77: the glass-panel helper (borrows sidebarBrush_, owns nothing).
  prism::win::glass::PanelPainter glassPanel_;
  prism::win::glass::Stats glassStats_;

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
  // C87: proof that the Quick Look card painted DECODED pixels (not the
  // gradient). The suite's preview assertion keys on this counter, so a card
  // that silently fell back to the stand-in cannot pass on luck.
  int dbgQuickLookThumbDraws = 0;
  // C91: proof that a mutation arm's injected branch really ran. Each mutant
  // binary arms its CAUGHT verdict on one of these, so a red that came from an
  // unrelated assertion can never print a false verdict (CONTRACTS D3).
  static inline int dbgMutantTheme = 0;
  static inline int dbgMutantBrushes = 0;
  static inline int dbgMutantIsland = 0;
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
  // V4: labels that needed the ellipsis (0 for the shipping table). The painter
  // owns the measurement; this is the seam the polish suite reads.
  int ActionLabelsClipped() const { return actionBar_.dbgLabelsClipped; }
  int ActionLabelsPainted() const { return actionBar_.dbgLabels; }

  // C-V2: proof that the action band reached the device, and that each visual
  // state was actually painted (the hover/disabled/focus assertions key on
  // these being non-zero -- a zero would make the pixel diff meaningless).
  int dbgActionButtons = 0;
  int dbgActionIcons = 0;
  int dbgActionLabels = 0;
  int dbgActionIslands = 0;
  int dbgActionClippedLabels = 0;
  int dbgActionDisabled = 0;
  int dbgActionHover = 0;
  int dbgActionPressed = 0;
  int dbgActionDropdowns = 0;
  // V4: proof that the status band painted its segments (four per frame).
  int dbgStatusSegments = 0;
  // C105: how many segment VALUES were painted in the PRIMARY role, and where
  // the last one started. The band's two-tone split is only real if the value
  // run exists AND lands to the right of its label, so the suite reads both
  // instead of trusting a brush constant.
  int dbgStatusValues = 0;
  float dbgLastStatusValueLeft = -1.0f;
  // C77: the backdrop layer's build/draw counters (see BackdropRebuilds).
  int dbgBackdropRebuilds = 0;
  int dbgBackdropDraws = 0;
  // C90: list-mode backdrop passes that drew the row GLYPHS. Zero in every
  // clean build (the increment is compiled into the mutant arm only); it is the
  // runtime half of the negative control's verdict (CONTRACTS D3): a mutant
  // that never reached the injected branch must not print CAUGHT.
  int dbgBackdropGlyphPasses = 0;
  // C101: how many ratio bars the LOUD mutant arm painted. Zero in every clean
  // build (the increment is compiled in only under PRISM_C101_MUTANT_LOUD), so a
  // mutant that never reached the injected branch cannot print CAUGHT (D3).
  static inline int dbgMutantRatioBars = 0;
  // C101: THE RATIO BAR AS IT REACHED THE DEVICE. One entry per bar the last
  // frame painted, in row order. The rectangles are the ones handed to
  // FillRoundedRectangle and the colour is read back from the brush AFTER
  // SetColor, so a suite cannot pass by agreeing with the caller's intent while
  // the target draws something else.
  struct RatioBarPaint {
    D2D1_RECT_F track{};
    D2D1_RECT_F fill{};
    float radius = 0.0f;
    D2D1_COLOR_F color{};
    uint32_t percent = 0;
  };
  static constexpr int kMaxRatioBarPaints = 64;
  int RatioBarsPainted() const { return ratioBarCount_; }
  const RatioBarPaint& RatioBarPainted(int index) const { return ratioBars_[index]; }

  // C90 follow-up: list-mode backdrop passes that drew the selection pill
  // (mutant arm only; zero in the clean build).
  int dbgBackdropStatePasses = 0;
  // C60: proof that the TAB STRIP reached pixels. TabsPainted() == 0 while no
  // archive is open is the positive half of case 7 ("the band is not drawn"),
  // and > 0 with two tabs is what makes case 1 a paint assertion instead of a
  // model assertion.
  // C77: the refraction layer's evidence. Rebuilds counts the expensive builds
  // (size/content changes only); draws counts every frame that composited it.
  int BackdropRebuilds() const { return dbgBackdropRebuilds; }
  int BackdropDraws() const { return dbgBackdropDraws; }
  int BackdropGlyphPasses() const { return dbgBackdropGlyphPasses; }
  int BackdropStatePasses() const { return dbgBackdropStatePasses; }
  int BackdropBitmapPixels() const {
    if (!backdropBitmap_) return 0;
    const D2D1_SIZE_U size = backdropBitmap_->GetPixelSize();
    return static_cast<int>(size.width * size.height);
  }
  // C77: what the panel helper painted (fill / rim / shadow counts).
  const prism::win::glass::Stats& GlassStats() const { return glassStats_; }
  void ResetGlassStats() { glassStats_ = prism::win::glass::Stats{}; }
  int TabBarTabsPainted() const { return tabPainter_.TabsPainted(); }
  int TabBarDirtyDotsPainted() const { return tabPainter_.DirtyDotsPainted(); }
  int TabBarInsertMarksPainted() const { return tabPainter_.InsertMarksPainted(); }

 private:
  ComPtr<IDWriteTextFormat> caption_, title_, small_;
  // C101: the read-back seam's storage (the type is declared in the public
  // block above; the frame's bars are refreshed by every list paint).
  RatioBarPaint ratioBars_[kMaxRatioBarPaints];
  int ratioBarCount_ = 0;
};

}  // namespace prism::win::gallery
