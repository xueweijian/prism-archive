// PrismArchive -- PrismWin
// Typography: ONE font stack, ONE TextFormat entry, ONE fallback (C103).
//
// BLUEPRINT:170 names the design token --font-stack-system. The Apple names in
// it are aspirational on Windows: SF Pro Display and PingFang SC are almost
// never installed on a user's PC, so a stack that stops there would be a no-op.
// This header therefore keeps TWO ordered lists with the same semantics --
// best Latin first, best CJK first, system fallback last:
//
//   Latin: SF Pro Display -> Inter -> Segoe UI Variable Display -> Segoe UI
//   CJK:   PingFang SC -> HarmonyOS Sans SC -> Microsoft YaHei UI -> Noto Sans SC
//
// A single IDWriteTextFormat family name cannot cover both scripts, so the base
// family is the resolved LATIN family and a custom IDWriteFontFallback maps the
// CJK Unicode ranges to the resolved CJK family (IDWriteFontFallbackBuilder +
// AddMapping). CreateTextLayout() attaches that fallback to every layout the
// window creates; CreateTextFormat() keeps the base family honest.
//
// WHAT THIS FILE OWNS
//   * FontStackFamilies()      the token as one ordered list of eight names;
//   * LatinFamilies() / CjkFamilies()   the two ordered halves;
//   * ResolveFamily()          first Latin family the collection really has;
//   * ResolveCjkFamily()       first CJK family the collection really has;
//   * FallbackTargets()        the CJK names actually written into the fallback;
//   * CreateTextFormat()       the only path a new IDWriteTextFormat may take;
//   * CreateTextLayout()       the only path a new IDWriteTextLayout may take
//                              (it attaches the fallback);
//   * SystemFamilyName()       the Latin answer for GDI callers (CreateFontW).
//
// FAILURE POLICY: a collection without any preferred family falls back to
// Segoe UI (Latin) and the shipped system fallback (CJK). Nothing invents a
// family; injected collections in the suite assert the exact names.
//
// NEGATIVE CONTROLS (CONTRACTS D3):
//   PRISM_C103_MUTANT_SEGOE        ResolveFamily always answers Segoe UI (the
//       pre-C103 behaviour) -> "Inter must win" goes red.
//   PRISM_C103_MUTANT_NO_FALLBACK  the fallback carries NO CJK mapping, so a
//       Chinese run resolves to the Latin base font -> the fallback-target and
//       advance-width assertions go red.
// Both count their hits so the D27 marker can only print after the injected
// branch actually ran.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dwrite.h>
#include <dwrite_2.h>  // IDWriteFontFallbackBuilder / IDWriteTextLayout2

#include <wrl/client.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace prism::win::typography {

// BLUEPRINT:170 plus the Windows-native middle step. The order is the token.
inline const std::vector<std::wstring>& FontStackFamilies() {
  static const std::vector<std::wstring> families = {
      L"SF Pro Display",           // Apple Latin
      L"Inter",                    // open-source Latin
      L"Segoe UI Variable Display",// Win11 default Latin
      L"Segoe UI",                 // system Latin fallback
      L"PingFang SC",              // Apple CJK
      L"HarmonyOS Sans SC",        // Huawei CJK
      L"Microsoft YaHei UI",       // Win11 default CJK
      L"Noto Sans SC",             // open-source CJK fallback
  };
  return families;
}

inline const std::vector<std::wstring>& LatinFamilies() {
  static const std::vector<std::wstring> families = {
      L"SF Pro Display", L"Inter", L"Segoe UI Variable Display", L"Segoe UI"};
  return families;
}

inline const std::vector<std::wstring>& CjkFamilies() {
  static const std::vector<std::wstring> families = {
      L"PingFang SC", L"HarmonyOS Sans SC", L"Microsoft YaHei UI", L"Noto Sans SC"};
  return families;
}

// The one fallback literal in the product: every other file asks this header.
inline constexpr wchar_t kFallbackFamily[] = L"Segoe UI";

// C109: the numeric columns need a family whose every glyph advances the same
// (tabular figures equalize DIGITS, not punctuation -- "." still drifts in the
// proportional face). The brief allows a mono family for the columns, so the
// numbers use Consolas and the proportional case (the mutation arm) uses the
// system UI face. They live HERE because this is the ONE file allowed to
// spell a family name (core.c103_font_literals scans every other source line).
inline constexpr const wchar_t* kMonoFamily = L"Consolas";
inline constexpr const wchar_t* kProportionalFamily = L"Segoe UI";

inline const std::wstring& FallbackFamilyName() {
  static const std::wstring fallback = kFallbackFamily;
  return fallback;
}

#if defined(PRISM_C103_MUTANT_SEGOE)
inline unsigned& MutationHits() {
  static unsigned hits = 0;
  return hits;
}
inline bool MutationActive() { return MutationHits() != 0; }
#endif

#if defined(PRISM_C103_MUTANT_NO_FALLBACK)
inline unsigned& NoFallbackHits() {
  static unsigned hits = 0;
  return hits;
}
inline bool NoFallbackMutationActive() { return NoFallbackHits() != 0; }
#endif

namespace detail {

struct FamilyCache {
  std::mutex mutex;
  // Latin and CJK are different questions about the same collection, so they
  // get different maps. Values are node-based: a rehash never invalidates a
  // reference already handed out.
  std::unordered_map<IDWriteFontCollection*, std::wstring> latin;
  std::unordered_map<IDWriteFontCollection*, std::wstring> cjk;
};

inline FamilyCache& ResolveCache() {
  static FamilyCache cache;
  return cache;
}

inline bool CollectionHas(IDWriteFontCollection* collection, const wchar_t* family) {
  if (collection == nullptr || family == nullptr) return false;
  UINT32 index = 0;
  BOOL exists = FALSE;
  return SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists != FALSE;
}

inline const std::wstring& ResolveFirst(IDWriteFontCollection* collection,
                                        const std::vector<std::wstring>& candidates,
                                        std::unordered_map<IDWriteFontCollection*, std::wstring>& cache) {
  if (collection == nullptr) return FallbackFamilyName();
  std::lock_guard<std::mutex> lock(ResolveCache().mutex);
  const auto cached = cache.find(collection);
  if (cached != cache.end()) return cached->second;
  std::wstring resolved = kFallbackFamily;
  for (const std::wstring& candidate : candidates) {
    if (CollectionHas(collection, candidate.c_str())) {
      resolved = candidate;
      break;
    }
  }
  return cache.emplace(collection, std::move(resolved)).first->second;
}

}  // namespace detail

// Latin base family. Cached per collection: a paint pass never re-walks it.
inline const std::wstring& ResolveFamily(IDWriteFactory* factory,
                                         IDWriteFontCollection* collection) {
#if defined(PRISM_C103_MUTANT_SEGOE)
  static_cast<void>(factory);
  static_cast<void>(collection);
  ++MutationHits();
  return FallbackFamilyName();
#else
  static_cast<void>(factory);
  return detail::ResolveFirst(collection, LatinFamilies(),
                              detail::ResolveCache().latin);
#endif
}

// CJK family for the fallback mapping. On a machine with none of the four CJK
// names installed this returns Segoe UI; FallbackTargets() then has no CJK
// candidate and the system fallback remains in charge (never an invented name).
inline const std::wstring& ResolveCjkFamily(IDWriteFactory* factory,
                                            IDWriteFontCollection* collection) {
  static_cast<void>(factory);
  return detail::ResolveFirst(collection, CjkFamilies(),
                              detail::ResolveCache().cjk);
}

// Test aid: the suite injects synthetic collections that are freed between
// cases, and a recycled pointer must never inherit the previous answer.
// Production never calls this.
inline void ResetResolveCacheForTests();

namespace detail {

struct FallbackEntry {
  Microsoft::WRL::ComPtr<IDWriteFontFallback> fallback;
  std::vector<std::wstring> targets;
};

struct FallbackCache {
  std::mutex mutex;
  std::unordered_map<IDWriteFactory*, FallbackEntry> byFactory;
};

inline FallbackCache& Fallbacks() {
  static FallbackCache cache;
  return cache;
}

// CJK blocks DWrite would otherwise hand to a Latin face: radicals, CJK
// symbols, Ext-A, unified ideographs, compatibility ideographs, full-width
// forms and Ext-B (surrogates).
inline const DWRITE_UNICODE_RANGE* CjkRanges(std::size_t* count) {
  static const DWRITE_UNICODE_RANGE ranges[] = {
      {0x2E80, 0x2EFF},   {0x3000, 0x303F},   {0x3400, 0x4DBF},
      {0x4E00, 0x9FFF},   {0xF900, 0xFAFF},   {0xFF00, 0xFFEF},
      {0x20000, 0x2FA1F},
  };
  if (count != nullptr) *count = sizeof(ranges) / sizeof(ranges[0]);
  return ranges;
}

inline FallbackEntry BuildFallback(IDWriteFactory* factory) {
  FallbackEntry entry;
  if (factory == nullptr) return entry;

  Microsoft::WRL::ComPtr<IDWriteFactory2> factory2;
  if (FAILED(factory->QueryInterface(IID_PPV_ARGS(factory2.GetAddressOf()))) ||
      factory2 == nullptr) {
    return entry;
  }
  Microsoft::WRL::ComPtr<IDWriteFontCollection> system;
  if (FAILED(factory->GetSystemFontCollection(system.GetAddressOf(), FALSE)) ||
      system == nullptr) {
    return entry;
  }

  Microsoft::WRL::ComPtr<IDWriteFontFallbackBuilder> builder;
  if (FAILED(factory2->CreateFontFallbackBuilder(builder.GetAddressOf())) ||
      builder == nullptr) {
    return entry;
  }

#if defined(PRISM_C103_MUTANT_NO_FALLBACK)
  // The pre-fallback behaviour: the layout has no CJK mapping, so a Chinese
  // run is shaped by the Latin base family. The TARGET LIST is empty too, so
  // FallbackTargets() reports the truth about what was wired.
  ++NoFallbackHits();
#else
  for (const std::wstring& candidate : CjkFamilies()) {
    if (CollectionHas(system.Get(), candidate.c_str())) {
      entry.targets.push_back(candidate);
    }
  }
  // Build the C-string view AFTER the vector is final: push_back may reallocate
  // and would leave earlier c_str() pointers dangling.
  std::vector<const WCHAR*> names;
  names.reserve(entry.targets.size());
  for (const std::wstring& target : entry.targets) {
    names.push_back(target.c_str());
  }
  std::size_t rangeCount = 0;
  const DWRITE_UNICODE_RANGE* ranges = CjkRanges(&rangeCount);
  if (!names.empty()) {
    builder->AddMapping(ranges, static_cast<UINT32>(rangeCount), names.data(),
                        static_cast<UINT32>(names.size()), system.Get());
  }
#endif

  if (FAILED(builder->CreateFontFallback(entry.fallback.GetAddressOf()))) {
    entry.fallback.Reset();
  }
  return entry;
}

}  // namespace detail

// The CJK names this factory's fallback really carries, in priority order.
// Empty = the system fallback is still in charge.
inline const std::vector<std::wstring>& FallbackTargets(IDWriteFactory* factory) {
  static const std::vector<std::wstring> empty;
  if (factory == nullptr) return empty;
  detail::FallbackCache& cache = detail::Fallbacks();
  std::lock_guard<std::mutex> lock(cache.mutex);
  const auto found = cache.byFactory.find(factory);
  if (found != cache.byFactory.end()) return found->second.targets;
  // Build while holding the lock: the builder is not thread-safe and two
  // renderers racing here would otherwise build two fallbacks.
  detail::FallbackEntry entry = detail::BuildFallback(factory);
  return cache.byFactory.emplace(factory, std::move(entry)).first->second.targets;
}

inline IDWriteFontFallback* FallbackForFactory(IDWriteFactory* factory) {
  if (factory == nullptr) return nullptr;
  detail::FallbackCache& cache = detail::Fallbacks();
  std::lock_guard<std::mutex> lock(cache.mutex);
  const auto found = cache.byFactory.find(factory);
  if (found != cache.byFactory.end()) return found->second.fallback.Get();
  detail::FallbackEntry entry = detail::BuildFallback(factory);
  return cache.byFactory.emplace(factory, std::move(entry)).first->second.fallback.Get();
}

inline void ResetResolveCacheForTests() {
  detail::FamilyCache& families = detail::ResolveCache();
  {
    std::lock_guard<std::mutex> lock(families.mutex);
    families.latin.clear();
    families.cjk.clear();
  }
  detail::FallbackCache& fallbacks = detail::Fallbacks();
  std::lock_guard<std::mutex> lock(fallbacks.mutex);
  fallbacks.byFactory.clear();
}

// The only way a new IDWriteTextFormat is created in PrismWin.
inline HRESULT CreateTextFormat(IDWriteFactory* factory, float size,
                                DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                                const wchar_t* locale, IDWriteTextFormat** out) {
  if (factory == nullptr || out == nullptr) return E_INVALIDARG;
  *out = nullptr;

  Microsoft::WRL::ComPtr<IDWriteFontCollection> system;
  if (FAILED(factory->GetSystemFontCollection(system.GetAddressOf(), FALSE)) ||
      system == nullptr) {
    return E_FAIL;
  }
  const std::wstring& family = ResolveFamily(factory, system.Get());
  return factory->CreateTextFormat(
      family.c_str(), nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size,
      locale != nullptr ? locale : L"en-US", out);
}

// C109: a format for a family the caller NAMES (the numeric columns pin a mono
// face). It still goes through this file so no other source line calls DWrite
// directly -- core.c103_font_literals asserts that.
inline HRESULT CreateTextFormatInFamily(IDWriteFactory* factory, const wchar_t* family,
                                        float size, DWRITE_FONT_WEIGHT weight,
                                        DWRITE_FONT_STYLE style, const wchar_t* locale,
                                        IDWriteTextFormat** out) {
  if (factory == nullptr || out == nullptr || family == nullptr) return E_INVALIDARG;
  *out = nullptr;
  return factory->CreateTextFormat(family, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL,
                                   size, locale != nullptr ? locale : L"en-US", out);
}
// The only way a new IDWriteTextLayout is created in PrismWin. The custom
// fallback is attached BEFORE the caller draws, so CJK runs take the resolved
// CJK family while Latin stays on the format's base family.
inline HRESULT CreateTextLayout(IDWriteFactory* factory, const WCHAR* text,
                                UINT32 length, IDWriteTextFormat* format,
                                FLOAT maxWidth, FLOAT maxHeight,
                                IDWriteTextLayout** out) {
  if (factory == nullptr || out == nullptr) return E_INVALIDARG;
  *out = nullptr;
  const HRESULT hr = factory->CreateTextLayout(text, length, format, maxWidth,
                                               maxHeight, out);
  if (FAILED(hr) || *out == nullptr) return hr;

  IDWriteFontFallback* fallback = FallbackForFactory(factory);
  if (fallback != nullptr) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout2> layout2;
    if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(layout2.GetAddressOf()))) &&
        layout2 != nullptr) {
      layout2->SetFontFallback(fallback);
    }
  }
  return hr;
}

// What Windows will actually use, for the suite and the PR evidence:
//   latin=Segoe UI Variable Display; cjk=Microsoft YaHei UI; fallback=[...]
inline std::wstring DescribeResolution(IDWriteFactory* factory) {
  std::wstring text;
  Microsoft::WRL::ComPtr<IDWriteFontCollection> system;
  const wchar_t* latin = kFallbackFamily;
  const wchar_t* cjk = kFallbackFamily;
  if (factory != nullptr &&
      SUCCEEDED(factory->GetSystemFontCollection(system.GetAddressOf(), FALSE)) &&
      system != nullptr) {
    latin = ResolveFamily(factory, system.Get()).c_str();
    cjk = ResolveCjkFamily(factory, system.Get()).c_str();
  }
  text += L"latin=";
  text += latin;
  text += L"; cjk=";
  text += cjk;
  text += L"; fallback=[";
  const std::vector<std::wstring>& targets = FallbackTargets(factory);
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (i != 0) text += L", ";
    text += targets[i];
  }
  text += L"]";
  return text;
}

namespace detail {

struct GdiFamilyProbe {
  unsigned present = 0;  // bit i = FontStackFamilies()[i] installed
};

inline int CALLBACK CollectGdiFamily(const LOGFONTW* logFont, const TEXTMETRICW*,
                                     DWORD, LPARAM param) {
  auto* probe = reinterpret_cast<GdiFamilyProbe*>(param);
  if (probe == nullptr || logFont == nullptr) return 1;
  const std::vector<std::wstring>& wanted = FontStackFamilies();
  for (std::size_t i = 0; i < wanted.size(); ++i) {
    if (_wcsicmp(wanted[i].c_str(), logFont->lfFaceName) == 0) {
      probe->present |= 1u << i;
    }
  }
  return 1;  // keep enumerating: the order comes from the stack, not the DC
}

}  // namespace detail

// GDI callers (CreateFontW inside PasswordDialog) have no IDWriteFactory and
// must not drag dwrite.lib into a dialog that never used it. This asks GDI's
// own enumerator which names exist ONCE, then picks the first LATIN family by
// the same order and caches the NAME.
inline const std::wstring& SystemFamilyName() {
  static const std::wstring family = [] {
    const std::vector<std::wstring>& wanted = LatinFamilies();
    detail::GdiFamilyProbe probe;
    HDC dc = ::GetDC(nullptr);
    if (dc != nullptr) {
      LOGFONTW pattern{};
      pattern.lfCharSet = DEFAULT_CHARSET;
      pattern.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
      ::EnumFontFamiliesExW(dc, &pattern, &detail::CollectGdiFamily,
                            reinterpret_cast<LPARAM>(&probe), 0);
      ::ReleaseDC(nullptr, dc);
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
      if ((probe.present & (1u << i)) != 0) return std::wstring(wanted[i]);
    }
    return std::wstring(kFallbackFamily);
  }();
  return family;
}

}  // namespace prism::win::typography
