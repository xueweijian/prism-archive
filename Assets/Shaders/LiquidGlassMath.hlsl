// PrismArchive -- Apple iOS 26 "Liquid Glass" optics, shared math
// Ports blueprint ch. 3.1 (five-layer pipeline) and ch. 3.3 (design tokens).
//
// This file holds pure functions only: no textures, no entry point. It is
// included by
//   * LiquidGlassEffect.hlsl    -- the production Direct2D 1.3 custom effect
//   * LiquidGlassPreviewPS.hlsl -- a plain ps_5_0 entry point that CI compiles
//     with fxc.exe on every push to prove the math still compiles.
#ifndef PRISM_LIQUID_GLASS_MATH_HLSL
#define PRISM_LIQUID_GLASS_MATH_HLSL

cbuffer LiquidGlassConstants : register(b0) {
  float2 u_panelCenter;      // floating island center, pixels
  float2 u_panelHalfSize;    // (w/2, h/2)
  float2 u_pointerPos;       // cursor position, drives the interactive rim
  float2 u_texelSize;        // (1/width, 1/height)
  float  u_cornerRadius;     // squircle radius, e.g. 22 px
  float  u_refractionBevel;  // lens band width, e.g. 8 px
  float  u_iorStrength;      // refraction offset strength, e.g. 14 px
  float  u_saturationBoost;  // Apple Vibrancy gain, e.g. 1.48
  float4 u_tintColor;        // glass body tint (daylight / midnight tokens)
  float4 u_specularColor;    // Fresnel rim colour + intensity
};

// Signed distance field of a rounded box (G2-continuous squircle corner).
float sdRoundedBox(float2 p, float2 b, float r) {
  float2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Surface normal of the SDF: this is what turns a flat rounded rect into a
// lens with real edge thickness.
float2 calcSDFGradient(float2 p, float2 b, float r) {
  const float eps = 0.75;
  float dx = sdRoundedBox(p + float2(eps, 0.0), b, r) - sdRoundedBox(p - float2(eps, 0.0), b, r);
  float dy = sdRoundedBox(p + float2(0.0, eps), b, r) - sdRoundedBox(p - float2(0.0, eps), b, r);
  return normalize(float2(dx, dy) + 1e-6);
}

// Interleaved gradient noise: kills banding in large translucent gradients
// without the visible pattern of an ordered dither.
float interleavedGradientNoise(float2 screenPos) {
  float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
  return frac(magic.z * frac(dot(screenPos, magic.xy)));
}

// Layer 3 -- Apple Vibrancy: push saturation away from luma.
float3 Vibrancy(float3 color, float boost) {
  float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
  return lerp(float3(luma, luma, luma), color, boost);
}

// Layer 5 -- Fresnel rim + cursor tracking highlight.
//   I_rim = base + peak * exp(-|p - m|^2 / 2 sigma^2) * max(0, n . L)
float SpecularRim(float2 fragPos, float2 localPos, float2 halfSize, float radius,
                  float dist, float2 normal2D) {
  float rimMask = smoothstep(-1.8, -0.2, dist) * (1.0 - smoothstep(-0.2, 0.5, dist));
  float2 lightDir = normalize(float2(-0.55, -0.83));  // ~56 deg top-left sky light
  float directionalRim = saturate(dot(normal2D, lightDir) * 0.65 + 0.35);
  float pointerDist = length(fragPos - u_pointerPos);
  float pointerGlow = exp(-(pointerDist * pointerDist) / (2.0 * 140.0 * 140.0));
  return rimMask * (directionalRim * 0.75 + pointerGlow * 0.95);
}

// Full shade for one pixel. `refracted` is the (already chromatically sampled)
// backdrop colour; `dist` is the SDF value at this pixel.
float4 ShadeGlass(float2 fragPos, float2 localPos, float dist, float3 refracted) {
  if (dist > 0.5) return float4(0, 0, 0, 0);  // anti-aliased clip outside the panel
  float edgeAlpha = saturate(0.5 - dist);

  float3 vibrant = Vibrancy(refracted, u_saturationBoost);
  float3 glassBody = lerp(vibrant, u_tintColor.rgb, u_tintColor.a);

  float2 normal2D = calcSDFGradient(localPos, u_panelHalfSize, u_cornerRadius);
  float specular = SpecularRim(fragPos, localPos, u_panelHalfSize, u_cornerRadius, dist, normal2D);
  float3 rgb = glassBody + u_specularColor.rgb * specular * u_specularColor.a;

  float dither = (interleavedGradientNoise(fragPos) - 0.5) * (1.5 / 255.0);
  rgb = saturate(rgb + dither);
  return float4(rgb * edgeAlpha, edgeAlpha);
}

// Layer 1 -- chromatic dispersion: R/G/B are sampled with different refractive
// indices (eta 1.012 / 1.018 / 1.025 in the blueprint, expressed here as the
// 0.88 / 1.00 / 1.14 offset scale).
float2 RefractionOffset(float2 localPos, float dist) {
  float bevelFactor = smoothstep(-u_refractionBevel, 0.0, dist);
  float2 normal2D = calcSDFGradient(localPos, u_panelHalfSize, u_cornerRadius);
  return -normal2D * (bevelFactor * bevelFactor) * u_iorStrength * u_texelSize;
}

#endif  // PRISM_LIQUID_GLASS_MATH_HLSL
