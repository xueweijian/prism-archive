// PrismArchive -- PrismWin
// C91: THE THEME REACHES THE CONTENT -- asserted on the renderer's own pixels.
//
// THE DEFECT (research B, 1,544,424 px whole-window diff): day -> night moved
// 122,969 px = 7.96 %, all of it in the title bar and the DWM border. y=64..810
// -- 92 % of the window -- was byte-identical in all three themes, because
// GalleryRenderer assembled its scene with sidebarTheme / actionTheme /
// tabTheme / glassTheme unset and cleared the target with the literal #0B0E13.
//
// WHAT THIS FILE DOES
//   1. builds ONE scene -- list mode, tab strip, rail, action band, breadcrumb,
//      status segments, an open catalog -- and renders it three times through
//      GalleryRenderer::Draw into a WIC target, changing ONLY Scene::theme;
//   2. samples the regions the card names (canvas, status strip, active tab
//      plate, rail panel, selected row, action island, breadcrumb island, list
//      header, list row ink, telemetry capsule) and compares each against
//      prism::chrome::ThemeSurfaceColor -- the ONE theme entry -- not against a
//      second copy of the arithmetic;
//   3. measures the headline: the CONTENT band must differ between day and
//      night over at least 80 % of its pixels (it was 0 %);
//   4. checks the window wiring: SetChromeVariant is the same field DrawFrame
//      hands the renderer as Scene::theme, so the caption and the canvas cannot
//      disagree.
//
// NEGATIVE CONTROLS (CONTRACTS D3 / D26 / D27), one binary each, all armed on a
// runtime counter so an unrelated red can never print a false verdict:
//   PRISM_C91_MUTANT_SCENE_MIDNIGHT    the whole frame resolves to Midnight
//   PRISM_C91_MUTANT_BRUSHES_ONCE      the text brushes keep their build colours
//   PRISM_C91_MUTANT_ISLAND_LITERAL    the islands keep the pre-C91 literal
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../mini_test.hpp"

#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

using Microsoft::WRL::ComPtr;
using prism::theme::Rgba;
using prism::theme::TokenId;
using prism::theme::Variant;
using prism::win::gallery::GalleryRenderer;

constexpr UINT32 kW = 1280;
constexpr UINT32 kH = 800;

IWICImagingFactory* gWic = nullptr;

#if defined(PRISM_C91_MUTANT_SCENE_MIDNIGHT)
constexpr const char* kMutation = "scene_midnight";
#elif defined(PRISM_C91_MUTANT_BRUSHES_ONCE)
constexpr const char* kMutation = "brushes_once";
#elif defined(PRISM_C91_MUTANT_ISLAND_LITERAL)
constexpr const char* kMutation = "island_literal";
#else
constexpr const char* kMutation = nullptr;
#endif

// ------------------------------------------------------------------ pixels

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
};

int Channel(UINT32 p, int shift) { return static_cast<int>((p >> shift) & 0xFFu); }

struct Pixel {
  int r = 0;
  int g = 0;
  int b = 0;
};

Pixel ToPixel(UINT32 p) { return Pixel{Channel(p, 16), Channel(p, 8), Channel(p, 0)}; }
Pixel ToPixel(const Rgba& c) {
  const auto q = [](float v) {
    const double scaled = static_cast<double>(v) * 255.0 + 0.5;
    return static_cast<int>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
  };
  return Pixel{q(c.r), q(c.g), q(c.b)};
}

int Distance(const Pixel& a, const Pixel& b) {
  return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
}

// A named expectation: the measured triple is always printed, so a red run says
// WHICH surface drifted and by how much instead of "false".
bool Near(const char* label, const Pixel& got, const Pixel& want, int tolerance) {
  const int d = Distance(got, want);
  const bool ok = d <= tolerance * 3 && std::abs(got.r - want.r) <= tolerance &&
                  std::abs(got.g - want.g) <= tolerance &&
                  std::abs(got.b - want.b) <= tolerance;
  std::printf("  [c91] %-30s got=(%3d,%3d,%3d) want=(%3d,%3d,%3d) +-%d %s\n", label, got.r,
              got.g, got.b, want.r, want.g, want.b, tolerance, ok ? "ok" : "RED");
  return ok;
}

// The most frequent colour of a box: the SURFACE, with glyphs and rims
// outvoted. This is what "the panel is this colour" means for a region that also
// carries text.
Pixel ModalColor(const Frame& frame, int left, int top, int right, int bottom) {
  std::map<UINT32, int> histogram;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (!frame.In(x, y)) continue;
      ++histogram[frame.At(x, y) & 0x00FFFFFFu];
    }
  }
  UINT32 best = 0;
  int bestCount = -1;
  for (const auto& entry : histogram) {
    if (entry.second > bestCount) {
      bestCount = entry.second;
      best = entry.first;
    }
  }
  return ToPixel(best);
}

// The strongest ink inside a box: the pixel furthest from the box's modal
// surface. A row's glyph core must BE --text-primary; a brush that was built
// once from a literal lands 200 levels away in the daylight theme.
Pixel StrongestInk(const Frame& frame, int left, int top, int right, int bottom,
                   Pixel* surfaceOut) {
  const Pixel surface = ModalColor(frame, left, top, right, bottom);
  if (surfaceOut != nullptr) *surfaceOut = surface;
  Pixel best = surface;
  int bestDistance = -1;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (!frame.In(x, y)) continue;
      const Pixel p = ToPixel(frame.At(x, y));
      const int d = Distance(p, surface);
      if (d > bestDistance) {
        bestDistance = d;
        best = p;
      }
    }
  }
  return best;
}

// --------------------------------------------------------------- the scene

struct Rig {
  std::vector<prism::ArchiveItem> catalog;
  std::vector<prism::list::SortItem> rows;
  prism::sidebar::Model model;
  prism::layout::Layout frame;
  prism::win::sidebar::Layout rail;
  prism::win::tabbar::View tabs;
  prism::actionbar::Model actions;

  Rig() : model(&catalog) {
    const auto folder = [this](const char* path, uint32_t index) {
      prism::ArchiveItem item;
      item.index = index;
      item.path = path;
      item.isDir = true;
      catalog.push_back(item);
    };
    const auto file = [this](const char* path, uint64_t size, uint32_t index) {
      prism::ArchiveItem item;
      item.index = index;
      item.path = path;
      item.size = size;
      item.packSize = size * 3 / 4;
      catalog.push_back(item);
    };
    folder("Design_Assets/", 0);
    folder("Design_Assets/01_Brand/", 1);
    folder("Design_Assets/02_UI_Kit/", 2);
    file("Design_Assets/gate_front.png", 4'812'331, 3);
    file("Design_Assets/mural_01.png", 7'220'464, 4);
    file("Design_Assets/quadriga.png", 2'214'908, 5);

    model.SetArchiveName("Design_Assets_v4.7z");
    prism::sidebar::ArchiveInfo info;
    info.method = "LZMA2";
    info.dictionaryBytes = 64ull << 20;
    info.encryption = prism::sidebar::Encryption::Aes256;
    model.SetArchiveInfo(info);
    model.PushRecent("D:/Work/Design_Assets.7z", 500);
    model.SetFocus(prism::sidebar::FocusTarget{prism::sidebar::Section::Tree, 1});

    rows.push_back({"01_Brand_Guidelines", 0, 0, true, false, prism::PreviewKind::Directory});
    rows.push_back({"02_UI_Kit_iOS26", 0, 0, true, false, prism::PreviewKind::Directory});
    rows.push_back({"gate_front.png", 4'812'331, 3'609'248, false, false,
                    prism::PreviewKind::Image});
    rows.push_back({"mural_01.png", 7'220'464, 5'415'348, false, false,
                    prism::PreviewKind::Image});

    // ONE layout for the frame, exactly like GalleryWindow hands it in.
    frame = prism::layout::Compute(static_cast<int>(kW), static_cast<int>(kH),
                                   /*sidebarVisible=*/true, /*listMode=*/true,
                                   /*tabBarVisible=*/true);
    prism::win::sidebar::Metrics metrics;
    metrics.width = GalleryRenderer::kSidebarColumnWidth;
    metrics.viewportTop = static_cast<float>(frame.content.top);
    metrics.viewportBottom = static_cast<float>(frame.content.bottom);
    rail = prism::win::sidebar::Build(model, metrics);

    tabs.items = {{L"Design_Assets_v4.7z", false}, {L"Brand_2026.7z", true}};
    tabs.active = 0;
    actions.SetContext(prism::actionbar::Context{true, true, false});
  }

  GalleryRenderer::Scene SceneFor(Variant variant) {
    GalleryRenderer::Scene scene;
    scene.theme = variant;
    scene.listMode = true;
    scene.listItems = &rows;
    scene.selectedRow = 2;
    scene.layout = &frame;
    scene.sidebar = &rail;
    scene.tabs = &tabs;
    scene.actions = &actions;
    scene.breadcrumb = L"Design_Assets_v4.7z";
    scene.status.crypto = prism::layout::CryptoState::Encrypted;
    scene.status.method = "LZMA2";
    scene.status.dictionary = "64.0 MB";
    scene.status.ratioPercent = 54;
    // The shipping default for the refraction layer: ON, so the canvas pixel is
    // the presented (veiled) composite -- the number the card quotes.
    scene.backdropRefraction = true;
    // The C77 PANEL language is OFF here on purpose: it paints drop shadows
    // (black @.18) that reach over the list header and the floating islands, and
    // this card measures the SURFACE each region resolves to, not the depth
    // layer C77 owns. With it off the islands are painted straight from the
    // theme token, which is exactly the path that used to be "(void)variant".
    scene.glassPanels = false;
    return scene;
  }
};

bool RenderFrame(Rig& rig, Variant variant, Frame* out) {
  GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(kW, kH, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer.Draw(target.Get(), rig.SceneFor(variant));
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(kW), static_cast<INT>(kH)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  out->width = kW;
  out->height = kH;
  out->pixels.resize(static_cast<size_t>(kW) * kH);
  for (UINT32 y = 0; y < kH; ++y) {
    std::memcpy(out->pixels.data() + static_cast<size_t>(y) * kW,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(kW) * 4);
  }
  return true;
}

// The rail's panel colour: the PANEL TIER of the surface ladder -- what
// GalleryRenderer::TokenColor(Panel) resolves. C124: it used to be the floating
// island at 94 %, the reason a passive column measured 1.32:1 against the canvas.
Rgba PanelColor(Variant variant) {
  return prism::chrome::SurfaceTierPresented(prism::theme::SurfaceTier::Panel, variant);
}

const prism::win::sidebar::Command* FindCommand(const prism::win::sidebar::Layout& layout,
                                                prism::win::sidebar::PrimKind kind) {
  for (const auto& command : layout.commands) {
    if (command.kind == kind) return &command;
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C91 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C91 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
#if defined(PRISM_C91_MUTANT_SCENE_MIDNIGHT)
      return GalleryRenderer::dbgMutantTheme > 0;
#elif defined(PRISM_C91_MUTANT_BRUSHES_ONCE)
      return GalleryRenderer::dbgMutantBrushes > 0;
#else
      return GalleryRenderer::dbgMutantIsland > 0;
#endif
    });
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("  [skip] COM could not be initialised");
    std::puts("C91 SKIPPED: no COM");
    return 77;
  }
  IWICImagingFactory* wic = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wic)))) {
    std::puts("  [skip] WIC unavailable");
    std::puts("C91 SKIPPED: no WIC");
    CoUninitialize();
    return 77;
  }
  gWic = wic;
  const int failures = ::prism::test::RunAll(argc, argv);
  wic->Release();
  CoUninitialize();
  if (failures == 0) std::puts("ALL THEME-SCENE CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE CANVAS + THE STATUS STRIP: the two places a literal lived.
// ---------------------------------------------------------------------------
PRISM_TEST("c91 the canvas and the status strip are the theme's") {
  Rig rig;
  Frame day;
  Frame night;
  Frame hc;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::Daylight, &day))) return;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::Midnight, &night))) return;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::HighContrast, &hc))) return;

  const Variant variants[3] = {Variant::Daylight, Variant::Midnight,
                               Variant::HighContrast};
  const Frame* frames[3] = {&day, &night, &hc};

  // The canvas sample the card names: (1200,500) -- inside the content band,
  // right of the rail, below the four rows.
  const int canvasX = 1200;
  const int canvasY = 500;
  PRISM_CHECK(rig.frame.contentInner.left < canvasX);
  PRISM_CHECK(rig.frame.contentInner.top < canvasY);
  const prism::layout::Rect lastRow = prism::layout::ListRowRect(rig.frame, 3, 0);
  PRISM_CHECK(lastRow.bottom < canvasY);

  for (int i = 0; i < 3; ++i) {
    std::printf("  [c91] --- %s ---\n",
                std::string(prism::theme::VariantName(variants[i])).c_str());
    const Frame& frame = *frames[i];

    // (1) CANVAS.
    const Pixel canvas = ToPixel(frame.At(canvasX, canvasY));
    PRISM_CHECK(Near("canvas (1200,500)", canvas,
                     ToPixel(prism::chrome::CanvasPresentedColor(variants[i])), 2));
    if (variants[i] == Variant::Daylight) {
      // C102 moved the Daylight canvas to the plan's #F5F5F7; the assertion
      // and its +-2 tolerance are unchanged, only the expected triple is.
      PRISM_CHECK(Near("canvas day literal", canvas, Pixel{245, 245, 247}, 2));
    } else if (variants[i] == Variant::Midnight) {
  // C121: the C77 veil is the canvas colour in every theme now (it used to be
  // --surface-backdrop-reference, i.e. DARKER than the canvas, which moved this
  // pixel by dE 2.4 when refraction was switched on). The value follows the
  // token, not the other way round.
      // C124: the surface ladder's canvas (#0B0D12, the audit's #0C0D11).
      PRISM_CHECK(Near("canvas night literal", canvas, Pixel{11, 13, 18}, 2));
    } else {
      PRISM_CHECK(Near("canvas hc literal", canvas, Pixel{0, 0, 0}, 0));
    }

    // (2) STATUS STRIP: the canvas + the documented lift, from the theme.
    const prism::layout::Rect strip = rig.frame.statusBar;
    const Pixel stripPixel =
        ModalColor(frame, strip.left + 6, strip.top + 1, strip.right - 6, strip.top + 6);
    Rgba wantStrip = prism::chrome::CanvasPresentedColor(variants[i]);
    wantStrip.r += 0.016f;
    wantStrip.g += 0.018f;
    wantStrip.b += 0.022f;
    PRISM_CHECK(Near("status strip", stripPixel, ToPixel(wantStrip), 3));
    if (variants[i] == Variant::Daylight) {
      PRISM_CHECK(Near("status strip day literal", stripPixel, Pixel{249, 250, 253}, 3));
    } else if (variants[i] == Variant::HighContrast) {
      PRISM_CHECK(Near("status strip hc literal", stripPixel, Pixel{4, 5, 6}, 3));
    }

    // (6) LIST HEADER: flat canvas, per A2.
    const prism::layout::Rect header = prism::layout::ListHeaderRect(rig.frame);
    // Sample clear of the rail's drop shadow (it is painted after the content
    // and darkens the first ~20 px of the band) and above the label ink, which
    // starts at header.top + 4.
    const Pixel headerPixel = ModalColor(frame, header.left + 400, header.top + 1,
                                         header.right - 300, header.top + 3);
    std::printf("  [c91] header rect=(%d,%d,%d,%d) contentInner=(%d,%d,%d,%d) "
                "pix=(%d,%d,%d) canvas=(%d,%d,%d)\n",
                header.left, header.top, header.right, header.bottom,
                rig.frame.contentInner.left, rig.frame.contentInner.top,
                rig.frame.contentInner.right, rig.frame.contentInner.bottom,
                ToPixel(frame.At(header.left + 3, header.top + 1)).r,
                ToPixel(frame.At(header.left + 3, header.top + 1)).g,
                ToPixel(frame.At(header.left + 3, header.top + 1)).b, canvas.r, canvas.g,
                canvas.b);
    PRISM_CHECK(Near("list header == canvas", headerPixel, canvas, 2));
  }

  // The two frames must not be the same picture: this is the assertion the
  // pre-C91 build failed everywhere below y=62.
  PRISM_CHECK(day.At(canvasX, canvasY) != night.At(canvasX, canvasY));
  PRISM_CHECK(night.At(canvasX, canvasY) != hc.At(canvasX, canvasY));
}

// ---------------------------------------------------------------------------
// 2. THE CHROME SURFACES: tab plate, rail panel, selected row, telemetry
//    capsule. Same scene, one field changed.
// ---------------------------------------------------------------------------
PRISM_TEST("c91 the tab plate, the rail and its rows follow the theme") {
  Rig rig;
  for (const Variant variant : {Variant::Daylight, Variant::Midnight,
                                Variant::HighContrast}) {
    Frame frame;
    if (!PRISM_CHECK(RenderFrame(rig, variant, &frame))) return;
    std::printf("  [c91] --- %s ---\n",
                std::string(prism::theme::VariantName(variant)).c_str());

    // (3) THE ACTIVE TAB PLATE. The strip itself is the canvas; the ACTIVE tab
    // is --surface-tab-active over it.
    const std::vector<prism::win::tabbar::TabGeometry> rects =
        prism::win::tabbar::TabRects(rig.frame.tabBar, rig.tabs);
    if (!PRISM_CHECK(rects.size() >= 1)) return;
    const prism::layout::Rect tab = rects[0].rect;
    const Pixel plate = ModalColor(frame, tab.left + 6, tab.top + 4, tab.right - 8,
                                   tab.bottom - 6);
    PRISM_CHECK(Near("active tab plate", plate,
                     ToPixel(prism::chrome::ThemeSurfaceColor(TokenId::SurfaceTabActive,
                                                              variant)),
                     2));
    if (variant == Variant::Daylight) {
      PRISM_CHECK(Near("tab plate day literal", plate, Pixel{254, 254, 255}, 2));
    } else if (variant == Variant::HighContrast) {
      // C124: the tab plate is the CARD tier in every theme -- (20,20,22) on
      // black, a value of its own before this card (one of the eleven).
      PRISM_CHECK(Near("tab plate hc literal", plate, Pixel{20, 20, 22}, 2));
    }

    // (4) THE RAIL PANEL and its selected row.
    const prism::win::sidebar::Box panel = rig.rail.panel;
    const Pixel panelPixel = ModalColor(frame, static_cast<int>(panel.right) - 30,
                                        static_cast<int>(panel.top) + 4,
                                        static_cast<int>(panel.right) - 6,
                                        static_cast<int>(panel.top) + 30);
    PRISM_CHECK(Near("rail panel", panelPixel, ToPixel(PanelColor(variant)), 2));
    if (variant == Variant::Daylight) {
      PRISM_CHECK(Near("rail panel day literal", panelPixel, Pixel{255, 255, 255}, 2));
    } else if (variant == Variant::HighContrast) {
      // C124: the rail is the PANEL tier -- (14,14,16) on black.
      PRISM_CHECK(Near("rail panel hc literal", panelPixel, Pixel{14, 14, 16}, 2));
    }

    const prism::win::sidebar::Command* selection =
        FindCommand(rig.rail, prism::win::sidebar::PrimKind::Selection);
    if (!PRISM_CHECK(selection != nullptr)) return;
    const Pixel wash = ModalColor(
        frame, static_cast<int>(selection->box.left) + 6,
        static_cast<int>(selection->box.top) + 4,
        static_cast<int>(selection->box.right) - 6,
        static_cast<int>(selection->box.bottom) - 4);
    const Rgba wantWash = prism::theme::CompositeOver(
        prism::theme::Lookup(TokenId::SelectionTint, variant).color, PanelColor(variant));
    PRISM_CHECK(Near("selected row wash", wash, ToPixel(wantWash), 3));
    if (variant == Variant::Daylight) {
      PRISM_CHECK(Near("selected row day literal", wash, Pixel{219, 236, 255}, 3));
    }

    // (7) THE TELEMETRY CAPSULE (the "info card"): --surface-canvas-base @.95
    // over the rail's panel.
    const prism::win::sidebar::Command* pill =
        FindCommand(rig.rail, prism::win::sidebar::PrimKind::StatusPill);
    if (!PRISM_CHECK(pill != nullptr)) return;
    Rgba pane = prism::theme::Lookup(TokenId::SurfaceCanvasBase, variant).color;
    pane.a = 0.95f;
    const Pixel capsule = ModalColor(
        frame, static_cast<int>(pill->box.left) + 8, static_cast<int>(pill->box.top) + 6,
        static_cast<int>(pill->box.right) - 8, static_cast<int>(pill->box.bottom) - 6);
    PRISM_CHECK(Near("info card", capsule,
                     ToPixel(prism::theme::CompositeOver(pane, PanelColor(variant))), 3));
    if (variant == Variant::Daylight) {
      PRISM_CHECK(Near("info card day literal", capsule, Pixel{242, 242, 245}, 3));
    }
  }
}

// ---------------------------------------------------------------------------
// 3. THE FLOATING ISLANDS and the LIST INK: the two surfaces that read
//    "variant ignored" in the old code ((void)variant; kIslandFill; brushes
//    built once).
// ---------------------------------------------------------------------------
PRISM_TEST("c91 the islands and the list ink follow the theme") {
  Rig rig;
  const prism::actionbar::Layout band = prism::actionbar::Compute(rig.frame);
  if (!PRISM_CHECK(!band.island.Empty())) return;
  const prism::layout::Rect nav = prism::layout::NavPillRect(rig.frame);

  for (const Variant variant : {Variant::Daylight, Variant::Midnight,
                                Variant::HighContrast}) {
    Frame frame;
    if (!PRISM_CHECK(RenderFrame(rig, variant, &frame))) return;
    std::printf("  [c91] --- %s ---\n",
                std::string(prism::theme::VariantName(variant)).c_str());

    // (5) THE ACTION ISLAND and the BREADCRUMB ISLAND: --surface-floating-island
    // over the canvas. They used to be kIslandFill in every theme.
    const Pixel island = ModalColor(frame, band.island.left + 6, band.island.top + 4,
                                    band.island.left + 120, band.island.bottom - 4);
    const Pixel wantIsland = ToPixel(
        prism::chrome::ThemeSurfaceColor(TokenId::SurfaceFloatingIsland, variant));
    PRISM_CHECK(Near("action island", island, wantIsland, 2));
    if (variant == Variant::Daylight) {
      PRISM_CHECK(Near("action island day literal", island, Pixel{253, 253, 254}, 2));
    } else if (variant == Variant::Midnight) {
      // C124: the FLOATING tier (#232833 -- the audit's action-island sample).
      PRISM_CHECK(Near("action island night literal", island, Pixel{35, 40, 51}, 2));
    }

    // The breadcrumb pill is the SAME token through a different painter (the
    // C77 glass panel draws fill + rim + shadow), so it is asserted against the
    // token rather than against the action island's pixel-for-pixel value.
    const Pixel breadcrumb = ModalColor(frame, nav.left + 6, nav.top + 4, nav.left + 80,
                                        nav.bottom - 4);
    std::printf("  [c91] breadcrumb=(%d,%d,%d) island=(%d,%d,%d)\n", breadcrumb.r,
                breadcrumb.g, breadcrumb.b, island.r, island.g, island.b);
    PRISM_CHECK(Near("breadcrumb island", breadcrumb, wantIsland, 6));

    // (6) THE LIST ROW INK. The brushes are recoloured every frame, so the
    // strongest glyph pixel must BE --text-primary -- and in the old build it
    // stayed (232,236,241) in every theme.
    const prism::layout::Rect row = prism::layout::ListRowRect(rig.frame, 2, 0);
    const int nameRight =
        static_cast<int>(rig.frame.contentInner.right) - 60 - 240 - 200 - 90;
    Pixel surface;
    const Pixel ink = StrongestInk(frame, row.left + 40, row.top + 4,
                                   (std::min)(nameRight, row.right - 120), row.bottom - 2,
                                   &surface);
    const Pixel wantInk =
        ToPixel(prism::theme::Lookup(TokenId::TextPrimary, variant).color);
    std::printf("  [c91] row ink=(%d,%d,%d) on (%d,%d,%d)\n", ink.r, ink.g, ink.b, surface.r,
                surface.g, surface.b);
    PRISM_CHECK(Near("list row ink", ink, wantInk, 3));
    if (variant == Variant::Daylight) {
      PRISM_CHECK(Near("row ink day literal", ink, Pixel{29, 29, 31}, 3));
    } else if (variant == Variant::HighContrast) {
      PRISM_CHECK(Near("row ink hc literal", ink, Pixel{255, 255, 255}, 2));
    }
  }
}

// ---------------------------------------------------------------------------
// 4. THE HEADLINE: the content band actually changes between themes. The
//    measured defect was "92 % of the window is pixel-identical".
// ---------------------------------------------------------------------------
PRISM_TEST("c91 the content band changes with the theme") {
  Rig rig;
  Frame day;
  Frame night;
  Frame hc;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::Daylight, &day))) return;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::Midnight, &night))) return;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::HighContrast, &hc))) return;

  const prism::layout::Rect band = rig.frame.contentInner;
  const auto changedFraction = [&](const Frame& a, const Frame& b) {
    size_t changed = 0;
    size_t total = 0;
    for (int y = band.top; y < band.bottom; ++y) {
      for (int x = band.left; x < band.right; ++x) {
        if (!a.In(x, y)) continue;
        ++total;
        const Pixel pa = ToPixel(a.At(x, y));
        const Pixel pb = ToPixel(b.At(x, y));
        if (Distance(pa, pb) > 8) ++changed;
      }
    }
    return total == 0 ? 0.0 : static_cast<double>(changed) / static_cast<double>(total);
  };
  const double dayNight = changedFraction(day, night);
  const double nightHc = changedFraction(night, hc);
  std::printf("  [c91] content band changed: day->night %.2f%%  night->hc %.2f%%\n",
              dayNight * 100.0, nightHc * 100.0);
  // The pre-C91 measurement was 0 % below y=62. Requiring 80 % leaves room for
  // glyphs that happen to survive an antialiasing change while still being a
  // statement about the WHOLE band, not about one sampled pixel.
  PRISM_CHECK(dayNight >= 0.80);
  PRISM_CHECK(nightHc >= 0.80);
}

// ---------------------------------------------------------------------------
// 5. THE WIRING: the window hands the SAME variant to the shell and to the
//    scene. This is the one-line glue that made the renderer's default
//    Midnight unreachable from the settings dialog.
// ---------------------------------------------------------------------------
PRISM_TEST("c91 the window's theme reaches the scene") {
  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"PrismArchive c91 theme", GetModuleHandleW(nullptr))) {
    std::puts("  [skip] no window station (the offscreen cases above still ran)");
    return;
  }
  for (const Variant variant : {Variant::Daylight, Variant::Midnight,
                                Variant::HighContrast}) {
    window.SetChromeVariant(variant);
    // SetChromeVariant is the window's ONE theme setter; DrawFrame hands this
    // same field to the renderer as Scene::theme.
    PRISM_CHECK(window.ContentTheme() == variant);
    PRISM_CHECK(window.ChromeVariant() == variant);
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}
