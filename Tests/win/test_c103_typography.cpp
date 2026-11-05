// PrismArchive -- PrismWin
// C103: the font stack is ONE list and ONE resolver (BLUEPRINT:170).
//
// The defect: every DirectWrite call site hardcoded L"Segoe UI", so the design
// token --font-stack-system (SF Pro Display / Inter -> PingFang SC /
// HarmonyOS Sans -> Segoe UI) was decorative. This suite drives the REAL
// typography::ResolveFamily / CreateTextFormat against INJECTED font
// collections, which is the only way to prove the order on a machine that may
// not have any of the preferred families installed:
//
//   * FontStackFamilies() is exactly the blueprint order, five names;
//   * a collection {Inter, Segoe UI}      -> "Inter"
//   * a collection {Segoe UI}             -> "Segoe UI"
//   * a collection {Segoe UI, Inter}      -> "Inter"   (order, not membership)
//   * a collection {Inter, SF Pro Display}-> "SF Pro Display" (highest wins)
//   * a collection with none of them      -> "Segoe UI" (the documented fallback)
//   * the SECOND call on one collection performs ZERO FindFamilyName calls
//     (the cache: a paint pass must not walk the stack per frame);
//   * CreateTextFormat() returns a format whose GetFontFamilyName() equals
//     ResolveFamily() for the system collection -- i.e. the entry really applies
//     the resolved family and nothing re-hardcodes a default;
//   * on this machine, if none of the four preferred families is installed,
//     the resolved family must be exactly Segoe UI (no invented substitution).
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C103_MUTANT_SEGOE makes
// ResolveFamily return Segoe UI unconditionally -- the shipped behaviour. The
// {Inter, Segoe UI} assertion MUST go red; the verdict prints at that first
// failure and its predicate proves the injected branch ran (MutationActive()).
// The CTest entry requires that marker, never WILL_FAIL.
#include <windows.h>

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../mini_test.hpp"

#include "../../Src/PrismWin/Typography.hpp"

#pragma comment(lib, "dwrite.lib")

namespace {

using Microsoft::WRL::ComPtr;
using prism::win::typography::CreateTextFormat;
using prism::win::typography::CjkFamilies;
using prism::win::typography::DescribeResolution;
using prism::win::typography::FallbackForFactory;
using prism::win::typography::FallbackTargets;
using prism::win::typography::FontStackFamilies;
using prism::win::typography::LatinFamilies;
using prism::win::typography::ResetResolveCacheForTests;
using prism::win::typography::ResolveCjkFamily;
using prism::win::typography::ResolveFamily;
using prism::win::typography::SystemFamilyName;

// An IDWriteFontCollection with exactly the families we hand it. FindFamilyName
// counts its calls so the cache assertion measures the real lookup cost; the
// other methods are never reached by the code under test.
class FakeFontCollection final : public IDWriteFontCollection {
 public:
  explicit FakeFontCollection(std::vector<std::wstring> families)
      : families_(std::move(families)) {}

  IFACEMETHOD_(ULONG, AddRef)() override { return ++refs_; }

  IFACEMETHOD_(ULONG, Release)() override {
    const ULONG refs = --refs_;
    if (refs == 0) delete this;
    return refs;
  }

  IFACEMETHOD(QueryInterface)(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (IsEqualIID(iid, __uuidof(IUnknown)) ||
        IsEqualIID(iid, __uuidof(IDWriteFontCollection))) {
      *object = static_cast<IDWriteFontCollection*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  IFACEMETHOD_(UINT32, GetFontFamilyCount)() override {
    return static_cast<UINT32>(families_.size());
  }

  IFACEMETHOD(GetFontFamily)(UINT32, IDWriteFontFamily** family) override {
    if (family != nullptr) *family = nullptr;
    return E_NOTIMPL;
  }

  IFACEMETHOD(FindFamilyName)(const WCHAR* name, UINT32* index, BOOL* exists) override {
    ++findCalls_;
    if (name == nullptr || index == nullptr || exists == nullptr) return E_INVALIDARG;
    for (std::size_t i = 0; i < families_.size(); ++i) {
      if (_wcsicmp(families_[i].c_str(), name) == 0) {
        *index = static_cast<UINT32>(i);
        *exists = TRUE;
        return S_OK;
      }
    }
    *index = UINT32_MAX;
    *exists = FALSE;
    return S_OK;
  }

  IFACEMETHOD(GetFontFromFontFace)(IDWriteFontFace*, IDWriteFont** font) override {
    if (font != nullptr) *font = nullptr;
    return E_NOTIMPL;
  }

  unsigned FindCalls() const { return findCalls_; }

 private:
  ULONG refs_ = 1;
  std::vector<std::wstring> families_;
  unsigned findCalls_ = 0;
};

ComPtr<IDWriteFactory> MakeFactory() {
  ComPtr<IDWriteFactory> factory;
  const HRESULT hr = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
  if (FAILED(hr)) factory.Reset();
  return factory;
}

bool CollectionHas(IDWriteFontCollection* collection, const wchar_t* family) {
  if (collection == nullptr) return false;
  UINT32 index = 0;
  BOOL exists = FALSE;
  return SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists != FALSE;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(PRISM_C103_MUTANT_SEGOE)
  std::printf("C103 MUTANT ACTIVE: segoe\n");
  ::prism::test::SetMutationVerdict(
      "C103 MUTATION CAUGHT: segoe",
      [] { return ::prism::win::typography::MutationActive(); });
#elif defined(PRISM_C103_MUTANT_NO_FALLBACK)
  std::printf("C103 MUTANT ACTIVE: no_fallback\n");
  ::prism::test::SetMutationVerdict(
      "C103 MUTATION CAUGHT: no_fallback",
      [] { return ::prism::win::typography::NoFallbackMutationActive(); });
#endif
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures != 0) return failures;
  std::puts("ALL C103 TYPOGRAPHY CHECKS PASSED");
  return 0;
}

PRISM_TEST("c103 the token is one ordered eight-family stack") {
  const std::vector<std::wstring>& families = FontStackFamilies();
  PRISM_CHECK_EQ(families.size(), std::size_t{8});
  if (families.size() == 8) {
    PRISM_CHECK(families[0] == L"SF Pro Display");
    PRISM_CHECK(families[1] == L"Inter");
    PRISM_CHECK(families[2] == L"Segoe UI Variable Display");
    PRISM_CHECK(families[3] == L"Segoe UI");
    PRISM_CHECK(families[4] == L"PingFang SC");
    PRISM_CHECK(families[5] == L"HarmonyOS Sans SC");
    PRISM_CHECK(families[6] == L"Microsoft YaHei UI");
    PRISM_CHECK(families[7] == L"Noto Sans SC");
  }
  const std::vector<std::wstring>& latin = LatinFamilies();
  PRISM_CHECK_EQ(latin.size(), std::size_t{4});
  const std::vector<std::wstring>& cjk = CjkFamilies();
  PRISM_CHECK_EQ(cjk.size(), std::size_t{4});
  if (latin.size() == 4 && cjk.size() == 4) {
    PRISM_CHECK(latin[0] == L"SF Pro Display");
    PRISM_CHECK(latin[3] == L"Segoe UI");
    PRISM_CHECK(cjk[0] == L"PingFang SC");
    PRISM_CHECK(cjk[2] == L"Microsoft YaHei UI");
  }
}

PRISM_TEST("c103 ResolveFamily returns the first family that really exists") {
  const ComPtr<IDWriteFactory> factory = MakeFactory();
  PRISM_CHECK(factory != nullptr);
  if (!factory) return;

  ResetResolveCacheForTests();

  // Kept alive for the whole case: the resolver caches by collection pointer.
  ComPtr<IDWriteFontCollection> interAndFallback;
  auto* interAndFallbackRaw = new FakeFontCollection({L"Inter", L"Segoe UI"});
  interAndFallback.Attach(interAndFallbackRaw);

  ComPtr<IDWriteFontCollection> fallbackOnly;
  fallbackOnly.Attach(new FakeFontCollection({L"Segoe UI"}));

  ComPtr<IDWriteFontCollection> reversed;
  reversed.Attach(new FakeFontCollection({L"Segoe UI", L"Inter"}));

  ComPtr<IDWriteFontCollection> topWins;
  topWins.Attach(new FakeFontCollection({L"Inter", L"SF Pro Display"}));

  ComPtr<IDWriteFontCollection> neither;
  neither.Attach(new FakeFontCollection({L"Arial", L"Consolas"}));

  // ASSERTION 2 -- the one the mutation arm breaks.
  PRISM_CHECK(ResolveFamily(factory.Get(), interAndFallback.Get()) == L"Inter");
  PRISM_CHECK(ResolveFamily(factory.Get(), fallbackOnly.Get()) == L"Segoe UI");
  PRISM_CHECK(ResolveFamily(factory.Get(), reversed.Get()) == L"Inter");
  PRISM_CHECK(ResolveFamily(factory.Get(), topWins.Get()) == L"SF Pro Display");
  PRISM_CHECK(ResolveFamily(factory.Get(), neither.Get()) == L"Segoe UI");

  // The Windows-native pair: the base family and the CJK fallback answer
  // DIFFERENT questions and must never collapse into one name.
  ComPtr<IDWriteFontCollection> win11Pair;
  win11Pair.Attach(new FakeFontCollection({L"Segoe UI Variable Display", L"Microsoft YaHei UI"}));
  PRISM_CHECK(ResolveFamily(factory.Get(), win11Pair.Get()) == L"Segoe UI Variable Display");
  PRISM_CHECK(ResolveCjkFamily(factory.Get(), win11Pair.Get()) == L"Microsoft YaHei UI");

  // The cache: the first call walked the stack (>= 1 lookup); the second one
  // must not add a single FindFamilyName call.
  ComPtr<IDWriteFontCollection> cached;
  auto* cachedRaw = new FakeFontCollection({L"Inter"});
  cached.Attach(cachedRaw);
  PRISM_CHECK(ResolveFamily(factory.Get(), cached.Get()) == L"Inter");
  const unsigned afterFirst = cachedRaw->FindCalls();
  PRISM_CHECK(afterFirst >= 1);
  PRISM_CHECK(ResolveFamily(factory.Get(), cached.Get()) == L"Inter");
  PRISM_CHECK_EQ(cachedRaw->FindCalls(), afterFirst);
}

PRISM_TEST("c103 the CJK fallback is wired and changes Chinese shaping") {
  const ComPtr<IDWriteFactory> factory = MakeFactory();
  PRISM_CHECK(factory != nullptr);
  if (!factory) return;

  ResetResolveCacheForTests();
  ComPtr<IDWriteFontCollection> system;
  if (FAILED(factory->GetSystemFontCollection(system.GetAddressOf(), FALSE)) || !system) {
    PRISM_CHECK(false);
    return;
  }
  const std::wstring& latin = ResolveFamily(factory.Get(), system.Get());
  const std::wstring& cjk = ResolveCjkFamily(factory.Get(), system.Get());
  PRISM_CHECK(std::find(LatinFamilies().begin(), LatinFamilies().end(), latin) !=
              LatinFamilies().end());
  PRISM_CHECK(std::find(CjkFamilies().begin(), CjkFamilies().end(), cjk) !=
              CjkFamilies().end());

  const std::vector<std::wstring>& targets = FallbackTargets(factory.Get());
  const std::wstring description = DescribeResolution(factory.Get());
  std::printf("  C103 RESOLUTION: latin=%ls cjk=%ls\n", latin.c_str(), cjk.c_str());
  std::printf("  C103 DESCRIBE: %ls\n", description.c_str());
  for (const std::wstring& target : targets) {
    std::printf("    fallback target: %ls\n", target.c_str());
  }

  // ASSERTION: the fallback really carries the resolved CJK family, first.
  PRISM_CHECK(!targets.empty());
  if (!targets.empty()) PRISM_CHECK(targets.front() == cjk);

  // Advance-width evidence: the same Chinese run must shape differently with the
  // fallback than with an EMPTY fallback (which leaves Chinese on the Latin
  // base). PRISM_C103_MUTANT_NO_FALLBACK makes the first case empty too, so
  // this assertion goes red.
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(CreateTextFormat(factory.Get(), 16.0f, DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL, L"zh-CN",
                              format.GetAddressOf()))) {
    PRISM_CHECK(false);
    return;
  }

  ComPtr<IDWriteFontFallback> emptyFallback;
  ComPtr<IDWriteFactory2> factory2;
  if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(factory2.GetAddressOf()))) &&
      factory2 != nullptr) {
    ComPtr<IDWriteFontFallbackBuilder> builder;
    if (SUCCEEDED(factory2->CreateFontFallbackBuilder(builder.GetAddressOf())) &&
        builder != nullptr) {
      builder->CreateFontFallback(emptyFallback.GetAddressOf());
    }
  }
  PRISM_CHECK(emptyFallback != nullptr);

  const wchar_t kChinese[] = L"压缩包子文件夹";
  const UINT32 length = static_cast<UINT32>(sizeof(kChinese) / sizeof(kChinese[0]) - 1);
  const auto measure = [&](IDWriteFontFallback* fallback) -> double {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(kChinese, length, format.Get(), 4096.0f, 200.0f,
                                         layout.GetAddressOf())) ||
        layout == nullptr) {
      return -1.0;
    }
    if (fallback != nullptr) {
      ComPtr<IDWriteTextLayout2> layout2;
      if (SUCCEEDED(layout.As(&layout2)) && layout2 != nullptr) {
        layout2->SetFontFallback(fallback);
      }
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return -1.0;
    return static_cast<double>(metrics.width);
  };
  const double withFallback = measure(FallbackForFactory(factory.Get()));
  const double withoutFallback = measure(emptyFallback.Get());
  std::printf("  C103 CJK advance: fallback=%.3f empty=%.3f\n", withFallback,
              withoutFallback);
  PRISM_CHECK(withFallback > 0.0);
  PRISM_CHECK(withoutFallback > 0.0);
  const double delta = withFallback - withoutFallback;
  PRISM_CHECK(delta > 0.5 || delta < -0.5);
}

PRISM_TEST("c103 every TextFormat is created through the resolved family") {
  const ComPtr<IDWriteFactory> factory = MakeFactory();
  PRISM_CHECK(factory != nullptr);
  if (!factory) return;

  ComPtr<IDWriteFontCollection> system;
  PRISM_CHECK(SUCCEEDED(factory->GetSystemFontCollection(system.GetAddressOf(), FALSE)));
  PRISM_CHECK(system != nullptr);
  if (!system) return;

  const std::wstring& resolved = ResolveFamily(factory.Get(), system.Get());

  // The resolved name is always one of the token's five names: a fallback that
  // invented a sixth family would be drift.
  const std::vector<std::wstring>& families = FontStackFamilies();
  PRISM_CHECK(std::find(families.begin(), families.end(), resolved) != families.end());

  // On this machine, if none of the four preferred families is installed, the
  // answer MUST be Segoe UI -- the documented Windows fallback, not a silent
  // substitution (the card's acceptance asks for the truth here).
  bool anyPreferred = false;
  for (std::size_t i = 0; i + 1 < families.size(); ++i) {
    if (CollectionHas(system.Get(), families[i].c_str())) anyPreferred = true;
  }
  if (!anyPreferred) {
    PRISM_CHECK(resolved == L"Segoe UI");
  }

  ComPtr<IDWriteTextFormat> format;
  const HRESULT hr = CreateTextFormat(factory.Get(), 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, L"en-US",
                                      format.GetAddressOf());
  PRISM_CHECK(SUCCEEDED(hr));
  PRISM_CHECK(format != nullptr);
  if (format) {
    const UINT32 length = format->GetFontFamilyNameLength();
    std::wstring name(static_cast<std::size_t>(length) + 1, L'\0');
    PRISM_CHECK(SUCCEEDED(format->GetFontFamilyName(&name[0], length + 1)));
    name.resize(length);
    PRISM_CHECK(name == resolved);
  }

  // The GDI path (PasswordDialog's CreateFontW) asks the same question and
  // must get the same answer.
  PRISM_CHECK(SystemFamilyName() == resolved);

  // A null factory is a programming error, not a silent default.
  PRISM_CHECK_EQ(CreateTextFormat(nullptr, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, L"en-US", nullptr),
                 E_INVALIDARG);
}
