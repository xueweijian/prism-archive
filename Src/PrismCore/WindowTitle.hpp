// PrismArchive -- PrismCore
// WindowTitle: the ONE rule for the main window's caption (card C127).
//
// Windows convention (and Bandizip's) is "<document> - <application>"; the app
// shipped "PrismArchive -- fixture.zip", i.e. the product name first and a
// double hyphen. The rule lives here, as a pure function, because a title is a
// string the suite can assert without a window: Main.cpp's two setters (Create
// and the archive switch) both call this, so there is no second truth.
#pragma once

#include <string>
#include <string_view>

namespace prism::ui {

inline constexpr std::wstring_view kAppName = L"PrismArchive";

// The title for the window showing `archiveFileName` (a FILE NAME, not a path:
// the caller strips directories). No archive -> the program name alone.
inline std::wstring WindowTitle(std::wstring_view archiveFileName) {
  if (archiveFileName.empty()) return std::wstring(kAppName);
  std::wstring title;
  title.reserve(archiveFileName.size() + 3 + kAppName.size());
  title.append(archiveFileName);
  title.append(L" - ");
  title.append(kAppName);
  return title;
}

}  // namespace prism::ui
