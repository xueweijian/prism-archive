// PrismArchive -- PrismCore
// Context-menu model (blueprint ch. 4.1): the single source of truth for which
// verbs a selection offers. The Win11 IExplorerCommand handler, the Win10
// IContextMenu DLL and the in-app right-click all render this model; none of
// them own the decisions.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "I18n.hpp"
#include "PathSafety.hpp"

namespace prism {

struct MenuCommand {
  std::string verb;      // stable id ("smart.extract.here") -- never localized
  std::string title;     // UI label; may embed titleArgument
  std::string titleArgument;  // e.g. the capsule name for "Extract to "<>\""
  bool isDefault = false;     // the bold/primary action
};

class ContextMenuModel {
 public:
  // C40: the labels are the resource table's, not this header's. The verb id IS
  // the key ("menu." + verb), so a verb can never ship without its string (C19's
  // audit case) and a translator that lacks the key keeps the English default --
  // a shell host with no Assets/Lang still gets a usable menu, not "menu.x.y".
  //
  // The translator is a parameter, not a member: the model stays a pure function
  // of (selection, language), which is what lets the suite assert the CHINESE
  // text of all twelve labels without a global.
  static std::vector<MenuCommand> Build(const std::vector<std::string>& selectedPaths,
                                        const prism::i18n::Translator* translator = nullptr) {
    std::vector<MenuCommand> items = BuildDefault(selectedPaths);
    if (translator == nullptr) return items;
    for (MenuCommand& item : items) {
      const std::string key = std::string("menu.") + item.verb;
      if (!translator->Has(key)) continue;
      item.title = translator->Translate(key, {{"name", item.titleArgument}});
    }
    return items;
  }

  // The English defaults, verb by verb. Kept as its own function so the table's
  // en-US values and these literals can be compared byte for byte (they are).
  static std::vector<MenuCommand> BuildDefault(const std::vector<std::string>& selectedPaths) {
    std::vector<MenuCommand> items;
    if (selectedPaths.empty()) return items;

    size_t archives = 0;
    std::string soleArchive;
    for (const std::string& path : selectedPaths) {
      if (IsArchivePath(path)) {
        ++archives;
        if (archives == 1) soleArchive = path;
      }
    }

    // A single archive drives the extract group even when the selection also
    // contains non-archives (mixed selection: extract group + compress group).
    if (archives == 1) {
      const std::string stem = ArchiveStem(soleArchive);
      items.push_back({"smart.extract.here", "Smart Extract Here", "", true});
      items.push_back({"extract.here", "Extract Here", "", false});
      items.push_back({"extract.to.subfolder", "Extract to \"" + stem + "\"", stem, false});
      items.push_back({"test.integrity", "Test Integrity", "", false});
      items.push_back({"open.in.prism", "Open with PrismArchive", "", false});
      // no early return: a mixed selection still gets the compress group below
    }

    if (archives >= 2) {
      items.push_back({"smart.extract.each.to.own.folder",
                       "Smart Extract (each to its own folder)", "", true});
      items.push_back({"extract.here", "Extract Here", "", false});
      items.push_back({"test.integrity", "Test Integrity", "", false});
    }

    if (archives < selectedPaths.size()) {
      // The selection includes non-archives: offer packing alongside.
      items.push_back({"compress.to.7z", "Compress to a .7z archive", "", false});
      items.push_back({"compress.to.zip", "Compress to a .zip archive", "", false});
    }
    return items;
  }

  // C40: where a SHELL HOST finds the tables. explorer.exe owns the process, so
  // "the executable's directory" would be C:\Windows -- the tables live next to
  // the MODULE that renders the menu (<dll dir>\Assets\Lang), which is also the
  // flat layout "cmake --install" produces. Pure string work so the rule is
  // assertable without a shell.
  static std::string LanguageDirectoryForModuleDir(std::string_view moduleDirectoryUtf8) {
    std::string dir(moduleDirectoryUtf8);
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    if (dir.empty()) return std::string();
    return dir + "\\Assets\\Lang";
  }

  static bool IsArchivePath(const std::string& path) {
    const std::string ext = ExtensionOf(path);  // ".zip" form, lowercased
    return ext == ".zip" || ext == ".7z" || ext == ".rar" || ext == ".tar" ||
           ext == ".gz" || ext == ".xz" || ext == ".zst" || ext == ".bz2" ||
           ext == ".tar.gz" || ext == ".tar.zst" || ext == ".tgz" || ext == ".cab" ||
           ext == ".iso" || ext == ".tar.bz2";
  }

  // The single command line executed for a verb. Kept next to the model so the
  // verb ids and their payloads can never drift apart.
  static std::string VerbPayload(const std::string& verb, const std::string& archivePath) {
    if (verb == "extract.to.subfolder") return ArchiveStem(archivePath);
    return archivePath;
  }
};

}  // namespace prism
