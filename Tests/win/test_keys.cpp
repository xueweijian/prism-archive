// PrismArchive -- Windows integration test
// KEY ROUTING on the real window class + keymap agreement
// (blueprint ch. 9.1).
//
// Two things only a Windows lane can prove:
//   1. the VK constants the platform-neutral keymap names are the REAL Win32
//      values (a typo there would remap a key without any test on Linux
//      noticing, because the map is self-consistent);
//   2. every key the window already implements produces the effect the keymap
//      says it should. When the window is later patched to CALL the keymap,
//      this suite is the regression pin.
//
// The chords that are not wired yet (Ctrl+K / Ctrl+E / Ctrl+Shift+N /
// Ctrl+Shift+P / Alt+Left / Alt+Right) are asserted at the DECISION layer only:
// pressing an unwired chord here would either prove nothing or, once wired,
// start an extraction inside a test. The wiring patch is in the PR.
//
// The stdout marker is part of the verdict (CONTRACTS D1): a silent SKIP is a
// failure and ctest requires this line.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/Shortcuts.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using prism::ArchiveItem;
using prism::keys::Command;
using prism::keys::Scope;
using prism::keys::Stroke;
using prism::win::gallery::GalleryWindow;

namespace {

std::vector<ArchiveItem> BuildCatalog() {
  std::vector<ArchiveItem> items;
  auto add = [&items](const char* path, bool dir, uint64_t size) {
    ArchiveItem item;
    item.index = static_cast<uint32_t>(items.size());
    item.path = path;
    item.isDir = dir;
    item.size = size;
    items.push_back(item);
  };
  add("docs", true, 0);                 // 0
  add("pack", true, 0);                 // 1
  add("pack/img1.png", false, 120);     // 2
  add("pack/note.txt", false, 30);      // 3
  add("root.png", false, 500);          // 4
  return items;
}

bool SetCtrlDown(bool down) {
  BYTE state[256] = {};
  if (!GetKeyboardState(state)) return false;
  state[VK_CONTROL] = down ? 0x80 : 0x00;
  state[VK_SHIFT] = 0x00;
  state[VK_MENU] = 0x00;
  return SetKeyboardState(state) != FALSE;
}

}  // namespace

int main() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("SKIP: COM init failed");
    return 1;
  }
  std::vector<ArchiveItem> items = BuildCatalog();
  GalleryWindow window;
  if (!window.Create(L"PrismArchive keys test", GetModuleHandleW(nullptr))) {
    std::puts("SKIP: window creation failed (no interactive window station)");
    return 1;
  }
  if (!PRISM_CHECK(window.Hwnd() != nullptr)) return 1;
  window.SetItems(&items);
  window.SetRootLabel(L"keys.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));

  auto key = [&window](UINT vk) { window.Dispatch(WM_KEYDOWN, static_cast<WPARAM>(vk), 0); };
  auto rowOf = [&window](const char* path) {
    const auto& rows = window.Items();
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].path == path) return static_cast<int>(i);
    }
    return -1;
  };

  // ---------------------------------------------- 1. VK constants vs windows.h
  // The keymap is platform-neutral, so this is the ONLY place the numbers can
  // be checked against the SDK. A drift here silently remaps a shortcut.
  if (!PRISM_CHECK_EQ(prism::keys::kKeyBack, static_cast<uint32_t>(VK_BACK))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyReturn, static_cast<uint32_t>(VK_RETURN))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyEscape, static_cast<uint32_t>(VK_ESCAPE))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeySpace, static_cast<uint32_t>(VK_SPACE))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyEnd, static_cast<uint32_t>(VK_END))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyHome, static_cast<uint32_t>(VK_HOME))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyLeft, static_cast<uint32_t>(VK_LEFT))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyUp, static_cast<uint32_t>(VK_UP))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyRight, static_cast<uint32_t>(VK_RIGHT))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyDown, static_cast<uint32_t>(VK_DOWN))) return 1;
  // Letter/digit chords: WM_KEYDOWN delivers the uppercase ASCII code, and the
  // SDK has no VK_K/VK_1 names for them -- so let the SDK itself confirm that
  // 0x4B is the K key and 0x31 is the 1 key (MapVirtualKeyW is the authority
  // here, not my memory of the table).
  auto vkChar = [](uint32_t vk) {
    return static_cast<uint32_t>(LOWORD(MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR)));
  };
  if (!PRISM_CHECK_EQ(vkChar('K'), static_cast<uint32_t>('K'))) return 1;
  if (!PRISM_CHECK_EQ(vkChar('E'), static_cast<uint32_t>('E'))) return 1;
  if (!PRISM_CHECK_EQ(vkChar('N'), static_cast<uint32_t>('N'))) return 1;
  if (!PRISM_CHECK_EQ(vkChar('P'), static_cast<uint32_t>('P'))) return 1;
  if (!PRISM_CHECK_EQ(vkChar('1'), static_cast<uint32_t>('1'))) return 1;
  if (!PRISM_CHECK_EQ(vkChar('2'), static_cast<uint32_t>('2'))) return 1;

  // ------------------------------- 2. Ctrl+1 / Ctrl+2 through the real WndProc
  if (!PRISM_CHECK(SetCtrlDown(true))) return 1;
  if (!PRISM_CHECK((GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
    std::puts("FAIL: SetKeyboardState cannot hold Ctrl on this station");
    return 1;
  }
  key('2');
  if (!PRISM_CHECK(window.ListMode())) return 1;
  key('1');
  if (!PRISM_CHECK(!window.ListMode())) return 1;
  if (!PRISM_CHECK(SetCtrlDown(false))) return 1;
  // The bare key is NOT the chord (no sticky mode switching by accident).
  key('2');
  if (!PRISM_CHECK(!window.ListMode())) return 1;

  // --------------------------- 3. each implemented key does what the table says
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_HOME, false, false, false}),
                      Command::SelectFirst)) return 1;
  key(VK_HOME);
  if (!PRISM_CHECK_EQ(window.Selection(), 0)) return 1;

  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_END, false, false, false}),
                      Command::SelectLast)) return 1;
  key(VK_END);
  if (!PRISM_CHECK_EQ(window.Selection(), window.ItemCount() - 1)) return 1;

  // Grid mode: RIGHT/LEFT step one column, and the clamp is the window's rule.
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_LEFT, false, false, false}),
                      Command::SelectLeft)) return 1;
  key(VK_LEFT);
  if (!PRISM_CHECK_EQ(window.Selection(), window.ItemCount() - 2)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_RIGHT, false, false, false}),
                      Command::SelectRight)) return 1;
  key(VK_RIGHT);
  if (!PRISM_CHECK_EQ(window.Selection(), window.ItemCount() - 1)) return 1;

  // Enter on a folder == ActivateSelection.
  window.SetSelection(rowOf("pack"));
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_RETURN, false, false, false}),
                      Command::ActivateSelection)) return 1;
  key(VK_RETURN);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack"))) return 1;

  // Backspace == GoUp.
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_BACK, false, false, false}),
                      Command::GoUp)) return 1;
  key(VK_BACK);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;

  // Space == QuickLookToggle; Esc == QuickLookClose.
  window.SetSelection(rowOf("root.png"));
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_SPACE, false, false, false}),
                      Command::QuickLookToggle)) return 1;
  key(VK_SPACE);
  if (!PRISM_CHECK(window.QuickLookOpen())) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_ESCAPE, false, false, false}, Scope::Gallery),
                      Command::QuickLookClose)) return 1;
  key(VK_ESCAPE);
  if (!PRISM_CHECK(!window.QuickLookOpen())) return 1;

  // ------------------------------------------- 4. the 9.1 rows that are new
  // Decided here, executed by the window once the (five-line) wiring patch in
  // the PR lands. Asserting the current no-op behaviour would lock the feature
  // OUT, so only the decision is pinned -- plus the fact that the chords do not
  // collide with the keys the window already handles.
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'K', true, false, false}),
                      Command::OpenPalette)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'E', true, false, false}),
                      Command::SmartExtract)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'N', true, true, false}),
                      Command::NewFolder)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'P', true, true, false}),
                      Command::ZoomNext)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_LEFT, false, false, true}),
                      Command::HistoryBack)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_RIGHT, false, false, true}),
                      Command::HistoryForward)) return 1;
  // None of them is a bare-key command: pressing them without the modifier must
  // not extract, create, zoom or navigate.
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'K', false, false, false}),
                      Command::None)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'E', false, false, false}),
                      Command::None)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'N', false, false, false}),
                      Command::None)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'P', false, false, false}),
                      Command::None)) return 1;
  // Alt+double-click is the pointer half of the Smart Extract row.
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(prism::keys::PointerStroke{2, true}),
                      prism::keys::PointerAction::SmartExtract)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(prism::keys::PointerStroke{2, false}),
                      prism::keys::PointerAction::Activate)) return 1;

  // ------------------------------- 5. the palette scope owns its own keyboard
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_ESCAPE, false, false, false}, Scope::Palette),
                      Command::ClosePalette)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{'E', false, false, false}, Scope::Palette),
                      Command::None)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_SPACE, false, false, false}, Scope::Palette),
                      Command::None)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(Stroke{VK_DOWN, false, false, false}, Scope::Palette),
                      Command::SelectDown)) return 1;

  window.Dispatch(WM_DESTROY, 0, 0);
  CoUninitialize();
  std::puts("ALL KEYS CHECKS PASSED");
  return prism::test::FailureCount() == 0 ? 0 : 1;
}
