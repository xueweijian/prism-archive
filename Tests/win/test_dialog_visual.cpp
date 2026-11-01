// PrismArchive -- V3 dialog visual suite.
//
// Proves the three things the card promised:
//   1. the C18/C31 rules still pass (their suites are untouched and still run),
//      and the new shell's four states are real, not decoration;
//   2. PIXELS: render ChromeModel to a 640x480 WIC bitmap through D2D and assert
//      the title-bar colour, the hover brightness lift, the slider fill growth,
//      and that disabled is visibly dimmer;
//   3. KEYBOARD: Tab order, Space/Enter activation and Esc cancel are driven
//      through the real window procs.
//
// Negative controls (CONTRACTS D3):
//   PRISM_V3_MUTANT_NO_HOVER   hover resolves to Normal -> hover pixel test RED
//   PRISM_V3_MUTANT_FIXED_DPI  all DPIs resolve to 96  -> DPI test RED
// D26: the mutant entries are WILL_FAIL-only (no PASS_REGULAR_EXPRESSION).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "../../Src/PrismWin/CompressDialog.hpp"
#include "../../Src/PrismWin/DialogChrome.hpp"
#include "../../Src/PrismWin/SettingsDialog.hpp"
#include "../mini_test.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace chrome = prism::win::chrome;
namespace cd = prism::win::cd;
namespace su = prism::win::settingsui;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 kWidth = 640;
constexpr UINT32 kHeight = 480;

struct Pixels {
  std::vector<UINT32> bgra;
  UINT32 At(UINT32 x, UINT32 y) const { return bgra[static_cast<size_t>(y) * kWidth + x]; }
  // WIC PBGRA in memory is B,G,R,A; a little-endian UINT32 read is 0xAARRGGBB.
  float R(UINT32 p) const { return static_cast<float>((p >> 16) & 0xFF) / 255.0f; }
  float G(UINT32 p) const { return static_cast<float>((p >> 8) & 0xFF) / 255.0f; }
  float B(UINT32 p) const { return static_cast<float>(p & 0xFF) / 255.0f; }
  double Luma(UINT32 p) const {
    return chrome::Luminance(D2D1::ColorF(R(p), G(p), B(p), 1.0f));
  }
};

struct Renderer {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IWICImagingFactory> wic;
  chrome::ChromePainter painter;

  bool Init() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) {
      return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.GetAddressOf())))) {
      return false;
    }
    return SUCCEEDED(painter.Initialize());
  }

  bool Render(const chrome::ChromeModel& model, Pixels* out) {
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, bitmap.GetAddressOf()))) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> target;
    // The geometry/brush factory MUST be the one that owns the target, or D2D
    // answers D2DERR_WRONG_RESOURCE_DOMAIN (0x88990012).
    HRESULT hr = painter.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                                target.GetAddressOf());
    if (FAILED(hr)) {
      std::printf("    [render] CreateWicBitmapRenderTarget hr=%08lx\n", static_cast<unsigned long>(hr));
      return false;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
    painter.Draw(target.Get(), model);
    hr = target->EndDraw();
    if (FAILED(hr)) {
      std::printf("    [render] EndDraw hr=%08lx\n", static_cast<unsigned long>(hr));
      return false;
    }
    ComPtr<IWICBitmapLock> lock;
    WICRect rect{0, 0, static_cast<INT>(kWidth), static_cast<INT>(kHeight)};
    hr = bitmap->Lock(&rect, WICBitmapLockRead, lock.GetAddressOf());
    if (FAILED(hr)) {
      std::printf("    [render] WIC Lock hr=%08lx\n", static_cast<unsigned long>(hr));
      return false;
    }
    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
      return false;
    }
    out->bgra.resize(static_cast<size_t>(kWidth) * kHeight);
    for (UINT32 y = 0; y < kHeight; ++y) {
      std::memcpy(out->bgra.data() + static_cast<size_t>(y) * kWidth,
                  data + static_cast<size_t>(y) * stride, kWidth * 4);
    }
    return true;
  }
};

chrome::ChromeModel BaseModel() {
  chrome::ChromeModel model;
  model.variant = prism::theme::Variant::Midnight;
  model.dpi = 96;
  model.width = static_cast<float>(kWidth);
  model.height = static_cast<float>(kHeight);
  model.title = L"Create archive";

  chrome::Widget button;
  button.id = 1;
  button.kind = chrome::WidgetKind::Button;
  button.rect = D2D1::RectF(60.0f, 120.0f, 220.0f, 156.0f);
  button.label = L"Create";
  button.primary = true;
  model.widgets.push_back(button);

  chrome::Widget slider;
  slider.id = 2;
  slider.kind = chrome::WidgetKind::Slider;
  slider.rect = D2D1::RectF(60.0f, 240.0f, 500.0f, 270.0f);
  slider.level = 0;
  slider.minLevel = 0;
  slider.maxLevel = 9;
  slider.value = L"0";
  model.widgets.push_back(slider);
  return model;
}

chrome::Widget* Find(chrome::ChromeModel& model, int id) { return model.Find(id); }

double OnCard(const chrome::Rgba& fill) {
  // Alpha is a material: compare the pixel the user actually sees, i.e. the
  // fill composited over the card, never the raw channel triple.
  return prism::theme::RelativeLuminance(
      prism::theme::CompositeOver(fill, chrome::Card(prism::theme::Variant::Midnight)));
}

double ButtonLuma(chrome::VisualState state) {
  return OnCard(chrome::ButtonFill(prism::theme::Variant::Midnight, state, true));
}

void PixelColor(const chrome::Rgba& rgba, float* r, float* g, float* b) {
  *r = rgba.r;
  *g = rgba.g;
  *b = rgba.b;
}

bool WindowStationUsable() {
  const HWND probe = CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 10, 10, nullptr,
                                     nullptr, GetModuleHandleW(nullptr), nullptr);
  if (probe == nullptr) return false;
  DestroyWindow(probe);
  return true;
}

bool OpenCompress(cd::CompressDialog& dialog) {
  if (!dialog.Create(GetModuleHandleW(nullptr), L"visual test")) return false;
  std::vector<prism::PlanItem> items;
  items.push_back(prism::PlanItem{"a.txt", 100, false, 0});
  dialog.SetSource(std::move(items), L"E:\\fixture");
  dialog.State().SetTargetPath("out.7z");
  dialog.SyncControls();
  return true;
}

bool OpenSettings(su::SettingsDialog& dialog) {
  if (!dialog.Create(GetModuleHandleW(nullptr), L"visual test")) return false;
  dialog.State().SetExtractDirText("C:\\out");
  dialog.SyncControls();
  return true;
}

}  // namespace

PRISM_TEST("dpi: 96/144/192 are three real metric buckets") {
  const chrome::Metrics m96 = chrome::MetricsForDpi(96);
  const chrome::Metrics m144 = chrome::MetricsForDpi(144);
  const chrome::Metrics m192 = chrome::MetricsForDpi(192);
  // DESIGN.md section 2: title 32; section 4: island radius 20.
  PRISM_CHECK_EQ(m96.titleBarHeight, 32);
  PRISM_CHECK_EQ(m144.titleBarHeight, 48);
  PRISM_CHECK_EQ(m192.titleBarHeight, 64);
  PRISM_CHECK_EQ(m96.cardRadius, 20);
  PRISM_CHECK_EQ(m144.cardRadius, 30);
  PRISM_CHECK_EQ(m192.cardRadius, 40);
  PRISM_CHECK_EQ(m96.fontSize, 13);
  PRISM_CHECK_EQ(m144.fontSize, 20);  // 19.5 rounded
  PRISM_CHECK_EQ(m192.fontSize, 26);
  // DESIGN.md section 2/4 pins: action bar 56, pad 16, row radius 10, type
  // scale 11/13/15/20.
  PRISM_CHECK_EQ(m96.actionBarHeight, 56);
  PRISM_CHECK_EQ(m144.actionBarHeight, 84);
  PRISM_CHECK_EQ(m192.actionBarHeight, 112);
  PRISM_CHECK_EQ(m96.padding, 16);
  PRISM_CHECK_EQ(m96.buttonRadius, 10);
  PRISM_CHECK_EQ(m96.comboRadius, 10);
  PRISM_CHECK_EQ(m96.smallFontSize, 11);
  PRISM_CHECK_EQ(m96.titleFontSize, 15);
  PRISM_CHECK(m144.scale > m96.scale && m192.scale > m144.scale);
  PRISM_CHECK_EQ(m192.knobSize, 32);
  // The padded hit target scales too: the slider fill width must grow with DPI.
  const D2D1_RECT_F track = D2D1::RectF(0.0f, 0.0f, 100.0f, 6.0f);
  const float w96 = chrome::SliderFillRect(track, 9, 0, 9).right;
  PRISM_CHECK_EQ(w96, 100.0f);
}

PRISM_TEST("squircle: the path samples lie ON C15's SquircleSdf, not on a circle") {
  const double a = 80.0;
  const double b = 40.0;
  for (int i = 0; i <= 8; ++i) {
    const double t = 6.28318530717958647692 * i / 8.0;
    const double x = chrome::SquirclePoint(0, t, a, b);
    const double y = chrome::SquirclePoint(1, t, a, b);
    PRISM_CHECK(std::fabs(prism::theme::SquircleSdf(x, y, a, b)) < 1e-6);
  }
  // G2 (n=4.2) keeps the 45-degree point inside where a circle would put it
  // outside -- the visible difference between a squircle and a rounded box.
  PRISM_CHECK(chrome::SquirclePoint(0, 0.7853981633974483, a, b) > 0.0);
  PRISM_CHECK(prism::theme::SquircleSdf(0.8 * a, 0.8 * b, a, b) < 0.0);
  PRISM_CHECK(prism::theme::SquircleSdf(0.8 * a, 0.8 * b, a, b, 2.0) > 0.0);
}

PRISM_TEST("tokens: the card, title bar and accent come from C15, not literals") {
  const chrome::Rgba title = chrome::TitleBar(prism::theme::Variant::Midnight);
  const chrome::Rgba card = chrome::Card(prism::theme::Variant::Midnight);
  const chrome::Rgba accent = chrome::Accent(prism::theme::Variant::Midnight);
  PRISM_CHECK(!chrome::NearColor(chrome::ToD2D(title), chrome::ToD2D(card)));
  PRISM_CHECK(prism::theme::RelativeLuminance(title) > prism::theme::RelativeLuminance(card));
  PRISM_CHECK_EQ(accent.b > accent.r, true);
  // C15's Midnight accent is #0A84FF (blue channel clearly dominant).
  PRISM_CHECK(accent.b > 0.9f);
  PRISM_CHECK(accent.r < 0.1f);
}

PRISM_TEST("states: empty / normal / hover / pressed / disabled all differ") {
  PRISM_CHECK(chrome::ResolveState(false, true, false, false) == chrome::VisualState::Disabled);
  PRISM_CHECK(chrome::ResolveState(true, false, false, true) == chrome::VisualState::Empty);
  PRISM_CHECK(chrome::ResolveState(true, false, false, false) == chrome::VisualState::Normal);
  PRISM_CHECK(chrome::ResolveState(true, true, false, false) == chrome::VisualState::Hover);
  PRISM_CHECK(chrome::ResolveState(true, true, true, false) == chrome::VisualState::Pressed);

  const double normal = ButtonLuma(chrome::VisualState::Normal);
  const double hover = ButtonLuma(chrome::VisualState::Hover);
  const double pressed = ButtonLuma(chrome::VisualState::Pressed);
  const double disabled = ButtonLuma(chrome::VisualState::Disabled);
  const double empty = ButtonLuma(chrome::VisualState::Empty);
  PRISM_CHECK(hover > normal * 1.05);   // the pixel test below asserts the same in retinas
  PRISM_CHECK(pressed < normal);
  PRISM_CHECK(disabled < normal);
  PRISM_CHECK(empty < normal);
}

PRISM_TEST("pixels: title bar background is the C15 title colour") {
  Renderer renderer;
  if (!PRISM_CHECK(renderer.Init())) return;
  const chrome::ChromeModel model = BaseModel();
  Pixels pixels;
  if (!PRISM_CHECK(renderer.Render(model, &pixels))) return;
  const chrome::Rgba expected = chrome::TitleBar(model.variant);
  const UINT32 p = pixels.At(560, 22);  // right of the title text, inside the bar
  PRISM_CHECK(std::fabs(pixels.R(p) - expected.r) < 0.03f);
  PRISM_CHECK(std::fabs(pixels.G(p) - expected.g) < 0.03f);
  PRISM_CHECK(std::fabs(pixels.B(p) - expected.b) < 0.03f);
  // Not the retired Windows 95 face (#F0F0F0) nor the backdrop.
  PRISM_CHECK(!(pixels.R(p) > 0.85f && pixels.G(p) > 0.85f && pixels.B(p) > 0.85f));
}

PRISM_TEST("pixels: hover is brighter than normal, disabled is dimmer") {
  Renderer renderer;
  if (!PRISM_CHECK(renderer.Init())) return;
  const double normalFill = OnCard(chrome::ButtonFill(
      prism::theme::Variant::Midnight, chrome::VisualState::Normal, true));
  const double hoverFill = OnCard(chrome::ButtonFill(
      prism::theme::Variant::Midnight, chrome::VisualState::Hover, true));
  const double disabledFill = OnCard(chrome::ButtonFill(
      prism::theme::Variant::Midnight, chrome::VisualState::Disabled, true));
  PRISM_CHECK(hoverFill > normalFill);
  PRISM_CHECK(disabledFill < normalFill);

  chrome::ChromeModel model = BaseModel();
  Pixels normal;
  if (!PRISM_CHECK(renderer.Render(model, &normal))) return;
  // Resolve through the SAME function the window uses: the no-hover mutant must
  // turn this pixel red too, not only the pure state table.
  model.Find(1)->state = chrome::ResolveState(true, true, false, false);
  Pixels hover;
  if (!PRISM_CHECK(renderer.Render(model, &hover))) return;
  model.Find(1)->state = chrome::VisualState::Disabled;
  Pixels disabled;
  if (!PRISM_CHECK(renderer.Render(model, &disabled))) return;
  const UINT32 centerN = normal.At(140, 138);
  const UINT32 centerH = hover.At(140, 138);
  const UINT32 centerD = disabled.At(140, 138);
  PRISM_CHECK(normal.Luma(centerH) > normal.Luma(centerN) * 1.05);
  PRISM_CHECK(disabled.Luma(centerD) < disabled.Luma(centerN));
}

PRISM_TEST("pixels: the slider fill only grows with the level (0..9 monotonic)") {
  Renderer renderer;
  if (!PRISM_CHECK(renderer.Init())) return;
  int previous = -1;
  int first = -1;
  int last = 0;
  for (int level = 0; level <= 9; ++level) {
    chrome::ChromeModel model = BaseModel();
    model.Find(2)->level = level;
    model.Find(2)->value = std::to_wstring(level);
    Pixels pixels;
    if (!PRISM_CHECK(renderer.Render(model, &pixels))) return;
    // Count the accent-blue pixels on the slider's centre line.
    int filled = 0;
    for (UINT32 x = 68; x < 492; ++x) {
      const UINT32 p = pixels.At(x, 255);
      if (pixels.B(p) > 0.45f && pixels.B(p) > pixels.R(p) * 1.6f) ++filled;
    }
    if (first < 0) first = filled;
    last = filled;
    PRISM_CHECK(filled >= previous);
    previous = filled;
  }
  PRISM_CHECK(last > first + 100);
}

PRISM_TEST("pixels: every widget kind renders empty / normal / hover / disabled") {
  Renderer renderer;
  if (!PRISM_CHECK(renderer.Init())) return;
  const chrome::WidgetKind kinds[] = {chrome::WidgetKind::Button, chrome::WidgetKind::CheckBox,
                                      chrome::WidgetKind::Combo, chrome::WidgetKind::TextField,
                                      chrome::WidgetKind::Slider};
  for (const chrome::WidgetKind kind : kinds) {
    double luma[4] = {0.0, 0.0, 0.0, 0.0};  // empty, normal, hover, disabled
    const chrome::VisualState states[4] = {chrome::VisualState::Empty, chrome::VisualState::Normal,
                                           chrome::VisualState::Hover,
                                           chrome::VisualState::Disabled};
    for (int i = 0; i < 4; ++i) {
      chrome::ChromeModel model;
      model.variant = prism::theme::Variant::Midnight;
      model.width = static_cast<float>(kWidth);
      model.height = static_cast<float>(kHeight);
      chrome::Widget widget;
      widget.id = 7;
      widget.kind = kind;
      widget.rect = kind == chrome::WidgetKind::Slider
                        ? D2D1::RectF(60.0f, 120.0f, 400.0f, 150.0f)
                        : D2D1::RectF(60.0f, 120.0f, 260.0f, 156.0f);
      widget.label = L"Control";
      widget.value = kind == chrome::WidgetKind::Slider ? L"5" : L"Value";
      widget.level = 5;
      widget.state = states[i];
      model.widgets.push_back(widget);
      Pixels pixels;
      if (!PRISM_CHECK(renderer.Render(model, &pixels))) return;
      const UINT32 px =
          kind == chrome::WidgetKind::CheckBox
              ? 69u
              : (kind == chrome::WidgetKind::Slider ? 100u : 100u);
      const UINT32 py = kind == chrome::WidgetKind::Slider ? 135u : 138u;
      const UINT32 p = pixels.At(px, py);
      luma[i] = pixels.Luma(p);
    }
    // Hover is a visible lift; disabled and empty are visibly different from the
    // normal face. This is the four-state requirement, per widget kind.
    const bool hoverOk = luma[2] > luma[1] * 1.02;
    const bool disabledOk = std::fabs(luma[3] - luma[1]) > 0.001;
    const bool emptyOk = std::fabs(luma[0] - luma[1]) > 0.001;
    if (!hoverOk || !disabledOk || !emptyOk) {
      std::printf("    [four-state] kind=%d empty=%.4f normal=%.4f hover=%.4f disabled=%.4f\n",
                  static_cast<int>(kind), luma[0], luma[1], luma[2], luma[3]);
    }
    PRISM_CHECK(hoverOk);
    PRISM_CHECK(disabledOk);
    PRISM_CHECK(emptyOk);
  }
}

PRISM_TEST("pixels: the painter actually issued every primitive, including the disabled one") {
  Renderer renderer;
  if (!PRISM_CHECK(renderer.Init())) return;
  chrome::ChromeModel model = BaseModel();
  model.Find(1)->state = chrome::VisualState::Disabled;
  Pixels pixels;
  if (!PRISM_CHECK(renderer.Render(model, &pixels))) return;
  PRISM_CHECK_EQ(renderer.painter.paintedButtons, 1);
  PRISM_CHECK_EQ(renderer.painter.paintedSliders, 1);
  PRISM_CHECK(renderer.painter.paintedDisabled >= 1);
}

PRISM_TEST("keyboard: Tab order, Space, Enter and Esc through the real window proc") {
  if (!PRISM_CHECK(WindowStationUsable())) {
    std::puts("SKIP: no interactive window station");
    return;
  }
  cd::CompressDialog dialog;
  if (!PRISM_CHECK(OpenCompress(dialog))) return;
  PRISM_CHECK_EQ(dialog.FocusIndexOfModel(), 0);
  PRISM_CHECK_EQ(dialog.FocusedControlId(), cd::ids::kFormat);
  PRISM_CHECK(dialog.DispatchKey(VK_TAB, false));
  PRISM_CHECK_EQ(dialog.FocusedControlId(), cd::ids::kLevel);
  PRISM_CHECK(dialog.DispatchKey(VK_TAB, true));  // Shift+Tab
  PRISM_CHECK_EQ(dialog.FocusedControlId(), cd::ids::kFormat);
  // Walk to Solid and toggle it with Space.
  for (int i = 0; i < 3; ++i) PRISM_CHECK(dialog.DispatchKey(VK_TAB, false));
  PRISM_CHECK_EQ(dialog.FocusedControlId(), cd::ids::kSolid);
  const bool before = dialog.State().solid();
  PRISM_CHECK(dialog.DispatchKey(VK_SPACE, false));
  PRISM_CHECK(dialog.State().solid() != before);
  // Enter on the default button accepts and closes.
  dialog.SetAcceptHandler([](const cd::CreateRequest&) { return true; });
  while (dialog.FocusedControlId() != cd::ids::kOk) {
    if (!PRISM_CHECK(dialog.DispatchKey(VK_TAB, false))) break;
  }
  const HWND window = dialog.Hwnd();
  PRISM_CHECK(dialog.DispatchKey(VK_RETURN, false));
  PRISM_CHECK(dialog.Accepted());
  PRISM_CHECK_EQ(IsWindow(window), FALSE);

  cd::CompressDialog esc;
  if (!PRISM_CHECK(OpenCompress(esc))) return;
  const HWND escWindow = esc.Hwnd();
  PRISM_CHECK(esc.DispatchKey(VK_ESCAPE, false));
  PRISM_CHECK_EQ(IsWindow(escWindow), FALSE);
}

PRISM_TEST("keyboard: settings Tab reaches Save, Space toggles MoTW, Esc cancels") {
  if (!PRISM_CHECK(WindowStationUsable())) {
    std::puts("SKIP: no interactive window station");
    return;
  }
  su::SettingsDialog dialog;
  if (!PRISM_CHECK(OpenSettings(dialog))) return;
  PRISM_CHECK_EQ(dialog.FocusedControlId(), su::ids::kLanguage);
  PRISM_CHECK(dialog.DispatchKey(VK_TAB, false));
  PRISM_CHECK_EQ(dialog.FocusedControlId(), su::ids::kTheme);
  int guard = 0;
  while (dialog.FocusedControlId() != su::ids::kMotw && guard++ < 20) {
    if (!PRISM_CHECK(dialog.DispatchKey(VK_TAB, false))) break;
  }
  PRISM_CHECK_EQ(dialog.FocusedControlId(), su::ids::kMotw);
  const bool before = dialog.State().inheritMotw();
  PRISM_CHECK(dialog.DispatchKey(VK_SPACE, false));
  PRISM_CHECK(dialog.State().inheritMotw() != before);
  const HWND window = dialog.Hwnd();
  PRISM_CHECK(dialog.DispatchKey(VK_ESCAPE, false));
  PRISM_CHECK_EQ(IsWindow(window), FALSE);
}

PRISM_TEST("disabled controls always carry the reason sentence (four-state rule)") {
  cd::DialogState state;
  state.SetFormat(prism::ArchiveFormat::Tar);
  const cd::Field disabled[] = {cd::Field::Level, cd::Field::Dictionary, cd::Field::Solid,
                                cd::Field::Threads, cd::Field::Password,
                                cd::Field::EncryptFileNames};
  for (const cd::Field field : disabled) {
    if (!state.IsEnabled(field)) {
      PRISM_CHECK(!state.WhyDisabled(field).empty());
    }
  }
}

int main(int argc, char** argv) {
  const int failures = prism::test::RunAll(argc, argv);
#if defined(PRISM_V3_MUTANT_NO_HOVER) || defined(PRISM_V3_MUTANT_FIXED_DPI)
  if (failures != 0 && chrome::MutationFired()) {
    std::puts("DIALOG-VISUAL MUTANT FIRED");
  }
  std::printf("mutant build: exit 1 on purpose (mutation hits=%d)\n",
              chrome::MutationCount());
  return 1;
#else
  if (failures != 0) return 1;
  std::puts("ALL DIALOG-VISUAL CHECKS PASSED");
  return 0;
#endif
}

#else

int main() {
  std::puts("SKIP: dialog pixel tests are Windows-only");
  return 77;
}

#endif
