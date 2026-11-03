// PrismArchive -- PrismCore
// C71: appearance settings -> the live theme overrides the renderers read.
//
// BEFORE THIS FILE
// appearance.accentColor and appearance.glassIntensity were persisted, migrated
// and validated -- and that was all. grep found the keys only in SettingsStore's
// schema and its v2->v3 migration, so changing either one changed no pixel.
//
// WHAT IS HONEST ABOUT THE DEFAULT
// The schema's accent default is "#4F8CFF" and its intensity default is 0.72.
// Neither is a user choice: a fresh document carries them. Treating them as an
// override would have repainted every existing install on first launch (the
// theme token accent is #007AFF / #0A84FF, not #4F8CFF). They are therefore
// SENTINELS: "#4F8CFF" means "follow the theme token" and 0.72 is the glass
// reference intensity (see ThemeTokens' live::kReferenceGlassIntensity).
//
// The mapping is pure and unit-tested on the Linux lane; the app only calls
// Apply() at startup and after a settings save.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "SettingsStore.hpp"
#include "ThemeTokens.hpp"

namespace prism::appearance {

inline constexpr char kAccentKey[] = "appearance.accentColor";
inline constexpr char kGlassIntensityKey[] = "appearance.glassIntensity";
// C77: the refraction layer's switch (DESIGN.md 2.1). Default ON.
inline constexpr char kBackdropKey[] = "appearance.backdropRefraction";
inline constexpr bool kDefaultBackdropRefraction = true;

// The schema defaults, spelled once: SettingsStore's table and this header must
// agree, and the pure test asserts that agreement rather than assuming it.
inline constexpr char kDefaultAccentHex[] = "#4F8CFF";
inline constexpr double kDefaultGlassIntensity = 0.72;

// "#RRGGBB" (6 hex digits after '#'). Returns false for anything else; the
// caller keeps the theme token rather than rendering an invented colour.
inline bool ParseHexColor(std::string_view text, theme::Rgba* out) {
  if (out == nullptr) return false;
  if (text.size() != 7 || text[0] != '#') return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int channels[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    const int hi = nibble(text[static_cast<std::size_t>(1 + i * 2)]);
    const int lo = nibble(text[static_cast<std::size_t>(2 + i * 2)]);
    if (hi < 0 || lo < 0) return false;
    channels[i] = hi * 16 + lo;
  }
  out->r = static_cast<float>(channels[0]) / 255.0f;
  out->g = static_cast<float>(channels[1]) / 255.0f;
  out->b = static_cast<float>(channels[2]) / 255.0f;
  out->a = 1.0f;
  return true;
}

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

// What Apply() did, for logs and tests (an absence must say so, D12).
struct Applied {
  bool accentParsed = false;   // the stored text was a valid #RRGGBB
  bool accentApplied = false;  // ... and it differed from the default sentinel
  double glassIntensity = kDefaultGlassIntensity;
  bool backdropRefraction = kDefaultBackdropRefraction;
};

// Pushes the two appearance keys into the theme runtime. A missing/invalid
// value falls back to the token/light defaults instead of inventing one.
inline Applied Apply(const settings::SettingsStore& store) {
  Applied applied;
  theme::live::Reset();

  double intensity =
      store.GetNumber(kGlassIntensityKey, kDefaultGlassIntensity);
  if (!(intensity >= 0.0)) intensity = 0.0;
  if (intensity > 1.0) intensity = 1.0;
  applied.glassIntensity = intensity;
  theme::live::SetGlassIntensity(intensity);

  // A missing key means "never chosen", which is the default: ON.
  const bool backdrop =
      store.GetBool(kBackdropKey, kDefaultBackdropRefraction);
  applied.backdropRefraction = backdrop;
  theme::live::SetBackdropRefraction(backdrop);

  const std::string hex = store.GetString(kAccentKey, kDefaultAccentHex);
  theme::Rgba color;
  applied.accentParsed = ParseHexColor(hex, &color);
  if (applied.accentParsed && !EqualsIgnoreCase(hex, kDefaultAccentHex)) {
    theme::live::SetAccent(color);
    applied.accentApplied = true;
  }
  return applied;
}

inline void Reset() { theme::live::Reset(); }

}  // namespace prism::appearance
