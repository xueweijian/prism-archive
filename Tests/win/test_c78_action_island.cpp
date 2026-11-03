// PrismArchive -- card C78: the action island's BUTTON layer (Liquid Glass v2).
//
// C77 owns the island's background (glass fill / rim / shadow); this card owns
// what lives INSIDE it and the geometry that places it. The measured rules:
//
//   1. a 24 px glyph, an 11 px label, 6 px above and below -- the old 20 px box
//      read as a 16 px icon in a 56 px band;
//   2. resting button = transparent; hover = token-white +4%; press = the
//      CONTENT sinks 1 px while the fill stays put; focus = a 2 px accent ring;
//      disabled = 38% ink and a TextSecondary label (measured < 3:1 contrast,
//      so a disabled word never still passes AA);
//   3. Groups are separated by a 1 px token-white seam at alpha 0.08, with a
//      12 px gap across the seam and 4 px inside a Group;
//   4. only the two menu actions carry a chevron, and only while hovered,
//      pressed or open -- no permanent dropdown affordance.
//
// Every rule below is asserted on RENDERED PIXELS (or on the integer geometry
// the painter reads), not by looking at the PNG. The PNG is written only when
// the suite is given an output directory, for the PR's before/after images.
//
// Negative controls (CONTRACTS D3 + D27; same source, compile-time switch):
//   PRISM_C78_MUTANT_NO_GROUP_SEPARATOR  the seams stop painting
//   PRISM_C78_MUTANT_NO_FOCUS_RING       the keyboard ring stops painting
//   PRISM_C78_MUTANT_NO_ARROW            the chevron stops painting
//   PRISM_C78_MUTANT_STRONG_HOVER        hover wash jumps to 18% (fails 3-8)
//   PRISM_C78_MUTANT_DISABLED_BRIGHT     disabled ink jumps to 70% (>=3:1)
// Each mutant is registered under PASS_REGULAR_EXPRESSION (never WILL_FAIL,
// D26) and the marker is printed at the FIRST failed assertion (D27).
//
// /WX- is the project's Windows-test convention (B9): SDK headers are not /W4.
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
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"

using Microsoft::WRL::ComPtr;
using prism::actionbar::ActionId;
using prism::actionbar::Context;
using prism::actionbar::Layout;
using prism::actionbar::Rect;

#if defined(PRISM_C78_MUTANT_NO_GROUP_SEPARATOR)
inline constexpr const char* kMutantBanner = "C78 MUTANT ACTIVE: no_group_separator";
inline constexpr const char* kMutantMarker = "C78 MUTATION CAUGHT: no_group_separator";
#elif defined(PRISM_C78_MUTANT_NO_FOCUS_RING)
inline constexpr const char* kMutantBanner = "C78 MUTANT ACTIVE: no_focus_ring";
inline constexpr const char* kMutantMarker = "C78 MUTATION CAUGHT: no_focus_ring";
#elif defined(PRISM_C78_MUTANT_NO_ARROW)
inline constexpr const char* kMutantBanner = "C78 MUTANT ACTIVE: no_arrow";
inline constexpr const char* kMutantMarker = "C78 MUTATION CAUGHT: no_arrow";
#elif defined(PRISM_C78_MUTANT_STRONG_HOVER)
inline constexpr const char* kMutantBanner = "C78 MUTANT ACTIVE: strong_hover";
inline constexpr const char* kMutantMarker = "C78 MUTATION CAUGHT: strong_hover";
#elif defined(PRISM_C78_MUTANT_DISABLED_BRIGHT)
inline constexpr const char* kMutantBanner = "C78 MUTANT ACTIVE: disabled_bright";
inline constexpr const char* kMutantMarker = "C78 MUTATION CAUGHT: disabled_bright";
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
#endif

namespace {

constexpr UINT32 kWidth = 1280;
constexpr UINT32 kHeight = 800;

IWICImagingFactory* gWic = nullptr;
const char* gOutDir = nullptr;

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
  int Luminance(int x, int y) const {
    return (Channel(x, y, 16) + Channel(x, y, 8) + Channel(x, y, 0)) / 3;
  }
};

struct Box {
  int left = 0, top = 0, right = 0, bottom = 0;
};

Box BoxOf(const Rect& r) { return Box{r.left, r.top, r.right, r.bottom}; }

int ColorDistance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

// Mean per-channel luminance over a box (no glyph lives there on purpose).
double MeanLuminance(const Frame& frame, const Box& box) {
  double sum = 0.0;
  int count = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      sum += frame.Luminance(x, y);
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

double LinearChannel(int v) {
  const double c = static_cast<double>(v) / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(UINT32 color) {
  return 0.2126 * LinearChannel(static_cast<int>((color >> 16) & 0xFFu)) +
         0.7152 * LinearChannel(static_cast<int>((color >> 8) & 0xFFu)) +
         0.0722 * LinearChannel(static_cast<int>(color & 0xFFu));
}

double ContrastRatio(UINT32 a, UINT32 b) {
  const double la = RelativeLuminance(a);
  const double lb = RelativeLuminance(b);
  const double hi = (std::max)(la, lb);
  const double lo = (std::min)(la, lb);
  return (hi + 0.05) / (lo + 0.05);
}

struct Render {
  bool ok = false;
  Frame frame;
};

prism::win::gallery::GalleryRenderer::Scene BaseScene(
    const prism::actionbar::Model* model, int hover, int pressed,
    prism::theme::Variant variant = prism::theme::Variant::Midnight) {
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.actions = model;
  scene.actionHover = hover;
  scene.actionPressed = pressed;
  scene.actionTheme = variant;
  scene.breadcrumb = L"Design_Assets_v4.7z";
  return scene;
}

bool RenderScene(const prism::win::gallery::GalleryRenderer::Scene& scene, Render* out,
                 UINT32 width = kWidth, UINT32 height = kHeight) {
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
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
  out->frame.width = width;
  out->frame.height = height;
  out->frame.pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(out->frame.pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  out->ok = true;
  return true;
}

bool WritePng(const wchar_t* path, UINT32 width, UINT32 height,
              const std::vector<UINT32>& pixels) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
                                          width * 4,
                                          static_cast<UINT>(pixels.size() * 4),
                                          reinterpret_cast<BYTE*>(
                                              const_cast<UINT32*>(pixels.data())),
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

void SaveBand(const wchar_t* name, const Frame& frame) {
  if (gOutDir == nullptr) return;
  const int top = 20, bottom = 120;
  std::vector<UINT32> crop(static_cast<size_t>(kWidth) * (bottom - top));
  for (int y = top; y < bottom; ++y) {
    std::memcpy(crop.data() + static_cast<size_t>(y - top) * kWidth,
                frame.pixels.data() + static_cast<size_t>(y) * kWidth,
                static_cast<size_t>(kWidth) * 4);
  }
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%ls", gOutDir, name);
  WritePng(path, kWidth, static_cast<UINT32>(bottom - top), crop);
}

// A clean strip to the LEFT of the glyph: inside the button fill, away from the
// icon, the label, the mnemonic and the chevron. Used for the hover measurement
// (no focus ring is present in those two frames).
Box LeftFillStrip(const Rect& button) {
  return Box{button.left + 3, button.top + 20, button.left + 9, button.top + 40};
}

// The same clean strip on the RIGHT; safe when the left edge carries a focus
// ring (ring columns are left+1..left+3).
Box RightFillStrip(const Rect& button) {
  return Box{button.right - 10, button.top + 20, button.right - 4, button.top + 40};
}

UINT32 FillSample(const Frame& frame, const Rect& button) {
  const Box strip = RightFillStrip(button);
  return frame.At((strip.left + strip.right) / 2, (strip.top + strip.bottom) / 2);
}

// The first row of ink inside a box, measured against the island fill just
// outside it. Returns -1 when the box is empty.
int FirstInkRow(const Frame& frame, const Box& box, UINT32 fill) {
  for (int y = box.top - 2; y < box.bottom + 2; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      if (ColorDistance(frame.At(x, y), fill) > 40) return y;
    }
  }
  return -1;
}

// The ink cloud's vertical centre of mass. A 1 px sink moves EVERY ink pixel of
// a glyph by one row, so the centroid moves by ~1.0 even when the topmost row
// of a stroke is too antialiased to change; the first-ink-row shortcut alone
// is not a reliable "the whole button moved" measurement.
double InkCentroidY(const Frame& frame, const Box& box, UINT32 fill) {
  double weighted = 0.0;
  double weight = 0.0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      const double d = ColorDistance(frame.At(x, y), fill);
      if (d <= 40.0) continue;
      weighted += (static_cast<double>(y) + 0.5) * d;
      weight += d;
    }
  }
  return weight > 0.0 ? weighted / weight : -1.0;
}

int InkPixels(const Frame& frame, const Box& box, UINT32 fill, int threshold) {
  int count = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      if (ColorDistance(frame.At(x, y), fill) > threshold) ++count;
    }
  }
  return count;
}

// The alpha a white seam was painted with, recovered from the rendered pixels:
// out = fill * (1 - a) + white * a  =>  a = (out - fill) / (255 - fill).
double RecoveredWhiteAlpha(UINT32 out, UINT32 fill) {
  double sum = 0.0;
  int count = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int o = static_cast<int>((out >> shift) & 0xFFu);
    const int f = static_cast<int>((fill >> shift) & 0xFFu);
    if (255 - f < 8) continue;
    sum += static_cast<double>(o - f) / static_cast<double>(255 - f);
    ++count;
  }
  return count > 0 ? sum / count : 0.0;
}

Layout Band1280() {
  return prism::actionbar::Compute(
      prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                             /*sidebarVisible=*/false, /*listMode=*/false));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. geometry: content stack + group rhythm (pure integers, every lane shape)
// ---------------------------------------------------------------------------
PRISM_TEST("c78 geometry: 24/11/6 stack and 12-vs-4 group rhythm") {
  using namespace prism::actionbar;
  PRISM_CHECK_EQ(kIconBox, 24);
  PRISM_CHECK_EQ(kIconTop, 6);
  PRISM_CHECK_EQ(kLabelGap, 6);
  PRISM_CHECK_EQ(kLabelHeight, 14);
  PRISM_CHECK_EQ(kIconLabelOffset, 25);
  PRISM_CHECK_EQ(kGroupGap, 12);
  PRISM_CHECK_EQ(kButtonGap, 4);
  // 8 actions -> 7 gaps: 3 Group seams at 12 px + 4 intra gaps at 4 px.
  PRISM_CHECK_EQ(TotalGaps(), 4 * 4 + 3 * 12);

  const Layout band = Band1280();
  if (!PRISM_CHECK_EQ(static_cast<int>(band.buttons.size()), 8)) return;
  for (std::size_t i = 0; i < band.buttons.size(); ++i) {
    const Rect icon = IconBoxRect(band.buttons[i].rect);
    const Rect label = LabelBoxRect(band.buttons[i].rect);
    PRISM_CHECK(band.buttons[i].rect.Contains(icon));
    PRISM_CHECK(band.buttons[i].rect.Contains(label));
    PRISM_CHECK_EQ(icon.Width(), 24);
    PRISM_CHECK_EQ(icon.top - band.buttons[i].rect.top, 6);
    PRISM_CHECK_EQ(label.top - icon.bottom, 6);
    // 6 px of air below the label stack, measured from the drawn boxes.
    PRISM_CHECK_EQ(band.buttons[i].rect.bottom - label.bottom, 6);
    PRISM_CHECK_EQ(icon.left + icon.Width() / 2, label.left + label.Width() / 2);
    if (i > 0) {
      const int gap = band.buttons[i].rect.left - band.buttons[i - 1].rect.right;
      PRISM_CHECK_EQ(gap, GapBefore(static_cast<int>(i)));
    }
  }

  // Four groups -> exactly three seams, each a 1 px column centred in the
  // 12 px gap and inside the island's vertical padding.
  if (!PRISM_CHECK_EQ(static_cast<int>(band.separators.size()), 3)) return;
  const Group expected[3][2] = {{Group::Archive, Group::Modify},
                                {Group::Modify, Group::Inspect},
                                {Group::Inspect, Group::Find}};
  for (std::size_t s = 0; s < band.separators.size(); ++s) {
    const GroupSeparator& sep = band.separators[s];
    PRISM_CHECK_EQ(static_cast<int>(sep.before), static_cast<int>(expected[s][0]));
    PRISM_CHECK_EQ(static_cast<int>(sep.after), static_cast<int>(expected[s][1]));
    PRISM_CHECK_EQ(sep.rect.Width(), 1);
    PRISM_CHECK_EQ(sep.rect.top, band.band.top + kGroupSeparatorInset);
    PRISM_CHECK_EQ(sep.rect.bottom, band.band.bottom - kGroupSeparatorInset);
    PRISM_CHECK(band.island.Contains(sep.rect));
    // The seam sits between two adjacent buttons, in THEIR gap, not on a glyph.
    int owner = -1;
    for (std::size_t i = 1; i < band.buttons.size(); ++i) {
      if (sep.rect.left > band.buttons[i - 1].rect.right &&
          sep.rect.right <= band.buttons[i].rect.left) {
        owner = static_cast<int>(i);
      }
    }
    PRISM_CHECK(owner > 0);
  }
  // 640 px is still the floor: eight buttons, no drop, with the wider gaps.
  const Layout narrow = prism::actionbar::Compute(
      prism::layout::Compute(640, 800, false, false));
  PRISM_CHECK_EQ(static_cast<int>(narrow.buttons.size()), 8);
  PRISM_CHECK_EQ(narrow.dropped, 0);

  // Only the two table entries with a menu get a chevron box, and it lives in
  // the label's 6 px bottom margin.
  int menuActions = 0;
  for (int i = 0; i < kActionCount; ++i) {
    if (!SpecOf(i).hasDropdown) continue;
    ++menuActions;
    const Rect arrow = ArrowBoxRect(band.buttons[static_cast<std::size_t>(i)].rect);
    PRISM_CHECK(band.buttons[static_cast<std::size_t>(i)].rect.Contains(arrow));
    PRISM_CHECK_EQ(arrow.top, band.buttons[static_cast<std::size_t>(i)].rect.top + kLabelTop +
                                  kLabelHeight + 1);
  }
  PRISM_CHECK_EQ(menuActions, 2);
}

// ---------------------------------------------------------------------------
// 2. hover: same button, +3..8 levels, achromatic (never a blue ribbon)
// ---------------------------------------------------------------------------
PRISM_TEST("c78 hover lifts the same button by 3-8 levels, achromatically") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  const Layout band = Band1280();
  const int extract = IndexOf(ActionId::ExtractTo);
  const Rect& button = band.buttons[static_cast<std::size_t>(extract)].rect;

  Render rest, hover;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, -1, -1), &rest))) return;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, extract, -1), &hover))) return;

  const Box strip = LeftFillStrip(button);
  const double lifted = MeanLuminance(hover.frame, strip) - MeanLuminance(rest.frame, strip);
  std::printf("  [c78] hover luminance delta = %.2f\n", lifted);
  PRISM_CHECK(lifted >= 3.0);
  PRISM_CHECK(lifted <= 8.0);

  // Depth, measured: the raised face carries ONE specular row along its top
  // inner edge. It is brighter than the fill (a top light) but not a second
  // border (the row below returns to the fill).
  // Sample the rim over the LEFT shoulder: still on the straight top edge, but
  // clear of the glyph (which owns the centre column and starts at top+6).
  const int rimX = button.left + 16;
  const UINT32 rim = hover.frame.At(rimX, button.top + 3);
  const double rimLift = static_cast<double>(hover.frame.Luminance(rimX, button.top + 3)) -
                         MeanLuminance(hover.frame, strip);
  const double underRim =
      ColorDistance(hover.frame.At(rimX, button.top + 5), hover.frame.At(rimX, button.top + 8));
  std::printf("  [c78] top rim lift = %.1f, row+2 vs fill = %.1f\n", rimLift, underRim);
  PRISM_CHECK(rimLift >= 10.0);
  PRISM_CHECK(rimLift <= 32.0);
  PRISM_CHECK(underRim < 4.0);
  PRISM_CHECK(ColorDistance(rim, rest.frame.At(rimX, button.top + 3)) > 8);

  // Achromatic: the three channels move together (the wash is token-white).
  double delta[3] = {0.0, 0.0, 0.0};
  int count = 0;
  for (int y = strip.top; y < strip.bottom; ++y) {
    for (int x = strip.left; x < strip.right; ++x) {
      for (int c = 0; c < 3; ++c) {
        delta[c] += hover.frame.Channel(x, y, c * 8) - rest.frame.Channel(x, y, c * 8);
      }
      ++count;
    }
  }
  for (double& d : delta) d /= (count > 0 ? count : 1);
  PRISM_CHECK(delta[0] > 2.0);
  PRISM_CHECK(std::abs(delta[2] - delta[0]) < 3.0);
  PRISM_CHECK(std::abs(delta[1] - delta[0]) < 3.0);

  // Locality: the neighbouring Group seam and the button next door do not move.
  const Rect& neighbour = band.buttons[0].rect;
  double neighbourDelta = 0.0;
  int neighbourCount = 0;
  for (int y = neighbour.top; y < neighbour.bottom; ++y) {
    for (int x = neighbour.left; x < neighbour.right; ++x) {
      neighbourDelta += std::abs(hover.frame.Luminance(x, y) - rest.frame.Luminance(x, y));
      ++neighbourCount;
    }
  }
  PRISM_CHECK(neighbourDelta / (neighbourCount > 0 ? neighbourCount : 1) < 0.5);
  SaveBand(L"c78-rest-band.png", rest.frame);
  SaveBand(L"c78-hover-band.png", hover.frame);
}

// ---------------------------------------------------------------------------
// 3. press: the content (icon AND label) sinks exactly one pixel
// ---------------------------------------------------------------------------
PRISM_TEST("c78 press sinks the content by one pixel, twice measured") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  const Layout band = Band1280();
  const int extract = IndexOf(ActionId::ExtractTo);
  const Rect& button = band.buttons[static_cast<std::size_t>(extract)].rect;

  Render hover, pressed;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, extract, -1), &hover))) return;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, extract, extract), &pressed))) return;

  const UINT32 fill = FillSample(hover.frame, button);
  const Box icon = BoxOf(prism::actionbar::IconBoxRect(button));
  const Box label = BoxOf(prism::actionbar::LabelBoxRect(button));
  const double hoverIconY = InkCentroidY(hover.frame, icon, fill);
  const double pressedIconY = InkCentroidY(pressed.frame, icon, fill);
  const double hoverLabelY = InkCentroidY(hover.frame, label, fill);
  const double pressedLabelY = InkCentroidY(pressed.frame, label, fill);
  const int hoverLabelTop = FirstInkRow(hover.frame, label, fill);
  const int pressedLabelTop = FirstInkRow(pressed.frame, label, fill);
  std::printf("  [c78] sink icon %.2f->%.2f label %.2f->%.2f (top row %d->%d)\n", hoverIconY,
              pressedIconY, hoverLabelY, pressedLabelY, hoverLabelTop, pressedLabelTop);
  PRISM_CHECK(hoverIconY > 0.0);
  PRISM_CHECK(hoverLabelY > 0.0);
  // "The whole button moved down one pixel": both ink clouds translate by 1.
  PRISM_CHECK(std::abs((pressedIconY - hoverIconY) - 1.0) < 0.25);
  PRISM_CHECK(std::abs((pressedLabelY - hoverLabelY) - 1.0) < 0.25);
  PRISM_CHECK_EQ(pressedLabelTop, hoverLabelTop + 1);

  // The pressed wash is deeper than hover but the BUTTON's rectangle did not
  // move: its clean right strip is brighter, and still achromatic.
  const double hoverFill = MeanLuminance(hover.frame, RightFillStrip(button));
  const double pressedFill = MeanLuminance(pressed.frame, RightFillStrip(button));
  PRISM_CHECK(pressedFill > hoverFill);
  PRISM_CHECK(pressedFill - hoverFill < 10.0);
  SaveBand(L"c78-pressed-band.png", pressed.frame);
}

// ---------------------------------------------------------------------------
// 4. Group seams: alpha 0.06..0.10 in ALL THREE themes; no seam inside a group
// ---------------------------------------------------------------------------
PRISM_TEST("c78 group seams measure alpha 0.06-0.10 on every theme") {
  const prism::theme::Variant themes[] = {prism::theme::Variant::Midnight,
                                          prism::theme::Variant::Daylight,
                                          prism::theme::Variant::HighContrast};
  const Layout band = Band1280();
  if (!PRISM_CHECK_EQ(static_cast<int>(band.separators.size()), 3)) return;
  for (const prism::theme::Variant variant : themes) {
    prism::actionbar::Model model;
    model.SetContext(Context{true, true, false});
    Render rest;
    if (!PRISM_CHECK(RenderScene(BaseScene(&model, -1, -1, variant), &rest))) return;
    const int y = (band.island.top + band.island.bottom) / 2;
    for (const prism::actionbar::GroupSeparator& sep : band.separators) {
      const UINT32 seam = rest.frame.At(sep.rect.left, y);
      const UINT32 fill = rest.frame.At(sep.rect.left - 2, y);
      const double alpha = RecoveredWhiteAlpha(seam, fill);
      std::printf("  [c78] %s seam x=%d alpha=%.3f\n",
                  std::string(prism::theme::VariantName(variant)).c_str(), sep.rect.left,
                  alpha);
      PRISM_CHECK(alpha >= 0.06);
      PRISM_CHECK(alpha <= 0.10);
    }
    // Inside the first Group the 4 px gap carries NO seam: its centre pixel is
    // the island fill, not a hairline.
    const int intra = band.buttons[0].rect.right + 2;
    const UINT32 gapPixel = rest.frame.At(intra, y);
    const UINT32 gapFill = rest.frame.At(intra - 1, y);
    PRISM_CHECK(RecoveredWhiteAlpha(gapPixel, gapFill) < 0.02);
  }
}

// ---------------------------------------------------------------------------
// 5. disabled: 38% ink, and the LABEL measures below 3:1
// ---------------------------------------------------------------------------
PRISM_TEST("c78 disabled label contrast is below 3:1") {
  prism::actionbar::Model disabled;
  disabled.SetContext(Context{false, false, false});
  prism::actionbar::Model enabled;
  enabled.SetContext(Context{true, true, false});
  const Layout band = Band1280();
  const int extract = IndexOf(ActionId::ExtractTo);
  const Rect& button = band.buttons[static_cast<std::size_t>(extract)].rect;

  Render off, on;
  if (!PRISM_CHECK(RenderScene(BaseScene(&disabled, -1, -1), &off))) return;
  if (!PRISM_CHECK(RenderScene(BaseScene(&enabled, -1, -1), &on))) return;
  PRISM_CHECK(!disabled.Enabled(ActionId::ExtractTo));

  const Box label = BoxOf(prism::actionbar::LabelBoxRect(button));
  const UINT32 offFill = FillSample(off.frame, button);
  const UINT32 onFill = FillSample(on.frame, button);
  double offMax = 0.0;
  double onMax = 0.0;
  for (int y = label.top; y < label.bottom; ++y) {
    for (int x = label.left; x < label.right; ++x) {
      if (!off.frame.In(x, y)) continue;
      offMax = (std::max)(offMax, ContrastRatio(off.frame.At(x, y), offFill));
      onMax = (std::max)(onMax, ContrastRatio(on.frame.At(x, y), onFill));
    }
  }
  std::printf("  [c78] label contrast enabled=%.2f disabled=%.2f\n", onMax, offMax);
  PRISM_CHECK(onMax > 4.5);   // the enabled word clears AA comfortably
  PRISM_CHECK(offMax > 1.4);  // ... and the disabled one still EXISTS
  PRISM_CHECK(offMax < 3.0);  // ... but never pretends to be actionable
}

// ---------------------------------------------------------------------------
// 6. keyboard focus: a 2 px accent ring, visible and the accent colour
// ---------------------------------------------------------------------------
PRISM_TEST("c78 focus ring is 2 px of the accent token") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  const Layout band = Band1280();
  const int extract = IndexOf(ActionId::ExtractTo);
  model.SetFocusIndex(extract);
  const Rect& button = band.buttons[static_cast<std::size_t>(extract)].rect;

  Render focus;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, -1, -1), &focus))) return;
  const UINT32 fill = FillSample(focus.frame, button);
  const int midY = (button.top + button.bottom) / 2;

  const prism::theme::Rgba accent =
      prism::theme::EffectiveAccent(prism::theme::Variant::Midnight);
  const auto composite = [&](int shift) {
    const double a = 0.90;
    const double f = static_cast<double>((fill >> shift) & 0xFFu);
    const double c = (shift == 16 ? accent.r : (shift == 8 ? accent.g : accent.b)) * 255.0;
    return static_cast<int>(std::lround(c * a + f * (1.0 - a)));
  };
  const UINT32 expected =
      (static_cast<UINT32>(composite(16)) << 16) | (static_cast<UINT32>(composite(8)) << 8) |
      static_cast<UINT32>(composite(0));
  const int d1 = ColorDistance(focus.frame.At(button.left + 1, midY), expected);
  const int d2 = ColorDistance(focus.frame.At(button.left + 2, midY), expected);
  const int outside = ColorDistance(focus.frame.At(button.left + 4, midY), expected);
  std::printf("  [c78] focus ring distance d1=%d d2=%d outside=%d\n", d1, d2, outside);
  PRISM_CHECK(d1 < 20);
  PRISM_CHECK(d2 < 20);
  PRISM_CHECK(outside > 30);
  // No ring on a different button (the ring follows FocusIndex, not the paint).
  const Rect& neighbour = band.buttons[0].rect;
  PRISM_CHECK(ColorDistance(focus.frame.At(neighbour.left + 2, midY),
                            FillSample(focus.frame, neighbour)) < 15);
  SaveBand(L"c78-focus-band.png", focus.frame);
}

// ---------------------------------------------------------------------------
// 7. chevron: menu actions only, hover only, and it is not a permanent glyph
// ---------------------------------------------------------------------------
PRISM_TEST("c78 chevron appears only for a hovered menu action") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  const Layout band = Band1280();
  const int extract = IndexOf(ActionId::ExtractTo);
  const int open = IndexOf(ActionId::Open);
  const Rect& menuButton = band.buttons[static_cast<std::size_t>(extract)].rect;
  const Rect& plainButton = band.buttons[static_cast<std::size_t>(open)].rect;

  Render rest, menuHover, plainHover;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, -1, -1), &rest))) return;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, extract, -1), &menuHover))) return;
  if (!PRISM_CHECK(RenderScene(BaseScene(&model, open, -1), &plainHover))) return;

  const Box restArrow = BoxOf(prism::actionbar::ArrowBoxRect(menuButton));
  const Box hoverArrow = restArrow;
  const Box plainArrow = BoxOf(prism::actionbar::ArrowBoxRect(plainButton));
  const int restingInk = InkPixels(rest.frame, restArrow, FillSample(rest.frame, menuButton), 25);
  const int hoverInk =
      InkPixels(menuHover.frame, hoverArrow, FillSample(menuHover.frame, menuButton), 25);
  const int plainInk =
      InkPixels(plainHover.frame, plainArrow, FillSample(plainHover.frame, plainButton), 25);
  std::printf("  [c78] chevron ink rest=%d hover=%d plainHover=%d\n", restingInk, hoverInk,
              plainInk);
  PRISM_CHECK_EQ(restingInk, 0);   // no permanent affordance
  PRISM_CHECK(hoverInk >= 5);      // it really paints on hover
  PRISM_CHECK_EQ(plainInk, 0);     // Open has no menu: no chevron, ever
  // The two menu actions are exactly the table's hasDropdown entries.
  PRISM_CHECK(prism::actionbar::SpecOf(IndexOf(ActionId::Compress)).hasDropdown);
}

int main(int argc, char** argv) {
  gOutDir = argc > 1 ? argv[1] : nullptr;
  if (gOutDir != nullptr) std::filesystem::create_directories(gOutDir);
  if (kMutantBanner != nullptr) std::printf("%s\n", kMutantBanner);
  if (kMutantMarker != nullptr) prism::test::SetMutationVerdict(kMutantMarker);

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed");
    return 1;
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory,
                              reinterpret_cast<void**>(&gWic)))) {
    std::puts("FAIL: WIC factory");
    return 1;
  }
  const int result = prism::test::RunAll(argc, argv);
  if (result != 0) {
    std::puts("ACTION-ISLAND CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C78 ACTION ISLAND CHECKS PASSED");
  return 0;
}
