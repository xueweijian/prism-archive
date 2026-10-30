// PrismArchive -- Layer 2 of the Liquid Glass pipeline (blueprint ch. 3.1):
// dual-pass Kawase blur with a dynamic radius (28 px ~ 64 px) and a depth
// falloff that breathes with window focus.
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
