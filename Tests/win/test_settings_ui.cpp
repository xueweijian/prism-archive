// PrismArchive -- settings dialog + command palette overlay (card C31).
//
// ONE source file, TWO ctest entries (Tests/suites/c31-ui.cmake):
//
//   core.c31_settings_ui   DialogState + PaletteOverlay: every RULE, every lane,
//                          milliseconds, no window station (the same split C18
//                          used for the compress dialog).
//   win.c31_settings_ui    the SAME DialogState driven through a REAL HWND:
//                          CreateWindowExW, real child controls, real
//                          WM_COMMAND/BM_CLICK, and a real atomic save into a
//                          temp %TEMP% folder through SettingsFile.
//
// The Windows half never calls the state directly for a rule it wants to prove:
// it types into the control, sends the notification the control would send, and
// reads the control back. A test that re-implemented the routing would prove
// nothing about the dialog (see Tests/win/test_compress_dialog.cpp).
//
// Negative controls (CONTRACTS D3, wired in the suite file):
//   PRISM_C31_MUTANT_ACCEPT_BAD_DIR        the extract-dir rule is dropped
//   PRISM_C31_MUTANT_ACCEPT_BAD_THUMBNAIL  the cache-cap rule is dropped
//   PRISM_C31_MUTANT_SKIP_STORE_PROBE      the SettingsStore verdict is discarded
// Each mutant is compiled from THIS source. The marker is a CONJUNCTION -- the
// run went red AND DialogState::MutationFired() is non-zero -- so CONTRACTS D26
// holds (no WILL_FAIL is ever combined with PASS_REGULAR_EXPRESSION) and a
// mutation that stopped applying prints no marker.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "../mini_test.hpp"
#include "../../Src/PrismCore/CommandPalette.hpp"
#include "../../Src/PrismCore/SettingsStore.hpp"
#include "../../Src/PrismWin/PaletteOverlay.hpp"
#include "../../Src/PrismWin/SettingsDialog.hpp"

namespace su = prism::win::settingsui;
namespace ov = prism::win::palette;
namespace st = prism::settings;

namespace {

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

std::vector<prism::ArchiveItem> FixtureCatalog() {
  std::vector<prism::ArchiveItem> items;
  items.push_back(prism::ArchiveItem{1, "docs/readme.txt", 4096, 1024, 0, 0, false, false});
  items.push_back(prism::ArchiveItem{2, "photos/root.png", 2000000, 1900000, 0, 1, false, false});
  items.push_back(prism::ArchiveItem{3, "photos/other.png", 3000000, 2800000, 0, 1, false, false});
  items.push_back(prism::ArchiveItem{4, "notes.txt", 512, 256, 0, 0, false, false});
  return items;
}

// A shell with the real command table and a small catalog, opened.
ov::PaletteOverlay OpenedOverlay(prism::win::palette::PaletteShell& shell) {
  shell.SetCommands(prism::palette::DefaultCommands());
  shell.SetCatalog(FixtureCatalog());
  ov::PaletteOverlay overlay;
  overlay.Attach(&shell);
  overlay.Open();
  return overlay;
}

}  // namespace

// ===========================================================================
// 1. DialogState -- defaults, units, ranges, mapping to the store.
// ===========================================================================

PRISM_TEST("state: a fresh dialog mirrors the store's documented defaults") {
  su::DialogState state;
  PRISM_CHECK_EQ(state.languageTag(), std::string("auto"));
  PRISM_CHECK_EQ(state.languageIndex(), 0);
  PRISM_CHECK(state.languages().size() >= 3);
  PRISM_CHECK(state.ThemeAt(0) == su::Theme::Daylight);
  PRISM_CHECK(state.theme() == su::Theme::Daylight);
  PRISM_CHECK_EQ(state.extractDirText(), std::string());
  PRISM_CHECK_EQ(state.thumbnailText(), std::string("512"));
  PRISM_CHECK_EQ(state.thumbnailUnit(), su::SizeUnit::MB);
  PRISM_CHECK_EQ(state.ThumbnailMb(), 512);
  PRISM_CHECK_EQ(state.logLevel(), su::LogLevel::Info);
  PRISM_CHECK(state.inheritMotw());
  // A first run is already good: Save is enabled and nothing is complained about.
  PRISM_CHECK(state.Valid());
  PRISM_CHECK_EQ(state.issues().size(), size_t{0});
  PRISM_CHECK(state.SaveEnabled());
}

PRISM_TEST("state: the language list is what ships, and an unshipped tag is refused") {
  su::DialogState state;
  state.SetLanguages({"auto", "zh-CN", "en-US"});
  PRISM_CHECK_EQ(state.languages().size(), size_t{3});
  PRISM_CHECK(state.SetLanguageTag("zh-CN"));
  PRISM_CHECK_EQ(state.languageTag(), std::string("zh-CN"));
  PRISM_CHECK_EQ(state.languageIndex(), 1);
  PRISM_CHECK(!state.SetLanguageTag("ja-JP"));  // not installed: never a silent no-op
  PRISM_CHECK_EQ(state.languageTag(), std::string("zh-CN"));
  PRISM_CHECK(state.SetLanguageIndex(2));
  PRISM_CHECK_EQ(state.languageTag(), std::string("en-US"));
  PRISM_CHECK(!state.SetLanguageIndex(9));
  PRISM_CHECK(!state.SetLanguageIndex(-1));
}

PRISM_TEST("state: the three themes map onto the schema value plus highContrast") {
  su::DialogState state;
  PRISM_CHECK(state.ThemeAt(1) == su::Theme::Midnight);
  PRISM_CHECK(state.ThemeAt(2) == su::Theme::HighContrast);
  PRISM_CHECK(state.ThemeAt(7) == su::Theme::Daylight);  // clamped, not UB

  st::SettingsStore store;
  state.SetTheme(su::Theme::Midnight);
  PRISM_CHECK(state.ApplyTo(store));
  PRISM_CHECK_EQ(store.GetString(su::kThemeKey), std::string("dark"));
  PRISM_CHECK_EQ(store.GetBool(su::kHighContrastKey, true), false);

  state.SetTheme(su::Theme::HighContrast);
  PRISM_CHECK(state.ApplyTo(store));
  PRISM_CHECK_EQ(store.GetString(su::kThemeKey), std::string("light"));
  PRISM_CHECK(store.GetBool(su::kHighContrastKey, false));

  // Reading it back is the same three-valued choice -- no drift on a round trip.
  su::DialogState reread;
  reread.Adopt(store);
  PRISM_CHECK(reread.theme() == su::Theme::HighContrast);

  state.SetTheme(su::Theme::Daylight);
  PRISM_CHECK(state.ApplyTo(store));
  PRISM_CHECK_EQ(store.GetString(su::kThemeKey), std::string("light"));
  PRISM_CHECK(!store.GetBool(su::kHighContrastKey, true));
}

PRISM_TEST("state: a stored 'system' theme loads as Day, and is pinned on save") {
  st::SettingsStore store;
  store.SetString(su::kThemeKey, "system");
  su::DialogState state;
  state.Adopt(store);
  PRISM_CHECK(state.theme() == su::Theme::Daylight);
  PRISM_CHECK(state.ApplyTo(store));
  PRISM_CHECK_EQ(store.GetString(su::kThemeKey), std::string("light"));
}

PRISM_TEST("state: the extraction folder must be an absolute drive path (B4)") {
  su::DialogState state;
  PRISM_CHECK(state.Valid());  // empty means "ask every time"

  state.SetExtractDirText("C:\\Users\\me\\Extracted");
  PRISM_CHECK(state.Valid());
  state.SetExtractDirText("C:/Users/me/Extracted/");
  // A forward slash is ACCEPTED but normalized: what reaches the store is a
  // Win32 path, never a mixed one.
  PRISM_CHECK(state.Valid());
  PRISM_CHECK_EQ(state.normalizedExtractDir(), std::string("C:\\Users\\me\\Extracted"));

  state.SetExtractDirText("C:\\");
  PRISM_CHECK(state.Valid());
  PRISM_CHECK_EQ(state.normalizedExtractDir(), std::string("C:\\"));  // the root keeps its slash

  state.SetExtractDirText("relative\\folder");
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK(state.HasIssue(su::Field::ExtractDir));
  PRISM_CHECK(Contains(state.IssueFor(su::Field::ExtractDir), "absolute"));
  PRISM_CHECK(!state.SaveEnabled());

  state.SetExtractDirText("C:\\a\\..\\b");
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK(Contains(state.IssueFor(su::Field::ExtractDir), ".."));

  state.SetExtractDirText("C:\\a\\b*c");
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK(Contains(state.IssueFor(su::Field::ExtractDir), "*"));

  // ".." inside a NAME is not a traversal and must not be refused.
  state.SetExtractDirText("C:\\a..b\\c");
  PRISM_CHECK(state.Valid());

  std::string tooLong = "C:\\";
  while (tooLong.size() < 262) tooLong += "x";
  state.SetExtractDirText(tooLong);
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK(Contains(state.IssueFor(su::Field::ExtractDir), "260"));
}

PRISM_TEST("state: the cache cap understands units and refuses the edges") {
  su::DialogState state;
  state.SetThumbnailText("512");
  PRISM_CHECK_EQ(state.ThumbnailMb(), 512);
  state.SetThumbnailText("2");
  state.SetThumbnailUnit(su::SizeUnit::GB);
  PRISM_CHECK_EQ(state.ThumbnailMb(), 2048);        // 2 GB in MB
  PRISM_CHECK_EQ(state.ThumbnailLabel(), std::string("2 GB (2048 MB)"));
  state.SetThumbnailText("0.5");
  PRISM_CHECK_EQ(state.ThumbnailMb(), 512);         // half a gigabyte
  state.SetThumbnailText("2.5");
  PRISM_CHECK_EQ(state.ThumbnailMb(), 2560);
  state.SetThumbnailText(" 17 ");
  PRISM_CHECK_EQ(state.ThumbnailMb(), -1);          // 17 GB is above the cap
  PRISM_CHECK(Contains(state.IssueFor(su::Field::ThumbnailLimit), "16384"));

  state.SetThumbnailUnit(su::SizeUnit::MB);
  state.SetThumbnailText("31");
  PRISM_CHECK_EQ(state.ThumbnailMb(), -1);
  PRISM_CHECK(Contains(state.IssueFor(su::Field::ThumbnailLimit), "32"));
  state.SetThumbnailText("512");
  PRISM_CHECK(state.Valid());

  // Not-a-number shapes, one at a time: each must be refused with its own text
  // (a single "invalid" message would not tell the user what to type).
  const char* bad[] = {"", "   ", "abc", "12ab", "1e3", "-5", "1.2.3", "512 MB"};
  for (const char* text : bad) {
    state.SetThumbnailText(text);
    PRISM_CHECK(!state.Valid());
    PRISM_CHECK(!state.IssueFor(su::Field::ThumbnailLimit).empty());
  }
  // A decimal that is not a whole number of megabytes is refused too.
  state.SetThumbnailText("0.001");
  state.SetThumbnailUnit(su::SizeUnit::GB);
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK(Contains(state.IssueFor(su::Field::ThumbnailLimit), "whole number"));
}

PRISM_TEST("state: log level and MoTW ride as ordinary keys and survive a reload") {
  su::DialogState state;
  state.SetLogLevel(su::LogLevel::Debug);
  state.SetInheritMotw(false);
  st::SettingsStore store;
  PRISM_CHECK(state.ApplyTo(store));
  PRISM_CHECK_EQ(store.GetString(su::kLogLevelKey), std::string("debug"));
  PRISM_CHECK_EQ(store.GetBool(su::kMotwKey, true), false);

  // ... and the store's own loader keeps keys it does not name (C14 contract),
  // so a settings file written by this dialog is still readable next start.
  st::SettingsStore reloaded;
  const st::LoadReport report = reloaded.Load(store.ToJson());
  PRISM_CHECK_EQ(st::LoadStatusName(report.status), std::string("Ok"));
  PRISM_CHECK_EQ(reloaded.GetString(su::kLogLevelKey), std::string("debug"));
  PRISM_CHECK(!reloaded.GetBool(su::kMotwKey, true));
  su::DialogState reread;
  reread.Adopt(reloaded);
  PRISM_CHECK_EQ(reread.logLevel(), su::LogLevel::Debug);
  PRISM_CHECK(!reread.inheritMotw());
  PRISM_CHECK_EQ(su::LogLevelFromKey("warn"), su::LogLevel::Warn);
  PRISM_CHECK_EQ(su::LogLevelFromKey("nonsense"), su::LogLevel::Info);
}

PRISM_TEST("state: an illegal draft leaves the store byte-for-byte untouched") {
  st::SettingsStore store;
  store.SetString("future.thing", "kept");
  const std::string before = store.ToJson();

  su::DialogState state;
  state.Adopt(store);
  state.SetExtractDirText("relative\\folder");
  PRISM_CHECK(!state.ApplyTo(store));
  PRISM_CHECK_EQ(store.ToJson(), before);  // D2: the CONTENT did not change
  PRISM_CHECK_EQ(store.GetString("future.thing"), std::string("kept"));

  // The same guarantee for the numeric field.
  state.SetExtractDirText("C:\\ok");
  state.SetThumbnailText("0");
  PRISM_CHECK(!state.ApplyTo(store));
  PRISM_CHECK_EQ(store.ToJson(), before);
}

PRISM_TEST("state: the store's own schema is the authority for a language tag") {
  // "de-DE" is a file the assets may ship but the schema does not allow. The
  // dialog must NOT invent a second allow-list: the repair the store reports IS
  // the complaint (CONTRACTS D7).
  su::DialogState state;
  state.SetLanguages({"auto", "de-DE"});
  PRISM_CHECK(state.SetLanguageTag("de-DE"));
  state.SetExtractDirText("C:\\ok");
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK(state.HasIssue(su::Field::Language));
  const std::string issue = state.IssueFor(su::Field::Language);
  PRISM_CHECK(Contains(issue, "language.tag"));
  PRISM_CHECK(Contains(issue, "not a documented value"));
  PRISM_CHECK(!state.SaveEnabled());

  // A tag the schema knows passes the same probe untouched.
  PRISM_CHECK(state.SetLanguageTag("auto"));
  PRISM_CHECK(state.Valid());
}

PRISM_TEST("state: a document from a newer build is read-only, and says so") {
  st::SettingsStore store;
  PRISM_CHECK_EQ(st::LoadStatusName(store.Load("{\"schemaVersion\": 99}").status),
                 std::string("TooNew"));
  PRISM_CHECK(!store.writable());
  su::DialogState state;
  state.Adopt(store);
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK(Contains(state.IssueFor(su::Field::Theme), "newer build"));
  PRISM_CHECK(!state.ApplyTo(store));
}

PRISM_TEST("state: unknown keys a future build wrote are preserved across a save") {
  st::SettingsStore store;
  const st::LoadReport report =
      store.Load("{\"schemaVersion\": 3, \"future\": {\"thing\": 42}}");
  // Only "future" is present, so the KNOWN keys are filled with defaults -- the
  // point here is that the unknown subtree is not touched by any of it.
  PRISM_CHECK_EQ(st::LoadStatusName(report.status), std::string("FilledDefaults"));
  su::DialogState state;
  state.Adopt(store);
  state.SetLogLevel(su::LogLevel::Warn);
  PRISM_CHECK(state.ApplyTo(store));
  PRISM_CHECK_EQ(store.GetInt("future.thing", 0), 42);
  PRISM_CHECK_EQ(store.GetString(su::kLogLevelKey), std::string("warn"));
}

PRISM_TEST("state: issues are one per field and name the field") {
  su::DialogState state;
  state.SetExtractDirText("nope");
  state.SetThumbnailText("0");
  PRISM_CHECK(!state.Valid());
  PRISM_CHECK_EQ(state.issues().size(), size_t{2});
  PRISM_CHECK(state.HasIssue(su::Field::ExtractDir));
  PRISM_CHECK(state.HasIssue(su::Field::ThumbnailLimit));
  PRISM_CHECK(!state.HasIssue(su::Field::Theme));
  PRISM_CHECK_EQ(su::FieldName(su::Field::ExtractDir), std::string("extract-dir"));
}

// ===========================================================================
// 2. PaletteOverlay -- draw commands, hit test, spans, Tab/Esc routing.
// ===========================================================================

namespace {

const ov::DrawCommand* FirstOf(const std::vector<ov::DrawCommand>& commands, ov::DrawKind kind) {
  for (const ov::DrawCommand& command : commands) {
    if (command.kind == kind) return &command;
  }
  return nullptr;
}

std::vector<const ov::DrawCommand*> AllOf(const std::vector<ov::DrawCommand>& commands,
                                          ov::DrawKind kind) {
  std::vector<const ov::DrawCommand*> found;
  for (const ov::DrawCommand& command : commands) {
    if (command.kind == kind) found.push_back(&command);
  }
  return found;
}

prism::keys::Stroke Key(std::uint32_t key) {
  prism::keys::Stroke stroke;
  stroke.key = key;
  return stroke;
}

}  // namespace

PRISM_TEST("overlay: closed means no commands at all") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay;
  overlay.Attach(&shell);
  PRISM_CHECK(!overlay.IsOpen());
  PRISM_CHECK(overlay.Build(1200, 800).empty());
  PRISM_CHECK_EQ(overlay.RowCount(), 0);
  // A key pressed while closed is not ours.
  PRISM_CHECK(!overlay.OnKey(Key(prism::keys::kKeyDown)).consumed);
}

PRISM_TEST("overlay: the card, the placeholder query and the scope badge are drawn") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay = OpenedOverlay(shell);

  const std::vector<ov::DrawCommand> commands = overlay.Build(1200, 800);
  const ov::DrawCommand* card = FirstOf(commands, ov::DrawKind::Card);
  PRISM_CHECK(card != nullptr);
  // Geometry has ONE source: the card is exactly the shell's frame.
  const prism::win::palette::Rect frame = shell.Frame(1200, 800);
  PRISM_CHECK_EQ(card->rect.left, frame.left);
  PRISM_CHECK_EQ(card->rect.top, frame.top);
  PRISM_CHECK_EQ(card->rect.right, frame.right);
  PRISM_CHECK_EQ(card->rect.bottom, frame.bottom);

  const ov::DrawCommand* query = FirstOf(commands, ov::DrawKind::QueryText);
  PRISM_CHECK(query != nullptr);
  PRISM_CHECK(Contains(query->text, "Type a command"));
  const ov::DrawCommand* badge = FirstOf(commands, ov::DrawKind::ScopeBadge);
  PRISM_CHECK(badge != nullptr);
  PRISM_CHECK_EQ(badge->text, std::string("All"));
  PRISM_CHECK(FirstOf(commands, ov::DrawKind::Separator) != nullptr);

  // One title + one subtitle per visible row, each inside the shell's row rect.
  const int rows = overlay.RowCount();
  PRISM_CHECK(rows > 0);
  PRISM_CHECK_EQ(AllOf(commands, ov::DrawKind::RowTitle).size(),
                 static_cast<size_t>(rows < overlay.VisibleRowCount(1200, 800)
                                         ? rows
                                         : overlay.VisibleRowCount(1200, 800)));
  const ov::DrawCommand* firstTitle = FirstOf(commands, ov::DrawKind::RowTitle);
  PRISM_CHECK(firstTitle != nullptr);
  PRISM_CHECK_EQ(firstTitle->row, 0);
  const prism::win::palette::Rect rowRect = shell.RowRect(1200, 800, 0);
  PRISM_CHECK(firstTitle->rect.top >= rowRect.top);
  PRISM_CHECK(firstTitle->rect.bottom <= rowRect.bottom);
}

PRISM_TEST("overlay: the query is ranked by the model and highlighted by BYTES") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay = OpenedOverlay(shell);
  shell.SetQuery("png");  // the palette's own text field

  const std::vector<ov::DrawCommand> commands = overlay.Build(1200, 800);
  const std::vector<const ov::DrawCommand*> titles = AllOf(commands, ov::DrawKind::RowTitle);
  PRISM_CHECK(!titles.empty());

  bool sawRootPng = false;
  for (const ov::DrawCommand* title : titles) {
    PRISM_CHECK(!title->spans.empty());  // every match must carry its highlight
    for (const ov::TextSpan& span : title->spans) {
      PRISM_CHECK(span.begin < span.end);
      PRISM_CHECK(span.end <= title->text.size());
    }
    if (title->text == "photos/root.png") {
      sawRootPng = true;
      // "png" sits at the end of the title: ONE merged run, and its CONTENT is
      // the query (D2: a span that points at nothing proves nothing).
      PRISM_CHECK_EQ(title->spans.size(), size_t{1});
      const ov::TextSpan span = title->spans.front();
      PRISM_CHECK_EQ(title->text.substr(span.begin, span.end - span.begin), std::string("png"));
      const prism::win::palette::Rect spanRect = ov::TextSpanRect(title->rect, span);
      PRISM_CHECK_EQ(spanRect.Width(), 3 * ov::kCharWidth);
    }
  }
  PRISM_CHECK(sawRootPng);
}

PRISM_TEST("overlay: a gapped match produces one span per run") {
  const std::vector<ov::TextSpan> spans =
      ov::PaletteOverlay::MergePositions({0, 2, 4}, 5);  // "a_b_c" with "abc"
  PRISM_CHECK_EQ(spans.size(), size_t{3});
  PRISM_CHECK_EQ(spans[0].begin, size_t{0});
  PRISM_CHECK_EQ(spans[0].end, size_t{1});
  PRISM_CHECK_EQ(spans[2].begin, size_t{4});
  PRISM_CHECK_EQ(spans[2].end, size_t{5});
  // Out-of-range positions are dropped, never clamped into another character.
  const std::vector<ov::TextSpan> clamped = ov::PaletteOverlay::MergePositions({0, 99}, 1);
  PRISM_CHECK_EQ(clamped.size(), size_t{1});
  PRISM_CHECK_EQ(clamped[0].end, size_t{1});
  // Unsorted + duplicated input is folded into runs.
  // {1,2,3} is one run and {5} is another; the duplicate 1 and the unsorted
  // order must not create a third span.
  const std::vector<ov::TextSpan> folded =
      ov::PaletteOverlay::MergePositions({3, 1, 2, 1, 5}, 6);
  PRISM_CHECK_EQ(folded.size(), size_t{2});
  PRISM_CHECK_EQ(folded[0].begin, size_t{1});
  PRISM_CHECK_EQ(folded[0].end, size_t{4});
  PRISM_CHECK_EQ(folded[1].begin, size_t{5});
  PRISM_CHECK_EQ(folded[1].end, size_t{6});
}

PRISM_TEST("overlay: the hit test and the painted rows are the same rectangles") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay = OpenedOverlay(shell);
  PRISM_CHECK(overlay.RowCount() >= 3);

  for (int row = 0; row < 3; ++row) {
    const prism::win::palette::Rect rect = shell.RowRect(1200, 800, row);
    const int x = (rect.left + rect.right) / 2;
    const int y = (rect.top + rect.bottom) / 2;
    PRISM_CHECK_EQ(overlay.RowAt(1200, 800, x, y), row);
    // The row the hit test names is inside the rect the command carries.
    const std::vector<ov::DrawCommand> commands = overlay.Build(1200, 800);
    const std::vector<const ov::DrawCommand*> backgrounds =
        AllOf(commands, ov::DrawKind::RowBackground);
    PRISM_CHECK(backgrounds.size() > static_cast<size_t>(row));
    PRISM_CHECK_EQ(backgrounds[static_cast<size_t>(row)]->rect.top, rect.top);
  }
  // The header is not a row, and neither is the margin outside the card.
  const prism::win::palette::Rect frame = shell.Frame(1200, 800);
  PRISM_CHECK_EQ(overlay.RowAt(1200, 800, frame.left + 20, frame.top + 10), -1);
  PRISM_CHECK_EQ(overlay.RowAt(1200, 800, 1, 1), -1);
  PRISM_CHECK_EQ(overlay.RowAt(1200, 800, frame.right - 1, frame.bottom - 1), -1);
}

PRISM_TEST("overlay: Up/Down move and wrap, and only the highlight is painted as such") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay = OpenedOverlay(shell);
  PRISM_CHECK_EQ(overlay.Highlight(), 0);

  overlay.OnKey(Key(prism::keys::kKeyDown));
  PRISM_CHECK_EQ(overlay.Highlight(), 1);
  overlay.OnKey(Key(prism::keys::kKeyUp));
  PRISM_CHECK_EQ(overlay.Highlight(), 0);
  overlay.OnKey(Key(prism::keys::kKeyUp));  // wraps to the last row
  PRISM_CHECK_EQ(overlay.Highlight(), overlay.RowCount() - 1);
  overlay.OnKey(Key(prism::keys::kKeyDown));  // and back to the first
  PRISM_CHECK_EQ(overlay.Highlight(), 0);

  overlay.SetHighlight(2);
  const std::vector<ov::DrawCommand> commands = overlay.Build(1200, 800);
  const std::vector<const ov::DrawCommand*> highlights =
      AllOf(commands, ov::DrawKind::RowHighlight);
  PRISM_CHECK_EQ(highlights.size(), size_t{1});
  PRISM_CHECK_EQ(highlights.front()->row, 2);
}

PRISM_TEST("overlay: Tab cycles All -> Commands -> Files, and filters real rows") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay = OpenedOverlay(shell);
  const int allRows = overlay.RowCount();
  PRISM_CHECK(allRows > 0);

  ov::PaletteOverlay::Outcome outcome = overlay.OnKey(Key(ov::PaletteOverlay::kKeyTab));
  PRISM_CHECK(outcome.consumed);
  PRISM_CHECK(outcome.scopeChanged);
  PRISM_CHECK_EQ(overlay.scope(), ov::Scope::Commands);
  // Every visible row really is a command (the model's ranked order, filtered).
  for (int row = 0; row < overlay.RowCount(); ++row) {
    const prism::palette::Entry* entry = overlay.EntryAt(row);
    PRISM_CHECK(entry != nullptr);
    if (entry != nullptr) PRISM_CHECK_EQ(entry->kind, prism::palette::Kind::Command);
  }
  PRISM_CHECK_EQ(overlay.RowCount(), static_cast<int>(prism::palette::DefaultCommands().size()));

  overlay.OnKey(Key(ov::PaletteOverlay::kKeyTab));
  PRISM_CHECK_EQ(overlay.scope(), ov::Scope::Files);
  for (int row = 0; row < overlay.RowCount(); ++row) {
    const prism::palette::Entry* entry = overlay.EntryAt(row);
    PRISM_CHECK(entry != nullptr);
    if (entry != nullptr) PRISM_CHECK_EQ(entry->kind, prism::palette::Kind::File);
  }

  overlay.OnKey(Key(ov::PaletteOverlay::kKeyTab));
  PRISM_CHECK_EQ(overlay.scope(), ov::Scope::All);
  PRISM_CHECK_EQ(overlay.RowCount(), allRows);

  // Shift+Tab goes backwards, wrapping the same way.
  prism::keys::Stroke shiftTab = Key(ov::PaletteOverlay::kKeyTab);
  shiftTab.shift = true;
  overlay.OnKey(shiftTab);
  PRISM_CHECK_EQ(overlay.scope(), ov::Scope::Files);

  // A scope with no match says so instead of drawing an empty card silently.
  shell.SetQuery("png");
  overlay.SetScope(ov::Scope::Commands);
  PRISM_CHECK_EQ(overlay.RowCount(), 0);
  const std::vector<ov::DrawCommand> commands = overlay.Build(1200, 800);
  const ov::DrawCommand* empty = FirstOf(commands, ov::DrawKind::EmptyText);
  PRISM_CHECK(empty != nullptr);
  PRISM_CHECK(Contains(empty->text, "No match in Commands"));
  PRISM_CHECK(AllOf(commands, ov::DrawKind::RowTitle).empty());
}

PRISM_TEST("overlay: Esc and Ctrl+K close both the overlay and the shell") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay = OpenedOverlay(shell);
  PRISM_CHECK(shell.IsOpen());

  ov::PaletteOverlay::Outcome outcome = overlay.OnKey(Key(prism::keys::kKeyEscape));
  PRISM_CHECK(outcome.consumed);
  PRISM_CHECK(outcome.closed);
  PRISM_CHECK(!overlay.IsOpen());
  PRISM_CHECK(!shell.IsOpen());  // one open state, not two
  PRISM_CHECK(overlay.Build(1200, 800).empty());

  overlay.Open();
  prism::keys::Stroke ctrlK;
  ctrlK.key = 'K';
  ctrlK.ctrl = true;
  outcome = overlay.OnKey(ctrlK);
  PRISM_CHECK(outcome.closed);
  PRISM_CHECK(!overlay.IsOpen());

  // Enter activates the highlighted row and does NOT close (the window decides).
  overlay.Open();
  outcome = overlay.OnKey(Key(prism::keys::kKeyReturn));
  PRISM_CHECK(outcome.activate);
  PRISM_CHECK(outcome.consumed);
  PRISM_CHECK(overlay.IsOpen());
  PRISM_CHECK(overlay.Highlighted() != nullptr);

  // A letter belongs to the shell's text field: not consumed, not swallowed.
  outcome = overlay.OnKey(Key('p'));
  PRISM_CHECK(!outcome.consumed);
}

PRISM_TEST("overlay: the footer hint only appears when the card has room for it") {
  prism::win::palette::PaletteShell shell;
  ov::PaletteOverlay overlay = OpenedOverlay(shell);
  // 12 entries (8 commands + 4 files) are taller than the card's 520 px cap, so
  // the visible rows stop short of the result count and the footer has room.
  const std::vector<ov::DrawCommand> commands = overlay.Build(1600, 1600);
  const ov::DrawCommand* hint = FirstOf(commands, ov::DrawKind::HintText);
  PRISM_CHECK(hint != nullptr);
  PRISM_CHECK(Contains(hint->text, "Tab: Commands"));
  PRISM_CHECK(Contains(hint->text, "Esc: close"));
}

// ===========================================================================
// 3. The real window (Windows only). Same rules, driven through real controls.
//
// PRISM_C31_CORE_ONLY is what makes the Linux-lane target honest on Windows too:
// the pure rules run in BOTH binaries, but only the win.c31_settings_ui target
// creates windows, so a headless session cannot turn the core lane into a SKIP.
// ===========================================================================
#if defined(_WIN32) && !defined(PRISM_C31_CORE_ONLY)

namespace {

std::string gSkipReason;  // non-empty => the whole run is a SKIP (never a pass)

// "Can this session create ANY window?" -- asked with a stock class so the
// answer is about the environment, never about our code (C18's rule: a broken
// dialog must never masquerade as a SKIP, and a SKIP must never pass, D1).
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

std::wstring TempRoot() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, buffer);
  std::wstring root(length > 0 ? std::wstring(buffer, length) : std::wstring(L".\\"));
  if (!root.empty() && root.back() == L'\\') root.pop_back();
  return root + L"\\prism-c31-ui-" + std::to_wstring(GetCurrentProcessId());
}

std::string ReadWholeFile(const std::wstring& path) {
  std::string bytes;
  DWORD error = 0;
  if (!prism::win::settingsfile::ReadAllBytes(path, bytes, &error)) return std::string();
  return bytes;
}

// A fresh folder per case: one test's settings file can never colour another's.
prism::win::settingsfile::Paths FreshPaths(const wchar_t* name) {
  const std::wstring root = TempRoot() + L"\\" + name;
  prism::win::settingsfile::EnsureDirectory(root);
  return prism::win::settingsfile::PathsUnder(root);
}

int CountTempFiles(const std::wstring& directory) {
  WIN32_FIND_DATAW data{};
  const std::wstring pattern = directory + L"\\*.tmp.*";
  HANDLE found = FindFirstFileW(pattern.c_str(), &data);
  if (found == INVALID_HANDLE_VALUE) return 0;
  int count = 0;
  do {
    ++count;
  } while (FindNextFileW(found, &data) != FALSE);
  FindClose(found);
  return count;
}

bool Open(su::SettingsDialog& dialog, const prism::win::settingsfile::Paths& paths) {
  if (!gSkipReason.empty()) return false;
  if (!WindowStationUsable()) {
    gSkipReason = "no interactive window station (CreateWindowExW on a stock class failed)";
    return false;
  }
  if (!dialog.Create(GetModuleHandleW(nullptr), L"PrismArchive settings test")) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    PRISM_CHECK(false);  // our bug, not the environment: this must be RED
    return false;
  }
  std::string error;
  dialog.SetPaths(paths);
  const bool loaded = dialog.LoadFrom(paths, &error);
  PRISM_CHECK(loaded);  // a temp folder we created cannot legitimately fail to load
  if (!loaded) std::printf("    [dialog] load failed: %s\n", error.c_str());
  return loaded;
}

// A real edit-control change: set the text, then send the notification the
// control itself would send.
void TypeInto(su::SettingsDialog& dialog, int id, const std::wstring& text) {
  const HWND edit = dialog.Control(id);
  SetWindowTextW(edit, text.c_str());
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(id, EN_CHANGE), reinterpret_cast<LPARAM>(edit));
}

// A genuine combo selection.
void SelectCombo(su::SettingsDialog& dialog, int id, int index) {
  const HWND combo = dialog.Control(id);
  SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
}

// The click a user would make. Dispatched rather than BM_CLICK so the case still
// reaches the handler when the button is disabled: the dialog must refuse in
// its own handler, not rely on the window manager (that is what makes the
// "illegal value never reaches the disk" rule testable at all).
void ClickButton(su::SettingsDialog& dialog, int id) {
  const HWND button = dialog.Control(id);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(button));
}

int CheckState(su::SettingsDialog& dialog, int id) {
  return static_cast<int>(SendMessageW(dialog.Control(id), BM_GETCHECK, 0, 0));
}

int ComboSel(su::SettingsDialog& dialog, int id) {
  return static_cast<int>(SendMessageW(dialog.Control(id), CB_GETCURSEL, 0, 0));
}

}  // namespace

PRISM_TEST("win: the dialog is built from code -- every control is a real HWND") {
  su::SettingsDialog dialog;
  if (!Open(dialog, FreshPaths(L"controls"))) return;

  WNDCLASSEXW info{};
  info.cbSize = sizeof(info);
  PRISM_CHECK(GetClassInfoExW(GetModuleHandleW(nullptr), su::SettingsDialog::kClassName, &info) !=
              FALSE);

  const int controls[] = {su::ids::kLanguage,        su::ids::kTheme,
                          su::ids::kExtractDir,      su::ids::kBrowse,
                          su::ids::kThumbnail,       su::ids::kUnit,
                          su::ids::kLogLevel,        su::ids::kMotw,
                          su::ids::kSummary,         su::ids::kSave,
                          su::ids::kCancel,          su::ids::kLanguageHint,
                          su::ids::kThemeHint,       su::ids::kExtractDirHint,
                          su::ids::kThumbnailHint,   su::ids::kLogLevelHint,
                          su::ids::kMotwHint};
  for (const int id : controls) {
    if (!PRISM_CHECK(dialog.Control(id) != nullptr)) {
      std::printf("    [dialog] missing control id %d\n", id);
      return;
    }
  }
  PRISM_CHECK(static_cast<int>(SendMessageW(dialog.Control(su::ids::kLanguage), CB_GETCOUNT, 0,
                                            0)) >= 3);
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(su::ids::kTheme), CB_GETCOUNT, 0, 0)),
                 3);  // day / night / high contrast
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(su::ids::kUnit), CB_GETCOUNT, 0, 0)),
                 2);  // MB / GB
  PRISM_CHECK_EQ(
      static_cast<int>(SendMessageW(dialog.Control(su::ids::kLogLevel), CB_GETCOUNT, 0, 0)), 5);
}

PRISM_TEST("win: a first run loads the defaults into the controls") {
  su::SettingsDialog dialog;
  if (!Open(dialog, FreshPaths(L"defaults"))) return;

  PRISM_CHECK_EQ(ComboSel(dialog, su::ids::kLanguage), 0);  // auto
  PRISM_CHECK_EQ(ComboSel(dialog, su::ids::kTheme), 0);     // day
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(su::ids::kThumbnail), std::string("512"));
  PRISM_CHECK_EQ(ComboSel(dialog, su::ids::kUnit), 0);      // MB
  PRISM_CHECK_EQ(ComboSel(dialog, su::ids::kLogLevel), 3);  // info
  PRISM_CHECK_EQ(CheckState(dialog, su::ids::kMotw), BST_CHECKED);
  PRISM_CHECK(dialog.ControlEnabled(su::ids::kSave));
  PRISM_CHECK(dialog.State().Valid());
  // The summary is the value the draft would persist -- shown before saving.
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(su::ids::kSummary), "512 MB"));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(su::ids::kExtractDirHint), std::string());
}

PRISM_TEST("win: an illegal folder is refused in place and the file is untouched") {
  const prism::win::settingsfile::Paths paths = FreshPaths(L"bad-dir");
  su::SettingsDialog dialog;
  if (!Open(dialog, paths)) return;
  const std::string before = ReadWholeFile(paths.file);
  PRISM_CHECK(!before.empty());

  TypeInto(dialog, su::ids::kExtractDir, L"relative\\folder");
  PRISM_CHECK(!dialog.ControlEnabled(su::ids::kSave));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(su::ids::kExtractDirHint), "absolute"));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(su::ids::kSummary), "extract-dir"));

  ClickButton(dialog, su::ids::kSave);
  // D2: the CONTENT is what proves "not written", not a file count.
  PRISM_CHECK(!dialog.saved());
  PRISM_CHECK_EQ(ReadWholeFile(paths.file), before);
  PRISM_CHECK(!dialog.ControlEnabled(su::ids::kSave));

  // Fixing the value clears the complaint and re-enables Save.
  TypeInto(dialog, su::ids::kExtractDir, L"C:\\out\\c31");
  PRISM_CHECK(dialog.ControlEnabled(su::ids::kSave));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(su::ids::kExtractDirHint), std::string());
}

PRISM_TEST("win: an illegal cache cap is refused in place") {
  const prism::win::settingsfile::Paths paths = FreshPaths(L"bad-thumb");
  // First pin a legal cap so the later "unchanged" assertion has something to
  // compare against (and so the mutation writes a DIFFERENT value, not the same
  // bytes twice).
  {
    su::SettingsDialog dialog;
    if (!Open(dialog, paths)) return;
    TypeInto(dialog, su::ids::kThumbnail, L"2048");
    ClickButton(dialog, su::ids::kSave);
    PRISM_CHECK(dialog.saved());
  }
  const std::string before = ReadWholeFile(paths.file);
  PRISM_CHECK(Contains(before, "2048"));

  su::SettingsDialog dialog;
  if (!Open(dialog, paths)) return;
  TypeInto(dialog, su::ids::kThumbnail, L"0");
  PRISM_CHECK(!dialog.ControlEnabled(su::ids::kSave));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(su::ids::kThumbnailHint), "32"));
  ClickButton(dialog, su::ids::kSave);
  PRISM_CHECK(!dialog.saved());
  PRISM_CHECK_EQ(ReadWholeFile(paths.file), before);

  // Out-of-range in GB is the same rule through the unit control.
  SelectCombo(dialog, su::ids::kUnit, 1);  // GB
  TypeInto(dialog, su::ids::kThumbnail, L"99");
  PRISM_CHECK(!dialog.ControlEnabled(su::ids::kSave));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(su::ids::kThumbnailHint), "16384"));
}

PRISM_TEST("win: the store probe refuses a language tag the schema does not allow") {
  const prism::win::settingsfile::Paths paths = FreshPaths(L"bad-language");
  su::SettingsDialog dialog;
  if (!Open(dialog, paths)) return;
  const std::string before = ReadWholeFile(paths.file);

  // A language asset the schema has never heard of (the realistic future case:
  // someone drops de-DE.json into Assets/Lang).
  dialog.State().SetLanguages({"auto", "de-DE"});
  dialog.RefreshLanguageChoices();
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(su::ids::kLanguage), CB_GETCOUNT, 0,
                                              0)),
                 2);
  SelectCombo(dialog, su::ids::kLanguage, 1);
  PRISM_CHECK_EQ(dialog.State().languageTag(), std::string("de-DE"));
  PRISM_CHECK(!dialog.ControlEnabled(su::ids::kSave));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(su::ids::kLanguageHint), "language.tag"));

  ClickButton(dialog, su::ids::kSave);
  PRISM_CHECK(!dialog.saved());
  PRISM_CHECK_EQ(ReadWholeFile(paths.file), before);
}

PRISM_TEST("win: a valid draft is saved atomically and reads back through the store") {
  const prism::win::settingsfile::Paths paths = FreshPaths(L"good-save");
  {
    su::SettingsDialog dialog;
    if (!Open(dialog, paths)) return;

    SelectCombo(dialog, su::ids::kTheme, 2);  // high contrast
    TypeInto(dialog, su::ids::kExtractDir, L"C:\\out\\c31");
    SelectCombo(dialog, su::ids::kUnit, 1);   // GB
    TypeInto(dialog, su::ids::kThumbnail, L"2");
    SelectCombo(dialog, su::ids::kLogLevel, 4);  // debug
    // BM_CLICK, not a synthetic WM_COMMAND: the handler reads BM_GETCHECK, so a
    // real click is what flips the state (a real user is the only client).
    SendMessageW(dialog.Control(su::ids::kMotw), BM_CLICK, 0, 0);
    PRISM_CHECK(!dialog.State().inheritMotw());
    PRISM_CHECK(Contains(dialog.ControlTextUtf8(su::ids::kSummary), "2 GB (2048 MB)"));
    PRISM_CHECK(dialog.ControlEnabled(su::ids::kSave));

    const HWND window = dialog.Hwnd();
    ClickButton(dialog, su::ids::kSave);
    PRISM_CHECK(dialog.saved());
    PRISM_CHECK_EQ(IsWindow(window), FALSE);  // an accepted save closes the dialog
    PRISM_CHECK(dialog.Hwnd() == nullptr);
    // Atomic write leaves no sibling temp file behind (SettingsFile deletes it
    // on every failure path and ReplaceFileW consumes it on success).
    PRISM_CHECK_EQ(CountTempFiles(paths.root), 0);
  }

  const std::string content = ReadWholeFile(paths.file);
  PRISM_CHECK(!content.empty());
  PRISM_CHECK(Contains(content, "\"highContrast\": true"));
  PRISM_CHECK(Contains(content, "\"thumbnailLimitMb\": 2048"));
  PRISM_CHECK(Contains(content, "\"level\": \"debug\""));

  prism::settings::SettingsStore store;
  const prism::settings::LoadReport report = store.Load(content);
  PRISM_CHECK_EQ(prism::settings::LoadStatusName(report.status), std::string("Ok"));
  PRISM_CHECK_EQ(store.GetString(su::kExtractDirKey), std::string("C:\\out\\c31"));
  PRISM_CHECK_EQ(store.GetBool(su::kHighContrastKey, false), true);
  PRISM_CHECK_EQ(store.GetInt(su::kThumbnailKey, 0), 2048);
  PRISM_CHECK_EQ(store.GetString(su::kLogLevelKey), std::string("debug"));
  PRISM_CHECK_EQ(store.GetBool(su::kMotwKey, true), false);

  su::DialogState reread;
  reread.Adopt(store);
  PRISM_CHECK(reread.theme() == su::Theme::HighContrast);
  PRISM_CHECK_EQ(reread.ThumbnailMb(), 2048);
  PRISM_CHECK_EQ(reread.logLevel(), su::LogLevel::Debug);
  PRISM_CHECK(!reread.inheritMotw());
  PRISM_CHECK(reread.Valid());
}

PRISM_TEST("win: Browse fills the folder control without saving anything") {
  su::SettingsDialog dialog;
  if (!Open(dialog, FreshPaths(L"browse"))) return;
  dialog.SetBrowseHandler([] { return std::wstring(L"C:\\Picked\\Dir"); });
  ClickButton(dialog, su::ids::kBrowse);
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(su::ids::kExtractDir), std::string("C:\\Picked\\Dir"));
  PRISM_CHECK(dialog.ControlEnabled(su::ids::kSave));
  PRISM_CHECK(!dialog.saved());  // Browse chooses, it never commits
}

PRISM_TEST("win: WM_CLOSE destroys the window (no hidden dialog, no WM_QUIT)") {
  su::SettingsDialog dialog;
  if (!Open(dialog, FreshPaths(L"close"))) return;
  const HWND window = dialog.Hwnd();
  PRISM_CHECK(window != nullptr);
  dialog.Dispatch(WM_CLOSE, 0, 0);
  PRISM_CHECK_EQ(IsWindow(window), FALSE);
  PRISM_CHECK(dialog.Hwnd() == nullptr);
}

#endif  // _WIN32 && !PRISM_C31_CORE_ONLY

int main(int argc, char** argv) {
  const int failures = prism::test::RunAll(argc, argv);

#if defined(PRISM_C31_MUTANT_ACCEPT_BAD_DIR) || defined(PRISM_C31_MUTANT_ACCEPT_BAD_THUMBNAIL) || \
    defined(PRISM_C31_MUTANT_SKIP_STORE_PROBE)
  // CONTRACTS D3 + D26: the marker is a CONJUNCTION -- the run went red AND the
  // deliberate defect actually fired. A mutation that silently stopped firing
  // prints nothing, so the mutant entry (which requires this marker) is RED.
  if (failures != 0 && su::DialogState::MutationFired()) {
    std::puts("SETTINGS-UI MUTANT CAUGHT");
  }
  std::printf("mutant build: failures=%d mutation hits=%d\n", failures,
              su::DialogState::MutationCount());
  // WILL_FAIL entries invert this: red here is green there, and a mutation that
  // stopped being caught (exit 0) turns THAT entry red.
  return failures != 0 ? 1 : 0;
#else
#if defined(_WIN32) && !defined(PRISM_C31_CORE_ONLY)
  if (!gSkipReason.empty()) {
    // CONTRACTS D1: a skip is PRINTED and it is never exit 0.
    std::printf("SKIP: %s\n", gSkipReason.c_str());
    return 77;  // SKIP_RETURN_CODE 77 (Tests/suites/c31-ui.cmake)
  }
#endif
  if (failures != 0) return 1;
  std::puts("ALL SETTINGS-UI CHECKS PASSED");
  return 0;
#endif
}
