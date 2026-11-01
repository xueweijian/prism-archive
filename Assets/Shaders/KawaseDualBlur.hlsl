// PrismArchive -- Layer 2 of the Liquid Glass pipeline (blueprint ch. 3.1):
// dual-pass Kawase blur with a dynamic radius (28 px ~ 64 px) and a depth
// falloff that breathes with window focus.
//
// TWO consumers, one file, so the math cannot fork:
//   1. the Direct2D 1.3 custom effect "PrismKawaseBlur"  (PRISM_D2D_EFFECT)
//   2. the standalone ps_5_0 reference shader CI compiles as a compile guard
//      (fxc /T ps_5_0 /E PSMain), the same entry the WebGL2 preview mirrors.
#if defined(PRISM_D2D_EFFECT)
// ---------------------------------------------------------------- D2D effect
#include "PrismD2DEffectEntry.hlsli"

// One register: x = tap offset in pixels (radius), y = pass scale
// (1.0 = first pass, 0.5 = second pass), zw unused. Matches
// GlassEffectRuntime's KawaseConstants struct byte for byte (A6).
cbuffer KawaseConstants : register(b0) {
  float4 u_kawase;  // radiusPx, passScale, 0, 0
};

D2D_PS_ENTRY(main) {
  const float2 o = u_kawase.xx * u_kawase.y;
  float4 sum = D2DSampleInputAtOffset(0, float2(0.0, 0.0));
  sum += D2DSampleInputAtOffset(0, float2(o.x, o.y));
  sum += D2DSampleInputAtOffset(0, float2(-o.x, o.y));
  sum += D2DSampleInputAtOffset(0, float2(o.x, -o.y));
  sum += D2DSampleInputAtOffset(0, float2(-o.x, -o.y));
  return sum / 5.0;
}

#else
// ------------------------------------------------- standalone reference shader
#include "LiquidGlassMath.hlsl"

Texture2D Source : register(t0);
SamplerState SourceSampler : register(s0);

cbuffer KawaseConstants : register(b1) {
  float2 u_sourceTexel;   // 1 / source size
  float  u_radiusPx;      // blur radius in pixels
  float  u_passIndex;     // 0 = first downsample, 1 = second
};

float4 PSMain(float4 pos : SV_POSITION) : SV_Target {
  float2 uv = pos.xy * u_sourceTexel * (u_passIndex > 0.5 ? 0.5 : 1.0);
  float2 halfTexel = u_sourceTexel * 0.5;

  // Kawase: four diagonal taps at a growing offset approximate a wide Gaussian
  // with a fraction of the samples.
  float2 o = halfTexel * (1.0 + u_radiusPx * 0.25);
  float4 sum = Source.Sample(SourceSampler, uv);
  sum += Source.Sample(SourceSampler, uv + float2(o.x, o.y));
  sum += Source.Sample(SourceSampler, uv + float2(-o.x, o.y));
  sum += Source.Sample(SourceSampler, uv + float2(o.x, -o.y));
  sum += Source.Sample(SourceSampler, uv + float2(-o.x, -o.y));
  return sum / 5.0;
}
#endif
