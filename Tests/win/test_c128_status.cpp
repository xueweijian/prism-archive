// PrismArchive -- PrismWin
// C128: THE STATUS BAR'S TRUTH (proposal 5.5 / 5.6, measured against the report).
//
// THE DEFECT the screenshot shows: the band and the info card print the SAME
// three facts twice.
//     info card : ENCRYPTION 未加密 / METHOD 未知 / DICTIONARY -- / RATIO 98.8%
//     status bar: 未加密 / 方法未知 / 字典未知 / 条目 5 / 选中 0 / 压缩率 98.8%
// Six slots, three of which are the card's words spoken a second time; the two
// counts describe the CURRENT FOLDER while the breadcrumb carries the archive
// total ("共 17 项"), so two numbers about two different things sat on one
// screen. Bandizip's band answers a different question -- WHAT AM I LOOKING AT --
// and answers it with three numbers: 文件: N / 文件夹: M / 压缩包大小: X.
//
// This suite pins the new truth through the REAL window (the catalog the app
// attaches), not through a hand-built model:
//   S1  the band has EXACTLY three text slots, in order, and their strings match
//       文件: \d+ / 文件夹: \d+ / 压缩包大小: [\d.]+ (B|KB|MB|GB) -- the numbers
//       are the CURRENT FOLDER's counts and the whole archive's packed bytes;
//   S1b drilling into a subfolder changes the counts and keeps the archive size
//       (the scope of each number is named by the assertion, not by a comment);
//   S2  the info card is ONE 22 px line by default (pill height 2*10 + 22 = 42)
//       and four label/value rows + the ratio bar when expanded (157) -- read
//       off the SAME command list the renderer paints;
//   S2b a click on the card expands it and a second click folds it (the state
//       the user sees is the state the hit test toggles);
//   S3  the three duplicated words are GONE from the band: no "加密", no "方法",
//       no "字典" in any status slot (only the card carries them);
//   S4  the fold is PERSISTED: written to the settings document, re-parsed and
//       read back -- and a document from before the key opens COLLAPSED, which
//       is the shipped default.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C128_MUTANT_OLD_STATUS restores
// the pre-card six-slot band, so S1 and S3 MUST go red. The mutant entry decides
// with PASS_REGULAR_EXPRESSION on "C128 MUTATION CAUGHT: old_status" -- never
// WILL_FAIL -- and the verdict is only printed while the injection proves it is
// in this binary (ActiveStatusMutation()), so an unrelated red stays red.
//
// /WX- is the project's Windows-test convention (SDK headers are not /W4 clean).
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"           // WindowState: the persistence port
#include "../../Src/PrismCore/SettingsStore.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using prism::win::gallery::GalleryWindow;
namespace sidebar = prism::win::sidebar;

namespace {

constexpr int kSkipReturnCode = 77;
GalleryWindow* g_window = nullptr;
std::vector<prism::ArchiveItem> g_catalog;

void Add(const char* path, bool dir, std::uint64_t size, std::uint64_t packed) {
  prism::ArchiveItem item;
  item.index = static_cast<std::uint32_t>(g_catalog.size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  item.packSize = packed;
  g_catalog.push_back(item);
}

// The archive the report's numbers come from: the ROOT holds one folder and two
// files; "docs" holds one file. The whole archive packs to 600 bytes.
void BuildCatalog() {
  Add("docs", true, 0, 0);
  Add("docs/readme.md", false, 53, 100);
  Add("a.txt", false, 200, 250);
  Add("b.txt", false, 300, 250);
}

// The band's labels, in PAINT ORDER -- the one spelling (StatusSegmentLabel)
// the painter, the accessibility tree and this suite share.
std::vector<std::string> BandLabels(const prism::layout::Layout& layout,
                                    const prism::layout::StatusInfo& info) {
  std::vector<std::string> out;
  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(layout, info);
  for (const prism::layout::StatusSegment& segment : segments) {
    const std::string label = prism::layout::StatusSegmentLabel(segment);
    if (!label.empty()) out.push_back(label);
  }
  return out;
}

// "文件: 2" / "文件夹: 1": the literal prefix, a space, then ONE decimal number.
bool IsCountSlot(const std::string& text, const char* prefix, int* number) {
  const std::size_t length = std::strlen(prefix);
  if (text.size() <= length + 1) return false;
  if (text.compare(0, length, prefix) != 0) return false;
  if (text[length] != ' ') return false;
  long long value = 0;
  for (std::size_t i = length + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  if (number != nullptr) *number = static_cast<int>(value);
  return true;
}

// "压缩包大小: 2.4 KB": the prefix, a space, a number with at most one decimal
// and one of the four UI units. This is the shape S1 names; the exact bytes are
// asserted separately against the ONE formatter.
bool IsArchiveSizeSlot(const std::string& text, std::string* number, std::string* unit) {
  const char* prefix = "压缩包大小: ";
  const std::size_t length = std::strlen(prefix);
  if (text.size() <= length) return false;
  if (text.compare(0, length, prefix) != 0) return false;
  const std::string rest = text.substr(length);
  const std::size_t space = rest.find(' ');
  if (space == std::string::npos || space == 0 || space + 1 >= rest.size()) return false;
  const std::string digits = rest.substr(0, space);
  const std::string suffix = rest.substr(space + 1);
  int dots = 0;
  for (const char c : digits) {
    if (c == '.') { ++dots; continue; }
    if (c < '0' || c > '9') return false;
  }
  if (dots > 1) return false;
  if (suffix != "B" && suffix != "KB" && suffix != "MB" && suffix != "GB") return false;
  if (number != nullptr) *number = digits;
  if (unit != nullptr) *unit = suffix;
  return true;
}

struct CardSlots {
  int labels = 0;
  int values = 0;
  int bars = 0;
  int icons = 0;
};

CardSlots CountCard(const sidebar::Layout& layout) {
  CardSlots out;
  for (const sidebar::Command& command : layout.commands) {
    switch (command.kind) {
      case sidebar::PrimKind::StatusLabel: ++out.labels; break;
      case sidebar::PrimKind::StatusText: ++out.values; break;
      case sidebar::PrimKind::RatioBar: ++out.bars; break;
      case sidebar::PrimKind::StatusIcon: ++out.icons; break;
      default: break;
    }
  }
  return out;
}

// The card's single collapsed line (the only StatusText a folded card emits).
std::string CollapsedLine(const sidebar::Layout& layout) {
  for (const sidebar::Command& command : layout.commands) {
    if (command.kind == sidebar::PrimKind::StatusText) return command.text;
  }
  return {};
}

// The card the report is about: one file, 1000 -> 988 bytes, method ZIP, no
// encryption. Its RATIO is the canonical view string ("98.8%").
prism::win::sidebar::Layout BuildCard(const std::vector<prism::ArchiveItem>& catalog,
                                     bool expanded, bool* hasArchive) {
  static prism::sidebar::Model model;
  model = prism::sidebar::Model(&catalog);
  prism::sidebar::ArchiveInfo info;
  info.encryption = prism::sidebar::Encryption::None;
  info.method = "ZIP";
  model.SetArchiveInfo(info);
  if (hasArchive != nullptr) *hasArchive = model.ComputeStatus().hasArchive;
  sidebar::Metrics metrics;
  metrics.viewportTop = 0.0f;
  metrics.viewportBottom = 800.0f;
  metrics.infoCardExpanded = expanded;
  sidebar::Labels labels;
  // The Chinese words the report reads on screen (the table-less build keeps
  // English defaults, so the suite installs the two it asserts).
  labels.notEncrypted = "未加密";
  return sidebar::Build(model, metrics, labels);
}

}  // namespace

// ---------------------------------------------------------------------------
// S1. Three slots, in order, with the report's own numbers.
// ---------------------------------------------------------------------------
PRISM_TEST("c128 s1: the status band is three numbers, not six words") {
  GalleryWindow& window = *g_window;
  window.SetItems(&g_catalog);
  window.SetRootLabel(L"c128.7z");
  const prism::layout::Layout layout = window.LayoutForTest();
  if (!PRISM_CHECK(!layout.statusBar.Empty())) return;
  const prism::layout::StatusInfo info = window.StatusForPublish();
  std::printf("C128 band: fileCount=%d folderCount=%d archiveBytes=%lld\n", info.fileCount,
              info.folderCount, static_cast<long long>(info.archiveBytes));

  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(layout, info);
  PRISM_CHECK_EQ(segments.size(), static_cast<std::size_t>(3));
  if (segments.size() != 3) return;
  PRISM_CHECK_EQ(segments[0].id, prism::layout::StatusId::FileCount);
  PRISM_CHECK_EQ(segments[1].id, prism::layout::StatusId::FolderCount);
  PRISM_CHECK_EQ(segments[2].id, prism::layout::StatusId::ArchiveSize);

  const std::vector<std::string> labels = BandLabels(layout, info);
  PRISM_CHECK_EQ(labels.size(), static_cast<std::size_t>(3));
  if (labels.size() != 3) return;
  for (const std::string& label : labels) std::printf("  [band] %s\n", label.c_str());

  int files = -1;
  int folders = -1;
  PRISM_CHECK(IsCountSlot(labels[0], "文件:", &files));
  PRISM_CHECK(IsCountSlot(labels[1], "文件夹:", &folders));
  std::string sizeNumber;
  std::string sizeUnit;
  PRISM_CHECK(IsArchiveSizeSlot(labels[2], &sizeNumber, &sizeUnit));
  // The CURRENT FOLDER holds two files and one folder; the whole archive packs
  // to 600 bytes. Both facts are asserted, so "which folder" is not a question a
  // reader has to answer from a comment.
  PRISM_CHECK_EQ(files, 2);
  PRISM_CHECK_EQ(folders, 1);
  PRISM_CHECK_EQ(labels[2], std::string("压缩包大小: ") + prism::list::FormatBytes(600));

  // The slots must not collide, must stay inside the band, and must be laid out
  // left to right -- at every width the product supports (a narrow window may
  // not stack two numbers on one pixel of the band).
  for (const int width : {640, 800, 1024, 1280, 1920}) {
    const prism::layout::Layout narrow =
        prism::layout::Compute(width, 800, /*sidebarVisible=*/false, /*listMode=*/true);
    const std::vector<prism::layout::StatusSegment> sweep =
        prism::layout::StatusSegments(narrow, info);
    PRISM_CHECK_EQ(sweep.size(), static_cast<std::size_t>(3));
    if (sweep.size() != 3) continue;
    for (std::size_t i = 0; i < sweep.size(); ++i) {
      PRISM_CHECK(narrow.statusBar.Contains(sweep[i].rect));
      PRISM_CHECK(sweep[i].rect.Width() >= prism::layout::kStatusSegmentMinWidth);
      if (i == 0) continue;
      PRISM_CHECK(!sweep[i].rect.Intersects(sweep[i - 1].rect));
      PRISM_CHECK(sweep[i - 1].rect.right <= sweep[i].rect.left);
    }
  }
}

// ---------------------------------------------------------------------------
// S1b. The counts describe the folder the user is LOOKING AT; the size does not
// move when the folder does.
// ---------------------------------------------------------------------------
PRISM_TEST("c128 s1b: the counts are the current folder's, the size is the archive's") {
  GalleryWindow& window = *g_window;
  window.SetItems(&g_catalog);
  window.SetRootLabel(L"c128.7z");
  const std::int64_t archiveBytes = window.StatusForPublish().archiveBytes;
  PRISM_CHECK_EQ(archiveBytes, static_cast<std::int64_t>(600));

  // Drill into "docs" through the window's own hit test (the same rectangle the
  // renderer paints): a double-click on a folder row navigates.
  PRISM_CHECK_EQ(window.ItemCount(), 3);
  const prism::layout::Rect row = window.ItemRect(0);
  if (!PRISM_CHECK(!row.Empty())) return;
  window.DoubleClickAt((row.left + row.right) / 2, (row.top + row.bottom) / 2);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("docs"))) return;

  const prism::layout::StatusInfo inside = window.StatusForPublish();
  std::printf("C128 band in docs: fileCount=%d folderCount=%d archiveBytes=%lld\n",
              inside.fileCount, inside.folderCount, static_cast<long long>(inside.archiveBytes));
  PRISM_CHECK_EQ(inside.fileCount, 1);
  PRISM_CHECK_EQ(inside.folderCount, 0);
  // The archive did not change size because the user opened a folder.
  PRISM_CHECK_EQ(inside.archiveBytes, archiveBytes);
  const std::vector<std::string> labels = BandLabels(window.LayoutForTest(), inside);
  if (!PRISM_CHECK_EQ(labels.size(), static_cast<std::size_t>(3))) return;
  PRISM_CHECK_EQ(labels[0], std::string("文件: 1"));
  PRISM_CHECK_EQ(labels[1], std::string("文件夹: 0"));
  PRISM_CHECK_EQ(labels[2], std::string("压缩包大小: 600 B"));

  // Back to the root for the remaining cases.
  window.SetItems(&g_catalog);
}

// ---------------------------------------------------------------------------
// S3. The card's three words left the band (they are printed ONCE, on the card).
// ---------------------------------------------------------------------------
PRISM_TEST("c128 s3: encryption / method / dictionary are not in the status band") {
  GalleryWindow& window = *g_window;
  window.SetItems(&g_catalog);
  window.SetRootLabel(L"c128.7z");
  const prism::layout::Layout layout = window.LayoutForTest();
  const std::vector<std::string> labels = BandLabels(layout, window.StatusForPublish());
  PRISM_CHECK(!labels.empty());
  for (const std::string& label : labels) {
    std::printf("  [band] %s\n", label.c_str());
    PRISM_CHECK(label.find("加密") == std::string::npos);
    PRISM_CHECK(label.find("方法") == std::string::npos);
    PRISM_CHECK(label.find("字典") == std::string::npos);
    // ... and the pre-card vocabulary is gone with them: no 条目 / 选中 /
    // 压缩率 slot survives on the band (the breadcrumb owns 共 N 项, the card
    // owns 压缩率).
    PRISM_CHECK(label.find("条目") == std::string::npos);
    PRISM_CHECK(label.find("选中") == std::string::npos);
    PRISM_CHECK(label.find("压缩率") == std::string::npos);
  }
  // The whole band's text, joined: a second, independent read of the same fact.
  std::string joined;
  for (const std::string& label : labels) joined += label + " | ";
  PRISM_CHECK(joined.find("加密") == std::string::npos);
  PRISM_CHECK(joined.find("方法") == std::string::npos);
  PRISM_CHECK(joined.find("字典") == std::string::npos);
}

// ---------------------------------------------------------------------------
// S2. The card is ONE line by default and four label/value rows when expanded.
// ---------------------------------------------------------------------------
PRISM_TEST("c128 s2: the info card folds to one line and expands to four rows") {
  std::vector<prism::ArchiveItem> catalog;
  {
    prism::ArchiveItem item;
    item.index = 0;
    item.path = "one.txt";
    item.size = 1000;
    item.packSize = 988;
    catalog.push_back(item);
  }
  bool hasArchive = false;
  const sidebar::Layout collapsed = BuildCard(catalog, /*expanded=*/false, &hasArchive);
  PRISM_CHECK(hasArchive);
  const CardSlots folded = CountCard(collapsed);
  std::printf("C128 card folded: pillH=%.1f labels=%d values=%d bars=%d icons=%d\n",
              static_cast<double>(collapsed.pill.Height()), folded.labels, folded.values,
              folded.bars, folded.icons);
  // 2 * 10 px padding + ONE 22 px line.
  PRISM_CHECK_EQ(collapsed.pill.Height(), 42.0f);
  PRISM_CHECK_EQ(folded.labels, 0);
  PRISM_CHECK_EQ(folded.values, 1);
  PRISM_CHECK_EQ(folded.bars, 0);
  PRISM_CHECK_EQ(folded.icons, 1);
  // The line is the summary the report asks for: encryption · method · ratio.
  PRISM_CHECK_EQ(CollapsedLine(collapsed), std::string("未加密 · ZIP · 98.8%"));

  const sidebar::Layout expanded = BuildCard(catalog, /*expanded=*/true, &hasArchive);
  const CardSlots open = CountCard(expanded);
  std::printf("C128 card expanded: pillH=%.1f labels=%d values=%d bars=%d icons=%d\n",
              static_cast<double>(expanded.pill.Height()), open.labels, open.values, open.bars,
              open.icons);
  // 2 * 10 padding + 4 * (12 + 16) rows + 3 * 6 gaps + the bar's own 4 + 3 row.
  PRISM_CHECK_EQ(expanded.pill.Height(), 157.0f);
  PRISM_CHECK_EQ(open.labels, 4);
  PRISM_CHECK_EQ(open.values, 4);
  PRISM_CHECK_EQ(open.bars, 1);
  PRISM_CHECK_EQ(open.icons, 4);
  // C105's layout is untouched: every value sits on its own line UNDER its label.
  int stacked = 0;
  for (const sidebar::Command& label : expanded.commands) {
    if (label.kind != sidebar::PrimKind::StatusLabel) continue;
    for (const sidebar::Command& value : expanded.commands) {
      if (value.kind != sidebar::PrimKind::StatusText) continue;
      if (value.box.left != label.box.left) continue;
      if (value.box.top == label.box.bottom) ++stacked;
    }
  }
  PRISM_CHECK_EQ(stacked, 4);
  // The fold really frees the space above the card (the defect was 157 px of the
  // rail's bottom-left corner for a line of text).
  PRISM_CHECK(expanded.pill.top < collapsed.pill.top);
  PRISM_CHECK_EQ(expanded.pill.top, collapsed.pill.top - 115.0f);
}

// ---------------------------------------------------------------------------
// S2b. The window the user clicks: the pill toggles.
// ---------------------------------------------------------------------------
PRISM_TEST("c128 s2b: clicking the card expands it, clicking again folds it") {
  GalleryWindow& window = *g_window;
  window.SetItems(&g_catalog);
  window.SetRootLabel(L"c128.7z");
  window.SetInfoCardExpanded(false);
  const sidebar::Layout& folded = window.SidebarLayout();
  PRISM_CHECK_EQ(folded.pill.Height(), 42.0f);
  const int x = static_cast<int>((folded.pill.left + folded.pill.right) * 0.5f);
  const int y = static_cast<int>((folded.pill.top + folded.pill.bottom) * 0.5f);
  PRISM_CHECK(window.ClickAt(x, y));
  PRISM_CHECK(window.InfoCardExpanded());
  const sidebar::Layout& open = window.SidebarLayout();
  PRISM_CHECK_EQ(open.pill.Height(), 157.0f);
  PRISM_CHECK_EQ(CountCard(open).labels, 4);
  // The click landed ON the card, not on a row behind it.
  PRISM_CHECK_EQ(window.Selection(), -1);

  const int x2 = static_cast<int>((open.pill.left + open.pill.right) * 0.5f);
  const int y2 = static_cast<int>((open.pill.top + open.pill.bottom) * 0.5f);
  PRISM_CHECK(window.ClickAt(x2, y2));
  PRISM_CHECK(!window.InfoCardExpanded());
  PRISM_CHECK_EQ(window.SidebarLayout().pill.Height(), 42.0f);
}

// ---------------------------------------------------------------------------
// S4. The fold is a SETTING: it survives a restart, and the default is folded.
// ---------------------------------------------------------------------------
PRISM_TEST("c128 s4: the fold is persisted and defaults to folded") {
  // A store straight from the defaults: the shipped state.
  prism::settings::SettingsStore store;
  const prism::app::WindowState defaults;
  PRISM_CHECK(!defaults.infoCardExpanded);
  prism::app::SaveWindowState(store, defaults);
  PRISM_CHECK(store.Has("window.infoCardExpanded"));
  PRISM_CHECK(!prism::app::LoadWindowState(store).infoCardExpanded);

  // The user expands the card; the app writes the document.
  prism::app::WindowState expanded = defaults;
  expanded.infoCardExpanded = true;
  prism::app::SaveWindowState(store, expanded);
  const std::string document = store.ToJson();
  PRISM_CHECK(document.find("\"infoCardExpanded\"") != std::string::npos);

  // RESTART: a fresh store parses the bytes off disk and answers the same bit.
  prism::settings::SettingsStore restarted;
  const prism::settings::LoadReport report = restarted.Load(document);
  PRISM_CHECK(report.readable);
  PRISM_CHECK(restarted.GetBool("window.infoCardExpanded", false));
  PRISM_CHECK(prism::app::LoadWindowState(restarted).infoCardExpanded);

  // ... and the window the app hands that bit to is really expanded.
  GalleryWindow& window = *g_window;
  window.SetItems(&g_catalog);
  window.SetRootLabel(L"c128.7z");
  window.SetInfoCardExpanded(prism::app::LoadWindowState(restarted).infoCardExpanded);
  PRISM_CHECK(window.InfoCardExpanded());
  PRISM_CHECK_EQ(window.SidebarLayout().pill.Height(), 157.0f);
  // Folding is symmetric: the bit is a state, not a one-way switch.
  window.SetInfoCardExpanded(false);
  PRISM_CHECK(!window.InfoCardExpanded());
  PRISM_CHECK_EQ(window.SidebarLayout().pill.Height(), 42.0f);

  // A document from BEFORE the key (no window.infoCardExpanded) opens FOLDED.
  prism::settings::SettingsStore legacy;
  (void)legacy.Erase("window.infoCardExpanded");
  PRISM_CHECK(!legacy.Has("window.infoCardExpanded"));
  PRISM_CHECK(!prism::app::LoadWindowState(legacy).infoCardExpanded);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::printf("SKIP: CoInitializeEx failed (this window station has no COM)\n");
    return kSkipReturnCode;
  }
  BuildCatalog();
  static GalleryWindow window;
  if (!window.Create(L"PrismArchive c128 status", GetModuleHandleW(nullptr))) {
    std::printf("SKIP: window creation failed (no interactive window station)\n");
    return kSkipReturnCode;
  }
  g_window = &window;
#if defined(PRISM_C128_MUTANT_OLD_STATUS)
  std::printf("C128 MUTANT ACTIVE: old_status\n");
  prism::test::SetMutationVerdict("C128 MUTATION CAUGHT: old_status", [] {
    return prism::layout::ActiveStatusMutation() == std::string_view("old_status");
  });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::printf("ALL C128 STATUS CHECKS PASSED (%d checks)\n", prism::test::CheckCount());
  ::CoUninitialize();
  return 0;
}
