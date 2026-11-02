// PrismArchive -- PrismCore
// Blueprint ch. 9.1 as DATA: the six shortcut groups, decoded into commands by
// a pure function so the table can be tested on Linux in milliseconds instead
// of being re-derived inside WndProc on every Windows CI round.
//
// Why this file exists (layer rule, HANDOVER ch.4): the window may only map
// events -> decisions -> calls. "Which chord means what" IS a decision, and
// the batch-A code had it inlined in GalleryWindow::Proc next to the pixel
// work. This header is the single authority for the table; the window keeps
// the execution (selection arithmetic, navigation) that it already owns.
//
// Design rules that the tests pin down:
//  * VK codes are passed straight through from WM_KEYDOWN's wParam. Letters
//    and digits have the same value as their ASCII uppercase form, so no
//    translation table can drift from Win32.
//  * Modifiers are STRICT: a chord matches only the exact modifier set. That
//    is what stops AltGr (which arrives as Ctrl+Alt) from triggering Ctrl+1,
//    and it makes "Ctrl+Shift+1" a no-op instead of a surprise mode switch.
//  * Esc is scoped: the overlay on top owns it (palette > quick look > gallery),
//    so a single Escape always closes the thing the user sees.
//  * While the palette is open its text field owns the keyboard: letters and
//    Space are NOT commands (typing "e" must never smart-extract). Only
//    navigation/confirm/close survive inside that scope.
#pragma once

#include <cstdint>
#include <ostream>

namespace prism::keys {

// Win32 virtual-key codes we name. Values are the documented VK_* numbers;
// keep them in sync with <windows.h> by construction (the window passes
// WM_KEYDOWN's wParam through unchanged, and a test asserts the agreement).
inline constexpr uint32_t kKeyBack = 0x08;
inline constexpr uint32_t kKeyReturn = 0x0D;
// C54: editing keys. VK_DELETE = 0x2E, VK_F2 = 0x71; the window passes WM_KEYDOWN's
// wParam straight through, so these values are the Win32 contract, not a guess.
inline constexpr uint32_t kKeyDelete = 0x2E;
inline constexpr uint32_t kKeyF2 = 0x71;
inline constexpr uint32_t kKeyEscape = 0x1B;
inline constexpr uint32_t kKeySpace = 0x20;
inline constexpr uint32_t kKeyEnd = 0x23;
inline constexpr uint32_t kKeyHome = 0x24;
inline constexpr uint32_t kKeyLeft = 0x25;
inline constexpr uint32_t kKeyUp = 0x26;
inline constexpr uint32_t kKeyRight = 0x27;
inline constexpr uint32_t kKeyDown = 0x28;

// Which overlay currently has the keyboard. The window picks it from its own
// state; PrismCore decides what the key MEANS inside that state.
enum class Scope : uint8_t { Gallery, Palette, QuickLook };

inline std::ostream& operator<<(std::ostream& os, Scope scope) {
  switch (scope) {
    case Scope::Gallery: return os << "Gallery";
    case Scope::Palette: return os << "Palette";
    case Scope::QuickLook: return os << "QuickLook";
  }
  return os << "?";
}

// Every action blueprint ch. 9.1 asks for, plus the navigation keys the window
// already implements (kept in the same table so there is ONE key authority).
enum class Command : uint8_t {
  None,
  // 9.1 row 1: Space -> Quick Look, Esc -> close whatever is on top.
  QuickLookToggle,
  QuickLookClose,
  // 9.1 row 2: Ctrl+Shift+P / pinch -> thumbnail ladder (64..512 px).
  ZoomNext,
  // 9.1 row 3: Ctrl+K -> command palette.
  OpenPalette,
  ClosePalette,
  // 9.1 row 4: Ctrl+E / Alt+double-click -> smart extract, no dialog.
  SmartExtract,
  // 9.1 row 5: Ctrl+Shift+N -> virtual folder in the archive (CoW transaction).
  NewFolder,
  // C54: editing an EXISTING archive. Delete removes the selection, F2 renames
  // it. The dialogs and the engine live above this table; the table only says
  // which key means which edit.
  DeleteSelected,
  RenameSelected,
  // 9.1 row 6: Alt+Left/Right -> history back/forward.
  HistoryBack,
  HistoryForward,
  // Batch-A keys, restated here so the table is complete.
  ToggleGrid,
  ToggleList,
  ActivateSelection,
  GoUp,
  SelectFirst,
  SelectLast,
  SelectLeft,
  SelectRight,
  SelectUp,
  SelectDown,
};

inline std::ostream& operator<<(std::ostream& os, Command command) {
  switch (command) {
    case Command::None: return os << "None";
    case Command::QuickLookToggle: return os << "QuickLookToggle";
    case Command::QuickLookClose: return os << "QuickLookClose";
    case Command::ZoomNext: return os << "ZoomNext";
    case Command::OpenPalette: return os << "OpenPalette";
    case Command::ClosePalette: return os << "ClosePalette";
    case Command::SmartExtract: return os << "SmartExtract";
    case Command::NewFolder: return os << "NewFolder";
    case Command::DeleteSelected: return os << "DeleteSelected";
    case Command::RenameSelected: return os << "RenameSelected";
    case Command::HistoryBack: return os << "HistoryBack";
    case Command::HistoryForward: return os << "HistoryForward";
    case Command::ToggleGrid: return os << "ToggleGrid";
    case Command::ToggleList: return os << "ToggleList";
    case Command::ActivateSelection: return os << "ActivateSelection";
    case Command::GoUp: return os << "GoUp";
    case Command::SelectFirst: return os << "SelectFirst";
    case Command::SelectLast: return os << "SelectLast";
    case Command::SelectLeft: return os << "SelectLeft";
    case Command::SelectRight: return os << "SelectRight";
    case Command::SelectUp: return os << "SelectUp";
    case Command::SelectDown: return os << "SelectDown";
  }
  return os << "?";
}

// One WM_KEYDOWN, reduced to what the table cares about.
struct Stroke {
  uint32_t key = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
};

inline bool CtrlOnly(const Stroke& s) { return s.ctrl && !s.shift && !s.alt; }
inline bool CtrlShiftOnly(const Stroke& s) { return s.ctrl && s.shift && !s.alt; }
inline bool AltOnly(const Stroke& s) { return s.alt && !s.ctrl && !s.shift; }

// Default scope is Gallery; overlays pass their own.
inline Command Resolve(const Stroke& stroke, Scope scope = Scope::Gallery) {
  const uint32_t key = stroke.key;

  // Esc first, for every scope: it closes the top-most surface, never the one
  // underneath it. (A user pressing Esc twice closes the palette, then Quick
  // Look -- exactly one layer per press.)
  if (key == kKeyEscape) {
    return scope == Scope::Palette ? Command::ClosePalette : Command::QuickLookClose;
  }

  // The palette owns printable input: its shell feeds characters to the query
  // and only these four survive as commands.
  if (scope == Scope::Palette) {
    if (CtrlOnly(stroke) && key == 'K') return Command::ClosePalette;
    if (stroke.ctrl || stroke.alt) return Command::None;  // chords are suspended
    if (key == kKeyUp) return Command::SelectUp;
    if (key == kKeyDown) return Command::SelectDown;
    if (key == kKeyReturn) return Command::ActivateSelection;
    return Command::None;  // letters / Space belong to the text field
  }

  // ---- Ctrl chords from the 9.1 table -------------------------------------
  if (CtrlOnly(stroke)) {
    switch (key) {
      case 'K': return Command::OpenPalette;   // 9.1 row 3
      case 'E': return Command::SmartExtract;  // 9.1 row 4
      case '1': return Command::ToggleGrid;    // batch A, kept in the table
      case '2': return Command::ToggleList;
      default: return Command::None;
    }
  }
  if (CtrlShiftOnly(stroke)) {
    switch (key) {
      case 'N': return Command::NewFolder;  // 9.1 row 5 (CoW virtual folder)
      case 'P': return Command::ZoomNext;   // 9.1 row 2 (thumbnail ladder)
      default: return Command::None;
    }
  }
  if (AltOnly(stroke)) {
    switch (key) {
      case kKeyLeft: return Command::HistoryBack;  // 9.1 row 6
      case kKeyRight: return Command::HistoryForward;
      default: return Command::None;
    }
  }
  // Any other modified stroke is NOT a bare key. Without this guard
  // Ctrl+Shift+1 would fall through to the unmodified table and switch views.
  if (stroke.ctrl || stroke.alt || stroke.shift) return Command::None;

  switch (key) {
    case kKeySpace: return Command::QuickLookToggle;  // 9.1 row 1
    case kKeyReturn: return Command::ActivateSelection;
    case kKeyDelete: return Command::DeleteSelected;  // C54: delete the selection
    case kKeyF2: return Command::RenameSelected;      // C54: rename the selection
    case kKeyBack: return Command::GoUp;
    case kKeyHome: return Command::SelectFirst;
    case kKeyEnd: return Command::SelectLast;
    case kKeyLeft: return Command::SelectLeft;
    case kKeyRight: return Command::SelectRight;
    case kKeyUp: return Command::SelectUp;
    case kKeyDown: return Command::SelectDown;
    default: return Command::None;
  }
}

// ---------------------------------------------------------------------------
// Zoom ladder (blueprint ch. 9.1 row 2: 64 px .. 512 px, elastic)
//
// Two behaviours, deliberately different:
//  * keyboard chord Ctrl+Shift+P CYCLES the presets (wraps at both ends), so a
//    key-only user can reach every stop with one repeated key;
//  * pinch is CONTINUOUS (ApplyPinch), snapping only to the hard 64..512
//    window -- the renderer interpolates in between.
// ---------------------------------------------------------------------------
namespace zoom {

inline constexpr int kMinPx = 64;
inline constexpr int kMaxPx = 512;
inline constexpr int kPresets[] = {64, 96, 128, 192, 256, 384, 512};
inline constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

inline int ClampPx(int px) {
#ifdef PRISM_ZOOM_MUTANT_NO_CLAMP
  return px;
#else
  if (px < kMinPx) return kMinPx;
  if (px > kMaxPx) return kMaxPx;
  return px;
#endif
}

// First preset strictly larger than the current size; wraps to the smallest at
// the top. A size produced by pinch (e.g. 200) therefore lands on 256 -- the
// stop the user can SEE next, never the one behind them.
inline int NextPreset(int current) {
  for (const int preset : kPresets) {
    if (preset > current) return preset;
  }
  return kMinPx;
}

// Mirror of NextPreset (used by the pinch/shift-wheel path and by tests).
inline int PrevPreset(int current) {
  for (size_t i = kPresetCount; i-- > 0;) {
    if (kPresets[i] < current) return kPresets[i];
  }
  return kMaxPx;
}

inline int ApplyPinch(int current, double factor) {
  if (!(factor > 0.0)) return ClampPx(current);  // NaN/0/negative: no change
  const double scaled = static_cast<double>(current) * factor;
  const int rounded =
      static_cast<int>(scaled + (scaled >= 0.0 ? 0.5 : -0.5));  // round-half-up
  return ClampPx(rounded);
}

}  // namespace zoom

// ---------------------------------------------------------------------------
// Pointer strokes (blueprint ch. 9.1 row 4: "Ctrl+E or Alt+double-click").
// The window already had double-click -> activate; the missing half of that
// row is Alt+double-click, which is decided here rather than sniffed there.
// ---------------------------------------------------------------------------
enum class PointerAction : uint8_t { None, Select, Activate, SmartExtract };

inline std::ostream& operator<<(std::ostream& os, PointerAction action) {
  switch (action) {
    case PointerAction::None: return os << "None";
    case PointerAction::Select: return os << "Select";
    case PointerAction::Activate: return os << "Activate";
    case PointerAction::SmartExtract: return os << "SmartExtract";
  }
  return os << "?";
}

struct PointerStroke {
  int clickCount = 1;
  bool alt = false;
  bool ctrl = false;
  bool shift = false;
  bool secondaryButton = false;  // right button: never an activate
};

inline PointerAction ResolvePointer(const PointerStroke& stroke) {
  if (stroke.secondaryButton) return PointerAction::None;
  if (stroke.clickCount >= 2) {
    // Only a bare Alt chord means "extract now"; Ctrl/Shift are other gestures
    // (multi-select, range) and must not silently start a disk write.
    if (stroke.alt && !stroke.ctrl && !stroke.shift) return PointerAction::SmartExtract;
    return PointerAction::Activate;
  }
  if (stroke.alt) return PointerAction::None;  // Alt+click is a shell gesture
  return PointerAction::Select;
}

}  // namespace prism::keys
