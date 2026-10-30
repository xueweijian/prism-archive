// PrismArchive -- standalone ps_5_0 entry point for the Liquid Glass math.
// Exists so CI can prove the optics still compile with fxc.exe without needing
// the Direct2D effect helpers:  fxc /T ps_5_0 /E PSMain LiquidGlassPreviewPS.hlsl
#include "LiquidGlassMath.hlsl"

Texture2D Backdrop : register(t0);
SamplerState BackdropSampler : register(s0);

float4 PSMain(float4 pos : SV_POSITION) : SV_Target {
  float2 fragPos = pos.xy;
  float2 localPos = fragPos - u_panelCenter;
  float dist = sdRoundedBox(localPos, u_panelHalfSize, u_cornerRadius);
  if (dist > 0.5) return float4(0, 0, 0, 0);

  float2 offset = RefractionOffset(localPos, dist) / u_texelSize;
  float2 uv = fragPos * u_texelSize;
  float rCh = Backdrop.Sample(BackdropSampler, uv + offset * 0.88 * u_texelSize).r;
  float gCh = Backdrop.Sample(BackdropSampler, uv + offset * 1.00 * u_texelSize).g;
  float bCh = Backdrop.Sample(BackdropSampler, uv + offset * 1.14 * u_texelSize).b;

  return ShadeGlass(fragPos, localPos, dist, float3(rCh, gCh, bCh));
}
