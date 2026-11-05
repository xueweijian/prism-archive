// PrismArchive -- PrismWin
// C104: the sidebar goes macOS -- a capSULE selection instead of the hard-edged
// light-blue rectangle, and a two-tone SOFT folder instead of the solid accent
// plate (the user's "Windows 98/XP Explorer" report).
//
// Every claim below is a PIXEL measurement taken from the REAL renderer
// (GalleryRenderer::Draw) into a headless WIC target -- no window, no GPU, no
// font file -- or a measurement of the DRAW COMMAND the view emitted. Nothing
// here reads a comment.
//
//   * the selection's CORNER pixel differs from its CENTRE pixel (a rounded
//     pill); a square fill makes them equal, which is exactly the pre-C104
//     rectangle, so this assertion can really go red;
//   * the wash alpha stays under 15% in both design themes (Midnight used to be
//     16%), with HighContrast pinned as its own accessibility FLOOR -- a
//     "clamp everything" edit that would hide the selection on black goes red;
//   * the folder glyph is translucent and lifted off the raw accent: the
//     measured body pixel matches the alpha-network composite to +/- 3 and sits
//     far from the opaque accent plate, and neither the view nor the folder
//     painter contains the literal #007AFF;
//   * one indent step is 14..16 px and the deltas between tree depths are real
//     (measured from the emitted RowHit boxes, not from a constant).
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): the mutant targets compile THIS
// source with one rule broken, and each is registered with a
// PASS_REGULAR_EXPRESSION on "C104 MUTATION CAUGHT: <arm>". The verdict is armed
// on the renderer's own proof that the injected branch ran, so an unrelated red
// run cannot print it.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"

#ifndef PRISM_C104_SOURCE_DIR
#define PRISM_C104_SOURCE_DIR "."
#endif

using Microsoft::WRL::ComPtr;

namespace {

using prism::ArchiveItem;
using prism::sidebar::Encryption;
using prism::sidebar::FocusTarget;
using prism::sidebar::Section;
using prism::theme::Rgba;
using prism::theme::TokenId;
using prism::theme::Variant;
using prism::win::sidebar::Box;
using prism::win::sidebar::Command;
using prism::win::sidebar::Layout;
using prism::win::sidebar::Metrics;
using prism::win::sidebar::PrimKind;

using Renderer = prism::win::gallery::GalleryRenderer;

constexpr UINT32 kWidth = 1400;
constexpr UINT32 kHeight = 900;

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
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
};

UINT32 ToPixel(const Rgba& c) {
  const auto byte = [](float v) {
    const double scaled = static_cast<double>(v) * 255.0 + 0.5;
    return static_cast<UINT32>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
  };
  return (byte(c.a) << 24) | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

Rgba FromPixel(UINT32 p) {
  const auto channel = [p](int shift) {
    return static_cast<float>((p >> shift) & 0xFFu) / 255.0f;
  };
  return Rgba(channel(16), channel(8), channel(0), channel(24));
}

double Distance(UINT32 a, UINT32 b) {
  double sum = 0.0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const double d = static_cast<double>((a >> shift) & 0xFFu) -
                     static_cast<double>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return std::sqrt(sum);
}

// The most frequent colour in a box: the fill of a pill/plate, immune to the
// glyph ink that sits on top of it (a single-pixel probe would land in a
// letterform and report the text, not the material).
UINT32 ModalColor(const Frame& frame, const Box& box) {
  UINT32 best = 0;
  int bestCount = 0;
  std::vector<UINT32> seen;
  std::vector<int> counts;
  for (int y = static_cast<int>(box.top); y < static_cast<int>(box.bottom); ++y) {
    for (int x = static_cast<int>(box.left); x < static_cast<int>(box.right); ++x) {
      if (!frame.In(x, y)) continue;
      const UINT32 p = frame.At(x, y);
      std::size_t i = 0;
      for (; i < seen.size(); ++i) {
        if (seen[i] == p) { ++counts[i]; break; }
      }
      if (i == seen.size()) {
        seen.push_back(p);
        counts.push_back(1);
      }
      if (counts[i] > bestCount) {
        bestCount = counts[i];
        best = p;
      }
    }
  }
  return best;
}

IWICImagingFactory* gWic = nullptr;
const char* gOutDir = nullptr;

bool RenderScene(const prism::win::gallery::GalleryRenderer::Scene& scene, UINT32 width,
                 UINT32 height, Frame* frame) {
  Renderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  frame->width = width;
  frame->height = height;
  frame->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  return true;
}

bool WritePng(const wchar_t* path, UINT32 width, UINT32 height,
              const std::vector<UINT32>& pixels) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(
          width, height, GUID_WICPixelFormat32bppPBGRA, width * 4,
          static_cast<UINT>(pixels.size() * 4),
          reinterpret_cast<BYTE*>(const_cast<UINT32*>(pixels.data())),
          bitmap.GetAddressOf()))) {
    return false;
  }
  return SUCCEEDED(gWic->CreateStream(stream.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
         SUCCEEDED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                       encoder.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
                                           bag.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(frame->Initialize(bag.Get())) && SUCCEEDED(frame->SetSize(width, height)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
}

// ------------------------------------------------------------------- fixtures
ArchiveItem File(const char* path, std::uint32_t index, std::uint64_t size) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.size = size;
  item.packSize = size / 2;
  return item;
}

std::vector<ArchiveItem> SampleCatalog() {
  std::vector<ArchiveItem> items;
  ArchiveItem dir;
  dir.path = "Design_Assets";
  dir.index = 1;
  dir.isDir = true;
  items.push_back(dir);
  ArchiveItem sub;
  sub.path = "Design_Assets/01_Brand";
  sub.index = 2;
  sub.isDir = true;
  items.push_back(sub);
  items.push_back(File("Design_Assets/01_Brand/logo.svg", 3, 1200));
  items.push_back(File("Design_Assets/02_UI_Kit/Hero.webp", 4, 18400000));
  items.push_back(File("root.png", 5, 500));
  return items;
}

// The C12 command list, built the way GalleryWindow::EnsureSidebar does.
Layout BuildLayout(const std::vector<ArchiveItem>& items, Variant /*variant*/, float viewportHeight,
                   FocusTarget focus) {
  prism::sidebar::Model model(&items);
  model.SetArchiveName("Design_Assets_v4.7z");
  model.SetFavorites({
      prism::sidebar::Favorite{"Desktop", "C:/Users/designer/Desktop", "home"},
      prism::sidebar::Favorite{"Downloads", "C:/Users/designer/Downloads", "download"},
  });
  model.PushRecent("D:/Work/Design_Assets.7z", 500);
  prism::sidebar::ArchiveInfo info;
  info.method = "LZMA2";
  info.dictionaryBytes = 64ull << 20;
  info.encryption = Encryption::Aes256;
  model.SetArchiveInfo(info);
  model.SetFocus(focus);

  Metrics metrics;
  metrics.width = Renderer::kSidebarColumnWidth;
  metrics.viewportTop = 0.0f;
  metrics.viewportBottom = viewportHeight;
  return prism::win::sidebar::Build(model, metrics);
}

const Command* Find(const Layout& layout, PrimKind kind) {
  for (const Command& command : layout.commands) {
    if (command.kind == kind) return &command;
  }
  return nullptr;
}

std::vector<const Command*> All(const Layout& layout, PrimKind kind) {
  std::vector<const Command*> out;
  for (const Command& command : layout.commands) {
    if (command.kind == kind) out.push_back(&command);
  }
  return out;
}

// The tree's folder glyph on the FIRST tree row (depth 0), i.e. the icon the
// fixture's "Design_Assets" folder emits.
const Command* FirstFolderGlyph(const Layout& layout) {
  for (const Command& command : layout.commands) {
    if (command.kind == PrimKind::Glyph && command.icon == "folder" &&
        command.section == Section::Tree) {
      return &command;
    }
  }
  return nullptr;
}

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// Comments are documentation, not a colour source: the scan is about CODE, so
// a later explanation of the defect cannot turn the assertion red by itself.
std::string StripComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
      i = (std::min)(text.size(), i + 2);
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

// The balanced body of one function: from the first '{' after 'signature' to
// its matching '}'. Scoping the scan to the folder painter keeps the assertion
// about THIS icon instead of the whole 2,700-line renderer (which legitimately
// carries the accent literal for the list's semantic bar colours).
std::string FunctionBody(const std::string& source, const std::string& signature) {
  const std::size_t start = source.find(signature);
  if (start == std::string::npos) return {};
  const std::size_t open = source.find('{', start);
  if (open == std::string::npos) return {};
  int depth = 0;
  for (std::size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') {
      ++depth;
    } else if (source[i] == '}') {
      --depth;
      if (depth == 0) return source.substr(open, i - open + 1);
    }
  }
  return {};
}

}  // namespace

// ---------------------------------------------------------------------- cases
// ASSERTION 1 (the roundness evidence). A capsule's CORNER pixel is background;
// its CENTRE is the wash. A hard-edged rectangle -- which is literally the
// shipped pre-C104 code, and what PRISM_C104_MUTANT_SQUARE compiles -- makes the
// two identical, so this check is red on the defect and green on the fix.
PRISM_TEST("c104 selection is a capsule: corner pixel is not the centre pixel") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout =
      BuildLayout(items, Variant::Midnight, static_cast<float>(kHeight),
                  FocusTarget{Section::Tree, 0});
  const Command* selection = Find(layout, PrimKind::Selection);
  if (!PRISM_CHECK(selection != nullptr)) return;

  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.sidebar = &layout;
  Frame frame;
  if (!PRISM_CHECK(RenderScene(scene, kWidth, kHeight, &frame))) return;

  const int left = static_cast<int>(selection->box.left);
  const int top = static_cast<int>(selection->box.top);
  const int right = static_cast<int>(selection->box.right);
  const int bottom = static_cast<int>(selection->box.bottom);

  const UINT32 corner = frame.At(left, top);
  const UINT32 centre = ModalColor(frame, Box{selection->box.left + 6.0f,
                                              selection->box.top + 6.0f,
                                              selection->box.right - 6.0f,
                                              selection->box.bottom - 6.0f});
  const UINT32 panel = frame.At(6, top + 2);
  const double cornerToCentre = Distance(corner, centre);
  const double cornerToPanel = Distance(corner, panel);
  std::printf("  [c104] pill corner #%08X centre #%08X panel #%08X: |corner-centre|=%.1f "
              "|corner-panel|=%.1f\n",
              corner, centre, panel, cornerToCentre, cornerToPanel);

  // THE assertion (D3): a square fill paints the corner and the centre with the
  // same wash, so this pair is red on exactly the defect the card fixes.
  PRISM_CHECK(corner != centre);
  PRISM_CHECK(cornerToCentre > 12.0);
  // Its two halves, so the pair cannot be satisfied by an empty pill: the
  // corner is the row's background, while the middle IS the wash.
  PRISM_CHECK(cornerToPanel < 6.0);
  PRISM_CHECK(Distance(centre, panel) > 12.0);
  (void)right;
  (void)bottom;
}

// ASSERTION 2 (the wash is a WASH). <= 15% in both design themes, with the
// HighContrast floor pinned separately so "clamp everything to the design band"
// -- which would make the selection invisible on pure black -- goes red.
PRISM_TEST("c104 the selected-row wash stays under 15 percent") {
  const Variant design[] = {Variant::Daylight, Variant::Midnight};
  for (const Variant variant : design) {
    const Rgba tint = prism::theme::Lookup(TokenId::SelectionTint, variant).color;
    std::printf("  [c104] selection tint %s alpha %.3f\n",
                std::string(prism::theme::VariantName(variant)).c_str(),
                static_cast<double>(tint.a));
    PRISM_CHECK(tint.a <= 0.15f);
    PRISM_CHECK(tint.a > 0.06f);  // still visible: not a token turned to zero
  }
  const Rgba highContrast =
      prism::theme::Lookup(TokenId::SelectionTint, Variant::HighContrast).color;
  std::printf("  [c104] selection tint HighContrast alpha %.3f (a11y floor)\n",
              static_cast<double>(highContrast.a));
  PRISM_CHECK(highContrast.a >= 0.20f);

  // Pixel corroboration: the row is the wash OVER the panel, nowhere near the
  // opaque accent (a plate). Measured on the real frame, in both design themes.
  const std::vector<ArchiveItem> items = SampleCatalog();
  for (const Variant variant : design) {
    const Layout layout = BuildLayout(items, variant, static_cast<float>(kHeight),
                                      FocusTarget{Section::Tree, 0});
    const Command* selection = Find(layout, PrimKind::Selection);
    if (!PRISM_CHECK(selection != nullptr)) return;
    prism::win::gallery::GalleryRenderer::Scene scene;
    scene.listMode = true;
    scene.sidebar = &layout;
    scene.theme = variant;
    Frame frame;
    if (!PRISM_CHECK(RenderScene(scene, kWidth, kHeight, &frame))) return;
    const UINT32 wash = ModalColor(frame, Box{selection->box.left + 6.0f,
                                              selection->box.top + 6.0f,
                                              selection->box.right - 6.0f,
                                              selection->box.bottom - 6.0f});
    const UINT32 panel = frame.At(6, static_cast<int>(selection->box.top) + 2);
    const UINT32 accent = ToPixel(
        prism::theme::Lookup(TokenId::AccentLiquidPrimary, variant).gradient.from);
    std::printf("  [c104] %s wash #%08X panel #%08X accent #%08X: |wash-panel|=%.1f "
                "|wash-accent|=%.1f\n",
                std::string(prism::theme::VariantName(variant)).c_str(), wash, panel, accent,
                Distance(wash, panel), Distance(wash, accent));
    PRISM_CHECK(Distance(wash, panel) > 8.0);    // the wash exists
    PRISM_CHECK(Distance(wash, accent) > 100.0); // and it is not the accent plate
  }
}

// The geometry the view owns: 8 px inset at both ends and a 6 px corner, both
// read from the emitted command rather than from the renderer's mood.
PRISM_TEST("c104 pill geometry: 8 px inset, 6 px corner, 3 px accent rule") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout =
      BuildLayout(items, Variant::Midnight, static_cast<float>(kHeight),
                  FocusTarget{Section::Tree, 0});
  const Command* selection = Find(layout, PrimKind::Selection);
  const Command* bar = Find(layout, PrimKind::SelectionBar);
  if (!PRISM_CHECK(selection != nullptr)) return;
  if (!PRISM_CHECK(bar != nullptr)) return;

  // The matching RowHit is the row the pill highlights.
  const Command* row = nullptr;
  for (const Command* hit : All(layout, PrimKind::RowHit)) {
    if (hit->section == selection->section && hit->payload == selection->payload) {
      row = hit;
    }
  }
  if (!PRISM_CHECK(row != nullptr)) return;

  std::printf("  [c104] row [%.1f,%.1f] pill [%.1f,%.1f] radius %.1f bar [%.1f,%.1f]\n",
              static_cast<double>(row->box.left), static_cast<double>(row->box.right),
              static_cast<double>(selection->box.left),
              static_cast<double>(selection->box.right),
              static_cast<double>(selection->cornerRadius),
              static_cast<double>(bar->box.left), static_cast<double>(bar->box.right));
  PRISM_CHECK(selection->box.left == row->box.left + 8.0f);
  PRISM_CHECK(selection->box.right == row->box.right - 8.0f);
  PRISM_CHECK(selection->cornerRadius == 6.0f);
  PRISM_CHECK(selection->box.top == row->box.top);
  PRISM_CHECK(selection->box.bottom == row->box.bottom);
  // The 3 px accent rule rides the pill's left edge (not the row's), so it can
  // never be left behind when the pill is inset.
  PRISM_CHECK(bar->box.left == selection->box.left);
  PRISM_CHECK(bar->box.Width() == 3.0f);
}

// ASSERTION 3 (the soft folder). Source scan + token + pixel, all three.
PRISM_TEST("c104 the folder glyph is a soft two-tone, not an opaque plate") {
  // (a) no literal accent in the view, and none in the folder painter either.
  const std::string view =
      ReadFile(std::string(PRISM_C104_SOURCE_DIR) + "/Src/PrismWin/SidebarView.hpp");
  const std::string renderer =
      ReadFile(std::string(PRISM_C104_SOURCE_DIR) + "/Src/PrismWin/GalleryRenderer.hpp");
  if (!PRISM_CHECK(!view.empty())) return;
  if (!PRISM_CHECK(!renderer.empty())) return;
  const std::string folderBody =
      StripComments(FunctionBody(renderer, "void DrawFolderGlyph("));
  if (!PRISM_CHECK(!folderBody.empty())) return;
  // A truncated extraction would make every scan below vacuously pass, so the
  // region is pinned: the folder painter draws two rounded fills.
  PRISM_CHECK(folderBody.size() > 200);
  PRISM_CHECK(folderBody.find("FillRoundedRectangle") != std::string::npos);
  const std::string loweredView = Lower(StripComments(view));
  const std::string loweredFolder = Lower(folderBody);
  std::printf("  [c104] folder painter scanned: %zu bytes; view scanned: %zu bytes\n",
              folderBody.size(), view.size());
  PRISM_CHECK(loweredView.find("#007aff") == std::string::npos);
  PRISM_CHECK(loweredFolder.find("007aff") == std::string::npos);
  PRISM_CHECK(loweredFolder.find("0.478") == std::string::npos);  // 122/255
  PRISM_CHECK(loweredFolder.find("122, channelfrombyte(255)") == std::string::npos);

  // (b) the token: translucent, and genuinely TWO tones.
  const prism::win::sidebar::FolderIcon token = prism::win::sidebar::kFolderIcon;
  std::printf("  [c104] folder token: body a=%.2f mix=%.2f / tab a=%.2f mix=%.2f\n",
              static_cast<double>(token.bodyAlpha), static_cast<double>(token.bodyMix),
              static_cast<double>(token.tabAlpha), static_cast<double>(token.tabMix));
  PRISM_CHECK(token.bodyAlpha <= 0.9f);
  PRISM_CHECK(token.tabAlpha <= 0.9f);
  PRISM_CHECK(token.bodyAlpha > 0.5f);  // still an icon, not a ghost
  PRISM_CHECK(token.tabAlpha < token.bodyAlpha);
  PRISM_CHECK(token.tabMix > token.bodyMix);  // the back tab is the softer tone

  // (c) the pixels: the body is the accent LIFTED toward the panel and painted
  // at bodyAlpha, so the measured pixel must match that composite and must sit
  // far from the opaque accent. The mutation arm repaints the plate and fails
  // both halves.
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout = BuildLayout(items, Variant::Midnight, static_cast<float>(kHeight),
                                    FocusTarget{Section::Favorites, 0});
  const Command* glyph = FirstFolderGlyph(layout);
  if (!PRISM_CHECK(glyph != nullptr)) return;
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.sidebar = &layout;
  scene.theme = Variant::Midnight;
  Frame frame;
  if (!PRISM_CHECK(RenderScene(scene, kWidth, kHeight, &frame))) return;

  const Box body{glyph->box.left + 4.0f, glyph->box.top + 8.0f,
                 glyph->box.right - 4.0f, glyph->box.bottom - 4.0f};
  const UINT32 observed = ModalColor(frame, body);
  const UINT32 background = frame.At(6, static_cast<int>(glyph->box.top) + 8);
  const Rgba accent = prism::theme::Lookup(TokenId::AccentLiquidPrimary, Variant::Midnight)
                          .gradient.from;
  Rgba panel = prism::theme::Lookup(TokenId::SurfaceFloatingIsland, Variant::Midnight).color;
  panel.a = 0.94f;
  Rgba tone = accent;
  tone.r = accent.r + (panel.r - accent.r) * token.bodyMix;
  tone.g = accent.g + (panel.g - accent.g) * token.bodyMix;
  tone.b = accent.b + (panel.b - accent.b) * token.bodyMix;
  tone.a = token.bodyAlpha;
  const UINT32 wantSoft = ToPixel(prism::theme::CompositeOver(tone, FromPixel(background)));
  Rgba opaque = tone;
  opaque.a = 1.0f;
  opaque.r = accent.r;
  opaque.g = accent.g;
  opaque.b = accent.b;
  const UINT32 wantPlate = ToPixel(opaque);
  const double toSoft = Distance(observed, wantSoft);
  const double toPlate = Distance(observed, wantPlate);
  std::printf("  [c104] folder body #%08X soft #%08X plate #%08X: |body-soft|=%.1f "
              "|body-plate|=%.1f\n",
              observed, wantSoft, wantPlate, toSoft, toPlate);
  PRISM_CHECK(toSoft <= 3.0);
  PRISM_CHECK(toPlate > 12.0);
}

// The tree indentation the briefing asks for: one indent step is 14..16 px, and
// the emitted rows really move by it (measured from the RowHit boxes).
PRISM_TEST("c104 tree indent is 14..16 px per level") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout =
      BuildLayout(items, Variant::Midnight, static_cast<float>(kHeight),
                  FocusTarget{Section::Tree, 0});
  const std::vector<const Command*> rows = All(layout, PrimKind::RowHit);
  if (!PRISM_CHECK(rows.size() >= 3)) return;

  // The fixture's tree is 2_levels: root (depth 0), its child (depth 1).
  std::printf("  [c104] tree row lefts:");
  for (const Command* row : rows) {
    if (row->section == Section::Tree) std::printf(" %.1f", static_cast<double>(row->box.left));
  }
  std::printf("\n");
  const Metrics metrics;
  PRISM_CHECK(metrics.indent >= 14.0f);
  PRISM_CHECK(metrics.indent <= 16.0f);

  double previous = -1.0;
  for (const Command* row : rows) {
    if (row->section != Section::Tree) continue;
    if (previous >= 0.0) {
      const double step = static_cast<double>(row->box.left) - previous;
      // Sibling rows share a depth, so the step is either 0 or one indent.
      PRISM_CHECK(step == 0.0 || (step >= 14.0 && step <= 16.0));
    }
    previous = row->box.left;
  }
  // Positive control: the fixture really has two depths, otherwise the loop
  // above compares equal lefts and proves nothing.
  const Command* first = nullptr;
  const Command* last = nullptr;
  for (const Command* row : rows) {
    if (row->section != Section::Tree) continue;
    if (first == nullptr) first = row;
    last = row;
  }
  if (!PRISM_CHECK(first != nullptr) || !PRISM_CHECK(last != nullptr)) return;
  PRISM_CHECK(last->box.left > first->box.left);
  PRISM_CHECK(last->box.left - first->box.left >= 14.0);
}

int main(int argc, char** argv) {
  const std::string_view mutation = Renderer::ActiveMutation();
  if (!mutation.empty()) {
    std::printf("C104 MUTANT ACTIVE: %.*s\n", static_cast<int>(mutation.size()),
                mutation.data());
  } else {
    std::puts("C104 MUTANT: none (clean build)");
  }
  if (argc > 1) gOutDir = argv[1];

#if defined(PRISM_C104_MUTANT_SQUARE)
  prism::test::SetMutationVerdict("C104 MUTATION CAUGHT: square", [] {
    return prism::win::gallery::GalleryRenderer::dbgMutantSquare > 0;
  });
#elif defined(PRISM_C104_MUTANT_FOLDER_OPAQUE)
  prism::test::SetMutationVerdict("C104 MUTATION CAUGHT: folder_opaque", [] {
    return prism::win::gallery::GalleryRenderer::dbgMutantFolder > 0;
  });
#endif

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the sidebar painter cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the sidebar painter cannot be verified");
    return 1;
  }
  gWic = wic.Get();

  const int failures = prism::test::RunAll(argc, argv);

  // Screenshot from the SAME render path (never a hand-drawn mockup): the
  // C29-shaped frame with a selected tree row, written only when asked.
  if (gOutDir != nullptr) {
    const std::vector<ArchiveItem> items = SampleCatalog();
    const Layout layout = BuildLayout(items, Variant::Daylight, static_cast<float>(kHeight),
                                      FocusTarget{Section::Tree, 1});
    prism::win::gallery::GalleryRenderer::Scene scene;
    scene.listMode = true;
    scene.sidebar = &layout;
    scene.theme = Variant::Daylight;
    Frame frame;
    if (RenderScene(scene, kWidth, kHeight, &frame)) {
      // The rail plus a slice of the list, so the column boundary is visible and
      // the sidebar is legible at 1:1 (a full 1400 px frame renders it tiny).
      const UINT32 cropWidth = 320;
      std::vector<UINT32> cropped(static_cast<size_t>(cropWidth) * kHeight);
      for (UINT32 y = 0; y < kHeight; ++y) {
        std::memcpy(cropped.data() + static_cast<size_t>(y) * cropWidth,
                    frame.pixels.data() + static_cast<size_t>(y) * kWidth,
                    static_cast<size_t>(cropWidth) * 4);
      }
      wchar_t path[MAX_PATH];
      std::swprintf(path, MAX_PATH, L"%hs\\c104-sidebar.png", gOutDir);
      std::printf("C104 SNAPSHOT: %hs (%s)\n", gOutDir,
                  WritePng(path, cropWidth, kHeight, cropped) ? "written" : "FAILED");
    }
  }

  if (failures != 0) {
    std::puts("C104 SIDEBAR CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C104 SIDEBAR CHECKS PASSED");
  return 0;
}
