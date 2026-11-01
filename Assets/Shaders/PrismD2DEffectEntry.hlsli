// PrismArchive -- the D2D 1.3 custom-effect full-shader entry contract.
//
// Why this file exists instead of #include "d2d1effecthelpers.hlsli":
// that SDK header only resolves when fxc is given the SDK include path, so
// "does the production effect compile?" could never be a self-contained gate.
// Vendoring the 20 lines we actually use makes the shader compile with a bare
//     fxc /T ps_5_0 /E main Assets/Shaders/LiquidGlassEffect.hlsl
// on any Windows SDK -- no /I, no environment -- so CI's compile guard stops
// being a SKIP.
//
// The semantics are byte-for-byte the SDK's (Include\<ver>\um\
// d2d1effecthelpers.hlsli, D2D_FULL_SHADER branch):
//   * one texture/sampler pair per effect input, at t0/s0, t1/s1 ...
//   * entry signature (float4 pos : SV_POSITION, float4 scenePos :
//     SCENE_POSITION, float4 uv0 : TEXCOORD0[, float4 uv1 : TEXCOORD1 ...])
//   * uv0.xy = input->scene offset, uv0.zw = input->scene scale, so
//     D2DSampleInputAtPosition(p) == uv0.xy + uv0.zw * (p - scenePos.xy)
// Verified against a live WARP device context (pixel-probe, then
// Tests/win/gallery_snapshot.cpp's refraction-band assertion).
#ifndef PRISM_D2D_EFFECT_ENTRY_HLSLI
#define PRISM_D2D_EFFECT_ENTRY_HLSLI

// PrismArchive's effects are single-input. Declaring more textures without
// using them only makes the reflection table lie.
Texture2D<float4> InputTexture0 : register(t0);
SamplerState InputSampler0 : register(s0);

static float4 __prism_scenePos = float4(0.0, 0.0, 0.0, 0.0);
static float4 __prism_uv0 = float4(0.0, 0.0, 1.0, 1.0);

inline float4 D2DGetScenePosition() { return __prism_scenePos; }

// uv0.zw is the scene->input scale, so multiplying by it turns a scene-space
// offset (pixels/DIPs) into a UV offset.
#define D2DSampleInputAtOffset(index, offsetPx) \
  InputTexture0.Sample(InputSampler0, __prism_uv0.xy + (offsetPx) * __prism_uv0.zw)

#define D2DSampleInputAtPosition(index, scenePosPx)                                 \
  InputTexture0.Sample(InputSampler0,                                               \
                       __prism_uv0.xy + __prism_uv0.zw * ((scenePosPx) - __prism_scenePos.xy))

// The shader body writes D2D_PS_ENTRY(main) { ... }; this turns it into
// main_Impl() and provides the real ps_5_0 entry point D2D reflects against.
#define D2D_PS_ENTRY(name) float4 name##_Impl()

float4 main_Impl();

float4 main(float4 pos : SV_POSITION,
            float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0) : SV_TARGET {
  __prism_scenePos = scenePos;
  __prism_uv0 = uv0;
  return main_Impl();
}

#endif  // PRISM_D2D_EFFECT_ENTRY_HLSLI
