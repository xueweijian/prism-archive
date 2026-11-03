// PrismArchive -- PrismCore
// GlassIslandScene: the model behind the floating Liquid Glass islands
// (blueprint ch. 3.1/3.2). It owns island geometry (nav 44px @ 12px from top,
// contextual pill centered, telemetry pill at the bottom), the pointer light,
// focus-state depth breathing (blur radius 28 <-> 64, spring-interpolated),
// the fluid-morphing context pill stretch, and serializes into the EXACT
// HLSL LiquidGlassConstants cbuffer layout (Assets/Shaders/
// LiquidGlassEffect.hlsl) -- 5 registers, 20 floats:
//   reg0 center.xy|halfSize.xy  reg1 pointer.xy|texel.xy
//   reg2 corner|bevel|ior|sat   reg3 tint.rgba  reg4 specular.rgba
//
// Determinism is a contract (identical tick sequences -> bitwise identical
// cbuffers), so the composition layer stays testable without a GPU.
#pragma once

#include <cstdint>

#include "SpringAnimator.hpp"
#include "ThemeTokens.hpp"

namespace prism::glass {

enum class IslandId : uint8_t { Nav, Context, Telemetry };
enum class Theme : uint8_t { Daylight, Midnight };

struct IslandRect {
  uint32_t x = 0, y = 0, w = 0, h = 0;
};

inline constexpr double kBlurFocused = 28.0;
inline constexpr double kBlurUnfocused = 64.0;

class GlassIslandScene {
 public:
  void Layout(uint32_t canvasW, uint32_t canvasH) {
    canvasW_ = canvasW;
    canvasH_ = canvasH;
    nav_ = {16, 12, canvasW / 3, 44};
    contextIdleW_ = static_cast<double>(canvasW / 2);
    context_ = {0, canvasH / 5, canvasW / 2, 48};
    telemetry_ = {canvasW / 4, canvasH - 12 - 32, canvasW / 2, 32};
    const double now = ClockSeconds();
    contextW_.Retarget(contextIdleW_, now);
    PlaceContext(contextIdleW_);
  }

  void SetPointer(float x, float y) {
    pointerX_ = x;
    pointerY_ = y;
  }

  void SetFocus(bool focused) {
    blur_.Retarget(focused ? kBlurFocused : kBlurUnfocused, ClockSeconds());
  }

  void SetTheme(Theme theme) { theme_ = theme; }

  // Fluid morphing (blueprint ch. 3.4): the contextual pill stretches into a
  // progress pill while a task runs, then settles back without overshoot.
  void SetTaskActive(bool active) {
    contextW_.Retarget(active ? contextIdleW_ * 1.6 : contextIdleW_, ClockSeconds());
  }

  // Deterministic fixed-dt advance: the scene keeps its own monotonic clock
  // (the animator takes absolute seconds), so identical Tick(dt) sequences
  // produce bitwise-identical state.
  void Tick(uint32_t dtMs) {
    clockMs_ += dtMs;
    const double now = ClockSeconds();
    blur_.Tick(now);
    contextW_.Tick(now);
    PlaceContext(contextW_.Value());
  }

  double BlurRadius() const { return blur_.Value(); }
  bool IsSettled() const { return blur_.IsSettled() && contextW_.IsSettled(); }

  IslandRect RectOf(IslandId id) const {
    switch (id) {
      case IslandId::Nav: return nav_;
      case IslandId::Context: return context_;
      case IslandId::Telemetry: return telemetry_;
    }
    return {};
  }

  // Serialized LiquidGlassConstants (see header comment for the layout).
  void WriteCbuffer(IslandId id, uint32_t contentW, uint32_t contentH,
                    float out[20]) const {
    const IslandRect r = RectOf(id);
    out[0] = r.x + r.w * 0.5f;
    out[1] = r.y + r.h * 0.5f;
    out[2] = r.w * 0.5f;
    out[3] = r.h * 0.5f;
    out[4] = pointerX_;
    out[5] = pointerY_;
    out[6] = 1.0f / static_cast<float>(contentW ? contentW : 1);
    out[7] = 1.0f / static_cast<float>(contentH ? contentH : 1);
    out[8] = static_cast<float>(r.h) * 0.5f;  // capsule corner = h/2
    out[9] = 8.0f;                            // refraction band px
    out[10] = 14.0f;                          // ior strength px
    out[11] = 1.48f;                          // vibrancy saturation
    // C71: reg3/reg4 come from the SAME token resolution every other painter
    // uses, so appearance.glassIntensity reaches the shader too. The literals
    // that used to live here (.58/.52, .85/.28) are exactly what these tokens
    // resolve to at the default intensity -- Tests/test_theme_tokens.cpp pins
    // the equality for both themes.
    const prism::theme::Variant tokenVariant =
        theme_ == Theme::Daylight ? prism::theme::Variant::Daylight
                                  : prism::theme::Variant::Midnight;
    const prism::theme::Rgba tint =
        prism::theme::EffectiveGlassSurface(tokenVariant);
    out[12] = tint.r;
    out[13] = tint.g;
    out[14] = tint.b;
    out[15] = tint.a;
    const prism::theme::Rgba specular =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, tokenVariant)
            .gradient.from;
    out[16] = specular.r;
    out[17] = specular.g;
    out[18] = specular.b;
    out[19] = specular.a;
  }

 private:
  double ClockSeconds() const { return clockMs_ / 1000.0; }
  void PlaceContext(double width) {
    context_.w = static_cast<uint32_t>(width);
    context_.x = (canvasW_ - context_.w) / 2;
  }

  uint32_t canvasW_ = 0, canvasH_ = 0;
  IslandRect nav_{}, context_{}, telemetry_{};
  float pointerX_ = 0, pointerY_ = 0;
  double contextIdleW_ = 0;
  double clockMs_ = 0;
  Theme theme_ = Theme::Daylight;
  // Depth breathing rides the fluid-sheet spring (no overshoot); the morph
  // stretch uses the same family so pills never bounce mid-task.
  SpringAnimator blur_{prism::kSpringFluidSheet, kBlurFocused};
  SpringAnimator contextW_{prism::kSpringFluidSheet, 0.0};
};

}  // namespace prism::glass
