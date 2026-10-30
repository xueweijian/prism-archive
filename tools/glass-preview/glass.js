/*
 * PrismArchive Liquid Glass optical reference.
 * Pure WebGL2, no dependencies, no build step.
 */
(function () {
  "use strict";

  const MAX_PANELS = 8;
  const TOKENS = {
    day: {
      canvas: [246 / 255, 248 / 255, 252 / 255, 0.72],
      island: [1, 1, 1, 0.58],
      specTop: 0.85,
      specBottom: 0.18,
      accentA: "#007aff",
      accentB: "#0055d4"
    },
    dark: {
      canvas: [18 / 255, 20 / 255, 26 / 255, 0.68],
      island: [38 / 255, 42 / 255, 54 / 255, 0.52],
      specTop: 0.28,
      specBottom: 0.04,
      accentA: "#0a84ff",
      accentB: "#0060df"
    }
  };
  const SPRINGS = {
    snappy: { mass: 1, stiffness: 420, damping: 28 },
    fluidSheet: { mass: 1, stiffness: 280, damping: 34 }
  };

  const canvas = document.getElementById("glass-canvas");
  const sourceCanvas = document.getElementById("desktop-source");
  const errorBox = document.getElementById("error-box");
  const fpsNode = document.getElementById("fps");
  const paramsNode = document.getElementById("params");
  const progressNode = document.querySelector(".progress-hud");
  const progressFill = document.querySelector(".progress-fill");
  const progressPercent = document.querySelector(".percent");
  const progressEta = document.querySelector(".eta");
  const rippleNode = document.getElementById("ripple");

  const controls = {
    blur: document.getElementById("blur"),
    saturation: document.getElementById("saturation"),
    bevel: document.getElementById("bevel"),
    strength: document.getElementById("strength"),
    radius: document.getElementById("radius"),
    noise: document.getElementById("noise"),
    theme: document.getElementById("theme")
  };

  const state = {
    theme: "day",
    pointerX: window.innerWidth * 0.5,
    pointerY: window.innerHeight * 0.5,
    lastTime: performance.now(),
    fpsTime: performance.now(),
    fpsFrames: 0,
    fpsValue: 0,
    cycle: -1,
    progress: { width: 0, height: 0, widthVelocity: 0, heightVelocity: 0 },
    targets: null,
    sourceTexture: null
  };

  function showError(title, detail) {
    errorBox.style.display = "block";
    errorBox.textContent = title + "\n\n" + String(detail || "Unknown error");
  }

  window.addEventListener("error", function (event) {
    if (!errorBox.textContent) showError("JavaScript runtime error", event.error && event.error.stack ? event.error.stack : event.message);
  });

  const gl = canvas.getContext("webgl2", {
    alpha: false,
    antialias: false,
    depth: false,
    stencil: false,
    premultipliedAlpha: false,
    preserveDrawingBuffer: false,
    powerPreference: "high-performance"
  });

  if (!gl) {
    showError("WebGL2 is unavailable", "This optical reference requires a browser with WebGL2 enabled.");
    return;
  }

  const vertexShaderSource = `#version 300 es
precision highp float;
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p * 0.5;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

  // Pass A: upload a Canvas 2D fake desktop, then move it in a real offscreen FBO.
  const backgroundFragmentSource = `#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uDesktop;
uniform vec2 uResolution;
uniform float uTime;
out vec4 outColor;
void main() {
    const float sourceAspect = 1.3333333333;
    float viewAspect = uResolution.x / max(uResolution.y, 1.0);
    vec2 uv = vUV;
    if (viewAspect > sourceAspect) {
        uv.y = (uv.y - 0.5) * (sourceAspect / viewAspect) + 0.5;
    } else {
        uv.x = (uv.x - 0.5) * (viewAspect / sourceAspect) + 0.5;
    }
    // Slow auto-scroll makes refraction measurable against moving text and grid lines.
    uv.y += sin(uTime * 0.19) * 0.045;
    uv.x += sin(uTime * 0.11 + 1.2) * 0.012;
    vec3 color = texture(uDesktop, clamp(uv, 0.001, 0.999)).rgb;
    float vignette = 1.0 - 0.13 * dot(vUV - 0.5, vUV - 0.5);
    outColor = vec4(color * vignette, 1.0);
}`;

  // Pass B: two downsampled Kawase taps, ping-ponged between independent FBOs.
  const kawaseFragmentSource = `#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uSource;
uniform vec2 uTexelSize;
uniform float uOffsetPx;
out vec4 outColor;
void main() {
    vec2 d = uTexelSize * uOffsetPx;
    vec3 c = texture(uSource, vUV + vec2(-d.x, -d.y)).rgb;
    c += texture(uSource, vUV + vec2( d.x, -d.y)).rgb;
    c += texture(uSource, vUV + vec2(-d.x,  d.y)).rgb;
    c += texture(uSource, vUV + vec2( d.x,  d.y)).rgb;
    outColor = vec4(c * 0.25, 1.0);
}`;

  // Pass C: authoritative §6.4 equations, repeated over at most eight panel quads.
  const compositeFragmentSource = `#version 300 es
precision highp float;
#define MAX_PANELS 8
in vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uBlurred;
uniform vec2 uResolution;
uniform vec2 uPointerPos;
uniform vec4 uPanels[MAX_PANELS];
uniform float uPanelRadii[MAX_PANELS];
uniform int uPanelCount;
uniform float uRefractionBevel;
uniform float uIorStrength;
uniform float uSaturationBoost;
uniform vec4 uTintColor;
uniform vec3 uSpecularColor;
uniform float uSpecTopAlpha;
uniform float uSpecBottomAlpha;
uniform float uNoiseEnabled;
uniform float uBlurMix;
out vec4 outColor;

// Blueprint §6.4: exact GLSL translation of sdRoundedBox.
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Blueprint §6.4: eps=0.75 central difference, including the 1e-6 stabilizer.
vec2 calcSDFGradient(vec2 p, vec2 b, float r) {
    const float eps = 0.75;
    float dx = sdRoundedBox(p + vec2(eps, 0.0), b, r) - sdRoundedBox(p - vec2(eps, 0.0), b, r);
    float dy = sdRoundedBox(p + vec2(0.0, eps), b, r) - sdRoundedBox(p - vec2(0.0, eps), b, r);
    return normalize(vec2(dx, dy) + vec2(1e-6));
}

// Blueprint §6.4: exact interleaved gradient noise constants.
float interleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

vec3 opticalSample(vec2 uv) {
    vec3 sharp = texture(uScene, clamp(uv, 0.0, 1.0)).rgb;
    vec3 soft = texture(uBlurred, clamp(uv, 0.0, 1.0)).rgb;
    return mix(sharp, soft, uBlurMix);
}

void main() {
    vec2 fragPos = vec2(gl_FragCoord.x, uResolution.y - gl_FragCoord.y);
    vec2 screenUV = gl_FragCoord.xy / uResolution;
    vec2 texelSize = 1.0 / uResolution;
    vec3 color = texture(uScene, screenUV).rgb;

    for (int i = 0; i < MAX_PANELS; ++i) {
        if (i >= uPanelCount) break;
        vec2 panelCenter = uPanels[i].xy;
        vec2 panelHalfSize = uPanels[i].zw;
        vec2 localPos = fragPos - panelCenter;
        if (any(greaterThan(abs(localPos), panelHalfSize + vec2(1.0)))) continue;

        float cornerRadius = min(uPanelRadii[i], min(panelHalfSize.x, panelHalfSize.y));
        float dist = sdRoundedBox(localPos, panelHalfSize, cornerRadius);
        if (dist > 0.5) continue;
        float edgeAlpha = clamp(0.5 - dist, 0.0, 1.0);

        // §6.4 Layer 1: squared bevel response and signed SDF normal.
        float bevelFactor = uRefractionBevel > 0.0001
            ? smoothstep(-uRefractionBevel, 0.0, dist)
            : 0.0;
        vec2 normal2D = calcSDFGradient(localPos, panelHalfSize, cornerRadius);
        vec2 refractOffsetPx = -normal2D * (bevelFactor * bevelFactor) * uIorStrength;
        // fragPos uses top-left coordinates, whereas OpenGL texture V points upward.
        vec2 refractUV = vec2(refractOffsetPx.x, -refractOffsetPx.y) * texelSize;

        // §6.4 exact R/G/B refraction multipliers: 0.88 / 1.00 / 1.14.
        float rCh = opticalSample(screenUV + refractUV * 0.88).r;
        float gCh = opticalSample(screenUV + refractUV * 1.00).g;
        float bCh = opticalSample(screenUV + refractUV * 1.14).b;
        vec3 refractedBg = vec3(rCh, gCh, bCh);

        // §6.4 Layer 3: Rec.709 luma weights and Apple vibrancy matrix.
        float luma = dot(refractedBg, vec3(0.2126, 0.7152, 0.0722));
        vec3 vibrantBg = mix(vec3(luma), refractedBg, uSaturationBoost);
        vec3 glassBody = mix(vibrantBg, uTintColor.rgb, uTintColor.a);

        // §6.4 Layer 5: exact rim mask, light direction, and pointer sigma=140 px.
        float rimMask = smoothstep(-1.8, -0.2, dist) * (1.0 - smoothstep(-0.2, 0.5, dist));
        vec2 lightDir = normalize(vec2(-0.55, -0.83));
        float directionalRim = clamp(dot(normal2D, lightDir) * 0.65 + 0.35, 0.0, 1.0);
        float pointerDist = length(fragPos - uPointerPos);
        float pointerGlow = exp(-(pointerDist * pointerDist) / (2.0 * 140.0 * 140.0));
        float totalSpecular = rimMask * (directionalRim * 0.75 + pointerGlow * 0.95);

        // The token's top-to-bottom alpha is host material data; optical math is unchanged.
        float topWeight = clamp(0.5 - 0.5 * localPos.y / max(panelHalfSize.y, 1.0), 0.0, 1.0);
        float specularAlpha = mix(uSpecBottomAlpha, uSpecTopAlpha, topWeight);
        vec3 finalRGB = glassBody + uSpecularColor * totalSpecular * specularAlpha;

        // §6.4 Layer 4: exact 1.5/255 anti-banding amplitude; UI can bypass it.
        float dither = (interleavedGradientNoise(fragPos) - 0.5) * (1.5 / 255.0) * uNoiseEnabled;
        finalRGB = clamp(finalRGB + dither, 0.0, 1.0);

        // Equivalent source-over of HLSL's premultiplied float4(finalRGB*alpha, alpha).
        color = mix(color, finalRGB, edgeAlpha);
    }
    outColor = vec4(color, 1.0);
}`;

  function numberedSource(source) {
    return source.split("\n").map(function (line, index) {
      return String(index + 1).padStart(4, " ") + " | " + line;
    }).join("\n");
  }

  function compileShader(type, source, label) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(shader);
      gl.deleteShader(shader);
      throw new Error(label + " shader compilation failed:\n" + log + "\n\n" + numberedSource(source));
    }
    return shader;
  }

  function createProgram(fragmentSource, label) {
    const vertexShader = compileShader(gl.VERTEX_SHADER, vertexShaderSource, label + " vertex");
    const fragmentShader = compileShader(gl.FRAGMENT_SHADER, fragmentSource, label + " fragment");
    const program = gl.createProgram();
    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);
    gl.deleteShader(vertexShader);
    gl.deleteShader(fragmentShader);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const log = gl.getProgramInfoLog(program);
      gl.deleteProgram(program);
      throw new Error(label + " program link failed:\n" + log);
    }
    return program;
  }

  function uniformLocations(program, names) {
    const result = {};
    names.forEach(function (name) {
      result[name] = gl.getUniformLocation(program, name);
    });
    return result;
  }

  function createTexture(width, height, data) {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, data || null);
    return texture;
  }

  function createTarget(width, height, label) {
    const texture = createTexture(width, height, null);
    const framebuffer = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    if (status !== gl.FRAMEBUFFER_COMPLETE) {
      gl.deleteTexture(texture);
      gl.deleteFramebuffer(framebuffer);
      throw new Error(label + " framebuffer is incomplete (0x" + status.toString(16) + ")");
    }
    return { texture: texture, framebuffer: framebuffer, width: width, height: height, label: label };
  }

  function deleteTarget(target) {
    if (!target) return;
    gl.deleteTexture(target.texture);
    gl.deleteFramebuffer(target.framebuffer);
  }

  function bindTexture(unit, texture, location) {
    gl.activeTexture(gl.TEXTURE0 + unit);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.uniform1i(location, unit);
  }

  function drawDesktopSource(themeName) {
    const ctx = sourceCanvas.getContext("2d", { alpha: false });
    const w = sourceCanvas.width;
    const h = sourceCanvas.height;
    const dark = themeName === "dark";

    const wallpaper = ctx.createLinearGradient(0, 0, w, h);
    if (dark) {
      wallpaper.addColorStop(0, "#081426");
      wallpaper.addColorStop(0.38, "#142d56");
      wallpaper.addColorStop(0.7, "#45256d");
      wallpaper.addColorStop(1, "#093f50");
    } else {
      wallpaper.addColorStop(0, "#86d7ff");
      wallpaper.addColorStop(0.32, "#b7c5ff");
      wallpaper.addColorStop(0.68, "#f4b6db");
      wallpaper.addColorStop(1, "#77e1cf");
    }
    ctx.fillStyle = wallpaper;
    ctx.fillRect(0, 0, w, h);

    // Large spectral ribbons make low-frequency optical displacement obvious.
    ctx.save();
    ctx.globalAlpha = dark ? 0.42 : 0.5;
    ctx.lineCap = "round";
    const ribbons = [
      ["#00c7be", 260, 1220, 1780, 210, 170],
      ["#007aff", -120, 760, 1680, 1390, 210],
      ["#ff5ca8", 380, -80, 2040, 1020, 145]
    ];
    ribbons.forEach(function (r) {
      ctx.strokeStyle = r[0];
      ctx.lineWidth = r[5];
      ctx.beginPath();
      ctx.moveTo(r[1], r[2]);
      ctx.bezierCurveTo(w * 0.34, h * 0.12, w * 0.62, h * 0.92, r[3], r[4]);
      ctx.stroke();
    });
    ctx.restore();

    // High-contrast stripes and a grid are deliberate refraction calibration targets.
    ctx.save();
    ctx.globalAlpha = dark ? 0.22 : 0.2;
    for (let x = -h; x < w + h; x += 46) {
      ctx.fillStyle = ((x / 46) & 1) ? "#ffffff" : "#143f8c";
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x + 18, 0);
      ctx.lineTo(x + h + 18, h);
      ctx.lineTo(x + h, h);
      ctx.closePath();
      ctx.fill();
    }
    ctx.restore();

    ctx.save();
    ctx.strokeStyle = dark ? "rgba(150,220,255,.26)" : "rgba(10,54,120,.20)";
    ctx.lineWidth = 2;
    for (let x = 0; x <= w; x += 64) {
      ctx.beginPath(); ctx.moveTo(x + 0.5, 0); ctx.lineTo(x + 0.5, h); ctx.stroke();
    }
    for (let y = 0; y <= h; y += 64) {
      ctx.beginPath(); ctx.moveTo(0, y + 0.5); ctx.lineTo(w, y + 0.5); ctx.stroke();
    }
    ctx.restore();

    const canvasToken = TOKENS[themeName].canvas;
    // §3.3 exact surface-canvas-base RGBA token over the generated wallpaper.
    ctx.fillStyle = "rgba(" + Math.round(canvasToken[0] * 255) + "," + Math.round(canvasToken[1] * 255) + "," + Math.round(canvasToken[2] * 255) + "," + canvasToken[3] + ")";
    ctx.fillRect(0, 0, w, h);

    // Fake desktop icons and file rows provide readable, moving edge detail.
    const files = [
      ["PR", "PrismArchive", "#007aff"], ["7Z", "Design_Assets_v4.7z", "#7d5cff"],
      ["UI", "iOS26_UI_Kit.fig", "#00a99d"], ["PDF", "Architecture_Spec.pdf", "#ff5b57"],
      ["IMG", "Hero_Banner_4K.webp", "#ff9500"], ["SRC", "prism-core", "#34c759"],
      ["MOV", "Launch_Film.mov", "#af52de"], ["ZIP", "Brand_Export.zip", "#007aff"],
      ["DOC", "Release_Notes.md", "#5ac8fa"], ["KEY", "signing-public.key", "#ffcc00"],
      ["TEX", "Glass_Noise_Atlas.png", "#00c7be"], ["LOG", "benchmark-120hz.log", "#637588"]
    ];
    ctx.textBaseline = "middle";
    files.forEach(function (file, index) {
      const col = index % 3;
      const row = Math.floor(index / 3);
      const x = 72 + col * 610;
      const y = 118 + row * 300;
      ctx.save();
      ctx.shadowColor = "rgba(0,0,0,.18)";
      ctx.shadowBlur = 18;
      ctx.fillStyle = file[2];
      roundRect2D(ctx, x, y, 92, 100, 23);
      ctx.fill();
      ctx.restore();
      ctx.fillStyle = "rgba(255,255,255,.92)";
      ctx.font = "700 22px -apple-system, Segoe UI, sans-serif";
      ctx.textAlign = "center";
      ctx.fillText(file[0], x + 46, y + 51);
      ctx.textAlign = "left";
      ctx.fillStyle = dark ? "rgba(245,249,255,.94)" : "rgba(11,30,58,.90)";
      ctx.font = "650 24px -apple-system, Segoe UI, sans-serif";
      ctx.fillText(file[1], x + 116, y + 31);
      ctx.fillStyle = dark ? "rgba(218,228,244,.62)" : "rgba(18,42,80,.58)";
      ctx.font = "500 19px ui-monospace, SFMono-Regular, monospace";
      ctx.fillText((12 + index * 17.3).toFixed(1) + " MB   ·   modified 10:" + String(11 + index).padStart(2, "0"), x + 116, y + 69);
      ctx.fillStyle = dark ? "rgba(255,255,255,.12)" : "rgba(255,255,255,.36)";
      roundRect2D(ctx, x + 110, y + 94, 400, 3, 2);
      ctx.fill();
    });
  }

  function roundRect2D(ctx, x, y, width, height, radius) {
    const r = Math.min(radius, width * 0.5, height * 0.5);
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + width, y, x + width, y + height, r);
    ctx.arcTo(x + width, y + height, x, y + height, r);
    ctx.arcTo(x, y + height, x, y, r);
    ctx.arcTo(x, y, x + width, y, r);
    ctx.closePath();
  }

  function uploadDesktopTexture(reallocate) {
    gl.bindTexture(gl.TEXTURE_2D, state.sourceTexture);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    if (reallocate) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, sourceCanvas);
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, sourceCanvas);
    }
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
  }

  function resizeTargetsIfNeeded() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const width = Math.max(1, Math.round(window.innerWidth * dpr));
    const height = Math.max(1, Math.round(window.innerHeight * dpr));
    if (canvas.width === width && canvas.height === height && state.targets) return;

    canvas.width = width;
    canvas.height = height;
    deleteTarget(state.targets && state.targets.scene);
    deleteTarget(state.targets && state.targets.blurA);
    deleteTarget(state.targets && state.targets.blurB);

    const halfWidth = Math.max(1, Math.ceil(width * 0.5));
    const halfHeight = Math.max(1, Math.ceil(height * 0.5));
    state.targets = {
      scene: createTarget(width, height, "Pass A scene"),
      blurA: createTarget(halfWidth, halfHeight, "Pass B Kawase ping"),
      blurB: createTarget(halfWidth, halfHeight, "Pass B Kawase pong")
    };
  }

  function readNumber(control) {
    return Number.parseFloat(control.value);
  }

  function bindControls() {
    ["blur", "saturation", "bevel", "strength", "radius"].forEach(function (name) {
      const input = controls[name];
      const output = document.querySelector('output[for="' + name + '"]');
      const update = function () {
        const value = readNumber(input);
        output.value = name === "saturation" ? value.toFixed(2) : String(value);
      };
      input.addEventListener("input", update);
      update();
    });

    controls.theme.addEventListener("click", function () {
      state.theme = state.theme === "day" ? "dark" : "day";
      document.documentElement.dataset.theme = state.theme;
      controls.theme.textContent = state.theme === "day" ? "Switch to dark" : "Switch to day";
      drawDesktopSource(state.theme);
      uploadDesktopTexture(false);
    });

    window.addEventListener("pointermove", function (event) {
      state.pointerX = event.clientX;
      state.pointerY = event.clientY;
    }, { passive: true });

    window.addEventListener("pointerdown", function (event) {
      state.pointerX = event.clientX;
      state.pointerY = event.clientY;
    }, { passive: true });

    progressNode.addEventListener("click", function () {
      state.manualExpandedUntil = performance.now() + 3000;
    });
  }

  function triggerRipple() {
    rippleNode.classList.remove("play");
    void rippleNode.offsetWidth;
    rippleNode.classList.add("play");
  }

  function springStep(value, velocity, target, deltaSeconds, spring) {
    const acceleration = (-spring.stiffness * (value - target) - spring.damping * velocity) / spring.mass;
    const nextVelocity = velocity + acceleration * deltaSeconds;
    return [value + nextVelocity * deltaSeconds, nextVelocity];
  }

  function updateProgressDemo(now, deltaSeconds) {
    const elapsed = (now - state.demoStart) / 1000;
    const cycleDuration = 9;
    const cycle = Math.floor(elapsed / cycleDuration);
    const local = elapsed - cycle * cycleDuration;
    if (cycle !== state.cycle) {
      state.cycle = cycle;
      triggerRipple();
    }

    const maximumWidth = Math.min(420, window.innerWidth - 24);
    let targetWidth = 0;
    let targetHeight = 0;
    let expanded = false;
    let progress = 0;

    // Blueprint §3.4: <400 ms ripple, 400 ms–5 s pill, >5 s expanded sheet.
    if (local >= 0.4 && local < 5) {
      targetWidth = maximumWidth;
      targetHeight = 54;
      progress = 4 + ((local - 0.4) / 4.6) * 72;
    } else if (local >= 5 && local < 8) {
      targetWidth = maximumWidth;
      targetHeight = 142;
      expanded = true;
      progress = 76 + ((local - 5) / 3) * 24;
    } else if (local >= 8 && local < 8.6) {
      targetWidth = maximumWidth;
      targetHeight = 54;
      progress = 100;
    }

    if (state.manualExpandedUntil > now && local >= 0.4 && local < 8.6) {
      targetWidth = maximumWidth;
      targetHeight = 142;
      expanded = true;
    }

    const dt = Math.min(deltaSeconds, 1 / 30);
    const widthStep = springStep(state.progress.width, state.progress.widthVelocity, targetWidth, dt, SPRINGS.fluidSheet);
    const heightStep = springStep(state.progress.height, state.progress.heightVelocity, targetHeight, dt, SPRINGS.fluidSheet);
    state.progress.width = Math.max(0, widthStep[0]);
    state.progress.widthVelocity = widthStep[1];
    state.progress.height = Math.max(0, heightStep[0]);
    state.progress.heightVelocity = heightStep[1];

    const materiallyVisible = state.progress.width > 3 && state.progress.height > 3;
    progressNode.style.width = state.progress.width.toFixed(2) + "px";
    progressNode.style.height = state.progress.height.toFixed(2) + "px";
    progressNode.dataset.glassVisible = materiallyVisible ? "true" : "false";
    progressNode.classList.toggle("visible", materiallyVisible);
    progressNode.classList.toggle("expanded", expanded && state.progress.height > 82);
    progressFill.style.width = Math.min(100, progress).toFixed(1) + "%";
    progressPercent.textContent = Math.round(progress) + "%";
    progressEta.textContent = progress >= 100 ? "Done" : Math.max(0, (100 - progress) / 18).toFixed(1) + "s";
  }

  function collectPanels() {
    const dprX = canvas.width / Math.max(canvas.clientWidth, 1);
    const dprY = canvas.height / Math.max(canvas.clientHeight, 1);
    const baseRadius = readNumber(controls.radius);
    const packed = new Float32Array(MAX_PANELS * 4);
    const radii = new Float32Array(MAX_PANELS);
    let count = 0;

    document.querySelectorAll("[data-glass]").forEach(function (element) {
      if (count >= MAX_PANELS || element.dataset.glassVisible === "false") return;
      const rect = element.getBoundingClientRect();
      if (rect.width < 1 || rect.height < 1) return;
      const halfWidth = rect.width * dprX * 0.5;
      const halfHeight = rect.height * dprY * 0.5;
      packed[count * 4] = (rect.left + rect.width * 0.5) * dprX;
      packed[count * 4 + 1] = (rect.top + rect.height * 0.5) * dprY;
      packed[count * 4 + 2] = halfWidth;
      packed[count * 4 + 3] = halfHeight;

      // §3.2 defaults: R=16 preview card; pills use R=H/2. The slider scales both.
      const defaultCssRadius = element.dataset.glass === "preview" ? 16 : rect.height * 0.5;
      radii[count] = Math.min(halfHeight, defaultCssRadius * (baseRadius / 16) * (dprX + dprY) * 0.5);
      count += 1;
    });
    return { packed: packed, radii: radii, count: count };
  }

  let backgroundProgram;
  let kawaseProgram;
  let compositeProgram;
  let backgroundUniforms;
  let kawaseUniforms;
  let compositeUniforms;
  let vao;

  function initializePipeline() {
    backgroundProgram = createProgram(backgroundFragmentSource, "Pass A background");
    kawaseProgram = createProgram(kawaseFragmentSource, "Pass B Kawase");
    compositeProgram = createProgram(compositeFragmentSource, "Pass C composite");
    backgroundUniforms = uniformLocations(backgroundProgram, ["uDesktop", "uResolution", "uTime"]);
    kawaseUniforms = uniformLocations(kawaseProgram, ["uSource", "uTexelSize", "uOffsetPx"]);
    compositeUniforms = uniformLocations(compositeProgram, [
      "uScene", "uBlurred", "uResolution", "uPointerPos", "uPanels[0]", "uPanelRadii[0]",
      "uPanelCount", "uRefractionBevel", "uIorStrength", "uSaturationBoost", "uTintColor",
      "uSpecularColor", "uSpecTopAlpha", "uSpecBottomAlpha", "uNoiseEnabled", "uBlurMix"
    ]);
    vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);

    state.sourceTexture = createTexture(sourceCanvas.width, sourceCanvas.height, null);
    drawDesktopSource(state.theme);
    uploadDesktopTexture(true);
    resizeTargetsIfNeeded();
  }

  function renderFrame(now) {
    const deltaSeconds = Math.min((now - state.lastTime) / 1000, 0.05);
    state.lastTime = now;
    resizeTargetsIfNeeded();
    updateProgressDemo(now, deltaSeconds);

    const scene = state.targets.scene;
    const blurA = state.targets.blurA;
    const blurB = state.targets.blurB;
    const blurRadius = readNumber(controls.blur);
    const dprX = canvas.width / Math.max(canvas.clientWidth, 1);
    const dprY = canvas.height / Math.max(canvas.clientHeight, 1);
    const pixelScale = (dprX + dprY) * 0.5;
    const blurRadiusPhysical = blurRadius * pixelScale;

    // Pass A — moving fake desktop into a full-resolution offscreen scene FBO.
    gl.bindFramebuffer(gl.FRAMEBUFFER, scene.framebuffer);
    gl.viewport(0, 0, scene.width, scene.height);
    gl.useProgram(backgroundProgram);
    bindTexture(0, state.sourceTexture, backgroundUniforms.uDesktop);
    gl.uniform2f(backgroundUniforms.uResolution, scene.width, scene.height);
    gl.uniform1f(backgroundUniforms.uTime, now * 0.001);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // Pass B1 — downsample and first Kawase diagonal tap into ping FBO.
    gl.bindFramebuffer(gl.FRAMEBUFFER, blurA.framebuffer);
    gl.viewport(0, 0, blurA.width, blurA.height);
    gl.useProgram(kawaseProgram);
    bindTexture(0, scene.texture, kawaseUniforms.uSource);
    gl.uniform2f(kawaseUniforms.uTexelSize, 1 / scene.width, 1 / scene.height);
    gl.uniform1f(kawaseUniforms.uOffsetPx, blurRadiusPhysical * 0.35);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // Pass B2 — second Kawase diagonal tap into pong FBO (half-res texels are 2x).
    gl.bindFramebuffer(gl.FRAMEBUFFER, blurB.framebuffer);
    gl.viewport(0, 0, blurB.width, blurB.height);
    gl.useProgram(kawaseProgram);
    bindTexture(0, blurA.texture, kawaseUniforms.uSource);
    gl.uniform2f(kawaseUniforms.uTexelSize, 1 / blurA.width, 1 / blurA.height);
    gl.uniform1f(kawaseUniforms.uOffsetPx, blurRadiusPhysical * 0.325);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // Pass C — compose all SDF glass panel quads in one full-screen shader loop.
    const panels = collectPanels();
    const token = TOKENS[state.theme];

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.useProgram(compositeProgram);
    bindTexture(0, scene.texture, compositeUniforms.uScene);
    bindTexture(1, blurB.texture, compositeUniforms.uBlurred);
    gl.uniform2f(compositeUniforms.uResolution, canvas.width, canvas.height);
    gl.uniform2f(compositeUniforms.uPointerPos, state.pointerX * dprX, state.pointerY * dprY);
    gl.uniform4fv(compositeUniforms["uPanels[0]"], panels.packed);
    gl.uniform1fv(compositeUniforms["uPanelRadii[0]"], panels.radii);
    gl.uniform1i(compositeUniforms.uPanelCount, panels.count);
    gl.uniform1f(compositeUniforms.uRefractionBevel, readNumber(controls.bevel) * pixelScale);
    gl.uniform1f(compositeUniforms.uIorStrength, readNumber(controls.strength) * pixelScale);
    gl.uniform1f(compositeUniforms.uSaturationBoost, readNumber(controls.saturation));
    gl.uniform4fv(compositeUniforms.uTintColor, token.island);
    gl.uniform3f(compositeUniforms.uSpecularColor, 1, 1, 1);
    gl.uniform1f(compositeUniforms.uSpecTopAlpha, token.specTop);
    gl.uniform1f(compositeUniforms.uSpecBottomAlpha, token.specBottom);
    gl.uniform1f(compositeUniforms.uNoiseEnabled, controls.noise.checked ? 1 : 0);
    gl.uniform1f(compositeUniforms.uBlurMix, Math.min(1, blurRadius * 0.5));
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    state.fpsFrames += 1;
    if (now - state.fpsTime >= 500) {
      state.fpsValue = Math.round(state.fpsFrames * 1000 / (now - state.fpsTime));
      state.fpsFrames = 0;
      state.fpsTime = now;
      fpsNode.textContent = state.fpsValue + " FPS";
      paramsNode.textContent =
        state.theme + " · blur " + blurRadius + "px · sat " + readNumber(controls.saturation).toFixed(2) +
        " · bevel " + readNumber(controls.bevel) + "px · ior " + readNumber(controls.strength) +
        "px · R " + readNumber(controls.radius) + "px · " + (controls.noise.checked ? "noise on" : "noise off");
    }
    requestAnimationFrame(renderFrame);
  }

  canvas.addEventListener("webglcontextlost", function (event) {
    event.preventDefault();
    showError("WebGL2 context lost", "Reload the page to recreate the optical pipeline and all framebuffer attachments.");
  });

  try {
    bindControls();
    initializePipeline();
    state.demoStart = performance.now();
    state.lastTime = state.demoStart;
    requestAnimationFrame(renderFrame);
  } catch (error) {
    showError("Liquid Glass pipeline initialization failed", error && error.stack ? error.stack : error);
  }
}());
