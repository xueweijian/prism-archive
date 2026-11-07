// PrismArchive -- PrismWin
// SystemTheme: the ONE place the OS's light/dark answer is read (card C125).
//
// WHY THIS FILE EXISTS. PrismCore cannot include <windows.h> (preflight
// check_arch_no_windows), so prism::chrome::ShellVariantFor takes the OS answer
// as a parameter. Something above the layer boundary has to produce it, and
// "something" must be one function: a second RegQueryValueExW elsewhere would
// let the shell and the canvas disagree about what "跟随系统" means.
//
// THE VALUE WINDOWS ACTUALLY WRITES: AppsUseLightTheme, a REG_DWORD under
// HKCU \ Software \ Microsoft \ Windows \ CurrentVersion \ Themes \ Personalize
// (spaced so no comment line ends in a line-continuation backslash). 1 means the
// apps (not the taskbar) are light, 0 means dark. On this machine it is 1 and the
// desktop is white -- exactly the case the pre-C125 build got wrong.
//
// THE THREE ANSWERS. Present-and-0 -> Dark, present-and-nonzero -> Light,
// missing / wrong type / unreadable -> Unknown. Unknown is NOT "dark by
// default": the mapper (prism::chrome::ShellVariantFor) keeps the previous shell
// for it, because inventing a preference from a failure would repaint a dark
// desktop white the first time a hive is unreadable.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

#include "../PrismCore/ChromeTheme.hpp"

namespace prism::win::systemtheme {

// The documented location of the OS appearance switch. Public so a test can
// name it instead of re-typing it (the per-user hive is the one Windows uses).
inline constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
inline constexpr wchar_t kAppsUseLightThemeValue[] = L"AppsUseLightTheme";

// One raw reading, kept separable from the mapping so the mapping can be tested
// without a hive and the reading can be tested without a policy.
struct Sample {
  bool present = false;
  std::uint32_t value = 0;
};

// Reads AppsUseLightTheme below (root, subKey). A missing value, a non-DWORD
// type and a size other than 4 all mean present == false: a REG_SZ "1" is not
// the switch Windows honours, so it must not be treated as one.
inline Sample ReadSample(HKEY root, const wchar_t* subKey) {
  Sample sample;
  if (subKey == nullptr) return sample;
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return sample;
  DWORD type = 0;
  DWORD bytes = static_cast<DWORD>(sizeof(std::uint32_t));
  std::uint32_t value = 0;
  const LSTATUS status = RegQueryValueExW(key, kAppsUseLightThemeValue, nullptr, &type,
                                          reinterpret_cast<BYTE*>(&value), &bytes);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(std::uint32_t)) {
    return sample;
  }
  sample.present = true;
  sample.value = value;
  return sample;
}

// The documented mapping, in one place: 0 is dark, anything else is light.
inline prism::chrome::SystemTheme FromSample(const Sample& sample) {
  if (!sample.present) return prism::chrome::SystemTheme::Unknown;
  return sample.value == 0 ? prism::chrome::SystemTheme::Dark
                           : prism::chrome::SystemTheme::Light;
}

// Production door: the OS's answer for the CURRENT USER. Callers that already
// have a reading (the settings dialog's preview, a test's temporary key) use
// FromSample directly.
inline prism::chrome::SystemTheme AppsUseLightTheme(HKEY root = HKEY_CURRENT_USER) {
  return FromSample(ReadSample(root, kPersonalizeKey));
}

// The WM_SETTINGCHANGE area that means "the light/dark switch (and the accent)
// changed". Every other area (fonts, metrics, wallpaper) leaves the shell alone.
inline constexpr wchar_t kImmersiveColorSet[] = L"ImmersiveColorSet";

// True when lParam of a WM_SETTINGCHANGE names that area. Case-insensitive the
// way the OS compares area names; a null lParam is never it.
inline bool IsImmersiveColorSetChange(LPARAM lparam) {
  if (lparam == 0) return false;
  const auto* area = reinterpret_cast<const wchar_t*>(lparam);
  return lstrcmpiW(area, kImmersiveColorSet) == 0;
}

}  // namespace prism::win::systemtheme
