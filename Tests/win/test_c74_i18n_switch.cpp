// PrismArchive -- PrismWin (card C74)
// A REAL window, a REAL language switch, and what the user can SEE afterwards.
//
// BEFORE THIS CARD: the eight band labels were compiled-in Chinese in
// ActionBarModel.hpp, so switching language changed the menu and the Settings
// rows but left the band in Chinese; the sidebar's telemetry capsule
// (NOT ENCRYPTED / UNKNOWN | DICT -- / RATIO 53.6%) was a set of literals in
// SidebarView, so it stayed English in a Chinese build.
//
// WHAT THIS SUITE PROVES, in order:
//   1. ApplyLanguage(en-US) makes all eight band labels English, read back from
//      the window (ActionBarLabelAt) -- not from a comment;
//   2. ApplyLanguage(zh-CN) makes them all Chinese, and each one differs;
//   3. the sidebar's ARCHIVE TREE heading and the capsule's three lines follow
//      the SAME switch;
//   4. the band's LABEL PIXELS really change (rendered through the real
//      GalleryRenderer onto a WIC target, same rectangle, different ink);
//   5. switching back restores English -- the switch is not one-way.
//
// NEGATIVE CONTROL (CONTRACTS D3 + D26 + D27): the same source compiled with
// PRISM_C74_MUTANT_NO_LABEL_REBUILD, where ApplyLanguage() refreshes the sidebar
// but never rebuilds the band's labels. Case 1 MUST go red; the verdict marker
// "C74 MUTATION CAUGHT: no_label_rebuild" is printed at the first red assertion
// and only when the injection is proved active.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using Microsoft::WRL::ComPtr;
using prism::actionbar::kActionCount;

#ifndef PRISM_C74_LANG_DIR
#define PRISM_C74_LANG_DIR "Assets\\Lang"
#endif

namespace {

bool g_skip = false;

bool WindowStationUsable() {
  static const bool usable = [] {
    const HWND probe = CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 10, 10,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (probe == nullptr) return false;
    DestroyWindow(probe);
    return true;
  }();
  return usable;
}

bool RequireStation() {
  if (!WindowStationUsable()) {
    g_skip = true;
    std::printf("    [skip] no interactive window station\n");
    return false;
  }
  return true;
}

prism::i18n::Translator& TranslatorFor(const wchar_t* tag) {
  static prism::i18n::Translator en;
  static prism::i18n::Translator zh;
  static bool loaded = false;
  if (!loaded) {
    std::string why;
    const std::string dir(PRISM_C74_LANG_DIR);
    en.LoadCatalogFile(dir + "\\en-US.json", &why);
    zh.LoadCatalogFile(dir + "\\en-US.json", &why);  // fallback chain, like the app
    zh.LoadCatalogFile(dir + "\\zh-CN.json", &why);
    en.SetLanguage("en-US");
    zh.SetLanguage("zh-CN");
    loaded = true;
  }
  return tag == std::wstring(L"zh-CN") ? zh : en;
}

const char* kEnglish[kActionCount] = {"Open", "Extract to...", "Compress", "Add",
                                      "Delete", "Test", "View", "Search"};
// UTF-8 byte escapes so the compiler's source charset cannot turn these into
// mojibake and still pass.
const char* kChinese[kActionCount] = {
    "\xE6\x89\x93\xE5\xBC\x80",                          // 打开
    "\xE8\xA7\xA3\xE5\x8E\x8B\xE5\x88\xB0\xE2\x80\xA6",  // 解压到…
    "\xE5\x8E\x8B\xE7\xBC\xA9",                          // 压缩
    "\xE6\xB7\xBB\xE5\x8A\xA0",                          // 添加
    "\xE5\x88\xA0\xE9\x99\xA4",                          // 删除
    "\xE6\xB5\x8B\xE8\xAF\x95",                          // 测试
    "\xE6\x9F\xA5\xE7\x9C\x8B",                          // 查看
    "\xE6\x90\x9C\xE7\xB4\xA2"};                         // 搜索

const char* kEnTree = "ARCHIVE TREE";
const char* kZhTree = "\xE5\xBD\x92\xE6\xA1\xA3\xE7\x9B\xAE\xE5\xBD\x95\xE6\xA0\x91";  // 归档目录树
const char* kEnNotEncrypted = "NOT ENCRYPTED";
const char* kZhNotEncrypted = "\xE6\x9C\xAA\xE5\x8A\xA0\xE5\xAF\x86";  // 未加密
// C105 (plan 3.2): the info card is a LABEL/VALUE stack, so each pair is
// asserted twice -- the 10 px FIELD LABEL above (which is a translated word) and
// the 12 px VALUE below (which is data, and only the "unknown" words move with
// the language). That is a stronger language-switch check than the old
// one-line readout: four labels and three values have to come from the table.
const char* kEnCryptoLabel = "ENCRYPTION";
const char* kZhCryptoLabel =
    "\xE5\x8A\xA0\xE5\xAF\x86\xE7\x8A\xB6\xE6\x80\x81";  // 加密状态
const char* kEnMethodLabel = "METHOD";
const char* kZhMethodLabel =
    "\xE5\x8E\x8B\xE7\xBC\xA9\xE6\x96\xB9\xE6\xB3\x95";  // 压缩方法
const char* kEnDictLabel = "DICTIONARY";
const char* kZhDictLabel = "\xE5\xAD\x97\xE5\x85\xB8";  // 字典
const char* kEnRatioLabel = "RATIO";
const char* kZhRatioLabel = "\xE5\x8E\x8B\xE7\xBC\xA9\xE7\x8E\x87";  // 压缩率
const char* kEnUnknown = "UNKNOWN";
const char* kZhUnknown = "\xE6\x9C\xAA\xE7\x9F\xA5";  // 未知
// The fixture is 100 B packed to 60 B: a REAL ratio, so a zero placeholder
// could not pass this line -- and a number is NOT translated.
const char* kEnRatio = "60.0%";
const char* kZhRatio = "60.0%";

std::string SectionText(const prism::win::sidebar::Layout& layout,
                        prism::sidebar::Section section) {
  for (const prism::win::sidebar::Command& command : layout.commands) {
    if (command.kind == prism::win::sidebar::PrimKind::SectionLabel &&
        command.section == section) {
      return command.text;
    }
  }
  return std::string();
}

// The card's VALUE line of pair <index> (C105: 0 crypto, 1 method, 2 dictionary,
// 3 ratio -- the order the card paints them in).
std::string StatusLine(const prism::win::sidebar::Layout& layout, int index) {
  for (const prism::win::sidebar::Command& command : layout.commands) {
    if (command.kind == prism::win::sidebar::PrimKind::StatusText &&
        command.payload == index) {
      return command.text;
    }
  }
  return std::string();
}

// ... and its LABEL line, which is the translated field word.
std::string StatusLabel(const prism::win::sidebar::Layout& layout, int index) {
  for (const prism::win::sidebar::Command& command : layout.commands) {
    if (command.kind == prism::win::sidebar::PrimKind::StatusLabel &&
        command.payload == index) {
      return command.text;
    }
  }
  return std::string();
}

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
  }
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
};

int ColorDistance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

// Renders the window's OWN scene (its layout, its sidebar commands, its action
// model) onto a headless WIC target -- the same renderer the screen uses.
bool RenderWindow(prism::win::gallery::GalleryWindow& window, IWICImagingFactory* wic,
                  Frame* frame) {
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  constexpr UINT32 kWidth = 1280;
  constexpr UINT32 kHeight = 800;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;

  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.layout = &window.LayoutForTest();
  scene.sidebar = &window.SidebarLayout();
  scene.sidebarCollapsed = false;
  scene.actions = &window.ActionBarState();
  scene.actionHover = -1;
  scene.actionPressed = -1;
  scene.breadcrumb = L"c74.7z";

  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(kWidth), static_cast<INT>(kHeight)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  frame->width = kWidth;
  frame->height = kHeight;
  frame->pixels.resize(static_cast<std::size_t>(kWidth) * kHeight);
  for (UINT32 y = 0; y < kHeight; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<std::size_t>(y) * kWidth,
                data + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(kWidth) * 4);
  }
  return true;
}

}  // namespace

PRISM_TEST("win: switching the language rebuilds all eight band labels") {
  if (!RequireStation()) return;
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"C74 language switch", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items;
  items.push_back(prism::ArchiveItem{1, "note.txt", 100, 60, 0, 0, false, false});
  window.SetItems(&items);
  window.SetRootLabel(L"c74.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));
  // C128 (proposal 5.6): the card is folded by default (one line). This case is
  // about the FOUR translated labels, so it asks for the expanded card -- the
  // fold's own translation is the collapsed line (未加密 · 方法 · 比率).
  window.SetInfoCardExpanded(true);

  // ---- 1. English --------------------------------------------------------
  window.ApplyLanguage(&TranslatorFor(L"en-US"));
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    PRISM_CHECK_EQ(window.ActionBarLabelAt(i), std::string(kEnglish[i]));
  }

  // ---- 2. Chinese --------------------------------------------------------
  window.ApplyLanguage(&TranslatorFor(L"zh-CN"));
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    PRISM_CHECK_EQ(window.ActionBarLabelAt(i), std::string(kChinese[i]));
    PRISM_CHECK(window.ActionBarLabelAt(i) != std::string(kEnglish[i]));
  }

  // ---- 3. the sidebar heading and the info card ---------------------------
  const prism::win::sidebar::Layout& sidebar = window.SidebarLayout();
  PRISM_CHECK_EQ(SectionText(sidebar, prism::sidebar::Section::Tree),
                 std::string(kZhTree));
  // C105: four labels + four values, in paint order.
  PRISM_CHECK_EQ(StatusLabel(sidebar, 0), std::string(kZhCryptoLabel));
  PRISM_CHECK_EQ(StatusLabel(sidebar, 1), std::string(kZhMethodLabel));
  PRISM_CHECK_EQ(StatusLabel(sidebar, 2), std::string(kZhDictLabel));
  PRISM_CHECK_EQ(StatusLabel(sidebar, 3), std::string(kZhRatioLabel));
  PRISM_CHECK_EQ(StatusLine(sidebar, 0), std::string(kZhNotEncrypted));
  PRISM_CHECK_EQ(StatusLine(sidebar, 1), std::string(kZhUnknown));
  PRISM_CHECK_EQ(StatusLine(sidebar, 2), std::string("--"));  // no dictionary: never a fabricated size
  PRISM_CHECK_EQ(StatusLine(sidebar, 3), std::string(kZhRatio));

  window.ApplyLanguage(&TranslatorFor(L"en-US"));
  const prism::win::sidebar::Layout& englishSidebar = window.SidebarLayout();
  PRISM_CHECK_EQ(SectionText(englishSidebar, prism::sidebar::Section::Tree),
                 std::string(kEnTree));
  PRISM_CHECK_EQ(StatusLabel(englishSidebar, 0), std::string(kEnCryptoLabel));
  PRISM_CHECK_EQ(StatusLabel(englishSidebar, 1), std::string(kEnMethodLabel));
  PRISM_CHECK_EQ(StatusLabel(englishSidebar, 2), std::string(kEnDictLabel));
  PRISM_CHECK_EQ(StatusLabel(englishSidebar, 3), std::string(kEnRatioLabel));
  PRISM_CHECK_EQ(StatusLine(englishSidebar, 0), std::string(kEnNotEncrypted));
  PRISM_CHECK_EQ(StatusLine(englishSidebar, 1), std::string(kEnUnknown));
  PRISM_CHECK_EQ(StatusLine(englishSidebar, 2), std::string("--"));
  PRISM_CHECK_EQ(StatusLine(englishSidebar, 3), std::string(kEnRatio));

  // ---- 5. and back again -------------------------------------------------
  window.ApplyLanguage(&TranslatorFor(L"zh-CN"));
  PRISM_CHECK_EQ(window.ActionBarLabelAt(0), std::string(kChinese[0]));
  ::DestroyWindow(window.Hwnd());
}

PRISM_TEST("win: the band's label PIXELS change with the language") {
  if (!RequireStation()) return;
  ComPtr<IWICImagingFactory> wic;
  if (!PRISM_CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                              CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                              reinterpret_cast<void**>(
                                                  wic.ReleaseAndGetAddressOf()))))) {
    return;
  }
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"C74 label pixels", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items;
  items.push_back(prism::ArchiveItem{1, "note.txt", 100, 60, 0, 0, false, false});
  window.SetItems(&items);
  window.SetRootLabel(L"c74.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));

  window.ApplyLanguage(&TranslatorFor(L"en-US"));
  Frame english;
  if (!PRISM_CHECK(RenderWindow(window, wic.Get(), &english))) return;

  window.ApplyLanguage(&TranslatorFor(L"zh-CN"));
  Frame chinese;
  if (!PRISM_CHECK(RenderWindow(window, wic.Get(), &chinese))) return;

  const prism::actionbar::Layout band = window.ActionBarLayout();
  const int index = prism::actionbar::IndexOf(prism::actionbar::ActionId::ExtractTo);
  PRISM_CHECK(index >= 0 && index < static_cast<int>(band.buttons.size()));
  const prism::actionbar::Rect labelBox =
      prism::actionbar::LabelBoxRect(band.buttons[static_cast<std::size_t>(index)].rect);

  int changed = 0;
  int inkEnglish = 0;
  int inkChinese = 0;
  const UINT32 backdrop = english.At(4, 4);
  for (int y = labelBox.top; y < labelBox.bottom; ++y) {
    for (int x = labelBox.left; x < labelBox.right; ++x) {
      if (!english.In(x, y) || !chinese.In(x, y)) continue;
      if (ColorDistance(english.At(x, y), chinese.At(x, y)) > 24) ++changed;
      if (ColorDistance(english.At(x, y), backdrop) > 40) ++inkEnglish;
      if (ColorDistance(chinese.At(x, y), backdrop) > 40) ++inkChinese;
    }
  }
  // Both words really painted (a blank label would make "they differ" trivial).
  PRISM_CHECK(inkEnglish > 20);
  PRISM_CHECK(inkChinese > 20);
  // ... and they are not the same ink at the same pixels.
  PRISM_CHECK(changed > 40);

  // The ICONS did not move or change: only the words did.
  const prism::actionbar::Rect iconBox =
      prism::actionbar::IconBoxRect(band.buttons[static_cast<std::size_t>(index)].rect);
  int iconChanged = 0;
  for (int y = iconBox.top; y < iconBox.bottom; ++y) {
    for (int x = iconBox.left; x < iconBox.right; ++x) {
      if (!english.In(x, y) || !chinese.In(x, y)) continue;
      if (ColorDistance(english.At(x, y), chinese.At(x, y)) > 24) ++iconChanged;
    }
  }
  PRISM_CHECK_EQ(iconChanged, 0);

  ::DestroyWindow(window.Hwnd());
}

int main(int argc, char** argv) {
#if defined(PRISM_C74_MUTANT_NO_LABEL_REBUILD)
  prism::test::SetMutationVerdict(
      "C74 MUTATION CAUGHT: no_label_rebuild",
      [] { return !prism::win::gallery::kActionBarLabelsRebuilt; });
#endif
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int failures = prism::test::RunAll(argc, argv);
  if (SUCCEEDED(com)) CoUninitialize();
  if (g_skip) {
    std::printf("SKIP: no interactive window station\n");
    return 77;
  }
  if (failures != 0) return 1;
  std::puts("ALL C74 I18N-SWITCH CHECKS PASSED");
  return 0;
}
