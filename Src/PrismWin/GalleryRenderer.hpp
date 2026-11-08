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
#include "../PrismCore/FileTypeIcon.hpp"    // C124: the 16 DIP file-type glyphs
#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/GlassIsland.hpp"
#include "../PrismCore/ListLayout.hpp"
#include "../PrismCore/PathSafety.hpp"  // FileNameOf
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/ProgressHud.hpp"
#include "../PrismCore/SidebarModel.hpp"
#include "../PrismCore/ThemeTokens.hpp"
#include "TabBar.hpp"  // C60: the tab strip (geometry + pixels)
#include "Typography.hpp"  // C103: the ONE font stack + TextFormat entry (BLUEPRINT:170)
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismCore/WindowLayout.hpp"  // V1: the ONE geometry authority
#include "ActionBar.hpp"    // C-V2: the action-band painter (DESIGN.md §2)
#include "GlassPanel.hpp"   // C77: the ONE glass-panel helper (fill/rim/shadow)
#include "ListType.hpp"     // C109: the numeric-column format + tnum, one authority
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
  // C110: THE TILE BODY IS A MATERIAL, NOT A LITERAL.
  //
  // It used to be this one value -- white at 6 % -- in every theme. On the
  // Midnight canvas (16,17,23) that reads as a glass card (+14 levels). On the
  // #F5F5F7 Day canvas it composites to 246 on 245: ONE level, i.e. no card at
  // all, which is why the folder tiles "float in the air" on the Day theme. A
  // tile has to be a material in both directions: DARKER than the canvas on Day
  // (black 5 %), LIGHTER on Night (white 6 %, unchanged) and on High Contrast
  // (white 10 % on black). TileFillFor() below is the ONE place that decides.
  static constexpr D2D1_COLOR_F kTileFill = {1.0f, 1.0f, 1.0f, 0.06f};  // = Night (pre-C110 literal)
  static constexpr D2D1_COLOR_F kTileFillDay = {0.0f, 0.0f, 0.0f, 0.05f};
  static constexpr D2D1_COLOR_F kTileFillNight = {1.0f, 1.0f, 1.0f, 0.06f};
  static constexpr D2D1_COLOR_F kTileFillHighContrast = {1.0f, 1.0f, 1.0f, 0.10f};
  static constexpr D2D1_COLOR_F kTileBorder = {1.0f, 1.0f, 1.0f, 0.12f};
  static constexpr D2D1_COLOR_F kIslandFill = {0.10f, 0.12f, 0.16f, 0.72f};   // floating glass
  static constexpr D2D1_COLOR_F kCardFill = {0.086f, 0.10f, 0.13f, 0.90f};    // Quick Look body
  static constexpr D2D1_COLOR_F kIslandBorder = {1.0f, 1.0f, 1.0f, 0.16f};
  // C124: #0A84FF -- THE ONE ACCENT. This is a build-time DEFAULT only: Draw()
  // recolours the brush from EffectiveAccent() every frame, so the value a frame
  // paints is always the token's. It used to be a fourth blue (#3E8BFF) that
  // nothing but a failed init could show.
  static constexpr D2D1_COLOR_F kAccent = {0.039216f, 0.517647f, 1.0f, 1.0f};  // #0A84FF
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

  // C104 negative controls (CONTRACTS D3/D26). Each arm is the SHIPPED defect,
  // not a lookalike: `square` paints the pre-C104 hard-edged selection
  // rectangle, `folder-opaque` restores the solid accent plate for the icon.
#if defined(PRISM_C104_MUTANT_SQUARE)
  static constexpr bool kC104SquareSelection = true;
#else
  static constexpr bool kC104SquareSelection = false;
#endif
#if defined(PRISM_C104_MUTANT_FOLDER_OPAQUE)
  static constexpr bool kC104FolderOpaque = true;
#else
  static constexpr bool kC104FolderOpaque = false;
#endif

  static constexpr std::string_view ActiveMutation() {
    if (kSidebarPaintDisabled) return "no-sidebar-paint";
    if (kSidebarCollapseInverted) return "collapse-inverted";
    if (kC104SquareSelection) return "square";
    if (kC104FolderOpaque) return "folder-opaque";
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
    // C122: the DEFAULT VIEW IS THE LIST. Bandizip opens a four-column listing
    // and the audit's verdict was "the first screen is another category of
    // product" -- the grid stays one Ctrl+1 (or View > 网格) away.
    //
    // A scene that carries items but NO list rows still paints the grid: the
    // effective mode is scene.listMode && listItems != nullptr (see Draw). That
    // is what keeps every pre-C122 grid scene -- which never set this field --
    // byte-identical instead of silently going blank.
    //
    // D3 NEGATIVE CONTROL: PRISM_C122_MUTANT_GRID_DEFAULT restores the grid
    // default; L1 (this class's Scene{}.listMode) MUST go red.
#if defined(PRISM_C122_MUTANT_GRID_DEFAULT)
    bool listMode = false;
#else
    bool listMode = true;
#endif
    std::vector<prism::list::SortItem>* listItems = nullptr;
    uint32_t listScroll = 0;
    int selectedRow = -1;
    // C122 §3: which header cell shows the sort arrow, and which way it points.
    // The WINDOW owns the state (a click on the header folds into its own sort
    // spec); the scene only says what to paint. Default: 名称 ascending, which
    // is the order the list opens in.
    prism::list::HeaderSort listSort;
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
    // C123: the 2 px accent rule under the crumb the user is STANDING ON. The
    // window computes it (it owns the measured spans); the renderer only paints
    // the rectangle, so the "seam" cannot be drawn a few pixels from the word
    // it marks. Empty in a legacy scene -- nothing is painted.
    prism::layout::Rect crumbCurrentRect;

    // C60: the tab strip. nullptr -> no strip at all (every pre-C60 scene, and
    // the "no archive is open" state, where the band is hidden and the action
    // island sits directly under the title). The band's rectangle comes from
    // V1's layout (Layout::tabBar); this view only says what to paint in it.
    const prism::win::tabbar::View* tabs = nullptr;

    // ------------------------------------------------------------- C77 glass
    // The BACKDROP LAYER: the PAGE BACKGROUND (canvas + static bands + rail)
    // rendered at 1/8 resolution, blurred and washed, painted under everything.
    // C121 took the ITEMS out of it: a refraction source that carries the
    // archive turns every photo into a coloured halo between its neighbours
    // (see the block above DrawPageBackground), and it is not what "glass over
    // a page" means.
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
    // C112: WHO OWNS THE BREADCRUMB PILL. When true the island (a DComp visual
    // ABOVE the window) paints the pill's fill, rim, hover capsule and TEXT, and
    // the window paints only the drop shadow plus the geometry it hit-tests
    // with. Painting the text here as well is the C107 defect: the island
    // captures this frame as its backdrop, blurs it, and paints the blur back
    // over the text -- a second, offset, blurred copy of every glyph.
    // Opt-in like every other optional layer: a pre-C112 scene is unchanged.
    bool pillOwnedByIsland = false;
    // A caller-side bump for "the items changed". The renderer ALSO fingerprints
    // the catalog (count + first/middle/last path), so a rename rebuilds even
    // when nobody bumps this; the field exists so a test can force the change.
    uint64_t contentRevision = 0;
  };

  // C122: the EFFECTIVE view mode of a scene. "list mode" is a REQUEST plus
  // the rows to honour it with: a scene that asks for the list but carries no
  // list rows (every pre-C122 grid scene, and a sidebar-only composition) paints
  // the grid instead of a blank content band. The renderer has no third state.
  static bool ListModeActive(const Scene& scene) {
    return scene.listMode && scene.listItems != nullptr;
  }

  // C122 D3: which default this binary carries, and the runtime proof the
  // mutant arm really compiled in (the mutant suite's CAUGHT marker is armed on
  // "a default Scene asks for the grid", i.e. on the injected defect itself).
  static constexpr bool DefaultViewIsList() {
#if defined(PRISM_C122_MUTANT_GRID_DEFAULT)
    return false;
#else
    return true;
#endif
  }

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
  // C122: the list header's REAL DirectWrite format (11 px), read by the suite
  // the same way CardLabelFormat is: "the header is 11 px" is measured on the
  // object the painter draws with.
  IDWriteTextFormat* ColumnHeaderFormat() const { return columnHeader_.Get(); }
  IDWriteTextFormat* CardValueFormat() const { return sidebarStatus_.Get(); }

  // ---------------------------------------------------------------- C112
  // The pill's CONTENT, painted by the ISLAND into the island's own surface.
  //
  // Why it has to live here and not in the window: the pill's geometry, its
  // panel language and its TEXT format are the renderer's (PillGeometryFor +
  // title_ + the tokens). A second implementation in the window would be a
  // second answer to "where does the breadcrumb start", which is the class of
  // bug that burned six rounds on the crumb hit test (batch A).
  //
  // The surface is ISLAND-LOCAL: the island's bitmap origin is the pill's
  // corner, so every rectangle is offset by the pill's origin. The caller has
  // already drawn the optics underneath (PaintIslandInto -> chain_.Draw), so
  // whatever this paints sits ON TOP of the glass and can never be fed back
  // into it -- that is the whole fix.
  bool PaintPillOnIsland(ID2D1RenderTarget* target, const std::wstring& text,
                         const prism::layout::Layout& layout,
                         const prism::actionbar::Model* actions, prism::theme::Variant theme,
                         const prism::layout::Rect& hoverRect, bool glassMaterial) {
    if (!target) return false;
    const PillGeometry geometry = PillGeometryFor(layout, actions);
    if (geometry.pill.Empty()) return false;
    const float ox = static_cast<float>(geometry.pill.left);
    const float oy = static_cast<float>(geometry.pill.top);
    const float width = static_cast<float>(geometry.pill.Width());
    const float height = static_cast<float>(geometry.pill.Height());

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                                             brush.GetAddressOf()))) {
      return false;
    }
    if (glassMaterial) {
      prism::win::glass::PanelPainter painter;
      painter.Bind(target, brush.Get());
      prism::win::glass::Parts parts;
      parts.shadow = false;  // the drop shadow falls OUTSIDE: the window keeps it
      painter.Paint(target, D2D1::RectF(0.0f, 0.0f, width, height), theme, parts, &glassStats_);
      painter.Unbind();
    } else {
      // appearance.backdropRefraction OFF: the pill is the pre-C77 flat capsule,
      // painted by whichever surface owns it. Without this arm the setting would
      // stop reaching the pill the moment the island took it over.
      const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(
          D2D1::RectF(0.0f, 0.0f, width, height), height / 2.0f, height / 2.0f);
      brush->SetColor(kIslandFill);
      target->FillRoundedRectangle(shape, brush.Get());
      brush->SetColor(kIslandBorder);
      target->DrawRoundedRectangle(shape, brush.Get(), 1.0f);
    }

    // The crumb under the pointer goes UNDER the words (C93's order).
    if (hoverRect.Width() > 0 && hoverRect.Height() > 0) {
      const D2D1_ROUNDED_RECT hover = D2D1::RoundedRect(
          D2D1::RectF(static_cast<float>(hoverRect.left) - ox,
                      static_cast<float>(hoverRect.top) - oy,
                      static_cast<float>(hoverRect.right) - ox,
                      static_cast<float>(hoverRect.bottom) - oy),
          8.0f, 8.0f);
      // C117: the hover wash is white 14% -- invisible on the Daylight pill.
      // Dark themes keep it; Daylight flips to a black wash of the same weight.
      brush->SetColor(theme == prism::theme::Variant::Daylight
                           ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f)
                           : kCrumbHover);
      target->FillRoundedRectangle(hover, brush.Get());
    }

    if (!text.empty()) {
      // C117: this used the midnight LITERAL (kTextPrimary = .91/.925/.945), so on
      // the Daylight canvas the breadcrumb was near-white on a near-white pill.
      // Measured on the shipped build: contrast 1.08:1 (WCAG wants 4.5). The
      // window-side painter already resolves the token every frame; the island
      // path C112 added must do the same.
      brush->SetColor(ToD2d(prism::chrome::ThemeSurfaceColor(
          prism::theme::TokenId::TextPrimary, theme)));
      DrawLabel(target, title_.Get(), text,
                D2D1::RectF(static_cast<float>(geometry.text.left) - ox,
                            static_cast<float>(geometry.text.top) - oy,
                            static_cast<float>(geometry.text.right) - ox,
                            static_cast<float>(geometry.text.bottom) - oy),
                brush.Get());
      ++dbgPillTextOnIsland;
    }
    ++dbgPillOnIsland;
    return true;
  }

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

    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        caption_.ReleaseAndGetAddressOf());
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        title_.ReleaseAndGetAddressOf());
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        small_.ReleaseAndGetAddressOf());
    // C122: the list header. 11 px, regular weight, secondary role -- the
    // design's "表头 11px 次要色" (proposal 4.2 / 5.3). Its own format (not a
    // size passed at DrawText time) so a suite can read the real size back.
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        columnHeader_.ReleaseAndGetAddressOf());
    if (columnHeader_) {
      columnHeader_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      columnHeader_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    // C109: the numeric columns (size / packed / ratio). One format, trailing
    // alignment, tabular advances -- see ListType.hpp for why the family is
    // monospaced instead of "the proportional face + tnum".
    hr = prism::win::listtype::CreateNumericFormat(
        writeFactory_.Get(), numbers_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    if (caption_) caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (title_) title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // C29: one recolourable brush for the rail (dozens of tiny runs; a brush per
    // token would churn device state) and two single-line text formats -- a
    // wrapped label would spill into its neighbour and be clipped mid-glyph.
    hr = target->CreateSolidColorBrush(kTextPrimary, sidebarBrush_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        sidebarLabel_.ReleaseAndGetAddressOf());
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        sidebarSmall_.ReleaseAndGetAddressOf());
    if (sidebarLabel_) {
      sidebarLabel_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      sidebarLabel_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (sidebarSmall_) sidebarSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    // C79: the status capsule's readout is 12 px secondary text (the group
    // headings reuse sidebarSmall_, the existing 11 px semibold format).
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        sidebarStatus_.ReleaseAndGetAddressOf());
    if (sidebarStatus_) {
      sidebarStatus_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      sidebarStatus_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // C105: the info card's LABEL line. 10 px, regular weight, secondary role --
    // the line ABOVE a value, per plan 3.2's "上行 10px 灰色小字". It is its own
    // IDWriteTextFormat (not a size passed at DrawText time) so a suite can read
    // the REAL format back through CardLabelFormat() and pin the size.
    // C103: this one came in with C105 after C103 branched, still carrying the
    // literal family. Every format goes through the resolved stack -- that is
    // what core.c103_font_literals asserts (Src/ carries no literal family name outside
    // Typography.hpp).
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 10.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        L"en-US", sidebarCardLabel_.ReleaseAndGetAddressOf());
    if (sidebarCardLabel_) {
      sidebarCardLabel_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      sidebarCardLabel_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // V4/C79: the status band's readout format (12 px, one line, leading,
    // centred in its 20 px inner band so the six segments share one baseline).
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, L"en-US",
        statusFormat_.ReleaseAndGetAddressOf());
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
    // C124: this frame's seams replace the previous frame's (the read-back is a
    // measurement of ONE frame, exactly like the C101 ratio-bar read-back).
    seamCount_ = 0;
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
    // C110: the tile body follows the THEME, every frame. It used to be built
    // once from the white-6 % literal, so every tile on every canvas was a
    // +1-level ghost of a card.
    if (tile_) tile_->SetColor(TileFillFor(theme));
    // V1: ONE layout for the whole frame -- bands, grid metrics, list body and
    // chrome. A component that wants a rectangle asks this object.
    // V4: the rail is CHROME (DESIGN.md §2 lists it as a band beside the
    // content, §3 lists it with its own four states), so it reserves width in
    // BOTH views. C29's "rail only in list mode" rule is the policy this card
    // reverses: the grid is not a different window, and a rail that disappears
    // when the user presses Ctrl+2 is a layout jump, not a mode.
    const bool railColumn = SidebarShown(scene);
    // C122: one answer for the whole frame -- the bands, the backdrop key, the
    // content pass and the status counts all use THIS, not scene.listMode
    // directly, so a scene without list rows cannot half-paint a list.
    const bool listMode = ListModeActive(scene);
    const prism::layout::Layout layout =
        scene.layout != nullptr
            ? *scene.layout
            : prism::layout::Compute(static_cast<int>(size.width),
                                     static_cast<int>(size.height), railColumn,
                                     listMode);
    target->Clear(canvasColor_);
    // C77: the refraction layer sits under EVERYTHING -- bands, content, chrome.
    // C121: it is built from the PAGE, never from the content and never from
    // state, so neither a photo, nor a hover, nor a focus pill can put a colour
    // into the background.
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
    if (listMode) {
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
  // THE BACKDROP LAYER (DESIGN.md 2.1), AS C121 SCOPE RE-STATES IT.
  //
  // WHAT IT IS: the PAGE BACKGROUND -- the canvas, the two static bands and the
  // rail -- re-rendered into a 1/8-resolution compatible bitmap, blurred with a
  // 15-tap separable gaussian (sigma 3.5 mini px = 28 px at 1:1), then
  // composited and washed with the veil token. Only THEN does the chrome paint.
  //
  // WHAT IT IS NOT: the archive. Thumbnails, file/folder glyphs, labels,
  // skeletons and the selection ring never enter the source. C110 had already
  // taken the tile CHROME out (fill, folder mark, label, ring) and kept the
  // decoded thumbnail "because it is archive content, and the content is the
  // wallpaper"; C77-1 then pinned "the layer must refract the items" with a
  // pixel count, which is what kept the thumbnail in. A 1/8 sample of a photo,
  // blurred 28 px, is a colour PLATE, not a soft glow: 0.35 x (210,60,50) +
  // 0.65 x (12,13,17) = (81,29,29), measured (81,29,28) in the 13 px gap
  // between two tiles (chroma 53 against 7 in the no-refraction frame), spread
  // 46-48 lp = 4.4x the gap, 21 % of the content area tinted. THE RULE (the
  // audit's R1) is therefore: a refraction source carries the PAGE, never the
  // content. C121 re-defined C77-1 to assert exactly that, and the same change
  // makes the layer chroma-NEUTRAL: the veil is the canvas colour in every
  // theme, so switching refraction on cannot move the canvas (it moved
  // (16,17,23) -> (13,14,18), dE 2.4, before).
  //
  // WHY 1/8 AND NOT A FULL-SIZE BLUR: at 1/8 a 28 px blur is a 3.5 px blur --
  // 30 DrawBitmap calls on a 200x125 surface instead of a full-window pass, and
  // the upscale back to 1:1 is free (the bilinear tap IS the last blur step).
  //
  // REBUILD POLICY: only on a SIZE or CONTENT change. Scrolling does not
  // rebuild (a backdrop that chased the scroll would be a full-window blur per
  // frame); the ghost is a still of the PAGE, which is what a refraction layer
  // is. dbgBackdropRebuilds / dbgBackdropDraws are the evidence.
  // C121: THE KEY IS THE IDENTITY OF THE PAGE BACKGROUND the layer is built
  // from -- its SIZE, its THEME, the content revision and the view mode. Two
  // fields are deliberately absent and each absence is a decision, not an
  // oversight:
  //   * scrollOffset: the ghost carries no pixel that moves with the scroll
  //     (the content is out of the source), so a key that carried it would pay
  //     a full-window blur per wheel notch for a bitmap that is byte-identical.
  //     The C121 suite pins that as an INVARIANT instead (scroll 100 px -> same
  //     ghost, zero rebuilds), which is C77-3's contract with the reason now
  //     spelled out.
  //   * selectedRow / hover: state is chrome, and chrome never enters the ghost.
  // THEME IS IN, and it is not cosmetic: the source is OPAQUE canvas colour, so
  // a stale bitmap left over from the previous theme would paint the OLD canvas
  // over the new one -- the whole window keeps the previous theme's background.
  // That was invisible while the ghost was a transparent content still (the
  // thumbnails do not change with the theme), which is exactly why nobody had
  // noticed the missing field.
  struct BackdropKey {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t count = 0;
    const void* items = nullptr;
    uint64_t fingerprint = 0;
    uint64_t revision = 0;
    bool listMode = false;
    prism::theme::Variant theme = prism::theme::Variant::Midnight;
    bool operator==(const BackdropKey& other) const {
      return width == other.width && height == other.height && count == other.count &&
             items == other.items && fingerprint == other.fingerprint &&
             revision == other.revision && listMode == other.listMode &&
             theme == other.theme;
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
    key.listMode = ListModeActive(scene);
    key.theme = scene.theme;
#if defined(PRISM_C121_MUTANT_KEY_NO_THEME)
    // C121 MUTANT ACTIVE: key-no-theme -- the shipped key, restored. A theme
    // switch then re-uses the previous theme's OPAQUE ghost, so the frame keeps
    // the old canvas colour; the suite's H4 MUST go red. The counter is what the
    // verdict is armed on (CONTRACTS D3), so a red for another reason stays red
    // without a false CAUGHT.
    ++dbgMutantKeyNoTheme;
    key.theme = prism::theme::Variant::Midnight;
#endif
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

    // 1. the PAGE BACKGROUND, at 1/8. C121: the content passes that used to run
    //    here (DrawListMode from C90, DrawGrid from C110) are gone from the
    //    clean build -- they are the negative-control arms below and nothing
    //    else. The counters are still saved and restored: this pass is not the
    //    frame's paint, and inflating dbgTileCells here would have made the
    //    C29/C36 pixel assertions read a number no frame ever produced.
    ComPtr<ID2D1BitmapRenderTarget> content;
    if (FAILED(makeTarget(content.ReleaseAndGetAddressOf()))) return false;
    content->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    content->BeginDraw();
    content->Clear(D2D1::ColorF(0, 0, 0, 0));
    content->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / kDownsample, 1.0f / kDownsample));
    const int savedTiles = dbgTileCells;
    const int savedRows = dbgRowCells;
    const int savedThumbDraws = dbgThumbDraws;
    DrawPageBackground(content.Get(), scene, layout, size);
#if defined(PRISM_C110_MUTANT_TILES_IN_BACKDROP)
    // C110 MUTANT ACTIVE: tiles-in-backdrop -- the shipped grid pass, restored:
    // the tiles (fill, folder mark, label, thumbnail) are re-drawn into the
    // ghost. C110's A1 MUST go red; dbgBackdropTilePasses, incremented inside
    // DrawGrid, is what its verdict is armed on (CONTRACTS D3).
    if (ListModeActive(scene)) {
      DrawListMode(content.Get(), scene, layout, /*backdropPass=*/true);
    } else if (scene.items != nullptr) {
      DrawGrid(content.Get(), scene, layout, /*backdropPass=*/true);
    }
#elif defined(PRISM_C77_MUTANT_TEXT_IN_BACKDROP) || defined(PRISM_C77_MUTANT_STATE_IN_BACKDROP)
    // C90 MUTANT ACTIVE: the list's content pass is restored so the injected
    // glyph / selection pill reaches the ghost again. The verdicts are armed on
    // the counters DrawListMode increments (CONTRACTS D3).
    if (ListModeActive(scene)) {
      DrawListMode(content.Get(), scene, layout, /*backdropPass=*/true);
    }
#elif defined(PRISM_C121_MUTANT_TILES_IN_GHOST)
    // C121 MUTANT ACTIVE: tiles-in-ghost -- the DECODED THUMBNAIL is back in the
    // refraction source (C110's rule, the defect C121 removes). H1 (the chroma
    // of the gap between two tiles) MUST go red; dbgBackdropEntryPasses is what
    // the verdict is armed on (CONTRACTS D3).
    ++dbgBackdropEntryPasses;
    if (scene.items != nullptr) {
      DrawGrid(content.Get(), scene, layout, /*backdropPass=*/true);
    }
#endif
    dbgTileCells = savedTiles;
    dbgRowCells = savedRows;
    dbgThumbDraws = savedThumbDraws;
    content->SetTransform(D2D1::Matrix3x2F::Identity());
    if (FAILED(content->EndDraw())) return false;
    ComPtr<ID2D1Bitmap> stage;
    if (FAILED(content->GetBitmap(stage.ReleaseAndGetAddressOf()))) return false;

    // 2. the blur: two separable passes, 15 taps each (sigma = 28 px at 1:1).
    //    C121: the PANEL_GLOW arm widens the radius to 64 px, so the panel
    //    edges' neutral falloff runs past the 32 px the page is allowed -- H5 MUST
    //    go red. That is the assertion that stops an approved, bounded falloff
    //    from one day growing back into the halo this card removed.
#if defined(PRISM_C121_MUTANT_PANEL_GLOW)
    ++dbgMutantPanelGlow;
    constexpr float kBlurRadiusPx = 64.0f;
#else
    constexpr float kBlurRadiusPx = 28.0f;
#endif
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
    // chosen for the STILL-TO-COME weight: drawing tap i leaves it with the final
    // weight a_i * prod_{j>i}(1 - a_j), so a_i must be
    //     g_i / (1 - sum_{j>i} g_j)          [suffix, not prefix]
    // C121: the divisor used to be the PREFIX sum, which is the mirror image and
    // is wrong for exactly this 8-bit target: the first taps then carry alphas
    // like 1e-4 (they vanish in 8 bits) while the LAST taps carry 0.5..1.0, so
    // the "blur" was a copy of the source displaced by ~5 mini px. Measured on
    // the ghost's own PNG: the rail's 50 % edge sat at 254 when the panel ends
    // at 220, and a 200x200 marker's plateau was off-centre by +19. With the
    // suffix form the same step's 50 % edge lands at 27.5 of 27.5 mini px.
    float alphas[2 * kTaps + 1];
    float suffix = 0.0f;
    for (int i = 2 * kTaps; i >= 0; --i) {
      const float g = weights[i] / total;
      alphas[i] = suffix < 1.0f ? (g / (1.0f - suffix)) : 1.0f;
      suffix += g;
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

  // C121: THE REFRACTION SOURCE = THE PAGE BACKGROUND.
  //
  // The ghost is the canvas plus the surfaces the PAGE itself paints: the two
  // static bands and the rail's panel. Nothing here came out of the archive, so
  // nothing here can put an item's colour into the background -- that is the
  // defect (the halo) this card removes, and it is asserted two ways in
  // Tests/win/test_c121_halo.cpp: the chroma in the gap between two photo tiles
  // (H1) and the spread of any chroma away from a tile edge (H3).
  //
  // WHY THE CANVAS IS FILLED OPAQUE. The 1/8 target used to be cleared to
  // transparent and then had the content drawn on it, so every pixel the content
  // did not cover kept alpha 0 and the ghost was a cut-out. The page background
  // is the opposite kind of object: it covers the frame, so the ghost does too,
  // and the blur has a real background to carry instead of a hole. That is also
  // what makes the veil chroma-neutral -- the veil is the canvas colour, so
  // "veil over ghost" lands on the canvas the frame was cleared to.
  //
  // The rectangles are the LAYOUT's and the colours are the SAME ones the bands
  // and the rail paint with (StatusBandColor / TokenColor(Panel)), never a
  // second copy: a source that invented its own colour would refract a surface
  // no frame ever shows.
  void DrawPageBackground(ID2D1RenderTarget* target, const Scene& scene,
                          const prism::layout::Layout& layout, const D2D1_SIZE_F& size) {
#if defined(PRISM_C121_MUTANT_EMPTY_GHOST)
    // C121 MUTANT ACTIVE: empty-ghost -- the layer is built from NOTHING, i.e.
    // the page background is out of the source too. The re-defined C77-1 ("the
    // ghost carries the rail's panel") MUST go red. The counter is what that
    // verdict is armed on (CONTRACTS D3).
    ++dbgMutantEmptyGhost;
    return;
#endif
    island_->SetColor(canvasColor_);
    target->FillRectangle(D2D1::RectF(0.0f, 0.0f, size.width, size.height), island_.Get());
    if (!layout.actionIsland.Empty() && !layout.statusBar.Empty()) {
      // The action band is the canvas (C64's rule), so it contributes nothing;
      // filling it anyway keeps the source a faithful copy of the bands.
      island_->SetColor(canvasColor_);
      target->FillRectangle(ToRectF(layout.actionIsland), island_.Get());
      island_->SetColor(StatusBandColor());
      target->FillRectangle(ToRectF(layout.statusBar), island_.Get());
    }
    if (SidebarShown(scene) && scene.sidebar != nullptr) {
      const prism::win::sidebar::Layout& rail = *scene.sidebar;
      const D2D1_RECT_F box =
          D2D1::RectF(rail.panel.left, (std::max)(rail.panel.top, 0.0f), rail.panel.right,
                      (std::min)(rail.panel.bottom, size.height));
      if (box.right > box.left && box.bottom > box.top) {
        island_->SetColor(
            ToD2d(TokenColor(prism::win::sidebar::ColorToken::Panel, scene.theme)));
        target->FillRectangle(box, island_.Get());
      }
    }
    island_->SetColor(islandColor_);
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
    // C121: the recipe moved into StatusBandColor() because the refraction
    // source carries the same band -- two spellings would let the ghost refract
    // a rectangle the frame never paints.
    island_->SetColor(StatusBandColor());
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

  // C121: THE STATUS BAND'S COLOUR, once. The band paints it and the refraction
  // source carries it; a second copy would be a second authority on what the
  // band "is".
  D2D1_COLOR_F StatusBandColor() const {
    D2D1_COLOR_F status = canvasColor_;
    status.r += 0.016f;
    status.g += 0.018f;
    status.b += 0.022f;
    return status;
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
      info.selectedCount = ListModeActive(scene) ? (scene.selectedRow >= 0 ? 1 : 0)
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
  // C110: THE TILE BODY'S material, per theme (see the kTileFill* tokens).
  // The three values are the design's own separation lifts: a card has to be
  // visible as a surface, and "visible" is a different direction on a light
  // canvas than on a dark one. Returning a literal here is the defect this card
  // removes -- the negative control restores exactly that.
  static D2D1_COLOR_F TileFillFor(prism::theme::Variant variant) {
#if defined(PRISM_C110_MUTANT_FLAT_TILE)
    // C110 MUTANT ACTIVE: flat-tile -- the pre-C110 white 6 % in every theme.
    // A3 (Day body >= 6 levels off the canvas) MUST go red.
    ++dbgMutantFlatTile;
    (void)variant;
    return kTileFill;
#else
    switch (variant) {
      case prism::theme::Variant::Daylight: return kTileFillDay;
      case prism::theme::Variant::HighContrast: return kTileFillHighContrast;
      case prism::theme::Variant::Midnight: break;
    }
    return kTileFillNight;
#endif
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
      case prism::win::sidebar::ColorToken::Panel:
        // C124: THE RAIL IS THE PANEL TIER, not the floating island at 94 %.
        // That 94 % wash is what made the passive column the loudest surface in
        // the window (UI-AUDIT 12.2: 1.32:1 against the canvas, while the action
        // island measured 1.15:1). The rail steps BACK to 1.06:1 and the island
        // comes FORWARD to 1.32:1 -- the ladder's two headline numbers.
        return color(TokenId::SurfacePanel);
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

  // C124: THE FOLDER IS GOLD -- a MATERIAL, not a tint of the accent.
  //
  // C104 shipped it as "the accent mixed toward the panel": soft, yes, but also
  // the FOURTH blue in a product that should have one (UI-AUDIT 2.2, 12.5:
  // #2480DF, the same 210-214 degrees as the selection bar and the tab rule, so
  // a user could not tell "folder" from "selected" by colour). Bandizip's folder
  // is warm gold and measures 1.61:1 on white: recognisable, quiet, and never
  // competing with the accent. The three tones are TOKENS now (FolderHighlight /
  // FolderBody / FolderShadow), identical in every theme, and the alphas are the
  // view's (kFolderIcon) so the row's wash still reads through the icon.
  void DrawFolderGlyph(ID2D1RenderTarget* target, const prism::win::sidebar::Box& box,
                       prism::theme::Variant variant) {
    if (!sidebarBrush_ || box.Empty()) return;
    const prism::win::sidebar::FolderIcon token = prism::win::sidebar::kFolderIcon;
    const auto tone = [variant](prism::theme::TokenId id, float alpha) {
      prism::theme::Rgba c = prism::theme::Lookup(id, variant).color;
      c.a = alpha;
      return c;
    };
    float bodyAlpha = token.bodyAlpha;
    float tabAlpha = token.tabAlpha;
    float shadowAlpha = token.shadowAlpha;
#if defined(PRISM_C104_MUTANT_FOLDER_OPAQUE)
    // NEGATIVE CONTROL (CONTRACTS D3): the shipped plate -- the raw accent at
    // full opacity, i.e. the hardcoded accent block. The translucency assertion
    // MUST go red here.
    ++dbgMutantFolder;
    bodyAlpha = 1.0f;
    tabAlpha = 1.0f;
    shadowAlpha = 1.0f;
#endif
    const float l = box.left, t = box.top, r = box.right, b = box.bottom;
    const float w = r - l, h = b - t;
    // Back tab: the HIGHLIGHT tone, because the tab is the surface the light
    // lands on first.
    sidebarBrush_->SetColor(ToD2d(tone(prism::theme::TokenId::FolderHighlight, tabAlpha)));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(l + w * 0.10f, t + h * 0.20f, l + w * 0.54f,
                                      t + h * 0.40f),
                          token.tabRadius, token.tabRadius),
        sidebarBrush_.Get());
    // Front face: the BODY.
    sidebarBrush_->SetColor(ToD2d(tone(prism::theme::TokenId::FolderBody, bodyAlpha)));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(l + w * 0.10f, t + h * 0.30f, r - w * 0.10f,
                                      b - h * 0.16f),
                          token.bodyRadius, token.bodyRadius),
        sidebarBrush_.Get());
    // The SHADOW row along the front face's bottom edge: one 1 DIP line, which
    // is what makes a flat two-tone glyph read as a solid object at 16 px (the
    // same trick C111's specular row uses on the grid glyph, inverted).
    sidebarBrush_->SetColor(
        ToD2d(tone(prism::theme::TokenId::FolderShadow, shadowAlpha)));
    target->FillRectangle(
        D2D1::RectF(l + w * 0.16f, b - h * 0.20f, r - w * 0.16f, b - h * 0.16f),
        sidebarBrush_.Get());
  }

  // Icon ids, not bitmaps (C12 contract): tiny vector geometry keeps the
  // snapshot crisp and needs no asset pipeline at 16 px.
  void DrawSidebarGlyph(ID2D1RenderTarget* target, const prism::win::sidebar::Box& box,
                        std::string_view icon, const D2D1_COLOR_F& color,
                        prism::theme::Variant variant) {
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
      DrawFolderGlyph(target, box, variant);
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
            SUCCEEDED(prism::win::typography::CreateTextLayout(
                writeFactory_.Get(), wide.c_str(), static_cast<UINT32>(wide.size()),
                sidebarSmall_.Get(), box.right - box.left, box.bottom - box.top,
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
      case PrimKind::Selection: {
        // C104: a CAPSULE, not a plate. The radius travels with the command
        // (the view owns geometry -- blueprint 3.2's squircle, now at the
        // sidebar's own 6 px token instead of a renderer-side 8).
#if defined(PRISM_C104_MUTANT_SQUARE)
        ++dbgMutantSquare;  // the injection ran and painted the shipped rectangle
        target->FillRectangle(box, sidebarBrush_.Get());
#else
        const float radius =
            command.cornerRadius > 0.0f ? command.cornerRadius : 6.0f;
        FillSquircleRoundedRect(target, box, radius, sidebarBrush_.Get());
#endif
        return;
      }
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
                         ToD2d(TokenColor(command.color, variant)), variant);
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
  //
  // C110: `backdropPass` is DrawListMode's argument, which the grid never had.
  // C77 builds the refraction layer by re-rendering the CONTENT into a 1/8
  // target and blurring it; in grid mode that pass used to re-draw every tile --
  // its fill, its accent folder mark, its name glyphs. A 120x220 tile is 15x27
  // mini pixels there, so the tile became a solid washed RECTANGLE, the 28 px
  // blur spread it into its neighbours, and the veil left 35 % of it: the user's
  // "large grey smear" (measured 30+ levels above the canvas where a tile sits,
  // with nothing on the canvas at all).
  //
  // THE RULE: the ghost carries PIXELS THAT CAME FROM THE ARCHIVE (a decoded
  // thumbnail -- "the content is the wallpaper"), never the chrome we drew
  // around them (the fill, the folder mark, the label, the skeleton, the
  // selection ring). That is the same split C90 made in list mode: glyphs and
  // the selection pill left the source, the folder icon and the ratio capsule
  // stayed. Folders and 32 px rows therefore contribute NOTHING to the ghost,
  // and a grid of folders has no smear at all -- which is assertion A1.
  void DrawGrid(ID2D1RenderTarget* target, const Scene& scene,
                const prism::layout::Layout& layout, bool backdropPass = false) {
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
#if defined(PRISM_C110_MUTANT_TILES_IN_BACKDROP)
    // C110 MUTANT ACTIVE: tiles-in-backdrop -- the shipped behaviour: the grid's
    // refraction pass re-draws the tile chrome. A1 MUST go red. The counter is
    // the runtime half of the verdict (CONTRACTS D3), so a red for an unrelated
    // reason can never print CAUGHT.
    const bool drawChrome = true;
    if (backdropPass) ++dbgBackdropTilePasses;
#else
    const bool drawChrome = !backdropPass;
#endif
    for (const prism::layout::FlowCell& cell : cells) {
      if (cell.index < 0 || cell.index >= count) continue;
      const prism::ArchiveItem& item = (*scene.items)[static_cast<size_t>(cell.index)];
      if (cell.kind == prism::layout::CellKind::Row) {
        if (drawChrome) DrawFallbackRow(target, scene, cell, item);
      } else if (item.isDir) {
        if (drawChrome) DrawFolderTile(target, scene, cell.rect, item);
      } else {
        // The thumbnail itself stays (it IS archive content); DrawThumbTile
        // drops everything it painted around the pixels.
        DrawThumbTile(target, scene, cell.rect, item,
                      !scene.debugForceSkeleton &&
                          flow[static_cast<size_t>(cell.index)].thumbnailAvailable,
                      drawChrome);
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
      // C109: the grid's fallback band shows the same numeric cell as the list,
      // on the same tabular face and aligned to the same box edge.
      DrawNumericTrailing(target, Utf8(prism::list::FormatBytes(item.size)),
                          band.right - 100.0f, band.right - 8.0f, band.top + 7.0f,
                          textSecondary_.Get());
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

    // C124: THE THREE GOLD TONES, straight from the tokens -- the same three the
    // 16 DIP rail glyph uses, so "the folder is gold" is one truth in two sizes.
    // The glyph is OPAQUE: it is a 68 x 56 object standing on a card, not a tint,
    // and (255,182,102) on white is 1.61:1 precisely BECAUSE it is the material
    // colour (DESIGN-PROPOSAL 12.5).
    const auto glyphTone = [theme](prism::theme::TokenId id) {
      return ToD2d(prism::theme::Lookup(id, theme).color);
    };
    const D2D1_COLOR_F highlight = glyphTone(prism::theme::TokenId::FolderHighlight);
    const D2D1_COLOR_F body = glyphTone(prism::theme::TokenId::FolderBody);
    const D2D1_COLOR_F shadow = glyphTone(prism::theme::TokenId::FolderShadow);
    const D2D1_COLOR_F frontFill = body;
    // The back tab is the SHADOW tone: it is behind the face, so it reads darker
    // than the body it hides behind (C111's B3 measures exactly that order).
    const D2D1_COLOR_F tabFill = shadow;
    const D2D1_COLOR_F frontRim = highlight;
    const D2D1_COLOR_F frontSheen = highlight;

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
    // The specular on the tab's top edge, inset by the corner radius. C124 made
    // it 2 DIP (it was 1): with the gold material the tab behind the face is now
    // the SHADOW tone, so a single highlight row could no longer put the icon's
    // top 2 px 8 levels above its centre -- and that pair of rows is exactly
    // what "the top edge catches the light" means (c111 B2).
    island_->SetColor(highlight);
    target->FillRectangle(D2D1::RectF(tab.left + radius * 0.8f, box.top,
                                      tab.right - radius * 0.8f,
                                      box.top + prism::theme::kSeamThicknessDip),
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

  // C110: `drawChrome` false = the refraction pass. The DECODED PIXELS are
  // archive content and stay; the fill behind them, the name plate and the
  // selection ring are chrome and do not reach the ghost.
  void DrawThumbTile(ID2D1RenderTarget* target, const Scene& scene,
                     const prism::layout::Rect& cell, const prism::ArchiveItem& item,
                     bool hasThumb, bool drawChrome = true) {
    if (drawChrome) ++dbgTileCells;
    constexpr float kRadius = 16.0f;
    const D2D1_RECT_F rect = CellRectF(cell);
    const D2D1_RECT_F photoRect =
        D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom - 30.0f);
    const prism::gallery::DecodedThumb* thumb =
        (hasThumb && scene.thumbs) ? scene.thumbs->Lookup(item.index) : nullptr;
    if (thumb) ++dbgThumbLookupHits;
    if (!thumb) {
      if (!drawChrome) return;  // no skeleton, no plate in the ghost
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
      if (drawChrome) target->FillRectangle(photoRect, tile_.Get());
      ++dbgThumbDraws;
      target->DrawBitmap(it->second.Get(), fit, 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (clipGeometry) target->PopLayer();
    if (!drawChrome) return;
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
      // C124: the ramp's blue IS the product's one blue (#0A84FF). The ratio bar
      // is a DATA ramp (hue = file kind), not a second accent, but a stray
      // #007AFF here was still a fourth blue in the source and the scan cannot
      // tell the two roles apart -- so it stops existing.
      case prism::list::BarColor::Blue: return {0.039216f, 0.517647f, 1.0f, 1.0f};  // #0A84FF
      case prism::list::BarColor::Green: return {0.204f, 0.780f, 0.349f, 1.0f}; // #34C759
      case prism::list::BarColor::Amber: return {1.0f, 0.584f, 0.0f, 1.0f};    // #FF9500
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
  }

  // C124: the 16 DIP folder of a LIST ROW -- highlight tab, body, shadow row.
  // The rail's glyph (SidebarView) and the grid's (68 px) use the same three
  // tokens, so "the folder is gold" is one truth at three sizes.
  void DrawListFolderGlyph(ID2D1RenderTarget* target, const D2D1_RECT_F& box,
                           prism::theme::Variant variant) {
    const auto tone = [variant](prism::theme::TokenId id) {
      prism::theme::Rgba c = prism::theme::Lookup(id, variant).color;
      c.a = 1.0f;
      return ToD2d(c);
    };
    const float l = box.left;
    const float t = box.top;
    const float r = box.right;
    const float b = box.bottom;
    const float w = r - l;
    const float h = b - t;
    island_->SetColor(tone(prism::theme::TokenId::FolderHighlight));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(l + w * 0.06f, t + h * 0.08f, l + w * 0.52f,
                                      t + h * 0.40f),
                          1.5f, 1.5f),
        island_.Get());
    island_->SetColor(tone(prism::theme::TokenId::FolderBody));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(l + w * 0.06f, t + h * 0.26f, r - w * 0.06f,
                                      b - h * 0.08f),
                          2.0f, 2.0f),
        island_.Get());
    island_->SetColor(tone(prism::theme::TokenId::FolderShadow));
    target->FillRectangle(D2D1::RectF(l + w * 0.12f, b - h * 0.15f, r - w * 0.12f,
                                      b - h * 0.08f),
                          island_.Get());
    island_->SetColor(islandColor_);
  }

  // C124: ONE painter for every file-type glyph. The shapes live in PrismCore
  // (prism::icons) as DIP inside a 16 x 16 box; this maps that box onto the slot
  // the row offers and strokes them in the SECONDARY INK role. The four kinds are
  // told apart by SILHOUETTE, not by hue: four more colours would rebuild the
  // "one colour, three meanings" defect the seam exists to remove (the accent is
  // the one colour that carries meaning, and it means "you are here").
  void DrawFileTypeGlyph(ID2D1RenderTarget* target, const D2D1_RECT_F& box,
                         prism::icons::FileKind kind) {
    if (!sidebarBrush_ || !textSecondary_ || box.right <= box.left) return;
    const prism::icons::Geometry geometry = prism::icons::GeometryOf(kind);
    if (geometry.count == 0) return;
    const float scale = (box.right - box.left) / prism::icons::kIconSizeDip;
    const auto px = [&](float v) { return box.left + v * scale; };
    const auto py = [&](float v) { return box.top + v * scale; };
    const D2D1_COLOR_F restore = sidebarBrush_->GetColor();
    sidebarBrush_->SetColor(textSecondary_->GetColor());
    for (std::size_t i = 0; i < geometry.count; ++i) {
      const prism::icons::Shape& shape = geometry.shapes[i];
      const float stroke = shape.stroke * scale;
      const D2D1_RECT_F rect =
          D2D1::RectF(px(shape.x0), py(shape.y0), px(shape.x1), py(shape.y1));
      switch (shape.type) {
        case prism::icons::ShapeType::StrokeRect:
          target->DrawRectangle(rect, sidebarBrush_.Get(), stroke);
          break;
        case prism::icons::ShapeType::FillRect:
          target->FillRectangle(rect, sidebarBrush_.Get());
          break;
        case prism::icons::ShapeType::Line:
          target->DrawLine(D2D1::Point2F(rect.left, rect.top),
                           D2D1::Point2F(rect.right, rect.bottom), sidebarBrush_.Get(),
                           stroke);
          break;
        case prism::icons::ShapeType::Ellipse:
          target->DrawEllipse(
              D2D1::Ellipse(D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                          (rect.top + rect.bottom) * 0.5f),
                            (rect.right - rect.left) * 0.5f,
                            (rect.bottom - rect.top) * 0.5f),
              sidebarBrush_.Get(), stroke);
          break;
        case prism::icons::ShapeType::FillEllipse:
          target->FillEllipse(
              D2D1::Ellipse(D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                          (rect.top + rect.bottom) * 0.5f),
                            (rect.right - rect.left) * 0.5f,
                            (rect.bottom - rect.top) * 0.5f),
              sidebarBrush_.Get());
          break;
      }
    }
    sidebarBrush_->SetColor(restore);
  }

  // The main-window file list -- C122 puts Bandizip's four columns in:
  //   名称 (flex, left) | 压缩后大小 (120, right) | 原始大小 (120, right) |
  //   类型 (100, left)
  // at 26 DIP rows (22 under 700 DIP, see ListRowHeightFor), virtualized from
  // WindowLayout. Every rectangle -- the header cells and the row cells alike --
  // comes from ListColumnRects/ListRowRect, so the painted cells and the
  // header's own hit test cannot disagree. The compression-ratio capsule stays
  // as a readout under the packed-size number; the 压缩率 COLUMN is retired.
  void DrawListMode(ID2D1RenderTarget* target, const Scene& scene,
                    const prism::layout::Layout& layout,
                    bool backdropPass = false) {
    if (!scene.listItems) return;
    // C101: this frame's bars replace the previous frame's.
    ratioBarCount_ = 0;
    const auto& rows = *scene.listItems;
    // C122: the four columns come from WindowLayout -- the ONE geometry
    // authority -- never from a constant copied into this file. The list no
    // longer reads the persisted C27 ColumnLayout: the design fixes the four
    // widths (120 / 120 / 100), so a stale user width cannot push the type cell
    // off the content edge or put the numbers under the wrong heading.
    const prism::layout::Rect header = prism::layout::ListHeaderRect(layout);
    const prism::layout::Rect body = prism::layout::ListBodyRect(layout);
    const prism::layout::ListColumnLayout cols = prism::layout::ListColumnRects(layout);
    const int scroll = static_cast<int>(scene.listScroll);
    // Header ink: 11 px, secondary role (design proposal 4.2 / 5.3), starting
    // 8 px below the band's top so the two glyph-free strips C81/A2 audits stay
    // bare canvas, and ending above the 1 px rule.
    const float textTop = static_cast<float>(header.top) + 8.0f;
    const float textBottom = textTop + 16.0f;
    // Row ink: 13 px, 5 px below the row's top -- visually centred in 26 DIP.
    constexpr float kRowTextInset = 5.0f;

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
      // C122: four labels, in Bandizip's order, from the ONE column model. The
      // numeric headings are trailing-aligned on the SAME baselines the numbers
      // end on, and the active column carries the sort arrow (a SHAPE-free
      // glyph, drawn in the secondary role -- C81/A2's "no accent ink in the
      // header" assertion stays true).
      const auto headerText = [&](prism::list::Column column) {
        std::wstring text = Utf8(prism::list::ColumnTitle(column));
        if (scene.listSort.column == column) {
          // U+2191/U+2193 rather than the solid triangles: the arrow block is
          // present in every UI face Windows ships, so the indicator cannot
          // render as tofu on a font without geometric shapes.
          text += scene.listSort.ascending ? L" \x2191" : L" \x2193";
        }
        return text;
      };
      DrawLabel(target, columnHeader_.Get(), headerText(prism::list::Column::Name),
                D2D1::RectF(static_cast<float>(cols.name.left), textTop,
                            static_cast<float>(cols.name.right), textBottom),
                textSecondary_.Get());
      DrawTrailingLabel(target, columnHeader_.Get(),
                        headerText(prism::list::Column::PackedSize),
                        static_cast<float>(cols.packed.left),
                        static_cast<float>(cols.packed.right), textTop, textSecondary_.Get());
      DrawTrailingLabel(target, columnHeader_.Get(),
                        headerText(prism::list::Column::OriginalSize),
                        static_cast<float>(cols.original.left),
                        static_cast<float>(cols.original.right), textTop, textSecondary_.Get());
      DrawLabel(target, columnHeader_.Get(), headerText(prism::list::Column::Type),
                D2D1::RectF(static_cast<float>(cols.type.left), textTop,
                            static_cast<float>(cols.type.right), textBottom),
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

    const prism::layout::RowRange window =
        prism::layout::VisibleListRows(layout, static_cast<int>(rows.size()), scroll);
    const float nameLeft = static_cast<float>(body.left) + prism::layout::kListIndent;
    const float nameRight = static_cast<float>(cols.name.right);
    const float packedLeft = static_cast<float>(cols.packed.left);
    const float packedRight = static_cast<float>(cols.packed.right);
    const float originalLeft = static_cast<float>(cols.original.left);
    const float originalRight = static_cast<float>(cols.original.right);
    const float typeLeft = static_cast<float>(cols.type.left);
    const float typeRight = static_cast<float>(cols.type.right);
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
        // C124 THE SEAM, position 3 of 4: the selected row's LEFT edge, 2 DIP,
        // full-strength accent. It leads the pill (which stays inset 8 px), the
        // same relationship the rail's rule has to its capsule -- one element,
        // two views, not two widths (C79 shipped 3 px in the tree and nothing
        // here).
        D2D1_COLOR_F seamColor = accentColor_;
        seamColor.a = 1.0f;
        const D2D1_RECT_F seam =
            D2D1::RectF(rowRect.left, top + 2.0f,
                        rowRect.left + prism::theme::kSeamThicknessDip, bottom - 2.0f);
        accent_->SetColor(seamColor);
        target->FillRectangle(seam, accent_.Get());
        accent_->SetColor(accentColor_);
        if (!backdropPass) RecordSeam(SeamRegion::SelectedRow, seam);
      }

      const std::wstring name = Utf8(row.name);
      const float textRowTop = top + kRowTextInset;
      const float textRowBottom = bottom - 2.0f;
      // C122: the 16 px folder mark is centred in the row, whatever the tier.
      const float iconTop = top + (static_cast<float>(rr.Height()) - 16.0f) / 2.0f;
      if (row.isDir) {
        // C124: THE FOLDER IS GOLD in the list too. It was one accent-coloured
        // rounded rectangle -- the fourth blue, and the only folder in the
        // product that was a tint rather than the material (the rail and the
        // grid had their own glyphs). Same three tones as both of those, at the
        // list's ONE icon size (16 DIP, centred in the row by C122).
        DrawListFolderGlyph(target,
                            D2D1::RectF(nameLeft + 4.0f, iconTop, nameLeft + 20.0f,
                                        iconTop + 16.0f),
                            scene.theme);
      } else {
        // C124: THE FILE-TYPE GLYPH, in the same slot the folder uses, so a list
        // row has ONE icon column. Before this a file row had no glyph at all
        // and the grid drew the same grey bar for .md and .bin (UI-AUDIT 12.6):
        // the icon carried no information. C122's 类型 column keeps its TEXT --
        // the glyph and the label are different jobs.
        DrawFileTypeGlyph(target,
                          D2D1::RectF(nameLeft + 4.0f, iconTop, nameLeft + 20.0f,
                                      iconTop + 16.0f),
                          prism::icons::KindFromPath(row.name));
      }
      if (drawGlyphs) {
        // 名称 -- flexes, left-aligned; the icon column (folder or file kind)
        // keeps 8 px of air before the name.
        const float nameTextLeft = nameLeft + 28.0f;
        DrawLabel(target, caption_.Get(), name,
                  D2D1::RectF(nameTextLeft, textRowTop, nameRight, textRowBottom),
                  textPrimary_.Get());
        // 压缩后大小 / 原始大小 -- the two numeric cells, trailing on their own
        // baselines. A folder has no byte stream, so it keeps the honest "--".
        if (row.isDir) {
          DrawNumericTrailing(target, L"--", packedLeft, packedRight, textRowTop,
                              textSecondary_.Get());
          DrawNumericTrailing(target, L"--", originalLeft, originalRight, textRowTop,
                              textSecondary_.Get());
        } else {
          DrawNumericTrailing(target, Utf8(prism::list::FormatBytes(row.packedSize)),
                              packedLeft, packedRight, textRowTop, textPrimary_.Get());
          DrawNumericTrailing(target, Utf8(prism::list::FormatBytes(row.size)),
                              originalLeft, originalRight, textRowTop, textPrimary_.Get());
        }
        // 类型 -- the five-class Chinese name; an unknown extension is blank
        // (ListColumns.hpp owns the table, so paint and L4 cannot disagree).
        const std::wstring type = Utf8(prism::list::TypeLabelOf(row.name, row.isDir));
        if (!type.empty()) {
          DrawLabel(target, caption_.Get(), type,
                    D2D1::RectF(typeLeft, textRowTop, typeRight, textRowBottom),
                    textPrimary_.Get());
        }
      }

      // The compression-ratio capsule (C101) survives, but the 压缩率 COLUMN
      // does not: Bandizip's fourth column is 类型 (design proposal's red line
      // #5), so the bar is a READOUT UNDER the packed-size number -- 4 px tall,
      // radius 2, at most 56 px, alpha 0.55, hue from the measured ratio. It
      // stays inside the packed column, so the retired fifth column cannot come
      // back by accident.
      if (prism::list::RatioBarVisible(row)) {
        const auto bar = prism::list::RatioBar(row);
        float barWidth = (std::min)(prism::list::kRatioBarMaxWidth,
                                    static_cast<float>(cols.packed.Width()) - 64.0f);
        float halfH = prism::list::kRatioBarHeight * 0.5f;
        float radius = prism::list::kRatioBarRadius;
        const prism::list::Rgb tint = prism::list::RatioBarTint(bar->percent);
        D2D1_COLOR_F fillColor =
            D2D1::ColorF(tint.r, tint.g, tint.b, prism::list::kRatioBarAlpha);
#if defined(PRISM_C101_MUTANT_LOUD)
        // C101 MUTANT ACTIVE: the shipped bar is restored -- 6 px, radius 3,
        // alpha 1.0, the full column, the file-kind colour. The suite's
        // geometry, alpha and pixel assertions MUST go red on this arm.
        barWidth = static_cast<float>(cols.packed.Width());
        halfH = 3.0f;
        radius = 3.0f;
        fillColor = BarColorOf(bar->color);
        ++dbgMutantRatioBars;
#endif
        const float cy = bottom - 3.0f;
        const float trackX = packedRight - barWidth;
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
        // what the caller intended. The backdrop pass paints the same bars into
        // a 1/8 target; only the presented frame records.
        if (!backdropPass) {
          RecordRatioBar(track.rect, fill.rect, radius, tileBorder_->GetColor(), bar->percent);
        }
        tileBorder_->SetColor(kTileBorder);
      }
    }
  }

  // C109: trailing alignment against a BOX [left, right]; the box's right edge
  // is the column baseline. The format is passed in because the header keeps the
  // proportional system face (CJK labels) while the numbers use numbers_.
  void DrawTrailingLabel(ID2D1RenderTarget* target, IDWriteTextFormat* format,
                         const std::wstring& text, float left, float right, float y,
                         ID2D1Brush* brush) {
    if (!format || text.empty() || right <= left) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawLabel(target, format, text, D2D1::RectF(left, y, right, y + 24.0f), brush);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }

  // C109: a numeric cell. The layout is the SAME object CreateNumericFormat and
  // ApplyTabularFigures describe -- the suite builds its advance probe with
  // those very functions, so "the renderer is tabular" is measured, not hoped.
  void DrawNumericTrailing(ID2D1RenderTarget* target, const std::wstring& text, float left,
                           float right, float y, ID2D1Brush* brush) {
    if (!numbers_ || text.empty() || right <= left) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(prism::win::listtype::CreateNumericLayout(
            writeFactory_.Get(), numbers_.Get(), text.c_str(),
            static_cast<UINT32>(text.size()), right - left, 24.0f,
            layout.ReleaseAndGetAddressOf()))) {
      return;
    }
    target->DrawTextLayout(D2D1::Point2F(left, y), layout.Get(), brush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
  // C112: the pill and its text run, from ONE place. The window paints the pill
  // (pre-C112) and the island paints it (C112) through the same numbers, so the
  // text cannot move between the two arms -- which is exactly what the suite's
  // "no second copy 1-3 px away" measurement would report as a ghost.
  struct PillGeometry {
    prism::layout::Rect pill;
    prism::layout::Rect text;
  };

  static PillGeometry PillGeometryFor(const prism::layout::Layout& layout,
                                      const prism::actionbar::Model* actions) {
    // C-V2: while an action band is present the pill is the SPLIT one -- the
    // toolbar may have taken width back, and the window hit-tests the same
    // rectangle (GalleryWindow::WindowLayoutForClient -> Compute -> navPill).
    // Without a band this is exactly V1's rect, so every pre-V2 scene (and the
    // snapshot) keeps its framing to the pixel.
    PillGeometry geometry;
    if (actions != nullptr) {
      const prism::actionbar::Layout band = prism::actionbar::Compute(layout);
      geometry.pill = band.navPill;
      geometry.text = prism::actionbar::NavTextRect(band);
    } else {
      geometry.pill = prism::layout::NavPillRect(layout);
      geometry.text = prism::layout::NavTextRect(layout);
    }
    return geometry;
  }

  // C123: THE PATH BAR -- the crumb's new body. Not an island: a single 28 px
  // row under the toolbar, left-aligned at contentBand.left + 16, whose current
  // segment carries the 2 px "seam" rule on the bar's own bottom edge.
  //
  // It paints NO fill: the row is the page's canvas (DrawBands cleared it), so
  // the path reads as part of the content chrome and never as a second button
  // group -- which is what the 700 x 67 pill was. The hover capsule and the
  // rule are the only ink the bar adds, plus the text.
  //
  // The text layout is built LEADING (DrawBreadcrumbRun's last argument). That
  // is not a detail: title_ is a CENTER-aligned format, so without it the run
  // would stay centred inside a box that is now left-aligned -- the 234 px the
  // audit measured would survive the geometry fix and nothing would have moved.
  void DrawBreadcrumbPathBar(ID2D1RenderTarget* target, const Scene& scene,
                             const prism::layout::Layout& layout) {
    const prism::layout::Rect bar = layout.crumbBar;
    const prism::layout::Rect text = prism::layout::CrumbTextRect(layout);
    if (bar.Empty() || text.Empty()) return;
    if (scene.crumbHoverRect.Width() > 0 && scene.crumbHoverRect.Height() > 0) {
      const D2D1_ROUNDED_RECT hover =
          D2D1::RoundedRect(ToRectF(scene.crumbHoverRect), 6.0f, 6.0f);
      islandBorder_->SetColor(kCrumbHover);
      target->FillRoundedRectangle(hover, islandBorder_.Get());
      islandBorder_->SetColor(kIslandBorder);
    }
    if (!scene.crumbCurrentRect.Empty()) {
      // The rule is the ACCENT, fully opaque: it is the same "seam" the selected
      // row and the active tab use, and a translucent line over canvas would
      // measure as a different colour in the three themes.
      prism::theme::Rgba seam =
          prism::theme::Lookup(prism::theme::TokenId::AccentLiquidPrimary, ThemeOf(scene))
              .gradient.from;
      seam.a = 1.0f;
      accent_->SetColor(ToD2d(seam));
      target->FillRectangle(ToRectF(scene.crumbCurrentRect), accent_.Get());
      accent_->SetColor(accentColor_);
      // C124: read back the seam this frame painted (position 2 of 4), so the
      // suite can assert "exactly one, exactly 2 DIP" instead of trusting the
      // window's rectangle. C123 supplies the geometry (it owns the spans).
      RecordSeam(SeamRegion::CrumbCurrent, ToRectF(scene.crumbCurrentRect));
    }
    DrawBreadcrumbRun(target, scene.breadcrumb, text, /*leading=*/true);
  }

  void DrawBreadcrumbIsland(ID2D1RenderTarget* target, const Scene& scene,
                            const prism::layout::Layout& layout) {
    if (layout.chrome == prism::layout::Chrome::C123) {
      DrawBreadcrumbPathBar(target, scene, layout);
      return;
    }
    const PillGeometry geometry = PillGeometryFor(layout, scene.actions);
    const prism::layout::Rect pillRect = geometry.pill;
    if (pillRect.Empty()) return;
    const prism::layout::Rect textRect = geometry.text;
    const D2D1_RECT_F pill = ToRectF(pillRect);
    const float height = static_cast<float>(pillRect.Height());
    if (scene.pillOwnedByIsland) {
      // C112: the ISLAND owns the pill's content. The window keeps only the drop
      // shadow -- it falls OUTSIDE the pill rectangle, on the canvas, so it is
      // not part of what the island's own surface can show -- and the hit-test
      // geometry (unchanged: WindowLayoutForClient -> actionbar::Compute).
      if (scene.glassPanels) {
        glassPanel_.PaintShadow(target, pill, scene.theme, &glassStats_);
      }
      return;
    }
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
    // C115: a crumb run wider than the box is TRIMMED (U+2026), not spilled.
    // The run is centre-aligned, so an over-wide string used to overflow BOTH
    // edges of the text box: the trailing half was clipped by D2D, the leading
    // half was not -- the first crumb could start 12 px left of the box and, on
    // a deep path, well past the pill. Now the layout itself is the box.
    DrawBreadcrumbRun(target, scene.breadcrumb, textRect);
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
        D2D1_COLOR_F fillColor = accentColor_;
        fillColor.a = kProgressFillAlpha;
        accent_->SetColor(fillColor);
        target->FillRoundedRectangle(fill, accent_.Get());
        accent_->SetColor(accentColor_);
        // C124 THE SEAM, position 4 of 4: the fill's LEADING edge. The fill is
        // the accent at the token's alpha (a wash you read as "how far"); the
        // 2 DIP line is the accent at FULL strength, i.e. the exact pixel the
        // progress has reached. One line, and its position is the number.
        // Snapped to a whole DIP: a 2 DIP line between two fractional edges is
        // anti-aliased into three columns of 60 % accent, which is neither the
        // line the design asks for nor a pixel a suite can measure.
        const float frontX = std::round(track.rect.left + fillW);
        const float half = prism::theme::kSeamThicknessDip * 0.5f;
        const D2D1_RECT_F seam =
            D2D1::RectF(frontX - half, track.rect.top, frontX + half, track.rect.bottom);
        D2D1_COLOR_F seamColor = accentColor_;
        seamColor.a = 1.0f;
        accent_->SetColor(seamColor);
        target->FillRectangle(seam, accent_.Get());
        accent_->SetColor(accentColor_);
        RecordSeam(SeamRegion::ProgressFront, seam);
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

  // C115: the breadcrumb's text run, trimmed to its box. Kept next to DrawLabel
  // because it is the same paint, with one extra promise: the LAST visible glyph
  // is an ellipsis when the string does not fit, and no glyph may leave the box.
  // Falling back to DrawLabel on a layout failure keeps the pre-C115 behaviour
  // rather than dropping the breadcrumb entirely.
  // 'leading' is the C123 path bar's alignment. title_ is a CENTER-aligned
  // format and CreateTextLayout inherits it, so a left-aligned BOX is not
  // enough: the run itself has to be told to start at the box's left edge.
  // The pill keeps the centred run (false), byte-for-byte.
  void DrawBreadcrumbRun(ID2D1RenderTarget* target, const std::wstring& text,
                         const prism::layout::Rect& box, bool leading = false) {
    if (!title_ || text.empty() || box.Width() <= 0 || box.Height() <= 0) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(prism::win::typography::CreateTextLayout(
            writeFactory_.Get(), text.c_str(), static_cast<UINT32>(text.size()),
            title_.Get(), static_cast<FLOAT>(box.Width()), static_cast<FLOAT>(box.Height()),
            layout.ReleaseAndGetAddressOf()))) {
      DrawLabel(target, title_.Get(), text, ToRectF(box), textPrimary_.Get());
      return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (leading) layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(
            title_.Get(), ellipsis.ReleaseAndGetAddressOf())) &&
        ellipsis != nullptr) {
      const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      layout->SetTrimming(&trimming, ellipsis.Get());
    }
    target->DrawTextLayout(D2D1::Point2F(static_cast<float>(box.left),
                                        static_cast<float>(box.top)),
                           layout.Get(), textPrimary_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
    if (numbers_) ++n;  // C109
    if (columnHeader_) ++n;  // C122
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
    numbers_.Reset();  // C109
    columnHeader_.Reset();  // C122
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
  D2D1_COLOR_F accentColor_{0.039216f, 0.517647f, 1.0f, 1.0f};  // C124: #0A84FF
  D2D1_COLOR_F islandColor_{0.10f, 0.12f, 0.16f, 0.72f};
  // C91: the canvas this frame cleared to -- the SAME colour the action band and
  // the list header refill themselves with (see DrawBands / DrawListMode).
  D2D1_COLOR_F canvasColor_{0.043f, 0.055f, 0.075f, 1.0f};

  ComPtr<ID2D1SolidColorBrush> accent_, tile_, tileBorder_, island_, card_, islandBorder_;
  ComPtr<ID2D1SolidColorBrush> textPrimary_, textSecondary_;
  // C29 sidebar: one recolourable brush + the two single-line text formats.
  ComPtr<ID2D1SolidColorBrush> sidebarBrush_;
  ComPtr<IDWriteTextFormat> numbers_;  // C109: size/packed columns
  ComPtr<IDWriteTextFormat> columnHeader_;  // C122: the 11 px list header
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
  // C112: how many times the ISLAND painted the pill's content / its text. The
  // suite asserts the count, so "the text moved to the island" is a number, not
  // a claim about intent.
  int dbgPillOnIsland = 0;
  int dbgPillTextOnIsland = 0;
  // C91: proof that a mutation arm's injected branch really ran. Each mutant
  // binary arms its CAUGHT verdict on one of these, so a red that came from an
  // unrelated assertion can never print a false verdict (CONTRACTS D3).
  static inline int dbgMutantTheme = 0;
  static inline int dbgMutantBrushes = 0;
  static inline int dbgMutantIsland = 0;
  // C104: proof that each negative control's paint branch really executed.
  static inline int dbgMutantSquare = 0;
  static inline int dbgMutantFolder = 0;
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
  // C110: grid-mode backdrop passes that re-drew the tile chrome -- the runtime
  // half of the tiles-in-backdrop verdict, zero in every clean build.
  int dbgBackdropTilePasses = 0;
  // C121: backdrop passes that re-drew the DECODED pixels into the ghost (the
  // tiles-in-ghost arm only; zero in every clean build).
  int dbgBackdropEntryPasses = 0;
  // C121: proof that each of the new negative controls really executed.
  static inline int dbgMutantEmptyGhost = 0;
  static inline int dbgMutantKeyNoTheme = 0;
  static inline int dbgMutantPanelGlow = 0;
  // C110: tile fills produced by the pre-C110 flat literal (flat-tile mutant).
  static inline int dbgMutantFlatTile = 0;
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

  // ------------------------------------------------------------- C124 THE SEAM
  // "THE SEAM" (DESIGN-PROPOSAL 16.3): ONE 2 DIP accent line, four positions --
  // the active tab's bottom edge, the current crumb's bottom edge, the selected
  // row's left edge, the progress fill's leading edge. One element with four
  // meanings, so the user follows a line instead of learning a legend.
  //
  // A seam is the accent at FULL strength: every wash and fill in this frame is
  // the accent at <= 26 %, so the line is findable in the pixels by its exact
  // colour instead of by "somewhere around here". These three are the ones THIS
  // painter draws; the fourth belongs to the tab strip and is measured on the
  // strip's own frame (c124 V6 does both).
  enum class SeamRegion : uint8_t { CrumbCurrent = 0, SelectedRow, ProgressFront };
  static constexpr std::size_t kSeamRegionCount = 3;
  // The progress fill is a WASH, the seam is the LINE. With an opaque fill the
  // leading edge is the same colour as the bar behind it, so "the front" would
  // be invisible -- the defect the seam exists to fix. 0.55 is C101's readout
  // alpha (a bar you read, not a rule you obey), and C71 samples this constant
  // so its "the saved accent reaches the bar" assertion measures the composite
  // the painter really produces.
  static constexpr float kProgressFillAlpha = 0.55f;
  struct SeamPaint {
    SeamRegion region = SeamRegion::CrumbCurrent;
    D2D1_RECT_F rect{};
  };
  int SeamsPainted() const { return seamCount_; }
  int SeamCount(SeamRegion region) const {
    int n = 0;
    for (int i = 0; i < seamCount_; ++i) {
      if (seams_[i].region == region) ++n;
    }
    return n;
  }
  const SeamPaint* SeamPainted(SeamRegion region, int index) const {
    for (int i = 0; i < seamCount_; ++i) {
      if (seams_[i].region != region) continue;
      if (index-- == 0) return &seams_[i];
    }
    return nullptr;
  }

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
  // C110: grid backdrop passes that re-drew the TILE chrome (mutant arm only;
  // zero in the clean build) and tile fills produced by the flat-literal mutant.
  int BackdropTilePasses() const { return dbgBackdropTilePasses; }
  int FlatTileMutantHits() const { return dbgMutantFlatTile; }
  // C121: the number of times the mutant arm put the items back into the
  // refraction source. Zero in every clean build (the increment is compiled in
  // only under PRISM_C121_MUTANT_TILES_IN_GHOST), so a mutant that never
  // reached the injected branch cannot print CAUGHT (CONTRACTS D3).
  int BackdropEntryPasses() const { return dbgBackdropEntryPasses; }
  // C121: how many times the empty-ghost / key-no-theme arms actually ran.
  int EmptyGhostMutantHits() const { return dbgMutantEmptyGhost; }
  int KeyNoThemeMutantHits() const { return dbgMutantKeyNoTheme; }
  int PanelGlowMutantHits() const { return dbgMutantPanelGlow; }
  // C121 TEST SEAM (not a feature): paint the refraction SOURCE alone -- the
  // 1/8 bitmap BuildBackdrop produced, WITHOUT the veil and without any chrome
  // -- so a suite can ask what the layer is refracting. The target is CLEARED to
  // transparent first, so the read-back is the source itself and its ALPHA says
  // which pixels this page actually painted (a source that paints nothing leaves
  // alpha 0, and a suite can tell "an empty page" from "the page, seen through a
  // transparent hole" -- the difference the empty-ghost arm exists to catch).
  // Returns false when the layer has not been built for this scene (the
  // no-backdrop mutant, or a pre-build frame): a suite must never read a stale
  // bitmap and call it evidence.
  bool PaintRefractionSource(ID2D1RenderTarget* target) {
    if (target == nullptr || !backdropBitmap_) return false;
    const D2D1_SIZE_F size = target->GetSize();
    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    target->DrawBitmap(backdropBitmap_.Get(), D2D1::RectF(0.0f, 0.0f, size.width, size.height),
                       1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
  }
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
  // C124: the seam read-back. Four lines is the whole product; 16 leaves room
  // for a pathological scene without allocating.
  static constexpr int kMaxSeams = 16;
  void RecordSeam(SeamRegion region, const D2D1_RECT_F& rect) {
    if (seamCount_ >= kMaxSeams) return;
    seams_[seamCount_].region = region;
    seams_[seamCount_].rect = rect;
    ++seamCount_;
  }
  SeamPaint seams_[kMaxSeams];
  int seamCount_ = 0;
};

}  // namespace prism::win::gallery
