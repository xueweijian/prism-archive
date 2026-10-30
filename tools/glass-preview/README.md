# PrismArchive Liquid Glass WebGL2 Reference

A zero-dependency visual reference for the PrismArchive DirectComposition/HLSL material. Open `index.html` directly in a WebGL2-capable browser; no HTTP server or build step is required.

## Pass map

| WebGL2 pass | Implementation | Blueprint mapping |
|---|---|---|
| **A — desktop capture surrogate** | A 2048×1536 Canvas 2D fake desktop is uploaded once as an RGBA8 texture, sampled with a slow two-axis auto-scroll, and rendered into a full-resolution FBO. The diagonal stripes, 64 px grid, icon edges, labels, and metadata are intentional refraction calibration targets. | §3.2 full-bleed content scrolling under floating islands. In production this input is the DirectComposition backdrop rather than a Canvas texture. |
| **B1 — Kawase ping** | Full-resolution scene → half-resolution FBO, four diagonal taps. | §3.1 Layer 2 and §3.3 default blur radius 36 px. |
| **B2 — Kawase pong** | Half-resolution ping → independent half-resolution pong FBO, four diagonal taps. Radius is split between the two passes and tracks the 0–64 px control. All render textures use `LINEAR` and `CLAMP_TO_EDGE`. | §3.1 Layer 2, two-pass Kawase blur/depth falloff. |
| **C — optical composite** | One full-screen draw loops over up to eight axis-aligned panel quads. Each panel is masked by an analytic SDF, samples the blurred scene with RGB offsets, applies vibrancy/tint, then adds rim light, pointer light, and anti-banding noise. | §3.1 Layers 1, 3, 4, and 5; authoritative equations from §6.4. |

## HLSL equivalence points

The composite shader deliberately preserves the §6.4 definitions and operation order:

- `sdRoundedBox` is a term-for-term GLSL translation.
- `calcSDFGradient` uses the same central-difference `eps = 0.75` and `1e-6` stabilizer.
- Bevel response is `smoothstep(-bevel, 0, dist)` and the refraction response is squared.
- Dispersion multipliers are R `0.88`, G `1.00`, B `1.14`.
- Vibrancy uses Rec.709 luma weights `0.2126 / 0.7152 / 0.0722`.
- Rim mask thresholds, `normalize(-0.55, -0.83)`, directional coefficients, pointer coefficients, and pointer Gaussian `sigma = 140` are unchanged.
- `interleavedGradientNoise` uses the same three constants and exact `(1.5 / 255.0)` amplitude.
- The WebGL source-over operation is algebraically equivalent to the HLSL premultiplied return `float4(finalRGB * edgeAlpha, edgeAlpha)`.

The day/night material values are the exact §3.3 tokens: island `rgba(255,255,255,.58)` / `rgba(38,42,54,.52)`, canvas base `rgba(246,248,252,.72)` / `rgba(18,20,26,.68)`, and rim alpha gradients `.85→.18` / `.28→.04`. Defaults are blur `36 px`, saturation `1.55`, and bevel `6 px`. Progress geometry is integrated with the §3.3 fluid-sheet spring (`m=1, k=280, c=34`) and demonstrates the §3.4 `<400 ms`, `400 ms–5 s`, and `>5 s` states. The snappy interaction constants (`m=1, k=420, c=28`) are retained in `SPRINGS` for parity.

## Intentional differences from the Windows HLSL path

1. **Squircle wording conflict:** §3.2 specifies a true superellipse exponent `n=4.2`, while the explicitly authoritative §6.4 shader defines a Euclidean rounded-box SDF and has no exponent. This reference follows §6.4 exactly. The default card is `R=16`; pills are `R=H/2`. The radius control scales those presets.
2. **Backdrop source:** Pass A uses a deterministic generated Canvas instead of the Windows desktop/DirectComposition backdrop.
3. **Blur graph:** `LiquidGlassEffect.hlsl` contains the optical pixel effect only. This page supplies the §3.1 Kawase graph explicitly as two half-resolution WebGL FBO passes. It uses eight bilinear taps total rather than a platform-specific Direct2D blur kernel.
4. **Coordinate convention:** DOM/HLSL positions are top-left-origin; OpenGL textures are bottom-left-origin. Only the refraction UV Y offset is sign-flipped. The SDF, pointer, and light calculations remain in top-left pixel coordinates.
5. **Rim token gradient:** HLSL exposes one `u_specularColor`; the page interpolates the §3.3 top/bottom alpha token per panel before applying the unchanged §6.4 specular equation.
6. **Noise source:** The blueprint calls the layer “blue noise,” but the authoritative shader implements deterministic interleaved gradient noise rather than a blue-noise atlas. The page preserves the authoritative function and adds only a bypass toggle.
7. **Surface format/color management:** The browser uses RGBA8 FBOs and browser-managed display color. Production may use linear/scRGB or HDR DirectComposition surfaces. No gamma conversion absent from §6.4 is inserted here.
8. **DPR:** Optical distances are specified in CSS pixels by the controls and converted to framebuffer physical pixels so the one-pixel SDF antialiasing and pointer field remain stable on high-DPI phones.

## Interaction and diagnostics

- Move a mouse or finger around panel rims to inspect the Gaussian pointer highlight.
- Set blur to `0` and refraction to a high value to isolate RGB dispersion against the grid and text.
- Disable noise to inspect gradient banding.
- The progress HUD loops every nine seconds; tap its pill during stage two to force the expanded state.
- Shader compile/link failures and FBO completeness failures are printed in the red in-page diagnostic overlay, including numbered GLSL source for compiler errors.
- The bottom telemetry strip reports measured FPS and all active optical parameters.

## Local checks

```sh
node --check glass.js
```

For visual smoke testing, open `index.html`, verify that four glass surfaces track their DOM bounds, move the pointer around each rim, toggle the theme, and resize/rotate the viewport to force FBO reallocation.
