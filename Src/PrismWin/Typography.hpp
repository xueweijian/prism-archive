// PrismArchive -- PrismWin
// Typography: ONE font stack, ONE TextFormat entry (card C103, BLUEPRINT:170).
//
// The design token --font-stack-system is, in order:
//     SF Pro Display / Inter            (Latin)
//     PingFang SC / HarmonyOS Sans SC   (CJK)
// with Segoe UI as the Windows fallback. Before this header, 19 DirectWrite
// call sites and two GDI CreateFontW calls each hardcoded L"Segoe UI", so the
// token could never resolve to anything else and a machine with Inter installed
// still painted Segoe.
//
// WHAT THIS FILE OWNS
//   * FontStackFamilies()      the token, in priority order, as ONE list;
//   * ResolveFamily(factory, collection)   the first family the collection
//     really contains (FindFamilyName, not a guess), cached per collection so a
//     paint pass never re-walks the stack;
//   * CreateTextFormat(...)    the only path a new IDWriteTextFormat may take;
//   * SystemFamilyName()       the resolved name for GDI callers (CreateFontW),
//     which have no IDWriteFactory of their own.
//
// FAILURE POLICY: a collection that contains none of the preferred families
// falls back to Segoe UI -- the same family Windows would have used before --
// so a machine without the design fonts still paints, it just stays on the
// fallback. Nothing here silently substitutes a DIFFERENT family: the test
// suite injects collections and asserts the exact name that comes back.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C103_MUTANT_SEGOE makes ResolveFamily
// return Segoe UI unconditionally (the shipped behaviour this card removes) and
// counts the hits, so the suite's "Inter must win" assertion goes red and the
// D27 marker can only print after the injected branch actually ran.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dwrite.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace prism::win::typography {

// BLUEPRINT:170 -- the order is the token, not a menu of alternatives.
inline const std::vector<std::wstring>& FontStackFamilies() {
  static const std::vector<std::wstring> families = {
      L"SF Pro Display",
      L"Inter",
      L"PingFang SC",
      L"HarmonyOS Sans",
      L"Segoe UI",  // the Windows fallback; kept last on purpose
  };
  return families;
}

// The one fallback literal in the product: every other file asks this header.
inline constexpr wchar_t kFallbackFamily[] = L"Segoe UI";

inline const std::wstring& FallbackFamilyName() {
  static const std::wstring fallback = kFallbackFamily;
  return fallback;
}

#if defined(PRISM_C103_MUTANT_SEGOE)
// D3 proof that the injected branch ran: the suite's predicate reads this.
inline unsigned& MutationHits() {
  static unsigned hits = 0;
  return hits;
}
inline bool MutationActive() { return MutationHits() != 0; }
#endif

namespace detail {

struct FamilyCache {
  std::mutex mutex;
  // Keyed by the collection pointer. DWrite hands the same system collection
  // back for the lifetime of a factory, and the fake collections in the suite
  // stay alive for the duration of their case, so a pointer is a stable key.
  // Values are node-based: a rehash never invalidates a returned reference.
  std::unordered_map<IDWriteFontCollection*, std::wstring> resolved;
};

inline FamilyCache& ResolveCache() {
  static FamilyCache cache;
  return cache;
}

inline bool SameFamily(const wchar_t* a, const wchar_t* b) {
  return a != nullptr && b != nullptr && _wcsicmp(a, b) == 0;
}

}  // namespace detail

// Resolve the first family of FontStackFamilies() that the collection really
// has. A null collection yields the documented fallback; production creation
// goes through CreateTextFormat(), which passes the factory's system collection
// (a caller with a factory and no collection can fetch it the same way). The
// result is cached per collection: a hit is one hash lookup, not five
// FindFamilyName calls per painted frame.
inline const std::wstring& ResolveFamily(IDWriteFactory* factory,
                                         IDWriteFontCollection* collection) {
#if defined(PRISM_C103_MUTANT_SEGOE)
  // The pre-C103 behaviour this card deletes: always Segoe UI, never look.
  static_cast<void>(factory);
  static_cast<void>(collection);
  ++MutationHits();
  return FallbackFamilyName();
#else
  if (collection == nullptr) {
    static_cast<void>(factory);
    return FallbackFamilyName();
  }
  detail::FamilyCache& cache = detail::ResolveCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  const auto cached = cache.resolved.find(collection);
  if (cached != cache.resolved.end()) return cached->second;

  std::wstring resolved = kFallbackFamily;
  for (const std::wstring& candidate : FontStackFamilies()) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(collection->FindFamilyName(candidate.c_str(), &index, &exists)) &&
        exists != FALSE) {
      resolved = candidate;
      break;
    }
  }
  return cache.resolved.emplace(collection, std::move(resolved)).first->second;
#endif
}

// Test aid: the suite injects synthetic collections that are freed between
// cases, and a recycled pointer must never inherit the previous answer.
// Production never calls this.
inline void ResetResolveCacheForTests() {
  detail::FamilyCache& cache = detail::ResolveCache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  cache.resolved.clear();
}

// The only way a new IDWriteTextFormat is created in PrismWin. The family is
// the resolved one, never a literal; the OUT parameter keeps the existing
// ComPtr::ReleaseAndGetAddressOf() call sites mechanical.
inline HRESULT CreateTextFormat(IDWriteFactory* factory, float size,
                                DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                                const wchar_t* locale, IDWriteTextFormat** out) {
  if (factory == nullptr || out == nullptr) return E_INVALIDARG;
  *out = nullptr;

  IDWriteFontCollection* system = nullptr;
  if (FAILED(factory->GetSystemFontCollection(&system, FALSE)) || system == nullptr) {
    return E_FAIL;
  }
  const std::wstring& family = ResolveFamily(factory, system);
  const HRESULT hr = factory->CreateTextFormat(
      family.c_str(), nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size,
      locale != nullptr ? locale : L"en-US", out);
  // The factory keeps its system collection alive; this only drops the
  // reference GetSystemFontCollection just handed us.
  system->Release();
  return hr;
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
// own enumerator which names exist ONCE, then picks by the SAME stack order and
// caches the NAME -- one decision, two renderers.
inline const std::wstring& SystemFamilyName() {
  static const std::wstring family = [] {
    const std::vector<std::wstring>& wanted = FontStackFamilies();
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
