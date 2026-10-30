// PrismArchive -- production Direct2D 1.3 custom effect.
// Compile with fxc.exe /T ps_5_0 /D D2D_FULL_SHADER (requires the Windows SDK
// d2d1effecthelpers.hlsli on the include path). CI compiles the standalone
// variant (LiquidGlassPreviewPS.hlsl) as a permanent compile guard.
#define D2D_INPUT_COUNT 1
#define D2D_INPUT0_COMPLEX
#define D2D_REQUIRES_SCENE_POSITION
#include "d2d1effecthelpers.hlsli"
#include "LiquidGlassMath.hlsl"

D2D_PS_ENTRY(main) {
  float2 fragPos = D2DGetScenePosition().xy;
  float2 localPos = fragPos - u_panelCenter;

  float dist = sdRoundedBox(localPos, u_panelHalfSize, u_cornerRadius);
  if (dist > 0.5) return float4(0.0, 0.0, 0.0, 0.0);

  // Layer 2 (Kawase blur) is a separate effect in the graph; here we consume
  // its already-blurred output and apply the lens + vibrancy + rim layers.
  float2 baseUV = D2DGetInputCoordinate(0).xy;
  float2 offset = RefractionOffset(localPos, dist) / u_texelSize;

  float rCh = D2DSampleInputAtPosition(0, fragPos + offset * 0.88).r;
  float gCh = D2DSampleInputAtPosition(0, fragPos + offset * 1.00).g;
  float bCh = D2DSampleInputAtPosition(0, fragPos + offset * 1.14).b;

  return ShadeGlass(fragPos, localPos, dist, float3(rCh, gCh, bCh));
}
