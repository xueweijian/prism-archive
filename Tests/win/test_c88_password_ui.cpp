// PrismArchive -- the password gate's LANGUAGE and PIXELS (card C88).
//
// The C35 suite proved the wiring: control ids, masking, budget, probe seam.
// This suite proves the two things C84's visibility fix exposed:
//
//   1. LANGUAGE. With the shipped zh-CN table installed, every control name the
//      accessibility tree publishes is Chinese -- no English literal survives.
//      With no table (the C35 shape) every visible string still equals the en-US
//      row, so the fallback cannot drift away from the shipped table. The
//      explicit assertion is "no ASCII word of 3+ letters anywhere in the UIA
//      names", plus an exact match against table values for the fixed labels.
//
//   2. PIXELS. The client is painted from ThemeTokens: the card interior equals
//      CompositeOver(EffectiveGlassSurface, Canvas) within 2/255 per channel, the
//      primary button equals CompositeOver(EffectiveAccent, Card), body text
//      clears WCAG 4.5:1 against the card, and the password field's focus ring
//      is the accent colour while the field owns the keyboard and is not when it
//      does not.
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27, no WILL_FAIL): the same source is
// built twice more --
//   PRISM_C88_MUTANT_SYSTEM_GREY       paints COLOR_BTNFACE instead of the card
//                                      tokens -> the pixel assertions go red;
//   PRISM_C88_MUTANT_HARDCODED_ENGLISH puts "This archive needs a password."
//                                      back -> the zh-CN language assertions go
//                                      red.
// main() arms "C88 MUTATION CAUGHT: <name>" against MutationFired(); the marker
// prints at the FIRST failure and exits 0, so a mutation that stopped applying
// is the only way to get "no marker" (a red test), never a false green.
//
// D1: the language directory is an ENVIRONMENT input configured by the suite.
// A missing directory is a FAIL with its path printed, not a silent skip: this
// suite has no legitimate skip (it creates one plain window).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <commctrl.h>
#include <uiautomation.h>
#include <UIAutomationClient.h>
#endif

#include "../mini_test.hpp"
#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/PasswordDialog.hpp"

namespace {

using prism::i18n::Catalog;
using prism::i18n::Translator;
namespace th = prism::theme;
namespace ui = prism::win::pwd_ui;

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Language tables, read from the directory the suite configures.
// ---------------------------------------------------------------------------
std::wstring LangDir() {
  wchar_t buffer[1024] = {};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_C88_LANG_DIR", buffer, 1024);
  if (length == 0 || length >= 1024) return std::wstring();
  return std::wstring(buffer, length);
}

Translator& TranslatorFor(bool chinese) {
  static Translator en;
  static Translator zh;
  static bool loaded = false;
  static bool ok = false;
  if (!loaded) {
    const std::wstring dir = LangDir();
    if (!dir.empty()) {
      const std::string base = ui::Utf8FromWide(dir);
      std::string why;
      ok = en.LoadCatalogFile(base + "/en-US.json", &why);
      ok = zh.LoadCatalogFile(base + "/en-US.json", &why) && ok;
      ok = zh.LoadCatalogFile(base + "/zh-CN.json", &why) && ok;
      en.SetLanguage("en-US");
      zh.SetLanguage("zh-CN");
    }
    loaded = true;
  }
  if (!ok) {
    std::printf("    [lang] tables could not be loaded from %ls\n", LangDir().c_str());
  }
  return chinese ? zh : en;
}

std::wstring TableValue(const Translator& translator, const char* key) {
  return ui::WideFromUtf8(translator.Translate(key));
}

// The "no English literal" predicate: a run of 3+ ASCII letters. Single letters
// are allowed (a unit or a placeholder remnant); any English word is not.
bool HasEnglishWord(const std::wstring& text) {
  int run = 0;
  for (wchar_t c : text) {
    const bool letter = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    run = letter ? run + 1 : 0;
    if (run >= 3) return true;
  }
  return false;
}

bool Contains(const std::vector<std::wstring>& haystack, const std::wstring& needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

void ForcePaint(HWND hwnd) {
  if (hwnd == nullptr) return;
  InvalidateRect(hwnd, nullptr, FALSE);
  UpdateWindow(hwnd);
  PumpMessages();
}

// ---------------------------------------------------------------------------
// Pixels. The dialog is shown, so BitBlt from its own DC is what the screen
// shows (the C69 settings suite proved this reads a D2D-painted window).
// ---------------------------------------------------------------------------
COLORREF CapturePixel(HWND hwnd, int x, int y) {
  HDC win = GetDC(hwnd);
  if (win == nullptr) return CLR_INVALID;
  HDC mem = CreateCompatibleDC(win);
  if (mem == nullptr) {
    ReleaseDC(hwnd, win);
    return CLR_INVALID;
  }
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HBITMAP bmp = CreateCompatibleBitmap(win, rc.right - rc.left, rc.bottom - rc.top);
  HGDIOBJ old = SelectObject(mem, bmp);
  const BOOL blitted =
      BitBlt(mem, 0, 0, rc.right - rc.left, rc.bottom - rc.top, win, 0, 0, SRCCOPY);
  const COLORREF color = blitted != FALSE ? GetPixel(mem, x, y) : CLR_INVALID;
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(hwnd, win);
  return color;
}

int ChannelDelta(COLORREF a, COLORREF b) {
  return std::abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b))) +
         std::abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b))) +
         std::abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
}

bool NearColor(COLORREF measured, COLORREF expected, int perChannel, const char* what) {
  const int dr = std::abs(static_cast<int>(GetRValue(measured)) - static_cast<int>(GetRValue(expected)));
  const int dg = std::abs(static_cast<int>(GetGValue(measured)) - static_cast<int>(GetGValue(expected)));
  const int db = std::abs(static_cast<int>(GetBValue(measured)) - static_cast<int>(GetBValue(expected)));
  const bool ok = dr <= perChannel && dg <= perChannel && db <= perChannel;
  if (!ok) {
    std::printf("    [pixel] %s measured=(%u,%u,%u) expected=(%u,%u,%u)\n", what,
                static_cast<unsigned>(GetRValue(measured)),
                static_cast<unsigned>(GetGValue(measured)),
                static_cast<unsigned>(GetBValue(measured)),
                static_cast<unsigned>(GetRValue(expected)),
                static_cast<unsigned>(GetGValue(expected)),
                static_cast<unsigned>(GetBValue(expected)));
  }
  return ok;
}

// ---------------------------------------------------------------------------
// UIA: the accessibility tree the screen reader walks.
// ---------------------------------------------------------------------------
bool EnsureCom() {
  static const bool ok = [] {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    return SUCCEEDED(hr);
  }();
  return ok;
}

struct UiItem {
  std::wstring name;
  CONTROLTYPEID type = 0;
  bool offscreen = false;
  std::wstring value;         // ValuePattern, when the element has one
  std::wstring automationId;  // "TitleBar"/"Close" are the OS's, not ours
  bool password = false;
};

// The window's title bar and its Close button are drawn by the OS: their
// accessible names come from the SYSTEM language, not from a table this build
// ships ("关闭" on a Chinese Windows, "Close" on an English one). C88's "no
// English literal" assertion is about the strings the dialog authors, so the
// scan skips exactly those two system ids and nothing else.
bool IsSystemChrome(const UiItem& item) {
  return item.automationId == L"TitleBar" || item.automationId == L"Close";
}

IUIAutomation* Automation() {
  static IUIAutomation* automation = nullptr;
  if (automation == nullptr && EnsureCom()) {
    IUIAutomation* created = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&created)))) {
      automation = created;
    }
  }
  return automation;
}

std::vector<UiItem> CollectUi(HWND hwnd) {
  std::vector<UiItem> items;
  IUIAutomation* automation = Automation();
  if (automation == nullptr) return items;
  IUIAutomationElement* root = nullptr;
  const HRESULT fromHandle = automation->ElementFromHandle(hwnd, &root);
  if (FAILED(fromHandle) || root == nullptr) {
    std::printf("    [uia] ElementFromHandle hr=0x%08lx\n",
                static_cast<unsigned long>(fromHandle));
    return items;
  }
  IUIAutomationCondition* condition = nullptr;
  automation->CreateTrueCondition(&condition);
  IUIAutomationElementArray* array = nullptr;
  const HRESULT findAll = root->FindAll(TreeScope_Subtree, condition, &array);
  if (condition != nullptr) condition->Release();
  if (FAILED(findAll) || array == nullptr) {
    std::printf("    [uia] FindAll hr=0x%08lx\n", static_cast<unsigned long>(findAll));
  }
  if (SUCCEEDED(findAll) && array != nullptr) {
    int length = 0;
    array->get_Length(&length);
    std::printf("    [uia] elements=%d\n", length);
    for (int i = 0; i < length; ++i) {
      IUIAutomationElement* element = nullptr;
      if (FAILED(array->GetElement(i, &element)) || element == nullptr) continue;
      UiItem item;
      BSTR name = nullptr;
      if (SUCCEEDED(element->get_CurrentName(&name)) && name != nullptr) {
        item.name = name;
      }
      SysFreeString(name);
      BSTR automationId = nullptr;
      if (SUCCEEDED(element->get_CurrentAutomationId(&automationId)) && automationId != nullptr) {
        item.automationId = automationId;
      }
      SysFreeString(automationId);
      element->get_CurrentControlType(&item.type);
      BOOL offscreen = FALSE;
      element->get_CurrentIsOffscreen(&offscreen);
      item.offscreen = offscreen != FALSE;
      BOOL password = FALSE;
      if (SUCCEEDED(element->get_CurrentIsPassword(&password))) item.password = password != FALSE;
      IUIAutomationValuePattern* value = nullptr;
      if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId,
                                              reinterpret_cast<IUnknown**>(&value))) &&
          value != nullptr) {
        BSTR text = nullptr;
        if (SUCCEEDED(value->get_CurrentValue(&text)) && text != nullptr) item.value = text;
        SysFreeString(text);
        value->Release();
      }
      items.push_back(std::move(item));
      element->Release();
    }
    array->Release();
  }
  root->Release();
  return items;
}

IUIAutomationElement* FindUi(HWND hwnd, CONTROLTYPEID type, const std::wstring& name) {
  IUIAutomation* automation = Automation();
  if (automation == nullptr) return nullptr;
  IUIAutomationElement* root = nullptr;
  if (FAILED(automation->ElementFromHandle(hwnd, &root)) || root == nullptr) return nullptr;
  IUIAutomationCondition* condition = nullptr;
  automation->CreateTrueCondition(&condition);
  IUIAutomationElementArray* array = nullptr;
  IUIAutomationElement* found = nullptr;
  if (SUCCEEDED(root->FindAll(TreeScope_Subtree, condition, &array)) && array != nullptr) {
    int length = 0;
    array->get_Length(&length);
    for (int i = 0; i < length && found == nullptr; ++i) {
      IUIAutomationElement* element = nullptr;
      if (FAILED(array->GetElement(i, &element)) || element == nullptr) continue;
      CONTROLTYPEID elementType = 0;
      element->get_CurrentControlType(&elementType);
      BSTR elementName = nullptr;
      element->get_CurrentName(&elementName);
      const bool match = elementType == type && elementName != nullptr &&
                         name == std::wstring(elementName);
      SysFreeString(elementName);
      if (match) {
        found = element;  // reference transferred to the caller
      } else {
        element->Release();
      }
    }
    array->Release();
  }
  root->Release();
  return found;
}

IUIAutomationElement* FindFirstByType(HWND hwnd, CONTROLTYPEID type) {
  IUIAutomation* automation = Automation();
  if (automation == nullptr) return nullptr;
  IUIAutomationElement* root = nullptr;
  if (FAILED(automation->ElementFromHandle(hwnd, &root)) || root == nullptr) return nullptr;
  IUIAutomationCondition* condition = nullptr;
  automation->CreateTrueCondition(&condition);
  IUIAutomationElementArray* array = nullptr;
  IUIAutomationElement* found = nullptr;
  if (SUCCEEDED(root->FindAll(TreeScope_Subtree, condition, &array)) && array != nullptr) {
    int length = 0;
    array->get_Length(&length);
    for (int i = 0; i < length && found == nullptr; ++i) {
      IUIAutomationElement* element = nullptr;
      if (FAILED(array->GetElement(i, &element)) || element == nullptr) continue;
      CONTROLTYPEID elementType = 0;
      element->get_CurrentControlType(&elementType);
      if (elementType == type) {
        found = element;
      } else {
        element->Release();
      }
    }
    array->Release();
  }
  root->Release();
  return found;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
prism::pwd::PasswordUiConfig Config(std::vector<std::string> candidates = {},
                                   std::vector<std::string> vault = {}) {
  prism::pwd::PasswordUiConfig config;
  config.archiveName = "enc.7z";
  for (const std::string& text : candidates) {
    prism::pwd::Candidate candidate;
    candidate.text = text;
    candidate.source = prism::pwd::Source::FilenameHint;
    config.candidates.push_back(std::move(candidate));
  }
  config.vaultEntries = std::move(vault);
  config.manualBudget = 3;
  config.canRemember = true;
  return config;
}

bool OpenDialog(ui::PasswordDialog& dialog, const prism::i18n::Translator* translator,
                const std::wstring& title, prism::pwd::PasswordUiConfig config) {
  if (translator != nullptr) dialog.SetTranslator(translator);
  if (!dialog.Create(GetModuleHandleW(nullptr), title)) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    return false;
  }
  dialog.SetConfig(std::move(config));
  dialog.Start();
  ForcePaint(dialog.Hwnd());
  return true;
}

void TypeInto(ui::PasswordDialog& dialog, const std::wstring& text) {
  const HWND edit = dialog.Control(ui::ids::kPassword);
  SetWindowTextW(edit, text.c_str());
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kPassword, EN_CHANGE),
                  reinterpret_cast<LPARAM>(edit));
}

void Click(ui::PasswordDialog& dialog, int id) {
  SendMessageW(dialog.Control(id), BM_CLICK, 0, 0);
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

// ===========================================================================
// 1. zh-CN: the whole dialog is Chinese, and the UIA tree agrees
// ===========================================================================
PRISM_TEST("c88_zh_dialog_publishes_only_chinese_text") {
  Translator& zh = TranslatorFor(true);
  if (!PRISM_CHECK(zh.HasCatalog("zh-CN"))) return;
  if (!PRISM_CHECK(zh.KeyCount("zh-CN") >= 129)) return;

  ui::PasswordDialog dialog;
  if (!PRISM_CHECK(OpenDialog(dialog, &zh, TableValue(zh, "password.title"),
                              Config({"Cand01#aa", "Cand02#bb"}, {"Qz8#Lm2Vt99"})))) {
    return;
  }

  const std::vector<UiItem> items = CollectUi(dialog.Hwnd());
  PRISM_CHECK(items.size() >= 12);  // the tree was read, not empty

  std::vector<std::wstring> names;
  int edits = 0;
  int buttons = 0;
  int checks = 0;
  int radios = 0;
  for (const UiItem& item : items) {
    if (item.name.empty() || item.offscreen) continue;
    names.push_back(item.name);
    if (item.type == UIA_EditControlTypeId) ++edits;
    if (item.type == UIA_ButtonControlTypeId) ++buttons;
    if (item.type == UIA_CheckBoxControlTypeId) ++checks;
    if (item.type == UIA_RadioButtonControlTypeId) ++radios;
  }

  // The controls C80's tree promised are still there.
  PRISM_CHECK_EQ(edits, 1);
  PRISM_CHECK(buttons >= 3);
  PRISM_CHECK_EQ(checks, 1);
  PRISM_CHECK_EQ(radios, 2);

  // Exact table values, one per fixed label / button.
  const char* keys[] = {"password.prompt",   "password.label",   "password.show",
                        "password.current",  "password.remember", "password.once",
                        "password.ring",     "password.vault",   "password.useSelected",
                        "password.open",     "password.cancel",  "password.none"};
  for (const char* key : keys) {
    const std::wstring expected = TableValue(zh, key);
    if (!PRISM_CHECK(Contains(names, expected))) {
      std::printf("    [i18n] no UIA name equals %s = \"%s\"\n", key,
                  ui::Utf8FromWide(expected).c_str());
    }
  }

  // The load-bearing assertion: not one English word anywhere the DIALOG writes
  // (the OS title-bar strings are excluded, see IsSystemChrome).
  int scanned = 0;
  for (const UiItem& item : items) {
    if (item.name.empty() || item.offscreen || IsSystemChrome(item)) continue;
    ++scanned;
    if (HasEnglishWord(item.name)) {
      std::printf("    [i18n] English-looking UIA name: \"%s\" (id=%s)\n",
                  ui::Utf8FromWide(item.name).c_str(),
                  ui::Utf8FromWide(item.automationId).c_str());
      PRISM_CHECK(false);
    }
  }
  PRISM_CHECK(scanned >= 12);
  PRISM_CHECK(true);
}

// ===========================================================================
// 2. The no-table fallback is the en-US row (so the two copies cannot drift)
// ===========================================================================
PRISM_TEST("c88_english_fallback_equals_the_en_us_table") {
  Translator& en = TranslatorFor(false);
  if (!PRISM_CHECK(en.HasCatalog("en-US"))) return;

  ui::PasswordDialog dialog;
  if (!PRISM_CHECK(OpenDialog(dialog, nullptr, L"Password required", Config()))) return;

  const struct {
    int id;
    const char* key;
  } rows[] = {
      {ui::ids::kPrompt, "password.prompt"},
      {ui::ids::kCaptionPassword, "password.label"},
      {ui::ids::kShowPassword, "password.show"},
      {ui::ids::kCaptionCurrent, "password.current"},
      {ui::ids::kCurrent, "password.none"},
      {ui::ids::kRemember, "password.remember"},
      {ui::ids::kOnce, "password.once"},
      {ui::ids::kCaptionRing, "password.ring"},
      {ui::ids::kCaptionVault, "password.vault"},
      {ui::ids::kUseVault, "password.useSelected"},
      {ui::ids::kOk, "password.open"},
      {ui::ids::kCancel, "password.cancel"},
  };
  for (const auto& row : rows) {
    const std::wstring expected = TableValue(en, row.key);
    const std::wstring actual = dialog.ControlText(row.id);
    if (actual != expected) {
      std::printf("    [fallback] %s actual=\"%s\" table=\"%s\"\n", row.key,
                  ui::Utf8FromWide(actual).c_str(), ui::Utf8FromWide(expected).c_str());
    }
    PRISM_CHECK(actual == expected);
  }

  // Progress and error lines are composed from table patterns: compare against
  // the same FormatPattern the table would produce.
  dialog.SetConfig(Config({"Cand01#aa", "Cand02#bb"}));
  dialog.Start();
  const std::wstring progress = ui::WideFromUtf8(
      prism::i18n::FormatPattern(en.Translate("password.progress"),
                                 {{"done", "0"}, {"total", "2"}}));
  const std::wstring progressActual = dialog.ControlText(ui::ids::kProgressLabel);
  if (progressActual != progress) {
    std::printf("    [fallback] progress actual=\"%s\" table=\"%s\"\n",
                ui::Utf8FromWide(progressActual).c_str(),
                ui::Utf8FromWide(progress).c_str());
  }
  PRISM_CHECK(progressActual == progress);
}

// ===========================================================================
// 3. Pixels: card token, accent button, contrast, focus ring
// ===========================================================================
PRISM_TEST("c88_pixels_come_from_theme_tokens") {
  ui::PasswordDialog dialog;
  if (!PRISM_CHECK(OpenDialog(dialog, &TranslatorFor(true), L"Password required",
                              Config()))) {
    return;
  }
  ForcePaint(dialog.Hwnd());

  // Card interior: a point inside the card but outside every control (left
  // padding, below the lists).
  const th::Rgba card = ui::CardPixel();
  const th::Rgba canvas = ui::CanvasPixel();
  const th::Rgba glass = th::EffectiveGlassSurface(th::Variant::Midnight);
  const th::Rgba expectedCard = th::CompositeOver(glass, canvas);
  // Relative to the card, never a hardcoded screen coordinate: the sampling
  // point is 30 px inside the card's bottom-left corner (inside the padding,
  // away from every control) at any DPI.
  RECT cardRect{};
  GetClientRect(dialog.Hwnd(), &cardRect);
  const int cardX = cardRect.left + (ui::layout::kCardInset + 30);
  const int cardY = cardRect.bottom - (ui::layout::kCardInset + 30);
  const COLORREF cardPixel = CapturePixel(dialog.Hwnd(), cardX, cardY);
  PRISM_CHECK(NearColor(cardPixel, ui::ToColorRef(expectedCard), 2, "card background"));
  PRISM_CHECK(NearColor(cardPixel, ui::ToColorRef(card), 2, "card token"));
  (void)canvas;

  // The primary button is disabled while the field is empty (that is the C35
  // contract); type one character so the sample is the enabled accent fill.
  TypeInto(dialog, L"x");
  ForcePaint(dialog.Hwnd());

  // Primary button: its fill is the accent composited over the card.
  const th::Rgba expectedPrimary = th::CompositeOver(
      th::EffectiveAccent(th::Variant::Midnight), expectedCard);
  const RECT ok = ui::layout::Ok();
  const COLORREF primaryPixel =
      CapturePixel(dialog.Hwnd(), ok.left + 9, (ok.top + ok.bottom) / 2);
  PRISM_CHECK(NearColor(primaryPixel, ui::ToColorRef(expectedPrimary), 2,
                        "primary button fill"));

  // WCAG: body text on the card.
  const double contrast = th::ContrastRatio(th::Lookup(th::TokenId::TextPrimary,
                                                       th::Variant::Midnight).color,
                                            expectedCard);
  if (!(contrast >= 4.5)) {
    std::printf("    [contrast] %.2f:1\n", contrast);
  }
  PRISM_CHECK(contrast >= 4.5);

  // Focus ring: the edit's EN_SETFOCUS is what the card paints the ring from,
  // so the test drives the real notification instead of a foreground window.
  const HWND edit = dialog.Control(ui::ids::kPassword);
  const RECT frame = ui::layout::PasswordFrame();
  const int ringX = (frame.left + frame.right) / 2;
  const int ringY = frame.top;
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kPassword, EN_KILLFOCUS),
                  reinterpret_cast<LPARAM>(edit));
  ForcePaint(dialog.Hwnd());
  const COLORREF unfocused = CapturePixel(dialog.Hwnd(), ringX, ringY);

  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kPassword, EN_SETFOCUS),
                  reinterpret_cast<LPARAM>(edit));
  ForcePaint(dialog.Hwnd());
  const COLORREF focused = CapturePixel(dialog.Hwnd(), ringX, ringY);

  // The ring is the accent composited over the card (the accent token carries
  // alpha .82, so the pixel is the same composite the primary button uses).
  PRISM_CHECK(NearColor(focused, ui::ToColorRef(ui::AccentPixel()), 4, "focus ring"));
  if (ChannelDelta(focused, unfocused) <= 20) {
    std::printf("    [ring] focused and unfocused pixels are too close (%d)\n",
                ChannelDelta(focused, unfocused));
  }
  PRISM_CHECK(ChannelDelta(focused, unfocused) > 20);
}

// ===========================================================================
// 4. Behaviour does not regress: wrong attempt, memo radios, disabled button
// ===========================================================================
PRISM_TEST("c88_behaviour_is_unchanged") {
  Translator& zh = TranslatorFor(true);
  ui::PasswordDialog dialog;
  if (!PRISM_CHECK(OpenDialog(dialog, &zh, L"Password required",
                              Config({}, {"Qz8#Lm2Vt99"})))) {
    return;
  }

  // The vault picker is disabled until something is selected.
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kUseVault));
  PRISM_CHECK(dialog.ControlVisible(ui::ids::kCandidates));
  PRISM_CHECK(!dialog.ControlVisible(ui::ids::kProgress));

  // Memo semantics: two radios, one checked at a time, model follows the click.
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(ui::ids::kOnce), BM_GETCHECK,
                                               0, 0)),
                 static_cast<int>(BST_CHECKED));
  Click(dialog, ui::ids::kRemember);
  PRISM_CHECK(dialog.Model().rememberChoice() == prism::pwd::RememberChoice::Remember);
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(ui::ids::kRemember),
                                               BM_GETCHECK, 0, 0)),
                 static_cast<int>(BST_CHECKED));
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(ui::ids::kOnce),
                                               BM_GETCHECK, 0, 0)),
                 static_cast<int>(BST_UNCHECKED));
  Click(dialog, ui::ids::kOnce);
  PRISM_CHECK(dialog.Model().rememberChoice() == prism::pwd::RememberChoice::Once);

  // A wrong password shows the translated error line and the dialog stays up.
  bool submitted = false;
  dialog.SetSubmitHandler([&submitted](const std::string&) { submitted = true; });
  TypeInto(dialog, L"definitely-wrong");
  Click(dialog, ui::ids::kOk);
  PRISM_CHECK(submitted);
  dialog.FinishProbe(false, false);
  PRISM_CHECK(dialog.Model().state() == prism::pwd::UiState::Wrong);
  PRISM_CHECK(IsWindow(dialog.Hwnd()) != FALSE);
  PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
  const std::wstring error = dialog.ControlText(ui::ids::kError);
  const std::wstring expectedPrefix = ui::WideFromUtf8(prism::i18n::FormatPattern(
      zh.Translate("password.error.wrong"), {{"n", "1"}, {"budget", "3"}}));
  if (error != expectedPrefix + L" (" + TableValue(zh, "password.error.left") + L")") {
    std::printf("    [error] \"%s\"\n", ui::Utf8FromWide(error).c_str());
  }
  PRISM_CHECK(error.find(expectedPrefix) != std::wstring::npos);

  // Selecting a vault row enables the button; using it submits that secret.
  const HWND vault = dialog.Control(ui::ids::kVault);
  SendMessageW(vault, LB_SETCURSEL, 0, 0);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kVault, LBN_SELCHANGE),
                  reinterpret_cast<LPARAM>(vault));
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kUseVault));
  int vaultSubmissions = 0;
  dialog.SetSubmitHandler([&vaultSubmissions](const std::string&) { ++vaultSubmissions; });
  Click(dialog, ui::ids::kUseVault);
  PRISM_CHECK_EQ(vaultSubmissions, 1);
  PRISM_CHECK(dialog.Model().state() == prism::pwd::UiState::Probing);
  PRISM_CHECK(dialog.ControlTextUtf8(ui::ids::kPassword).empty());
}

// ===========================================================================
// 5. UIA keeps the textfield writable and the buttons invokable
// ===========================================================================
PRISM_TEST("c88_uia_textfield_and_buttons_still_work") {
  Translator& zh = TranslatorFor(true);
  ui::PasswordDialog dialog;
  if (!PRISM_CHECK(OpenDialog(dialog, &zh, TableValue(zh, "password.title"),
                              Config()))) {
    return;
  }

  const std::vector<UiItem> items = CollectUi(dialog.Hwnd());
  bool havePasswordField = false;
  for (const UiItem& item : items) {
    if (item.type == UIA_EditControlTypeId && !item.offscreen) {
      havePasswordField = true;
      PRISM_CHECK(item.password);  // a real password field, not a plain box
    }
  }
  PRISM_CHECK(havePasswordField);

  // The edit has no accessible name (the caption names it visually), so it is
  // located by control type.
  IUIAutomationElement* edit = FindFirstByType(dialog.Hwnd(), UIA_EditControlTypeId);
  if (!PRISM_CHECK(edit != nullptr)) return;
  IUIAutomationValuePattern* value = nullptr;
  if (PRISM_CHECK(SUCCEEDED(edit->GetCurrentPattern(
          UIA_ValuePatternId, reinterpret_cast<IUnknown**>(&value)))) &&
      value != nullptr) {
    BSTR typed = SysAllocString(L"typed-by-uia");
    const HRESULT set = value->SetValue(typed);
    SysFreeString(typed);
    PRISM_CHECK(SUCCEEDED(set));
    PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kPassword), std::string("typed-by-uia"));
    value->Release();
  }
  edit->Release();

  // The Cancel button is reachable and invoking it closes the gate.
  IUIAutomationElement* cancel =
      FindUi(dialog.Hwnd(), UIA_ButtonControlTypeId, TableValue(zh, "password.cancel"));
  if (!PRISM_CHECK(cancel != nullptr)) return;
  IUIAutomationInvokePattern* invoke = nullptr;
  if (PRISM_CHECK(SUCCEEDED(cancel->GetCurrentPattern(
          UIA_InvokePatternId, reinterpret_cast<IUnknown**>(&invoke)))) &&
      invoke != nullptr) {
    PRISM_CHECK(SUCCEEDED(invoke->Invoke()));
    invoke->Release();
  }
  cancel->Release();
  const ULONGLONG deadline = GetTickCount64() + 3000;  // D29: bounded
  while (GetTickCount64() < deadline && IsWindow(dialog.Hwnd()) != FALSE) {
    PumpMessages();
    Sleep(10);
  }
  PRISM_CHECK_EQ(IsWindow(dialog.Hwnd()), FALSE);
}

#endif  // _WIN32

int main(int argc, char** argv) {
#if defined(PRISM_C88_MUTANT_SYSTEM_GREY)
  prism::test::SetMutationVerdict("C88 MUTATION CAUGHT: system_grey",
                                  [] { return ui::MutationFired(); });
#elif defined(PRISM_C88_MUTANT_HARDCODED_ENGLISH)
  prism::test::SetMutationVerdict("C88 MUTATION CAUGHT: hardcoded_english",
                                  [] { return ui::MutationFired(); });
#endif
  const int failures = prism::test::RunAll(argc, argv);

#if defined(PRISM_C88_MUTANT_SYSTEM_GREY) || defined(PRISM_C88_MUTANT_HARDCODED_ENGLISH)
  // No failed check went through ReportFailure: stay red instead of printing a
  // late marker (CONTRACTS D27).
  std::printf("mutant build: no first-failure verdict (checks=%d failures=%d)\n",
              prism::test::CheckCount(), failures);
  return 1;
#else
  if (failures != 0) return 1;
  std::puts("ALL C88 PASSWORD-UI CHECKS PASSED");
  return 0;
#endif
}