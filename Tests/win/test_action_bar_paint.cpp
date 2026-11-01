// PrismArchive -- PrismWin
// V2: PIXEL + HWND tests for the action band (DESIGN.md §2/§3/§6).
//
// The model suite (Tests/test_action_bar_model.cpp) proves the table, the
// enable rules, the mnemonics and the geometry are correct as NUMBERS. This
// suite proves the two things only Windows can:
//
//   * the numbers reach the DEVICE and stay inside layout.action: the band
//     paints between y=32 and y=88 and nowhere else;
//   * the four visual states are real, measured colour differences -- hover
//     LIFTS a local fill (achromatic: no Bandizip-blue ribbon), disabled ink is
//     40% of enabled ink (measured, not asserted by comment), pressing sinks
//     the CONTENT by exactly one pixel;
//   * icons render inside their 20 px box and never bleed into the button;
//   * a dropdown hangs below its button and paints its rows;
//   * on a REAL HWND, WM_SYSKEYDOWN (the only door an Alt chord uses) runs the
//     action the mnemonic names -- counted by the app's own handler -- and a
//     DISABLED action's chord runs nothing.
//
// The stdout marker is a CONJUNCTION with the exit code (CONTRACTS D1). The
// mutants are registered WILL_FAIL without a PASS regex (D26) and print their
// name so the log proves the injection compiled in (D3).
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
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using Microsoft::WRL::ComPtr;
using prism::actionbar::ActionId;
using prism::actionbar::Context;
using prism::actionbar::Layout;
using prism::actionbar::Model;
using prism::actionbar::Rect;

namespace {

constexpr UINT32 kWidth = 1280;
constexpr UINT32 kHeight = 800;

IWICImagingFactory* gWic = nullptr;

// The band is a function of V1's window layout, exactly as the window builds it
// for a 1280x800 grid-mode client (rail folded: the grid keeps full bleed).
prism::layout::Layout WindowLayout() {
  return prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                /*sidebarVisible=*/false, /*listMode=*/false);
}

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

int ColorDistance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

struct Box {
  int left = 0, top = 0, right = 0, bottom = 0;
};

double MeanDiff(const Frame& a, const Frame& b, const Box& box) {
  double sum = 0.0;
  int count = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!a.In(x, y) || !b.In(x, y)) continue;
      for (int shift = 0; shift <= 16; shift += 8) {
        sum += std::abs(a.Channel(x, y, shift) - b.Channel(x, y, shift));
      }
      ++count;
    }
  }
  return count > 0 ? sum / (count * 3.0) : 0.0;
}

// Ink energy: how far the pixels of a box are from a flat fill colour. Used to
// compare a enabled icon with the same icon painted disabled (40%).
double InkEnergy(const Frame& frame, const Box& box, UINT32 fill) {
  double sum = 0.0;
  int count = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      for (int shift = 0; shift <= 16; shift += 8) {
        sum += std::abs(frame.Channel(x, y, shift) -
                        static_cast<int>((fill >> shift) & 0xFFu));
      }
      ++count;
    }
  }
  return count > 0 ? sum / (count * 3.0) : 0.0;
}

struct Counters {
  int buttons = 0;
  int icons = 0;
  int disabled = 0;
  int hover = 0;
  int pressed = 0;
  int dropdowns = 0;
};

bool RenderScene(IWICImagingFactory* wic, const prism::win::gallery::GalleryRenderer::Scene& scene,
                 UINT32 width, UINT32 height, Frame* frame, Counters* counters) {
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
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
  if (counters) {
    counters->buttons = renderer.dbgActionButtons;
    counters->icons = renderer.dbgActionIcons;
    counters->disabled = renderer.dbgActionDisabled;
    counters->hover = renderer.dbgActionHover;
    counters->pressed = renderer.dbgActionPressed;
    counters->dropdowns = renderer.dbgActionDropdowns;
  }
  return true;
}

// The icon box of a button, exactly as ActionBar.hpp lays it out.
Box IconBox(const Rect& button) {
  const int centre = (button.left + button.right) / 2;
  return Box{centre - 10, button.top + 9, centre + 10, button.top + 29};
}

// A flat patch of the button that carries no glyph: where the island fill shows.
Box FillPatch(const Rect& button) {
  return Box{button.left + 4, button.top + 4, button.left + 12, button.top + 8};
}

UINT32 FillSample(const Frame& frame, const Rect& button) {
  const Box patch = FillPatch(button);
  return frame.At((patch.left + patch.right) / 2, (patch.top + patch.bottom) / 2);
}

prism::win::gallery::GalleryRenderer::Scene BaseScene(const Model* model, int hover, int pressed) {
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.actions = model;
  scene.actionHover = hover;
  scene.actionPressed = pressed;
  scene.breadcrumb = L"Design_Assets_v4.7z";
  return scene;
}

}  // namespace

PRISM_TEST("the band paints inside layout.action and nowhere else") {
  Model model;
  model.SetContext(Context{true, true, false});
  const Layout layout = prism::actionbar::Compute(WindowLayout());
  PRISM_CHECK_EQ(layout.band.top, 32);
  PRISM_CHECK_EQ(layout.band.bottom, 88);

  Frame frame;
  Counters counters;
  const auto scene = BaseScene(&model, -1, -1);
  if (!PRISM_CHECK(RenderScene(gWic, scene, kWidth, kHeight, &frame, &counters))) return;

  // The band really reached the device: eight buttons, eight icons.
  PRISM_CHECK_EQ(counters.buttons, 8);
  PRISM_CHECK_EQ(counters.icons, 8);

  // The title bar (above) is pure backdrop at a button's x...
  const int probeX = (layout.buttons[1].rect.left + layout.buttons[1].rect.right) / 2;
  const UINT32 backdrop = frame.At(4, 4);
  PRISM_CHECK(ColorDistance(frame.At(probeX, layout.band.top - 1), backdrop) < 6);
  PRISM_CHECK(ColorDistance(frame.At(probeX, layout.band.top - 2), backdrop) < 6);
  // ... the band itself is not backdrop ...
  PRISM_CHECK(ColorDistance(frame.At(probeX, layout.band.top + 4), backdrop) > 8);
  // ... and below V1's 1 px band hairline the content is backdrop again at the
  // menu side (the dropdown is closed, so nothing hangs there).
  PRISM_CHECK(ColorDistance(frame.At(layout.buttons[0].rect.left + 2, layout.band.bottom + 2),
                            backdrop) < 6);

  // The island spans the buttons and stops before the path pill; the gap
  // between them is V1's band tint (and nothing else).
  const UINT32 gap = frame.At(layout.navPill.right + 6, 60);
  PRISM_CHECK(frame.At(layout.buttons[0].rect.left + 3, 60) != gap);
  PRISM_CHECK(frame.At(layout.island.right - 2, 60) != gap);
  PRISM_CHECK(ColorDistance(frame.At(layout.island.right + 4, 60), gap) < 4);
  // The nav pill is a real pill in the same row (it is what shares the 56 px).
  PRISM_CHECK(frame.At((layout.navPill.left + layout.navPill.right) / 2, 60) != gap);
}

PRISM_TEST("hover lifts a local fill without turning blue") {
  Model model;
  model.SetContext(Context{true, true, false});
  const Layout layout = prism::actionbar::Compute(WindowLayout());
  Frame rest, hover;
  Counters restCounters, hoverCounters;
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, -1, -1), kWidth, kHeight, &rest,
                              &restCounters))) {
    return;
  }
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, 1, -1), kWidth, kHeight, &hover,
                              &hoverCounters))) {
    return;
  }
  PRISM_CHECK_EQ(restCounters.hover, 0);
  PRISM_CHECK_EQ(hoverCounters.hover, 1);

  const Rect& button = layout.buttons[1].rect;
  const Box whole{button.left, button.top, button.right, button.bottom};
  PRISM_CHECK(MeanDiff(rest, hover, whole) > 3.0);

  // Locality: the neighbour and the path pill did not change.
  const Rect& neighbour = layout.buttons[0].rect;
  PRISM_CHECK(MeanDiff(rest, hover, Box{neighbour.left, neighbour.top, neighbour.right,
                                        neighbour.bottom}) < 0.5);
  PRISM_CHECK(MeanDiff(rest, hover,
                       Box{layout.navPill.left, layout.navPill.top, layout.navPill.right,
                           layout.navPill.bottom}) < 0.5);

  // The wash is ACHROMATIC: every channel rises by about the same amount. A
  // Bandizip-style blue ribbon would push B far above R.
  const Box patch = FillPatch(button);
  double delta[3] = {0.0, 0.0, 0.0};
  int count = 0;
  for (int y = patch.top; y < patch.bottom; ++y) {
    for (int x = patch.left; x < patch.right; ++x) {
      for (int c = 0; c < 3; ++c) {
        delta[c] += hover.Channel(x, y, c * 8) - rest.Channel(x, y, c * 8);
      }
      ++count;
    }
  }
  for (double& d : delta) d /= (count > 0 ? count : 1);
  PRISM_CHECK(delta[0] > 2.0);                       // it really brightened
  PRISM_CHECK(std::abs(delta[2] - delta[0]) < 3.0);  // ... without a hue shift
  PRISM_CHECK(std::abs(delta[1] - delta[0]) < 3.0);
}

PRISM_TEST("a disabled action is 40% ink and ignores the pointer") {
  Model model;
  model.SetContext(Context{false, false, false});  // no archive: extract is illegal
  const Layout layout = prism::actionbar::Compute(WindowLayout());
  const int openIndex = prism::actionbar::IndexOf(ActionId::Open);
  const int extractIndex = prism::actionbar::IndexOf(ActionId::ExtractTo);
  PRISM_CHECK(model.Enabled(ActionId::Open));
  PRISM_CHECK(!model.Enabled(ActionId::ExtractTo));

  Model enabledModel;
  enabledModel.SetContext(Context{true, true, false});
  Frame enabledFrame, frame, hoveredDisabled;
  Counters counters;
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&enabledModel, -1, -1), kWidth, kHeight,
                              &enabledFrame, nullptr))) {
    return;
  }
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, -1, -1), kWidth, kHeight, &frame,
                              &counters))) {
    return;
  }
  PRISM_CHECK(counters.disabled >= 3);  // extract/test/view/search at least
  PRISM_CHECK_EQ(openIndex, prism::actionbar::IndexOf(ActionId::Open));

  // The SAME glyph in the same place, once enabled and once disabled: comparing
  // two different icons would be comparing their shapes, not their opacity.
  const Rect& button = layout.buttons[static_cast<size_t>(extractIndex)].rect;
  const double enabledInk =
      InkEnergy(enabledFrame, IconBox(button), FillSample(enabledFrame, button));
  const double disabledInk = InkEnergy(frame, IconBox(button), FillSample(frame, button));
  PRISM_CHECK(enabledInk > 20.0);   // the enabled icon is visible at all
  PRISM_CHECK(disabledInk > 5.0);   // the disabled one is dim, not invisible
  const double ratio = disabledInk / (enabledInk > 0.0 ? enabledInk : 1.0);
  PRISM_CHECK(ratio > 0.30);
  PRISM_CHECK(ratio < 0.52);  // 0.40 with room for antialiasing

  // Hovering a disabled button must change NOTHING (no false affordance).
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, extractIndex, -1), kWidth, kHeight,
                              &hoveredDisabled, nullptr))) {
    return;
  }
  PRISM_CHECK(MeanDiff(frame, hoveredDisabled,
                       Box{button.left, button.top, button.right, button.bottom}) < 0.5);
}

PRISM_TEST("pressing sinks the content by exactly one pixel") {
  Model model;
  model.SetContext(Context{true, true, false});
  const Layout layout = prism::actionbar::Compute(WindowLayout());
  const int index = prism::actionbar::IndexOf(ActionId::ExtractTo);
  const Rect& button = layout.buttons[static_cast<size_t>(index)].rect;

  Frame hover, pressed;
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, index, -1), kWidth, kHeight, &hover,
                              nullptr))) {
    return;
  }
  Counters counters;
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, index, index), kWidth, kHeight,
                              &pressed, &counters))) {
    return;
  }
  PRISM_CHECK_EQ(counters.pressed, 1);
  PRISM_CHECK(MeanDiff(hover, pressed,
                       Box{button.left, button.top, button.right, button.bottom}) > 1.0);

  // The topmost ink row of the icon: exactly one pixel lower while pressed.
  const Box icon = IconBox(button);
  auto firstInkRow = [](const Frame& f, const Box& box) {
    const UINT32 fill = f.At(box.left - 2, box.top - 1);
    for (int y = box.top - 2; y < box.bottom + 2; ++y) {
      for (int x = box.left; x < box.right; ++x) {
        if (!f.In(x, y)) continue;
        if (ColorDistance(f.At(x, y), fill) > 40) return y;
      }
    }
    return -1;
  };
  const int hoverTop = firstInkRow(hover, icon);
  const int pressedTop = firstInkRow(pressed, icon);
  PRISM_CHECK(hoverTop > 0);
  PRISM_CHECK_EQ(pressedTop, hoverTop + 1);
}

PRISM_TEST("icons stay inside their 20 px box") {
  Model model;
  model.SetContext(Context{true, true, false});
  const Layout layout = prism::actionbar::Compute(WindowLayout());
  Frame frame;
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, -1, -1), kWidth, kHeight, &frame,
                              nullptr))) {
    return;
  }
  for (const prism::actionbar::Button& button : layout.buttons) {
    const Box icon = IconBox(button.rect);
    const UINT32 fill = FillSample(frame, button.rect);
    // The icon really paints (a blank icon would pass every clipping check).
    int ink = 0;
    for (int y = icon.top; y < icon.bottom; ++y) {
      for (int x = icon.left; x < icon.right; ++x) {
        if (ColorDistance(frame.At(x, y), fill) > 40) ++ink;
      }
    }
    PRISM_CHECK(ink > 12);
    // Left margin and the rows just above/below the box are untouched: nothing
    // is clipped by the button edge and nothing bleeds over the label.
    const Box leftMargin{icon.left - 5, icon.top, icon.left - 1, icon.bottom};
    const Box topMargin{icon.left, icon.top - 3, icon.right, icon.top - 1};
    for (int y = leftMargin.top; y < leftMargin.bottom; ++y) {
      for (int x = leftMargin.left; x < leftMargin.right; ++x) {
        if (!frame.In(x, y)) continue;
        PRISM_CHECK(ColorDistance(frame.At(x, y), fill) < 6);
      }
    }
    for (int y = topMargin.top; y < topMargin.bottom; ++y) {
      for (int x = topMargin.left; x < topMargin.right; ++x) {
        if (!frame.In(x, y)) continue;
        PRISM_CHECK(ColorDistance(frame.At(x, y), fill) < 6);
      }
    }
  }
}

PRISM_TEST("the dropdown hangs below its button and paints its rows") {
  Model model;
  model.SetContext(Context{true, true, false});
  model.SetFocusIndex(prism::actionbar::IndexOf(ActionId::ExtractTo));
  PRISM_CHECK_EQ(model.PressDown().kind, prism::actionbar::Effect::Kind::OpenDropdown);

  const Layout layout = prism::actionbar::Compute(WindowLayout());
  Frame frame;
  Counters counters;
  if (!PRISM_CHECK(RenderScene(gWic, BaseScene(&model, -1, -1), kWidth, kHeight, &frame,
                              &counters))) {
    return;
  }
  PRISM_CHECK_EQ(counters.dropdowns, 1);

  const int owner = prism::actionbar::IndexOf(ActionId::ExtractTo);
  const Rect panel = prism::actionbar::DropdownRect(layout, owner);
  const Rect& button = layout.buttons[static_cast<size_t>(owner)].rect;
  PRISM_CHECK(panel.top >= button.bottom);
  // The panel is real pixels below the band, and its three rows are painted.
  PRISM_CHECK(frame.At((panel.left + panel.right) / 2, panel.top + 2) != frame.At(4, 4));
  for (int row = 0; row < 3; ++row) {
    const int y = panel.top + prism::actionbar::kDropdownPad +
                  row * prism::actionbar::kDropdownRowHeight +
                  prism::actionbar::kDropdownRowHeight / 2;
    int ink = 0;
    for (int x = panel.left + 8; x < panel.right - 8; ++x) {
      if (frame.In(x, y) && frame.Luminance(x, y) > 90) ++ink;
    }
    PRISM_CHECK(ink > 4);  // the row label exists
  }
}

// ---------------------------------------------------------------- real HWND
namespace {

std::vector<prism::ArchiveItem> BuildCatalog() {
  std::vector<prism::ArchiveItem> items;
  auto add = [&items](const char* path, bool dir, std::uint64_t size) {
    prism::ArchiveItem item;
    item.index = static_cast<std::uint32_t>(items.size());
    item.path = path;
    item.isDir = dir;
    item.size = size;
    items.push_back(item);
  };
  add("docs", true, 0);
  add("pack", true, 0);
  add("root.png", false, 500);
  return items;
}

}  // namespace

PRISM_TEST("Alt mnemonics run the app's command counter on a real HWND") {
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive action bar test", GetModuleHandleW(nullptr)))) {
    std::puts("FAIL: window creation failed (no interactive window station)");
    return;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"actions.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(kWidth, kHeight));

  int counts[static_cast<int>(ActionId::Count)] = {};
  int lastItem = -2;
  window.SetActionHandler([&](ActionId action, int item) {
    ++counts[static_cast<int>(action)];
    lastItem = item;
  });

  auto chord = [&window](char letter) {
    window.Dispatch(WM_SYSKEYDOWN, static_cast<WPARAM>(letter), 0);
  };

  // Archive open, nothing selected.
  PRISM_CHECK(window.ActionContext().archiveOpen);
  PRISM_CHECK(!window.ActionContext().hasSelection);

  chord('O');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Open)], 1);
  chord('E');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::ExtractTo)], 1);
  // Disabled while nothing is selected: the chord is claimed, nothing runs.
  chord('V');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::QuickLook)], 0);
  chord('D');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Delete)], 0);
  // The ring still moved to the disabled button (feedback: "it exists").
  PRISM_CHECK_EQ(window.ActionBarFocus(), prism::actionbar::IndexOf(ActionId::Delete));

  // Select a row: the destructive and the preview actions become legal.
  window.SetSelection(2);
  PRISM_CHECK(window.ActionContext().hasSelection);
  chord('V');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::QuickLook)], 1);
  chord('D');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Delete)], 1);

  // A read-only archive turns Add/Delete off again, before confirmation.
  window.SetArchiveReadOnly(true);
  PRISM_CHECK(window.ActionContext().readOnly);
  chord('D');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Delete)], 1);
  chord('A');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Add)], 0);
  // The non-destructive actions are untouched by read-only.
  chord('T');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Test)], 1);
  window.SetArchiveReadOnly(false);

  // An unclaimed letter runs nothing at all.
  const int before = counts[static_cast<int>(ActionId::Open)];
  chord('Q');
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Open)], before);
}

PRISM_TEST("Tab / Enter / arrows drive the band through the real WndProc") {
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive action keys", GetModuleHandleW(nullptr)))) {
    return;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"actions.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(kWidth, kHeight));
  window.SetSelection(2);

  int counts[static_cast<int>(ActionId::Count)] = {};
  int lastItem = -2;
  window.SetActionHandler([&](ActionId action, int item) {
    ++counts[static_cast<int>(action)];
    lastItem = item;
  });
  auto key = [&window](UINT vk) { window.Dispatch(WM_KEYDOWN, static_cast<WPARAM>(vk), 0); };

  PRISM_CHECK_EQ(window.ActionBarFocus(), -1);
  key(VK_TAB);
  PRISM_CHECK_EQ(window.ActionBarFocus(), prism::actionbar::IndexOf(ActionId::Open));
  key(VK_TAB);
  PRISM_CHECK_EQ(window.ActionBarFocus(), prism::actionbar::IndexOf(ActionId::ExtractTo));

  // Enter on a dropdown action opens the MENU first (Bandizip), no command yet.
  key(VK_RETURN);
  PRISM_CHECK(window.ActionBarState().DropdownOpen());
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::ExtractTo)], 0);
  key(VK_DOWN);
  PRISM_CHECK_EQ(window.ActionBarState().DropdownIndex(), 1);
  key(VK_RETURN);
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::ExtractTo)], 1);
  PRISM_CHECK_EQ(lastItem, 1);  // the SECOND menu row, not the default variant
  PRISM_CHECK(!window.ActionBarState().DropdownOpen());

  // Escape drops the ring; from then on arrows belong to the grid again.
  key(VK_ESCAPE);
  PRISM_CHECK_EQ(window.ActionBarFocus(), -1);
  const int selection = window.Selection();
  key(VK_LEFT);
  PRISM_CHECK(window.Selection() != selection);  // the grid moved, not the band
}

PRISM_TEST("a click on a button runs it; the island swallows the rest") {
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive action click", GetModuleHandleW(nullptr)))) {
    return;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"actions.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(kWidth, kHeight));
  window.SetSelection(2);

  int counts[static_cast<int>(ActionId::Count)] = {};
  window.SetActionHandler(
      [&](ActionId action, int) { ++counts[static_cast<int>(action)]; });

  const Layout layout = window.ActionBarLayout();
  const int searchIndex = prism::actionbar::IndexOf(ActionId::Search);
  const Rect& search = layout.buttons[static_cast<size_t>(searchIndex)].rect;
  const int cx = (search.left + search.right) / 2;
  const int cy = (search.top + search.bottom) / 2;
  const int selectionBefore = window.Selection();
  window.Dispatch(WM_LBUTTONDOWN, 0, MAKELPARAM(cx, cy));
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Search)], 1);
  PRISM_CHECK_EQ(window.Selection(), selectionBefore);  // the band is chrome

  // A click on the island's padding is consumed and runs nothing.
  window.Dispatch(WM_LBUTTONDOWN, 0, MAKELPARAM(layout.island.left + 2, cy));
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::Search)], 1);
  PRISM_CHECK_EQ(window.Selection(), selectionBefore);

  // The arrow zone of a dropdown action opens the menu instead of running it.
  const int extractIndex = prism::actionbar::IndexOf(ActionId::ExtractTo);
  const Rect& extract = layout.buttons[static_cast<size_t>(extractIndex)].rect;
  window.Dispatch(WM_LBUTTONDOWN, 0,
                  MAKELPARAM(extract.right - 4, (extract.top + extract.bottom) / 2));
  PRISM_CHECK(window.ActionBarState().DropdownOpen());
  PRISM_CHECK_EQ(window.ActionBarState().DropdownOwner(), ActionId::ExtractTo);
  PRISM_CHECK_EQ(counts[static_cast<int>(ActionId::ExtractTo)], 0);
}

int main() {
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
  if (!prism::actionbar::ActiveMutation().empty()) {
    std::printf("V2 MUTANT ACTIVE: %.*s\n",
                static_cast<int>(prism::actionbar::ActiveMutation().size()),
                prism::actionbar::ActiveMutation().data());
  }
  if (!prism::win::actionbar::PaintMutation().empty()) {
    std::printf("V2 PAINT MUTANT ACTIVE: %.*s\n",
                static_cast<int>(prism::win::actionbar::PaintMutation().size()),
                prism::win::actionbar::PaintMutation().data());
  }
  const int result = prism::test::RunAll(0, nullptr);
  if (result != 0) {
    std::puts("ACTION-BAR PAINT CHECKS FAILED");
    return 1;
  }
  std::puts("ALL ACTION-BAR PAINT CHECKS PASSED");
  return 0;
}
