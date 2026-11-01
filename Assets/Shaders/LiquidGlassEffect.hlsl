// PrismArchive -- production Direct2D 1.3 custom effect ("PrismLiquidGlass").
//
// This is the real effect pixel shader: Direct2D 1.3 instantiates it through
// ID2D1DrawInfo::SetPixelShader after ID2D1Factory1::RegisterEffectFromString,
// so the five-layer optics from docs/BLUEPRINT ch. 3.1 run on the GPU instead
// of being approximated with D2D brushes. The host side lives in
// Src/PrismWin/GlassIslandHost.hpp (GlassEffectRuntime / GlassChain).
//
// Compile (exactly what CI runs, no SDK include path needed):
//   fxc /nologo /T ps_5_0 /E main Assets/Shaders/LiquidGlassEffect.hlsl
//
// The cbuffer layout is a CONTRACT (A6): 5 registers / 20 floats, serialized by
// prism::glass::GlassIslandScene::WriteCbuffer and bit-asserted in
// Tests/test_glass_island.cpp. Do not reorder a single field.
#include "PrismD2DEffectEntry.hlsli"
#include "LiquidGlassMath.hlsl"

D2D_PS_ENTRY(main) {
  // Scene space == island pixels: the effect runs 1:1 against the island
  // surface, so D2D's scene position is already our pixel grid.
  float2 fragPos = D2DGetScenePosition().xy;
  float2 localPos = fragPos - u_panelCenter;
  float dist = sdRoundedBox(localPos, u_panelHalfSize, u_cornerRadius);
  if (dist > 0.5) return float4(0.0, 0.0, 0.0, 0.0);

  // Layer 1 -- chromatic dispersion. RefractionOffset is normalized (shared
  // with the standalone preview shader), so divide by the texel size to get a
  // pixel offset in the island's own space.
  float2 offsetPx = RefractionOffset(localPos, dist) / u_texelSize;
  float rCh = D2DSampleInputAtOffset(0, offsetPx * 0.88).r;
  float gCh = D2DSampleInputAtOffset(0, offsetPx * 1.00).g;
  float bCh = D2DSampleInputAtOffset(0, offsetPx * 1.14).b;

  // Layers 2-5 (vibrancy, tint body, Fresnel rim + pointer glow, dither) are
  // the shared math, so the GPU path and the WebGL2 reference cannot drift.
  return ShadeGlass(fragPos, localPos, dist, float3(rCh, gCh, bCh));
}
