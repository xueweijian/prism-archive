// PrismArchive -- PrismWin
// DialogChrome: the shared self-painted shell for the V3 dialogs (compression and
// settings). One renderer, two screens -- the same relationship GalleryRenderer
// has to GalleryWindow.
//
// C18/C31 built their dialogs from stock Win32 controls (grey push buttons, the
// system trackbar, the system combo box). This file replaces the *shell*: a dark
// Liquid Glass card, a squircle (G2 continuous corner) outline taken from C15's
// SquircleSdf parameters, a self-drawn title bar with a close button, and
// four-state self-drawn buttons / slider / combo box. The dialogs keep their real
// child controls as invisible message/state adapters, so every DialogState rule
// and every C18/C31 assertion still runs unchanged.
//
// COLOUR HAS ONE SOURCE (C15 tokens): no literal colour is allowed here. Every
// fill is derived from prism::theme::Lookup(TokenId, Variant).
//
// CONTRACTS: B1 parenthesised min/max, B2 own <cstdint>, D3 compile-time mutants
// with a fired counter, D26 the mutant binaries are WILL_FAIL-only.
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
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../PrismCore/ThemeTokens.hpp"

// The C18/C31 suites have their own link lists (they were written for stock
// controls); DialogChrome carries its own dependencies so a TU that only
// includes the dialog headers still links. This keeps the change out of
// Tests/CMakeLists.txt and out of other cards' suite files.
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace prism::win::chrome {

using Microsoft::WRL::ComPtr;
using prism::theme::Rgba;
using prism::theme::TokenId;
using prism::theme::Variant;

namespace mutation {
#if defined(PRISM_V3_MUTANT_NO_HOVER)
inline constexpr bool kNoHover = true;
#else
inline constexpr bool kNoHover = false;
#endif
#if defined(PRISM_V3_MUTANT_FIXED_DPI)
inline constexpr bool kFixedDpi = true;
#else
inline constexpr bool kFixedDpi = false;
#endif
}  // namespace mutation

inline int& MutationCount() {
  static int count = 0;
  return count;
}
inline bool MutationFired() { return MutationCount() != 0; }

enum class VisualState : std::uint8_t { Empty = 0, Normal, Hover, Pressed, Disabled, Count };

inline const char* VisualStateName(VisualState state) {
  switch (state) {
    case VisualState::Empty: return "empty";
    case VisualState::Normal: return "normal";
    case VisualState::Hover: return "hover";
    case VisualState::Pressed: return "pressed";
    case VisualState::Disabled: return "disabled";
    case VisualState::Count: return "count";
  }
  return "?";
}

// The ONE place hover may be suppressed. The mutant turns the hover bit into
// Normal, so the pixel assertion that hover is brighter must go red -- and the
// counter proves the mutant actually ran.
inline VisualState ResolveState(bool enabled, bool hovered, bool pressed, bool empty) {
  if (!enabled) return VisualState::Disabled;
  if (mutation::kNoHover) {
    if (hovered || pressed) static_cast<void>(MutationCount()++);
    if (empty) return VisualState::Empty;
    return VisualState::Normal;
  }
  if (pressed) return VisualState::Pressed;
  if (hovered) return VisualState::Hover;
  return empty ? VisualState::Empty : VisualState::Normal;
}

// 96-DPI baseline. Every value below is a DIP; the painter multiplies by
// Metrics::scale. The test pins 96/144/192 (1.0 / 1.5 / 2.0).
struct Metrics {
  float scale = 1.0f;
  int titleBarHeight = 44;
  int cardRadius = 22;
  int padding = 20;
  int buttonHeight = 30;
  int buttonRadius = 15;
  int sliderHeight = 26;
  int sliderTrack = 6;
  int knobSize = 16;
  int comboHeight = 30;
  int comboRadius = 10;
  int checkBox = 18;
  int rowGap = 12;
  int hintHeight = 16;
  int fontSize = 13;
  int titleFontSize = 15;
  int smallFontSize = 11;
  int closeButton = 28;

  float S(float value) const { return value * scale; }
};

inline Metrics MetricsForDpi(int dpi) {
  Metrics m;
  if (mutation::kFixedDpi) {
    static_cast<void>(MutationCount()++);
    dpi = 96;
  }
  if (dpi < 48) dpi = 48;
  if (dpi > 480) dpi = 480;
  m.scale = static_cast<float>(dpi) / 96.0f;
  m.titleBarHeight = static_cast<int>(std::lround(44.0f * m.scale));
  m.cardRadius = static_cast<int>(std::lround(22.0f * m.scale));
  m.padding = static_cast<int>(std::lround(20.0f * m.scale));
  m.buttonHeight = static_cast<int>(std::lround(30.0f * m.scale));
  m.buttonRadius = static_cast<int>(std::lround(15.0f * m.scale));
  m.sliderHeight = static_cast<int>(std::lround(26.0f * m.scale));
  m.sliderTrack = static_cast<int>(std::lround(6.0f * m.scale));
  m.knobSize = static_cast<int>(std::lround(16.0f * m.scale));
  m.comboHeight = static_cast<int>(std::lround(30.0f * m.scale));
  m.comboRadius = static_cast<int>(std::lround(10.0f * m.scale));
  m.checkBox = static_cast<int>(std::lround(18.0f * m.scale));
  m.rowGap = static_cast<int>(std::lround(12.0f * m.scale));
  m.hintHeight = static_cast<int>(std::lround(16.0f * m.scale));
  m.fontSize = static_cast<int>(std::lround(13.0f * m.scale));
  m.titleFontSize = static_cast<int>(std::lround(15.0f * m.scale));
  m.smallFontSize = static_cast<int>(std::lround(11.0f * m.scale));
  m.closeButton = static_cast<int>(std::lround(28.0f * m.scale));
  return m;
}

inline int CurrentDpi(HWND hwnd) {
  if (hwnd != nullptr) {
    const UINT dpi = GetDpiForWindow(hwnd);
    if (dpi >= 48) return static_cast<int>(dpi);
  }
  HDC dc = GetDC(nullptr);
  const int dpi = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc != nullptr) ReleaseDC(nullptr, dc);
  return dpi > 0 ? dpi : 96;
}

// ---------------------------------------------------------------------------
// Colour -- every channel comes from a C15 token. No literal palette here.
// ---------------------------------------------------------------------------
inline D2D1_COLOR_F ToD2D(const Rgba& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

inline Rgba TokenColor(TokenId id, Variant variant) {
  return prism::theme::Lookup(id, variant).color;
}
inline Rgba TokenGradientFrom(TokenId id, Variant variant) {
  return prism::theme::Lookup(id, variant).gradient.from;
}
inline Rgba Composite(const Rgba& top, const Rgba& bottom) {
  return prism::theme::CompositeOver(top, bottom);
}
// "amount" is a fraction of the distance to the token's white; this is how the
// whole design language derives hover/pressed from ONE accent token instead of
// inventing a second colour.
inline Rgba Lighten(const Rgba& c, float amount) {
  return Rgba(c.r + (1.0f - c.r) * amount, c.g + (1.0f - c.g) * amount,
              c.b + (1.0f - c.b) * amount, c.a);
}
inline Rgba Darken(const Rgba& c, float amount) {
  return Rgba(c.r * (1.0f - amount), c.g * (1.0f - amount), c.b * (1.0f - amount), c.a);
}
inline Rgba WithAlpha(const Rgba& c, float alpha) { return Rgba(c.r, c.g, c.b, alpha); }

inline Rgba Backdrop(Variant variant) {
  return TokenColor(TokenId::SurfaceBackdropReference, variant);
}
inline Rgba Canvas(Variant variant) {
  return Composite(TokenColor(TokenId::SurfaceCanvasBase, variant), Backdrop(variant));
}
inline Rgba Card(Variant variant) {
  return Composite(TokenColor(TokenId::SurfaceFloatingIsland, variant), Canvas(variant));
}
inline Rgba TitleBar(Variant variant) {
  const Rgba island = TokenColor(TokenId::SurfaceFloatingIsland, variant);
  const Rgba layered = Composite(island, Card(variant));
  return Lighten(layered, 0.028f);
}
inline Rgba Accent(Variant variant) {
  return TokenGradientFrom(TokenId::AccentLiquidPrimary, variant);
}
inline Rgba TextPrimary(Variant variant) { return TokenColor(TokenId::TextPrimary, variant); }
inline Rgba TextSecondary(Variant variant) { return TokenColor(TokenId::TextSecondary, variant); }
inline Rgba BorderTop(Variant variant) {
  return TokenGradientFrom(TokenId::BorderSpecularRim, variant);
}

inline Rgba ButtonFill(Variant variant, VisualState state, bool primary) {
  const Rgba base = primary ? Accent(variant) : Card(variant);
  switch (state) {
    case VisualState::Hover: return Lighten(base, primary ? 0.16f : 0.10f);
    case VisualState::Pressed: return Darken(base, primary ? 0.14f : 0.08f);
    case VisualState::Disabled: return WithAlpha(TextSecondary(variant), 0.10f);
    case VisualState::Empty: return WithAlpha(Backdrop(variant), 0.35f);
    case VisualState::Normal:
    case VisualState::Count: break;
  }
  return base;
}
inline Rgba ButtonText(Variant variant, VisualState state, bool primary) {
  if (state == VisualState::Disabled) return WithAlpha(TextSecondary(variant), 0.55f);
  if (primary) return TextPrimary(variant);
  return state == VisualState::Hover ? TextPrimary(variant) : TextSecondary(variant);
}
inline Rgba TrackColor(Variant variant, VisualState state) {
  const Rgba base = WithAlpha(TextSecondary(variant), 0.22f);
  if (state == VisualState::Disabled) return WithAlpha(base, 0.10f);
  if (state == VisualState::Hover) return WithAlpha(base, 0.30f);
  return base;
}
inline Rgba SliderFillColor(Variant variant, VisualState state) {
  const Rgba accent = Accent(variant);
  if (state == VisualState::Disabled) return WithAlpha(TextSecondary(variant), 0.30f);
  if (state == VisualState::Hover) return Lighten(accent, 0.12f);
  if (state == VisualState::Pressed) return Darken(accent, 0.10f);
  if (state == VisualState::Empty) return WithAlpha(accent, 0.35f);
  return accent;
}
inline Rgba ComboFill(Variant variant, VisualState state) {
  const Rgba base = Card(variant);
  switch (state) {
    case VisualState::Hover: return Lighten(base, 0.08f);
    case VisualState::Pressed: return Darken(base, 0.06f);
    case VisualState::Disabled: return WithAlpha(TextSecondary(variant), 0.08f);
    case VisualState::Empty: return WithAlpha(Backdrop(variant), 0.35f);
    case VisualState::Normal:
    case VisualState::Count: break;
  }
  return base;
}
inline Rgba HintColor(Variant variant, bool error) {
  return error ? TextPrimary(variant) : TextSecondary(variant);
}
inline double Luminance(const D2D1_COLOR_F& c) {
  const Rgba rgba(c.r, c.g, c.b, c.a);
  return prism::theme::RelativeLuminance(rgba);
}
inline bool NearColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float tolerance = 0.02f) {
  return std::fabs(a.r - b.r) <= tolerance && std::fabs(a.g - b.g) <= tolerance &&
         std::fabs(a.b - b.b) <= tolerance;
}

// ---------------------------------------------------------------------------
// Squircle geometry: the same G2 exponent C15's SquircleSdf uses (4.2). The
// parametric point |x/a|^n + |y/b|^n = 1 is what the path is built from, and the
// test re-feeds every sampled point to SquircleSdf and requires ~0.
// ---------------------------------------------------------------------------
inline double SquirclePoint(int axis, double t, double halfW, double halfH) {
  const double n = prism::theme::kSquircleExponent;
  const double c = std::cos(t);
  const double s = std::sin(t);
  const double x = (c < 0.0 ? -1.0 : 1.0) * std::pow(std::fabs(c), 2.0 / n) * halfW;
  const double y = (s < 0.0 ? -1.0 : 1.0) * std::pow(std::fabs(s), 2.0 / n) * halfH;
  return axis == 0 ? x : y;
}

inline HRESULT CreateSquircleGeometry(ID2D1Factory* factory, const D2D1_RECT_F& rect,
                                      float radius, ID2D1PathGeometry** out) {
  *out = nullptr;
  if (factory == nullptr) return E_POINTER;
  const float halfW = (std::max)(0.5f, (rect.right - rect.left) * 0.5f);
  const float halfH = (std::max)(0.5f, (rect.bottom - rect.top) * 0.5f);
  const float r = (std::min)(radius, (std::min)(halfW, halfH));
  if (r <= 0.0f) return E_INVALIDARG;
  ComPtr<ID2D1PathGeometry> geometry;
  HRESULT hr = factory->CreatePathGeometry(geometry.GetAddressOf());
  if (FAILED(hr)) return hr;
  ComPtr<ID2D1GeometrySink> sink;
  hr = geometry->Open(sink.GetAddressOf());
  if (FAILED(hr)) return hr;
  const float cx = (rect.left + rect.right) * 0.5f;
  const float cy = (rect.top + rect.bottom) * 0.5f;
  const int kSteps = 64;
  const double step = 6.28318530717958647692 / kSteps;
  sink->BeginFigure(D2D1::Point2F(static_cast<float>(cx + SquirclePoint(0, 0.0, halfW, halfH)),
                                  static_cast<float>(cy + SquirclePoint(1, 0.0, halfW, halfH))),
                    D2D1_FIGURE_BEGIN_FILLED);
  for (int i = 1; i <= kSteps; ++i) {
    const double t = step * i;
    sink->AddLine(D2D1::Point2F(static_cast<float>(cx + SquirclePoint(0, t, halfW, halfH)),
                                static_cast<float>(cy + SquirclePoint(1, t, halfW, halfH))));
  }
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  hr = sink->Close();
  if (FAILED(hr)) return hr;
  *out = geometry.Detach();
  return S_OK;
}

// ---------------------------------------------------------------------------
// Slider geometry: one pure function, so the pixel test and the painter cannot
// disagree about where the filled segment ends.
// ---------------------------------------------------------------------------
inline float SliderRatio(int level, int minLevel, int maxLevel) {
  if (maxLevel <= minLevel) return 0.0f;
  const int clamped = (std::max)(minLevel, (std::min)(level, maxLevel));
  return static_cast<float>(clamped - minLevel) / static_cast<float>(maxLevel - minLevel);
}

inline D2D1_RECT_F SliderFillRect(const D2D1_RECT_F& track, int level, int minLevel,
                                  int maxLevel) {
  const float ratio = SliderRatio(level, minLevel, maxLevel);
  const float x = track.left + (track.right - track.left) * ratio;
  return D2D1::RectF(track.left, track.top, (std::max)(track.left, x), track.bottom);
}

// ---------------------------------------------------------------------------
// Keyboard vocabulary. Pure: the dialogs call this from WM_KEYDOWN and the test
// pins the same table, so "Tab order / Space / Enter / Esc" is a decision, not a
// switch scattered across two window procs.
// ---------------------------------------------------------------------------
enum class KeyCommand : std::uint8_t {
  None = 0,
  Next,
  Previous,
  Activate,
  Cancel,
  Increase,
  Decrease,
};

inline KeyCommand ClassifyKey(unsigned vk, bool shift, bool sliderFocused,
                              bool comboFocused) {
  switch (vk) {
    case VK_TAB: return shift ? KeyCommand::Previous : KeyCommand::Next;
    case VK_RETURN: return KeyCommand::Activate;
    case VK_SPACE: return KeyCommand::Activate;
    case VK_ESCAPE: return KeyCommand::Cancel;
    case VK_LEFT:
    case VK_DOWN: return sliderFocused || comboFocused ? KeyCommand::Decrease : KeyCommand::None;
    case VK_RIGHT:
    case VK_UP: return sliderFocused || comboFocused ? KeyCommand::Increase : KeyCommand::None;
    default: return KeyCommand::None;
  }
}

inline int NextFocus(int current, int count, bool backwards) {
  if (count <= 0) return -1;
  int next = current;
  if (next < 0 || next >= count) next = backwards ? 0 : -1;
  next = backwards ? next - 1 : next + 1;
  if (next < 0) next = count - 1;
  if (next >= count) next = 0;
  return next;
}

// ---------------------------------------------------------------------------
// The model the dialogs build and the painter consumes.
// ---------------------------------------------------------------------------
enum class WidgetKind : std::uint8_t {
  Label = 0,
  Button,
  CheckBox,
  Slider,
  Combo,
  TextField,
  Title,
  Hint,
};

struct Widget {
  int id = 0;
  WidgetKind kind = WidgetKind::Button;
  D2D1_RECT_F rect = D2D1::RectF();
  std::wstring label;
  std::wstring value;
  std::wstring hint;
  bool enabled = true;
  bool checked = false;
  bool focused = false;
  bool primary = false;
  bool error = false;
  VisualState state = VisualState::Normal;
  int level = 0;
  int minLevel = 0;
  int maxLevel = 9;

  bool Focusable() const {
    return kind == WidgetKind::Button || kind == WidgetKind::CheckBox ||
           kind == WidgetKind::Slider || kind == WidgetKind::Combo ||
           kind == WidgetKind::TextField;
  }
};

struct ChromeModel {
  Variant variant = Variant::Midnight;
  int dpi = 96;
  float width = 640.0f;
  float height = 480.0f;
  std::wstring title;
  std::wstring footer;
  bool footerError = false;
  bool titleHover = false;
  bool closeHover = false;
  bool closePressed = false;
  std::vector<Widget> widgets;

  Metrics metrics() const { return MetricsForDpi(dpi); }
  const Widget* Find(int id) const {
    for (const Widget& w : widgets) {
      if (w.id == id) return &w;
    }
    return nullptr;
  }
  Widget* Find(int id) {
    for (Widget& w : widgets) {
      if (w.id == id) return &w;
    }
    return nullptr;
  }
  const Widget* Focused() const {
    for (const Widget& w : widgets) {
      if (w.focused && w.Focusable()) return &w;
    }
    return nullptr;
  }
  int FocusedId() const {
    const Widget* w = Focused();
    return w != nullptr ? w->id : -1;
  }
  int FocusIndex() const {
    int index = 0;
    for (const Widget& w : widgets) {
      if (!w.Focusable()) continue;
      if (w.focused) return index;
      ++index;
    }
    return -1;
  }
  int FocusCount() const {
    int count = 0;
    for (const Widget& w : widgets) {
      if (w.Focusable()) ++count;
    }
    return count;
  }
  void FocusIndex(int wanted) {
    int index = 0;
    for (Widget& w : widgets) {
      if (!w.Focusable()) continue;
      w.focused = index == wanted;
      ++index;
    }
  }
  int HitTest(float x, float y) const {
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
      if (!it->Focusable()) continue;
      if (x >= it->rect.left && x <= it->rect.right && y >= it->rect.top &&
          y <= it->rect.bottom) {
        return it->id;
      }
    }
    return -1;
  }
  D2D1_RECT_F CloseRect() const {
    const Metrics m = metrics();
    return D2D1::RectF(width - m.closeButton - 12.0f, (m.titleBarHeight - m.closeButton) * 0.5f,
                       width - 12.0f, (m.titleBarHeight + m.closeButton) * 0.5f);
  }
  D2D1_RECT_F TitleRect() const {
    const Metrics m = metrics();
    return D2D1::RectF(0.0f, 0.0f, width, static_cast<float>(m.titleBarHeight));
  }
  bool HitClose(float x, float y) const {
    const D2D1_RECT_F r = CloseRect();
    return x >= r.left - 4.0f && x <= r.right + 4.0f && y >= r.top - 4.0f &&
           y <= r.bottom + 4.0f;
  }
  bool HitTitle(float x, float y) const {
    const D2D1_RECT_F t = TitleRect();
    return y <= t.bottom && !HitClose(x, y);
  }
};

// Screen-space helpers: the model is in 96-DPI DIPs, the window/test is in
// physical pixels. One conversion, used by both hit testing and the painter.
inline D2D1_RECT_F ScaleRect(const D2D1_RECT_F& r, float scale) {
  return D2D1::RectF(r.left * scale, r.top * scale, r.right * scale, r.bottom * scale);
}
inline int HitTestScreen(const ChromeModel& model, float px, float py) {
  const float s = model.metrics().scale;
  return s > 0.0f ? model.HitTest(px / s, py / s) : -1;
}
inline bool HitCloseScreen(const ChromeModel& model, float px, float py) {
  const float s = model.metrics().scale;
  return s > 0.0f && model.HitClose(px / s, py / s);
}
inline bool HitTitleScreen(const ChromeModel& model, float px, float py) {
  const float s = model.metrics().scale;
  return s > 0.0f && model.HitTitle(px / s, py / s);
}

// ---------------------------------------------------------------------------
// Painter. One device-resource setup, then Draw(model) is pure against the RT.
// ---------------------------------------------------------------------------
class ChromePainter {
 public:
  HRESULT Initialize() {
    if (factory_) return S_OK;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   factory_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(
                                   writeFactory_.ReleaseAndGetAddressOf()));
  }

  void ResetDeviceResources() { text_.Reset(); }
  ID2D1Factory* Factory() const { return factory_.Get(); }
  IDWriteFactory* WriteFactory() const { return writeFactory_.Get(); }

  HRESULT CreateDeviceResources(ID2D1RenderTarget* target, int fontPx) {
    if (text_ && bodySize_ == fontPx) return S_OK;
    text_.Reset();
    if (!writeFactory_) return E_FAIL;
    HRESULT hr = writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL,
                                                 static_cast<float>(fontPx), L"en-US",
                                                 text_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    bodySize_ = fontPx;
    static_cast<void>(target);
    return S_OK;
  }

  void Draw(ID2D1RenderTarget* target, const ChromeModel& model) {
    if (target == nullptr) return;
    const Metrics m = model.metrics();
    const float s = m.scale;
    if (FAILED(CreateDeviceResources(target, m.fontSize))) return;
    target->Clear(ToD2D(Backdrop(model.variant)));
    ComPtr<ID2D1SolidColorBrush> cardBrush;
    ComPtr<ID2D1SolidColorBrush> titleBrush;
    ComPtr<ID2D1SolidColorBrush> primaryBrush;
    ComPtr<ID2D1SolidColorBrush> secondaryBrush;
    if (FAILED(target->CreateSolidColorBrush(ToD2D(Card(model.variant)),
                                             cardBrush.GetAddressOf()))) {
      return;
    }
    if (FAILED(target->CreateSolidColorBrush(ToD2D(TitleBar(model.variant)),
                                             titleBrush.GetAddressOf()))) {
      return;
    }
    if (FAILED(target->CreateSolidColorBrush(ToD2D(TextPrimary(model.variant)),
                                             primaryBrush.GetAddressOf()))) {
      return;
    }
    if (FAILED(target->CreateSolidColorBrush(ToD2D(TextSecondary(model.variant)),
                                             secondaryBrush.GetAddressOf()))) {
      return;
    }
    textBrush_ = primaryBrush;
    subBrush_ = secondaryBrush;

    const D2D1_RECT_F cardRect = D2D1::RectF(0.5f * s, 0.5f * s, model.width * s - 0.5f * s,
                                             model.height * s - 0.5f * s);
    DrawSquircleFill(target, cardRect, static_cast<float>(m.cardRadius) * s, cardBrush.Get());

    const D2D1_RECT_F titleRect =
        D2D1::RectF(0.5f * s, 0.5f * s, model.width * s - 0.5f * s,
                    static_cast<float>(m.titleBarHeight) * s);
    DrawSquircleFill(target, titleRect, static_cast<float>(m.cardRadius) * s * 0.7f,
                     titleBrush.Get());
    target->FillRectangle(
        D2D1::RectF(titleRect.left, titleRect.bottom - m.cardRadius * s * 0.5f, titleRect.right,
                    titleRect.bottom),
        titleBrush.Get());
    DrawSquircleStroke(target, cardRect, static_cast<float>(m.cardRadius) * s,
                       ToD2D(BorderTop(model.variant)), 1.0f);

    primaryBrush->SetColor(ToD2D(TextPrimary(model.variant)));
    DrawLabelText(target, model.title,
                  D2D1::RectF(m.padding * s, 0.0f,
                              (model.width - m.closeButton - m.padding * 2.0f) * s,
                              static_cast<float>(m.titleBarHeight) * s),
                  true);
    DrawCloseButton(target, model);

    for (const Widget& widget : model.widgets) {
      switch (widget.kind) {
        case WidgetKind::Button: DrawButton(target, model, widget); break;
        case WidgetKind::CheckBox: DrawCheckBox(target, model, widget); break;
        case WidgetKind::Slider: DrawSlider(target, model, widget); break;
        case WidgetKind::Combo: DrawCombo(target, model, widget); break;
        case WidgetKind::TextField: DrawTextField(target, model, widget); break;
        case WidgetKind::Label: DrawPlainLabel(target, model, widget, false); break;
        case WidgetKind::Hint: DrawPlainLabel(target, model, widget, true); break;
        case WidgetKind::Title: break;
      }
    }

    if (!model.footer.empty()) {
      secondaryBrush->SetColor(ToD2D(HintColor(model.variant, model.footerError)));
      DrawLabelText(target, model.footer,
                    D2D1::RectF(m.padding * s,
                                (model.height - m.padding - m.hintHeight * 2.0f) * s,
                                (model.width - m.padding) * s,
                                (model.height - m.padding) * s),
                    false);
    }
  }

  // Counters: the pixel test proves the primitives reached the target, not just
  // that a function was called.
  int paintedButtons = 0;
  int paintedSliders = 0;
  int paintedCombos = 0;
  int paintedDisabled = 0;

 private:
  void DrawSquircleFill(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                        ID2D1Brush* brush) {
    if (!factory_ || brush == nullptr) return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(CreateSquircleGeometry(factory_.Get(), rect, radius, geometry.GetAddressOf()))) {
      return;
    }
    target->FillGeometry(geometry.Get(), brush);
  }

  void DrawSquircleStroke(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                          const D2D1_COLOR_F& color, float width) {
    if (!factory_) return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(CreateSquircleGeometry(factory_.Get(), rect, radius, geometry.GetAddressOf()))) {
      return;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    target->DrawGeometry(geometry.Get(), brush.Get(), width);
  }

  void DrawTextRaw(ID2D1RenderTarget* target, const std::wstring& text, const D2D1_RECT_F& rect,
                   ID2D1Brush* brush) {
    if (text.empty() || !text_ || brush == nullptr) return;
    target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), text_.Get(), rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  void DrawLabelText(ID2D1RenderTarget* target, const std::wstring& text,
                     const D2D1_RECT_F& rect, bool title) {
    if (!text_) return;
    text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    static_cast<void>(title);
    DrawTextRaw(target, text, rect, title ? textBrush_.Get() : subBrush_.Get());
  }

  void DrawCloseButton(ID2D1RenderTarget* target, const ChromeModel& model) {
    const Metrics m = model.metrics();
    const float s = m.scale;
    const D2D1_RECT_F r = ScaleRect(model.CloseRect(), s);
    const VisualState state = ResolveState(true, model.closeHover, model.closePressed, false);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(
            ToD2D(WithAlpha(TextSecondary(model.variant),
                            state == VisualState::Hover ? 0.28f : 0.12f)),
            brush.GetAddressOf()))) {
      return;
    }
    DrawSquircleFill(target, r, static_cast<float>(m.cardRadius) * s * 0.5f, brush.Get());
    ComPtr<ID2D1SolidColorBrush> glyph;
    if (FAILED(target->CreateSolidColorBrush(ToD2D(TextPrimary(model.variant)),
                                             glyph.GetAddressOf()))) {
      return;
    }
    const float inset = 9.0f * s;
    target->DrawLine(D2D1::Point2F(r.left + inset, r.top + inset),
                     D2D1::Point2F(r.right - inset, r.bottom - inset), glyph.Get(), 1.6f);
    target->DrawLine(D2D1::Point2F(r.right - inset, r.top + inset),
                     D2D1::Point2F(r.left + inset, r.bottom - inset), glyph.Get(), 1.6f);
  }

  void DrawFocusRing(ID2D1RenderTarget* target, const ChromeModel& model,
                     const D2D1_RECT_F& rect, bool focused) {
    if (!focused) return;
    const Metrics m = model.metrics();
    const float s = m.scale;
    DrawSquircleStroke(target,
                       D2D1::RectF(rect.left - 2.0f * s, rect.top - 2.0f * s,
                                   rect.right + 2.0f * s, rect.bottom + 2.0f * s),
                       (rect.bottom - rect.top) * 0.5f + 2.0f * s,
                       ToD2D(Accent(model.variant)), 2.0f);
  }

  void DrawButton(ID2D1RenderTarget* target, const ChromeModel& model, const Widget& widget) {
    ++paintedButtons;
    if (widget.state == VisualState::Disabled) ++paintedDisabled;
    const Metrics m = model.metrics();
    const D2D1_RECT_F rect = ScaleRect(widget.rect, m.scale);
    ComPtr<ID2D1SolidColorBrush> fill;
    target->CreateSolidColorBrush(ToD2D(ButtonFill(model.variant, widget.state, widget.primary)),
                                  fill.GetAddressOf());
    DrawSquircleFill(target, rect, static_cast<float>(m.buttonRadius) * m.scale, fill.Get());
    ComPtr<ID2D1SolidColorBrush> label;
    target->CreateSolidColorBrush(ToD2D(ButtonText(model.variant, widget.state, widget.primary)),
                                  label.GetAddressOf());
    text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawTextRaw(target, widget.label, rect, label.Get());
    text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawFocusRing(target, model, rect, widget.focused);
  }

  void DrawCheckBox(ID2D1RenderTarget* target, const ChromeModel& model, const Widget& widget) {
    if (widget.state == VisualState::Disabled) ++paintedDisabled;
    const Metrics m = model.metrics();
    const float s = m.scale;
    const D2D1_RECT_F rect = ScaleRect(widget.rect, s);
    const float side = static_cast<float>(m.checkBox) * s;
    const D2D1_RECT_F box =
        D2D1::RectF(rect.left, rect.top + (rect.bottom - rect.top - side) * 0.5f,
                    rect.left + side, rect.top + (rect.bottom - rect.top + side) * 0.5f);
    ComPtr<ID2D1SolidColorBrush> fill;
    target->CreateSolidColorBrush(
        ToD2D(widget.checked ? Accent(model.variant) : ComboFill(model.variant, widget.state)),
        fill.GetAddressOf());
    DrawSquircleFill(target, box, side * 0.32f, fill.Get());
    if (widget.checked) {
      ComPtr<ID2D1SolidColorBrush> tick;
      target->CreateSolidColorBrush(ToD2D(TextPrimary(model.variant)), tick.GetAddressOf());
      target->DrawLine(D2D1::Point2F(box.left + side * 0.24f, box.top + side * 0.52f),
                       D2D1::Point2F(box.left + side * 0.44f, box.top + side * 0.72f),
                       tick.Get(), 1.8f);
      target->DrawLine(D2D1::Point2F(box.left + side * 0.44f, box.top + side * 0.72f),
                       D2D1::Point2F(box.left + side * 0.78f, box.top + side * 0.30f),
                       tick.Get(), 1.8f);
    }
    ComPtr<ID2D1SolidColorBrush> label;
    target->CreateSolidColorBrush(
        ToD2D(widget.state == VisualState::Disabled
                  ? WithAlpha(TextSecondary(model.variant), 0.55f)
                  : TextPrimary(model.variant)),
        label.GetAddressOf());
    DrawTextRaw(target, widget.label,
                D2D1::RectF(box.right + 8.0f * s, rect.top, rect.right, rect.bottom),
                label.Get());
    DrawFocusRing(target, model, rect, widget.focused);
    if (!widget.hint.empty() && widget.state == VisualState::Disabled) {
      ComPtr<ID2D1SolidColorBrush> hint;
      target->CreateSolidColorBrush(ToD2D(HintColor(model.variant, false)), hint.GetAddressOf());
      DrawTextRaw(target, widget.hint,
                  D2D1::RectF(rect.left, rect.bottom, model.width * s - m.padding * s,
                              rect.bottom + m.hintHeight * s),
                  hint.Get());
    }
  }

  void DrawSlider(ID2D1RenderTarget* target, const ChromeModel& model, const Widget& widget) {
    ++paintedSliders;
    if (widget.state == VisualState::Disabled) ++paintedDisabled;
    const Metrics m = model.metrics();
    const float s = m.scale;
    const D2D1_RECT_F rect = ScaleRect(widget.rect, s);
    const float trackH = static_cast<float>(m.sliderTrack) * s;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float knob = static_cast<float>(m.knobSize) * s;
    const D2D1_RECT_F track = D2D1::RectF(rect.left + knob * 0.5f, cy - trackH * 0.5f,
                                          rect.right - knob * 0.5f, cy + trackH * 0.5f);
    ComPtr<ID2D1SolidColorBrush> trackBrush;
    target->CreateSolidColorBrush(ToD2D(TrackColor(model.variant, widget.state)),
                                  trackBrush.GetAddressOf());
    target->FillRoundedRectangle(D2D1::RoundedRect(track, trackH * 0.5f, trackH * 0.5f),
                                 trackBrush.Get());
    const D2D1_RECT_F fill = SliderFillRect(track, widget.level, widget.minLevel, widget.maxLevel);
    ComPtr<ID2D1SolidColorBrush> fillBrush;
    target->CreateSolidColorBrush(ToD2D(SliderFillColor(model.variant, widget.state)),
                                  fillBrush.GetAddressOf());
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(fill.left, fill.top,
                                       (std::max)(fill.left + trackH, fill.right), fill.bottom),
                          trackH * 0.5f, trackH * 0.5f),
        fillBrush.Get());
    const float kx = fill.right;
    const D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(kx, cy), knob * 0.5f, knob * 0.5f);
    target->FillEllipse(ellipse, fillBrush.Get());
    target->DrawEllipse(ellipse, trackBrush.Get(), 1.0f);
    ComPtr<ID2D1SolidColorBrush> dotBrush;
    target->CreateSolidColorBrush(
        ToD2D(widget.state == VisualState::Disabled
                  ? WithAlpha(TextSecondary(model.variant), 0.45f)
                  : TextPrimary(model.variant)),
        dotBrush.GetAddressOf());
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), knob * 0.18f, knob * 0.18f),
                        dotBrush.Get());
    ComPtr<ID2D1SolidColorBrush> valueBrush;
    target->CreateSolidColorBrush(
        ToD2D(widget.state == VisualState::Disabled
                  ? WithAlpha(TextSecondary(model.variant), 0.55f)
                  : TextPrimary(model.variant)),
        valueBrush.GetAddressOf());
    DrawTextRaw(target, widget.value,
                D2D1::RectF(rect.right, rect.top, rect.right + 40.0f * s, rect.bottom),
                valueBrush.Get());
    DrawFocusRing(target, model, rect, widget.focused);
  }

  void DrawCombo(ID2D1RenderTarget* target, const ChromeModel& model, const Widget& widget) {
    ++paintedCombos;
    if (widget.state == VisualState::Disabled) ++paintedDisabled;
    const Metrics m = model.metrics();
    const float s = m.scale;
    const D2D1_RECT_F rect = ScaleRect(widget.rect, s);
    ComPtr<ID2D1SolidColorBrush> fill;
    target->CreateSolidColorBrush(ToD2D(ComboFill(model.variant, widget.state)),
                                  fill.GetAddressOf());
    DrawSquircleFill(target, rect, static_cast<float>(m.comboRadius) * s, fill.Get());
    DrawSquircleStroke(target, rect, static_cast<float>(m.comboRadius) * s,
                       ToD2D(WithAlpha(BorderTop(model.variant), 0.55f)), 1.0f);
    ComPtr<ID2D1SolidColorBrush> label;
    target->CreateSolidColorBrush(
        ToD2D(widget.state == VisualState::Disabled
                  ? WithAlpha(TextSecondary(model.variant), 0.55f)
                  : TextPrimary(model.variant)),
        label.GetAddressOf());
    DrawTextRaw(target, widget.value,
                D2D1::RectF(rect.left + 10.0f * s, rect.top, rect.right - 24.0f * s,
                            rect.bottom),
                label.Get());
    ComPtr<ID2D1SolidColorBrush> chevron;
    target->CreateSolidColorBrush(
        ToD2D(widget.state == VisualState::Disabled
                  ? WithAlpha(TextSecondary(model.variant), 0.45f)
                  : TextSecondary(model.variant)),
        chevron.GetAddressOf());
    const float cx = rect.right - 14.0f * s;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    target->DrawLine(D2D1::Point2F(cx - 4.0f * s, cy - 2.0f * s),
                     D2D1::Point2F(cx, cy + 2.0f * s), chevron.Get(), 1.4f);
    target->DrawLine(D2D1::Point2F(cx, cy + 2.0f * s),
                     D2D1::Point2F(cx + 4.0f * s, cy - 2.0f * s), chevron.Get(), 1.4f);
    DrawFocusRing(target, model, rect, widget.focused);
  }

  void DrawTextField(ID2D1RenderTarget* target, const ChromeModel& model,
                     const Widget& widget) {
    if (widget.state == VisualState::Disabled) ++paintedDisabled;
    const Metrics m = model.metrics();
    const float s = m.scale;
    const D2D1_RECT_F rect = ScaleRect(widget.rect, s);
    ComPtr<ID2D1SolidColorBrush> fill;
    target->CreateSolidColorBrush(
        ToD2D(Composite(ComboFill(model.variant, widget.state),
                        Backdrop(model.variant))),
        fill.GetAddressOf());
    DrawSquircleFill(target, rect, static_cast<float>(m.comboRadius) * s, fill.Get());
    DrawSquircleStroke(target, rect, static_cast<float>(m.comboRadius) * s,
                       ToD2D(WithAlpha(BorderTop(model.variant),
                                       widget.focused ? 0.85f : 0.40f)),
                       widget.focused ? 1.5f : 1.0f);
    ComPtr<ID2D1SolidColorBrush> text;
    target->CreateSolidColorBrush(
        ToD2D(widget.state == VisualState::Disabled
                  ? WithAlpha(TextSecondary(model.variant), 0.55f)
                  : TextPrimary(model.variant)),
        text.GetAddressOf());
    const std::wstring shown = widget.value.empty() ? widget.label : widget.value;
    DrawTextRaw(target, shown,
                D2D1::RectF(rect.left + 8.0f * s, rect.top, rect.right - 8.0f * s,
                            rect.bottom),
                text.Get());
    DrawFocusRing(target, model, rect, widget.focused);
  }

  void DrawPlainLabel(ID2D1RenderTarget* target, const ChromeModel& model,
                      const Widget& widget, bool hint) {
    const Metrics m = model.metrics();
    const float s = m.scale;
    const D2D1_RECT_F rect = ScaleRect(widget.rect, s);
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(
        ToD2D(hint ? HintColor(model.variant, widget.error)
                   : (widget.state == VisualState::Disabled
                          ? WithAlpha(TextSecondary(model.variant), 0.55f)
                          : TextSecondary(model.variant))),
        brush.GetAddressOf());
    DrawTextRaw(target, widget.label,
                D2D1::RectF(rect.left, rect.top, rect.right,
                            rect.bottom + m.hintHeight * s),
                brush.Get());
  }

  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<IDWriteTextFormat> text_;
  ComPtr<ID2D1SolidColorBrush> textBrush_;
  ComPtr<ID2D1SolidColorBrush> subBrush_;
  int bodySize_ = 13;
};

}  // namespace prism::win::chrome
