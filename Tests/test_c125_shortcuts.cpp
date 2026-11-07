// PrismArchive -- PrismCore test
// C125: THE TWO CHORDS THAT MUST NOT ACT UNASKED.
//
// Bandizip 6.29, read off its live menus:
//     Options(O) -> Settings(&S)  F5
//     File(F)    -> Extract(&E)   Ctrl+E
// Both were wrong here, in opposite directions: F5 was UNBOUND (so the shortcut
// a migrated user reaches for did nothing), and Ctrl+E was bound to SmartExtract
// (so the same finger wrote the whole archive to disk without asking).
//
// This file pins the DECISION layer only (prism::keys::Resolve and
// prism::chrome::ShellVariantFor). The window and the registry are covered by
// win.test_c125_signals; the app wiring is covered by the c106 chord test,
// which now asserts that Ctrl+E reaches the dialog seam and not the extractor.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). No WILL_FAIL: each arm is this
// SAME source with one rule injected, arms mini_test verdict printing, and its
// CTest entry requires "C125 MUTATION CAUGHT: <arm>". The arms:
//   f5_extract            F5 -> SmartExtract (the accident this card prevents)
//   ctrl_e_smart          Ctrl+E -> SmartExtract (the shipped defect)
//   system_always_midnight "system" ignores the OS answer (the shipped defect)
#include <cstdint>
#include <cstdio>

#include "../Src/PrismCore/ChromeTheme.hpp"
#include "../Src/PrismCore/Shortcuts.hpp"
#include "mini_test.hpp"

namespace {

using prism::chrome::SystemTheme;
using prism::keys::Command;
using prism::keys::Scope;
using prism::keys::Stroke;
using prism::theme::Variant;

Command Key(std::uint32_t key, bool ctrl = false, bool shift = false, bool alt = false,
            Scope scope = Scope::Gallery) {
  return prism::keys::Resolve(Stroke{key, ctrl, shift, alt}, scope);
}

}  // namespace

// ---------------------------------------------------------------------------
// S1. F5 IS SETTINGS. The card exists because "F5 = extract" is the obvious
//     wrong guess, and a migrated user pressing F5 to change a setting would
//     have been handed a disk write instead.
// ---------------------------------------------------------------------------
PRISM_TEST("c125 S1: F5 opens settings and never starts an extraction") {
  // Win32 VK_F5; WM_KEYDOWN passes wParam straight through, so this is the key.
  if (!PRISM_CHECK_EQ(prism::keys::kKeyF5, 0x74u)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyF5), Command::OpenSettings)) return;
  // The assertion that matters is the NEGATIVE one: the same stroke must not
  // resolve to the silent extractor (the mutation arm binds exactly this).
  if (!PRISM_CHECK(Key(prism::keys::kKeyF5) != Command::SmartExtract)) return;
  // A chord is not a bare key: modified F5 stays inert.
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyF5, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyF5, false, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyF5, false, false, true), Command::None)) return;
  // F5 inside the palette is the text field's (chords are suspended there).
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyF5, false, false, false, Scope::Palette),
                      Command::None)) return;
}

// ---------------------------------------------------------------------------
// S2. Ctrl+E ASKS. The dialog is the consequence boundary: the chord may open a
//     surface that writes nothing, and the SILENT writer must live on another
//     chord (Ctrl+Alt+E), not behind the same finger.
// ---------------------------------------------------------------------------
PRISM_TEST("c125 S2: Ctrl+E raises the extract dialog, Ctrl+Alt+E is the silent write") {
  if (!PRISM_CHECK_EQ(Key('E', true), Command::OpenExtractDialog)) return;
  if (!PRISM_CHECK(Key('E', true) != Command::SmartExtract)) return;
  // The silent path still exists -- it just moved one modifier away.
  if (!PRISM_CHECK_EQ(Key('E', true, false, true), Command::SmartExtract)) return;
  // Near misses are inert, so a slip cannot become an unasked write.
  if (!PRISM_CHECK_EQ(Key('E', true, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('E', false, false, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('E'), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('E', true, false, false, Scope::Palette), Command::None)) return;
  // The other Ctrl chords did not move.
  if (!PRISM_CHECK_EQ(Key('K', true), Command::OpenPalette)) return;
  if (!PRISM_CHECK_EQ(Key('O', true), Command::OpenArchive)) return;
  if (!PRISM_CHECK_EQ(Key('F', true), Command::OpenSearch)) return;
  if (!PRISM_CHECK_EQ(Key('N', true, true), Command::NewFolder)) return;
}

// ---------------------------------------------------------------------------
// S3. "system" FOLLOWS the OS. The registry DWORD (AppsUseLightTheme) is read in
//     PrismWin (PrismCore may not include <windows.h>); this layer maps the
//     three possible answers. 1 -> Daylight, 0 -> Midnight, unreadable -> keep
//     the previous shell rather than guess.
// ---------------------------------------------------------------------------
PRISM_TEST("c125 S3: the system appearance decides what 跟随系统 means") {
  if (!PRISM_CHECK_EQ(prism::chrome::ShellVariantFor("system", false, SystemTheme::Light),
                      Variant::Daylight)) return;
  if (!PRISM_CHECK_EQ(prism::chrome::ShellVariantFor("system", false, SystemTheme::Dark),
                      Variant::Midnight)) return;
  if (!PRISM_CHECK_EQ(prism::chrome::ShellVariantFor("system", false, SystemTheme::Unknown),
                      Variant::Midnight)) return;
  // An explicit choice is the user's, not the desktop's.
  if (!PRISM_CHECK_EQ(prism::chrome::ShellVariantFor("dark", false, SystemTheme::Light),
                      Variant::Midnight)) return;
  if (!PRISM_CHECK_EQ(prism::chrome::ShellVariantFor("light", false, SystemTheme::Dark),
                      Variant::Daylight)) return;
  // Accessibility wins over everything.
  if (!PRISM_CHECK_EQ(prism::chrome::ShellVariantFor("system", true, SystemTheme::Light),
                      Variant::HighContrast)) return;
}

int main(int argc, char** argv) {
#if defined(PRISM_C125_MUTANT_F5_EXTRACT)
  // D3: the verdict is armed on the injection PROVABLY firing -- the same
  // Resolve() call S1 asserts against now answers "extract".
  prism::test::SetMutationVerdict("C125 MUTATION CAUGHT: f5_extract", [] {
    return prism::keys::Resolve(Stroke{prism::keys::kKeyF5, false, false, false}) ==
           Command::SmartExtract;
  });
#elif defined(PRISM_C125_MUTANT_CTRL_E_SMART)
  prism::test::SetMutationVerdict("C125 MUTATION CAUGHT: ctrl_e_smart", [] {
    return prism::keys::Resolve(Stroke{'E', true, false, false}) == Command::SmartExtract;
  });
#elif defined(PRISM_C125_MUTANT_SYSTEM_ALWAYS_MIDNIGHT)
  prism::test::SetMutationVerdict("C125 MUTATION CAUGHT: system_always_midnight", [] {
    return prism::chrome::ShellVariantFor("system", false, SystemTheme::Light) ==
           Variant::Midnight;
  });
#endif
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C125 SHORTCUT CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C125 SHORTCUT CHECKS PASSED");
  return 0;
}
