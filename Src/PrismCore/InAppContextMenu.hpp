// PrismArchive -- PrismCore
// In-app context menus (card C92): the ONE description of what a right-click
// offers, per SURFACE (a list row, empty list space, a sidebar node).
//
// WHY A MODEL AND NOT THREE HAND-WRITTEN MENUS
// The window builds an HMENU from this model and dispatches the chosen verb; the
// suite asserts the same model through the real WM_CONTEXTMENU path. A verb
// that exists only in the Win32 code would be invisible to every non-Windows
// lane, and a disabled item with no reason would be indistinguishable from a
// bug. Here each disabled item MUST carry a reason (a table key), which the
// window paints into the status line on hover.
//
// IDs: every surface owns a disjoint id range, so "which menu did this command
// come from" is answerable from the id alone and the three id sets can never be
// confused with one another.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "I18n.hpp"

namespace prism::ctxmenu {

enum class Surface : std::uint8_t { Row = 0, Empty = 1, Sidebar = 2, Count = 3 };

inline const char* SurfaceName(Surface surface) {
  switch (surface) {
    case Surface::Row: return "row";
    case Surface::Empty: return "empty";
    case Surface::Sidebar: return "sidebar";
    case Surface::Count: break;
  }
  return "?";
}

enum class Command : std::uint8_t {
  None = 0,
  Open,
  ExtractSelected,
  ExtractAll,
  SmartExtractHere,
  CopyPath,
  RevealArchive,
  SelectAll,
  Refresh,
  Count
};

inline const char* CommandName(Command command) {
  switch (command) {
    case Command::None: return "None";
    case Command::Open: return "Open";
    case Command::ExtractSelected: return "ExtractSelected";
    case Command::ExtractAll: return "ExtractAll";
    case Command::SmartExtractHere: return "SmartExtractHere";
    case Command::CopyPath: return "CopyPath";
    case Command::RevealArchive: return "RevealArchive";
    case Command::SelectAll: return "SelectAll";
    case Command::Refresh: return "Refresh";
    case Command::Count: break;
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, Command command) {
  return os << CommandName(command);
}

inline std::ostream& operator<<(std::ostream& os, Surface surface) {
  return os << SurfaceName(surface);
}

// 32 ids per surface: room for a menu twice this size without ever colliding.
inline constexpr std::uint32_t kIdSlotsPerSurface = 32;
inline constexpr std::uint32_t kIdBase = 0x9200;

inline std::uint32_t CommandId(Surface surface, std::size_t slot) {
  return kIdBase + static_cast<std::uint32_t>(surface) * kIdSlotsPerSurface +
         static_cast<std::uint32_t>(slot);
}

inline bool IsContextCommandId(std::uint32_t id) {
  return id >= kIdBase && id < kIdBase + 3 * kIdSlotsPerSurface;
}

inline Surface SurfaceOfCommandId(std::uint32_t id) {
  return static_cast<Surface>((id - kIdBase) / kIdSlotsPerSurface);
}

inline std::size_t SlotOfCommandId(std::uint32_t id) {
  return static_cast<std::size_t>((id - kIdBase) % kIdSlotsPerSurface);
}

// The words, copied by value. A live Translator pointer is NOT kept: the
// Settings dialog hands out a translator that dies with the modal call, the same
// trap ApplyLanguage() documents for the sidebar labels.
struct Labels {
  std::string open = "Open";
  std::string extractSelected = "Extract Selected\u2026";
  std::string extractAll = "Extract All";
  std::string smartExtractHere = "Smart Extract Here";
  std::string copyPath = "Copy Path";
  std::string revealArchive = "Show Archive in Explorer";
  std::string selectAll = "Select All";
  std::string refresh = "Refresh";
  // Disabled reasons.
  std::string reasonNoSelection = "No items are selected";
  std::string reasonNotAFile = "The selection is not a single file";
  std::string reasonNoArchive = "No archive is open";
};

// Key names, one place, so the audit's "every key-shaped literal" rule and the
// shipped tables can never drift: LabelsFrom reads exactly these.
inline const char* LabelKey(Command command) {
  switch (command) {
    case Command::Open: return "context.open";
    case Command::ExtractSelected: return "context.extract.selected";
    case Command::ExtractAll: return "context.extract.all";
    case Command::SmartExtractHere: return "context.smart.extract.here";
    case Command::CopyPath: return "context.copy.path";
    case Command::RevealArchive: return "context.reveal.archive";
    case Command::SelectAll: return "context.select.all";
    case Command::Refresh: return "context.refresh";
    case Command::None:
    case Command::Count: break;
  }
  return "";
}

inline Labels LabelsFrom(const prism::i18n::Translator* translator) {
  Labels labels;
  if (translator == nullptr) return labels;
  struct Binding {
    Command command;
    std::string* target;
  };
  const Binding bindings[] = {
      {Command::Open, &labels.open},
      {Command::ExtractSelected, &labels.extractSelected},
      {Command::ExtractAll, &labels.extractAll},
      {Command::SmartExtractHere, &labels.smartExtractHere},
      {Command::CopyPath, &labels.copyPath},
      {Command::RevealArchive, &labels.revealArchive},
      {Command::SelectAll, &labels.selectAll},
      {Command::Refresh, &labels.refresh},
  };
  for (const Binding& binding : bindings) {
    const char* key = LabelKey(binding.command);
    if (translator->Has(key)) *binding.target = translator->Translate(key);
  }
  struct ReasonBinding {
    const char* key;
    std::string* target;
  };
  const ReasonBinding reasons[] = {
      {"context.disabled.no.selection", &labels.reasonNoSelection},
      {"context.disabled.not.a.file", &labels.reasonNotAFile},
      {"context.disabled.no.archive", &labels.reasonNoArchive},
  };
  for (const ReasonBinding& reason : reasons) {
    if (translator->Has(reason.key)) *reason.target = translator->Translate(reason.key);
  }
  return labels;
}

// What the window knows about the selection at the moment of the click. Facts,
// never conclusions: the menu rules below are the only place that turns them
// into items.
struct Facts {
  bool archiveOpen = false;
  bool hasSelection = false;
  bool singleFileSelected = false;
  bool singleFolderSelected = false;
  int rowCount = 0;
};

struct Item {
  Command command = Command::None;
  std::string label;
  bool enabled = true;
  // Non-empty exactly when !enabled: the sentence the status line shows while
  // the item is hovered. An empty reason on a disabled item is a menu lie.
  std::string disabledReason;
  bool separatorBefore = false;
};

struct Menu {
  Surface surface = Surface::Empty;
  std::vector<Item> items;

  bool Empty() const { return items.empty(); }
  const Item* Find(Command command) const {
    for (const Item& item : items) {
      if (item.command == command) return &item;
    }
    return nullptr;
  }
  std::size_t EnabledCount() const {
    std::size_t count = 0;
    for (const Item& item : items) {
      if (item.enabled) ++count;
    }
    return count;
  }
  bool ReasonsOnEveryDisabledItem() const {
    for (const Item& item : items) {
      if (!item.enabled && item.disabledReason.empty()) return false;
    }
    return true;
  }
};

namespace detail {

inline Item Make(Command command, const std::string& label, bool enabled,
                 const std::string& reason) {
  Item item;
  item.command = command;
  item.label = label;
  item.enabled = enabled;
  item.disabledReason = enabled ? std::string() : reason;
  return item;
}

}  // namespace detail

// The three menus. The bullet lists are the card's: a file row opens, extracts
// the selection, extracts all, smart-extracts beside the archive, copies the
// path and shows the archive; empty space offers the archive-level verbs; a
// sidebar node offers the archive it belongs to.
inline Menu Build(Surface surface, const Facts& facts, const Labels& labels) {
  using detail::Make;
  Menu menu;
  menu.surface = surface;
  const std::string& noSelection = labels.reasonNoSelection;
  const std::string& noArchive = labels.reasonNoArchive;

  if (surface == Surface::Row) {
    menu.items.push_back(Make(Command::Open, labels.open,
                              facts.singleFileSelected, labels.reasonNotAFile));
    menu.items.push_back(Make(Command::ExtractSelected, labels.extractSelected,
                              facts.hasSelection && facts.archiveOpen,
                              facts.hasSelection ? noArchive : noSelection));
    menu.items.push_back(Make(Command::ExtractAll, labels.extractAll, facts.archiveOpen,
                              noArchive));
    menu.items.push_back(
        Make(Command::SmartExtractHere, labels.smartExtractHere, facts.archiveOpen, noArchive));
    menu.items.push_back(Make(Command::CopyPath, labels.copyPath, facts.hasSelection,
                              noSelection));
    menu.items.push_back(Make(Command::RevealArchive, labels.revealArchive, facts.archiveOpen,
                              noArchive));
    return menu;
  }

  if (surface == Surface::Sidebar) {
    // The node IS the archive, so every verb here is archive-scoped.
    menu.items.push_back(
        Make(Command::SmartExtractHere, labels.smartExtractHere, facts.archiveOpen, noArchive));
    menu.items.push_back(Make(Command::ExtractAll, labels.extractAll, facts.archiveOpen,
                              noArchive));
    menu.items.push_back(Make(Command::RevealArchive, labels.revealArchive, facts.archiveOpen,
                              noArchive));
    menu.items.push_back(Make(Command::CopyPath, labels.copyPath, facts.archiveOpen, noArchive));
    menu.items.push_back(Make(Command::Refresh, labels.refresh, true, std::string()));
    return menu;
  }

  // Empty space: no row is under the pointer, so nothing row-scoped is offered.
  menu.items.push_back(
      Make(Command::SmartExtractHere, labels.smartExtractHere, facts.archiveOpen, noArchive));
  menu.items.push_back(Make(Command::ExtractAll, labels.extractAll, facts.archiveOpen,
                            noArchive));
  menu.items.push_back(Make(Command::SelectAll, labels.selectAll, facts.rowCount > 0,
                            std::string("The folder is empty")));
  menu.items.push_back(Make(Command::Refresh, labels.refresh, true, std::string()));
  menu.items.push_back(Make(Command::RevealArchive, labels.revealArchive, facts.archiveOpen,
                            noArchive));
  return menu;
}

}  // namespace prism::ctxmenu
