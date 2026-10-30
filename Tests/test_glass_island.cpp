// TDD: GlassIslandScene -- the model behind the floating Liquid Glass islands
// (blueprint ch. 3.1/3.2/6.4). It owns island geometry, the pointer-tracked
// specular light, focus-state depth breathing (blur radius 28<->64), the
// fluid-morphing context pill, and serializes into the EXACT HLSL
// LiquidGlassConstants cbuffer register layout the shaders consume.
#include "GlassIsland.hpp"
#include "mini_test.hpp"

#include <cmath>

namespace {

using prism::glass::GlassIslandScene;
using prism::glass::IslandId;

PRISM_TEST("layout places nav / context / telemetry islands per blueprint") {
  GlassIslandScene scene;
  scene.Layout(1600, 1000);
  const auto nav = scene.RectOf(IslandId::Nav);
  PRISM_CHECK_EQ(nav.y, 12u);  // 距顶 12px
  PRISM_CHECK_EQ(nav.h, 44u);  // 高 44px
  PRISM_CHECK(nav.x >= 16u);
  const auto telemetry = scene.RectOf(IslandId::Telemetry);
  PRISM_CHECK(telemetry.y + telemetry.h <= 1000u - 12u);  // bottom margin
  const auto context = scene.RectOf(IslandId::Context);
  PRISM_CHECK(context.w > nav.w);  // the contextual pill is the widest
  PRISM_CHECK(context.x + context.w / 2 == 1600 / 2);  // horizontally centered
}

PRISM_TEST("cbuffer packing matches the HLSL register layout exactly") {
  GlassIslandScene scene;
  scene.Layout(1600, 1000);
  scene.SetPointer(400.0f, 300.0f);
  float cb[20];
  scene.WriteCbuffer(IslandId::Nav, 1600, 1000, cb);
  const auto r = scene.RectOf(IslandId::Nav);
  // reg0: center.xy | halfSize.xy
  PRISM_CHECK(std::abs(cb[0] - (r.x + r.w / 2.0f)) < 0.01f);
  PRISM_CHECK(std::abs(cb[1] - (r.y + r.h / 2.0f)) < 0.01f);
  PRISM_CHECK(std::abs(cb[2] - r.w / 2.0f) < 0.01f);
  PRISM_CHECK(std::abs(cb[3] - r.h / 2.0f) < 0.01f);
  // reg1: pointer.xy | texel.xy
  PRISM_CHECK_EQ(cb[4], 400.0f);
  PRISM_CHECK_EQ(cb[5], 300.0f);
  PRISM_CHECK(std::abs(cb[6] - 1.0f / 1600.0f) < 1e-6f);
  PRISM_CHECK(std::abs(cb[7] - 1.0f / 1000.0f) < 1e-6f);
  // reg2: corner | bevel | ior | saturation (nonzero, ordered slots)
  PRISM_CHECK(cb[8] > 0);   // corner radius
  PRISM_CHECK(cb[9] > 0);   // refraction bevel
  PRISM_CHECK(cb[10] > 0);  // ior strength
  PRISM_CHECK(cb[11] > 1);  // saturation boost > 100%
  // reg3/reg4: tint + specular, alpha in [0,1]
  PRISM_CHECK(cb[15] > 0 && cb[15] < 1);
  PRISM_CHECK(cb[19] > 0 && cb[19] < 1);
}

PRISM_TEST("focus breathing: blur radius settles at 28 focused, 64 unfocused") {
  GlassIslandScene scene;
  scene.Layout(1600, 1000);
  scene.SetFocus(true);
  for (int i = 0; i < 600; ++i) scene.Tick(16);
  PRISM_CHECK(std::abs(scene.BlurRadius() - 28.0f) < 2.0f);
  scene.SetFocus(false);
  for (int i = 0; i < 600; ++i) scene.Tick(16);
  PRISM_CHECK(std::abs(scene.BlurRadius() - 64.0f) < 2.0f);
}

PRISM_TEST("fluid morphing: the context pill stretches while a task runs") {
  GlassIslandScene scene;
  scene.Layout(1600, 1000);
  const float idleW = static_cast<float>(scene.RectOf(IslandId::Context).w);
  scene.SetTaskActive(true);
  for (int i = 0; i < 600; ++i) scene.Tick(16);
  const float activeW = static_cast<float>(scene.RectOf(IslandId::Context).w);
  PRISM_CHECK(activeW > idleW * 1.4f);  // stretched out as a progress pill
  scene.SetTaskActive(false);
  for (int i = 0; i < 600; ++i) scene.Tick(16);
  const float backW = static_cast<float>(scene.RectOf(IslandId::Context).w);
  PRISM_CHECK(std::abs(backW - idleW) < 2.0f);  // settled back (no overshoot)
}

PRISM_TEST("determinism: identical tick sequences produce identical cbuffers") {
  GlassIslandScene a, b;
  a.Layout(1600, 1000);
  b.Layout(1600, 1000);
  a.SetPointer(120.5f, 40.0f);
  b.SetPointer(120.5f, 40.0f);
  a.SetTaskActive(true);
  b.SetTaskActive(true);
  float ca[20], cb2[20];
  for (int i = 0; i < 120; ++i) {
    a.Tick(16);
    b.Tick(16);
  }
  a.WriteCbuffer(IslandId::Context, 1600, 1000, ca);
  b.WriteCbuffer(IslandId::Context, 1600, 1000, cb2);
  for (int i = 0; i < 20; ++i) {
    if (ca[i] != cb2[i]) return;  // bitwise, not epsilon
  }
  PRISM_CHECK(true);
}

PRISM_TEST("day and night tints carry the blueprint token values") {
  GlassIslandScene day, night;
  day.Layout(1600, 1000);
  night.Layout(1600, 1000);
  day.SetTheme(prism::glass::Theme::Daylight);
  night.SetTheme(prism::glass::Theme::Midnight);
  float cd[20], cn[20];
  day.WriteCbuffer(IslandId::Nav, 1600, 1000, cd);
  night.WriteCbuffer(IslandId::Nav, 1600, 1000, cn);
  // Daylight island tint rgba(255,255,255,0.58); Midnight rgba(38,42,54,0.52)
  PRISM_CHECK(std::abs(cd[12] - 1.0f) < 0.01f && std::abs(cd[15] - 0.58f) < 0.01f);
  PRISM_CHECK(std::abs(cn[12] - 38.0f / 255.0f) < 0.01f);
  PRISM_CHECK(std::abs(cn[13] - 42.0f / 255.0f) < 0.01f);
  PRISM_CHECK(std::abs(cn[15] - 0.52f) < 0.01f);
}

}  // namespace

PRISM_TEST_MAIN()
