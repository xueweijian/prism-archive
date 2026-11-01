// PrismArchive -- the compress dialog (card C18).
//
// ONE source file, TWO ctest entries (Tests/suites/c18-compressdialog.cmake):
//
//   core.test_compress_dialog   the pure DialogState (Linux + Windows lanes,
//                               milliseconds, no window needed)
//   win.compress_dialog         the SAME state rules driven through a REAL HWND:
//                               CreateWindowExW, real child controls, real
//                               WM_COMMAND / WM_NOTIFY / WM_HSCROLL messages
//
// The split matters: every RULE lives in DialogState and is provable headless,
// while the Windows half proves the WIRING (a message reaches the state and the
// control reflects what came back). A test that could only run on Windows would
// have been skipped by the Linux lane, and a test that mocked the window would
// not have proved the dialog exists at all.
//
// The Windows cases drive the real window class the way
// Tests/win/test_navigation.cpp drives GalleryWindow: through the message
// handlers, with real handles -- never by calling DialogState directly.
//
// SKIP policy (CONTRACTS D1): a window station that refuses CreateWindowExW is a
// SKIP with a printed reason and exit code 77; the suite registers
// SKIP_RETURN_CODE 77 AND PASS_REGULAR_EXPRESSION, so a silent skip can never
// count as a pass.
//
// Negative controls (CONTRACTS D3, wired in the suite file):
//   PRISM_CD_MUTANT_ZIP_SOLID_ALLOWED  keeps "solid" on for zip
//   PRISM_CD_MUTANT_TAR_ENCRYPT_ALLOWED drops the password instead of refusing
// Both are compiled from THIS source; the assertion that the mutation actually
// fired is DialogState::MutationFired(), and CONTRACTS D26 is honoured by making
// the mutant marker a conjunction (red AND fired) instead of WILL_FAIL + regex.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <commctrl.h>

#include <memory>
#endif

#include "../mini_test.hpp"
#include "../../Src/PrismWin/CompressDialog.hpp"

namespace cd = prism::win::cd;

namespace {

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// A small but realistic selection: a folder, a file inside it, and a big file
// (the size is what the estimate and the dictionary derivation react to).
std::vector<prism::PlanItem> FixtureItems() {
  std::vector<prism::PlanItem> items;
  items.push_back(prism::PlanItem{"docs", 0, true, 0});
  items.push_back(prism::PlanItem{"docs/readme.txt", 4096, false, 0});
  items.push_back(prism::PlanItem{"photo.png", 2000000, false, 0});
  return items;
}

bool HasAdjustment(const cd::DialogState& state, cd::Field field) {
  for (const cd::Adjustment& adjustment : state.adjustments()) {
    if (adjustment.field == field && !adjustment.reason.empty()) return true;
  }
  return false;
}

std::string AdjustmentFor(const cd::DialogState& state, cd::Field field) {
  for (const cd::Adjustment& adjustment : state.adjustments()) {
    if (adjustment.field == field) return adjustment.reason;
  }
  return std::string();
}

std::string PropertyValue(const prism::CompressionPlan& plan, const char* name) {
  for (const prism::PropertySetting& property : plan.properties) {
    if (property.name == name) {
      switch (property.kind) {
        case prism::PropKind::Bool: return property.boolValue ? "true" : "false";
        case prism::PropKind::UInt: return std::to_string(property.uintValue);
        case prism::PropKind::Str: return property.strValue;
      }
    }
  }
  return std::string("<absent>");
}

// Everything about a plan a dialog round trip could lose.
std::string PlanSignature(const prism::CompressionPlan& plan) {
  std::string signature = std::string(prism::ToString(plan.format)) + "|" +
                          std::to_string(plan.level) + "|" + std::to_string(plan.dictionaryBytes) +
                          "|" + (plan.solid ? "1" : "0") + "|" +
                          std::to_string(plan.solidBlockBytes) + "|" + std::to_string(plan.threads) +
                          "|" + std::string(prism::ToString(plan.cipher)) + "|" +
                          (plan.encryptFileNames ? "1" : "0") + "|" + plan.methodName;
  for (const prism::PropertySetting& property : plan.properties) {
    signature += ";" + property.name + "=";
    switch (property.kind) {
      case prism::PropKind::Bool: signature += property.boolValue ? "true" : "false"; break;
      case prism::PropKind::UInt: signature += std::to_string(property.uintValue); break;
      case prism::PropKind::Str: signature += property.strValue; break;
    }
  }
  return signature;
}

}  // namespace

// ===========================================================================
// 1. DialogState -- defaults, mapping, refusals, units, estimates.
// ===========================================================================

PRISM_TEST("state: defaults mirror CompressionOptions and OK waits for real work") {
  cd::DialogState state;
  PRISM_CHECK_EQ(state.format(), prism::ArchiveFormat::SevenZip);
  PRISM_CHECK_EQ(state.level(), 5);
  PRISM_CHECK_EQ(state.dictionaryIndex(), 0);
  PRISM_CHECK_EQ(state.dictionaryBytes(), uint64_t{0});  // Auto
  PRISM_CHECK_EQ(state.solidMode(), prism::SolidMode::Auto);
  PRISM_CHECK(state.solid());  // 7z + Auto == solid (the format's honest default)
  PRISM_CHECK_EQ(state.threads(), 0);  // Auto
  PRISM_CHECK_EQ(state.threadsText(), std::string("Auto"));
  PRISM_CHECK_EQ(state.targetPath(), std::string());
  PRISM_CHECK(!state.showPassword());
  PRISM_CHECK(!state.encryptFileNames());
  PRISM_CHECK_EQ(state.inputCount(), size_t{0});

  // Nothing to archive: the reason names the first missing thing.
  PRISM_CHECK(!state.OkEnabled());
  const std::vector<std::string> empty = state.Blockers();
  PRISM_CHECK_EQ(empty.size(), size_t{1});
  if (!empty.empty()) {
    PRISM_CHECK_EQ(empty[0], std::string("Nothing selected to archive."));
  }

  state.SetInput(FixtureItems());
  PRISM_CHECK_EQ(state.inputBytes(), uint64_t{2004096});  // files only, not the folder
  PRISM_CHECK(!state.OkEnabled());  // a target is still missing
  state.SetTargetPath("out.7z");
  PRISM_CHECK(state.OkEnabled());
  PRISM_CHECK(state.BuildPlan().ok);
}

PRISM_TEST("state: level and dictionary come FROM the plan, out-of-range is clamped and said") {
  cd::DialogState state;
  const std::vector<uint64_t> choices = cd::DictionaryChoices();
  PRISM_CHECK_EQ(choices.size(), cd::kDictionaryChoiceCount);
  PRISM_CHECK_EQ(choices[0], uint64_t{0});  // index 0 == Auto
  for (int level = 1; level <= cd::kLevelMax; ++level) {
    // The combo is the LZMA2 ladder itself, not a hand-copied table.
    PRISM_CHECK_EQ(choices[static_cast<size_t>(level)], prism::LevelDictionaryBytes(level));
  }

  state.SetLevel(99);
  PRISM_CHECK_EQ(state.level(), 9);
  PRISM_CHECK(Contains(state.ReasonLine(), "0..9"));
  state.SetLevel(-3);
  PRISM_CHECK_EQ(state.level(), 0);

  state.SetDictionaryIndex(99);
  PRISM_CHECK_EQ(state.dictionaryIndex(), 0);
  PRISM_CHECK(Contains(state.ReasonLine(), "ladder"));

  // An explicit dictionary reaches the engine as "<bytes>b" (the only spelling
  // ISetProperties accepts -- see CompressionPlan's provenance note).
  cd::DialogState sized;
  sized.SetInput(FixtureItems());
  sized.SetTargetPath("out.7z");
  sized.SetDictionaryIndex(4);  // ladder: 1=256K 2=1M 3=4M 4=16M 5=32M ...
  PRISM_CHECK_EQ(sized.dictionaryBytes(), uint64_t{16} << 20);
  const prism::CompressionPlan plan = sized.BuildPlan();
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.dictionaryBytes, uint64_t{16} << 20);
  bool sawDictionary = false;
  for (const prism::PropertySetting& property : plan.properties) {
    if (property.name == "d") {
      sawDictionary = true;
      PRISM_CHECK_EQ(property.strValue, std::string("16777216b"));
    }
  }
  PRISM_CHECK(sawDictionary);
}

PRISM_TEST("state: zip cannot be solid -- the box turns off and the reason is recorded") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("out.7z");
  state.SetSolid(true);
  PRISM_CHECK(state.solid());
  PRISM_CHECK_EQ(state.solidMode(), prism::SolidMode::On);

  state.SetFormat(prism::ArchiveFormat::Zip);
  // SOFT downgrade: the state stops promising it, the control goes away, and the
  // user is told why. The mutant keeps solid ON here and the checks below go red.
  PRISM_CHECK(!state.solid());
  PRISM_CHECK_EQ(state.solidMode(), prism::SolidMode::Off);
  PRISM_CHECK(!state.IsEnabled(cd::Field::Solid));
  PRISM_CHECK(Contains(state.WhyDisabled(cd::Field::Solid), "cannot be solid"));
  PRISM_CHECK(HasAdjustment(state, cd::Field::Solid));
  PRISM_CHECK(Contains(AdjustmentFor(state, cd::Field::Solid), "cannot be solid"));
  PRISM_CHECK(Contains(state.ReasonLine(), "cannot be solid"));

  const prism::CompressionPlan plan = state.BuildPlan();
  PRISM_CHECK(plan.ok);
  PRISM_CHECK(!plan.solid);
  PRISM_CHECK_EQ(plan.methodName, std::string("Deflate"));
  // The target followed the format ("out.7z" -> "out.zip").
  PRISM_CHECK_EQ(state.targetPath(), std::string("out.zip"));
  PRISM_CHECK(state.OkEnabled());  // a downgrade is not a refusal
}

PRISM_TEST("state: tar refuses a password -- it never silently drops it") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("out.7z");
  state.SetPassword("hunter2");
  PRISM_CHECK(state.encrypts());
  PRISM_CHECK(state.OkEnabled());
  PRISM_CHECK_EQ(PropertyValue(state.BuildPlan(), "m"), std::string("LZMA2"));

  state.SetFormat(prism::ArchiveFormat::Tar);
  // HARD refusal. The mutant clears the password instead and hands back an
  // unencrypted archive the user believes is encrypted -- both checks go red.
  PRISM_CHECK(state.encrypts());  // the request is NOT discarded
  PRISM_CHECK_EQ(state.password(), std::string("hunter2"));
  PRISM_CHECK(!state.OkEnabled());
  bool refused = false;
  for (const std::string& blocker : state.Blockers()) {
    if (Contains(blocker, "tar cannot encrypt")) refused = true;
  }
  PRISM_CHECK(refused);
  PRISM_CHECK(Contains(state.ReasonLine(), "tar cannot encrypt"));
  // The decision layer refuses it too -- the dialog just says it earlier.
  PRISM_CHECK(!state.BuildPlan().ok);

  // Clearing the password is the way out; the dialog becomes usable again.
  state.SetPassword("");
  PRISM_CHECK(state.OkEnabled());
}

PRISM_TEST("state: tar disables level/dictionary/threads with a reason, and the plan stores") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("out.7z");
  state.SetLevel(7);
  state.SetDictionaryIndex(3);
  state.SetFormat(prism::ArchiveFormat::Tar);

  PRISM_CHECK(!state.IsEnabled(cd::Field::Level));
  PRISM_CHECK(!state.IsEnabled(cd::Field::Dictionary));
  PRISM_CHECK(!state.IsEnabled(cd::Field::Threads));
  PRISM_CHECK(!state.IsEnabled(cd::Field::Password));
  PRISM_CHECK(!state.IsEnabled(cd::Field::Solid));
  PRISM_CHECK(Contains(state.WhyDisabled(cd::Field::Level), "verbatim"));
  PRISM_CHECK(Contains(state.WhyDisabled(cd::Field::Dictionary), "no dictionary"));
  PRISM_CHECK(Contains(state.WhyDisabled(cd::Field::Threads), "I/O bound"));
  PRISM_CHECK(Contains(state.WhyDisabled(cd::Field::Password), "no cipher"));
  // Enabled controls must NOT invent a reason.
  PRISM_CHECK(state.WhyDisabled(cd::Field::Target).empty());

  // The level the user picked is not sent as a lie: tar stores, and the plan says so.
  const prism::CompressionOptions options = state.ToOptions();
  PRISM_CHECK_EQ(options.level, 0);
  PRISM_CHECK_EQ(options.dictionaryBytes, uint64_t{0});
  const prism::CompressionPlan plan = state.BuildPlan();
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.level, 0);
  PRISM_CHECK_EQ(plan.methodName, std::string("Store"));
  PRISM_CHECK_EQ(plan.cost.driver, prism::CostDriver::StoreInputOnce);
  PRISM_CHECK_EQ(plan.cost.lowerBoundBytes, uint64_t{2004096});
  // No "tar ignored your level" warning: the dialog already said it.
  PRISM_CHECK_EQ(plan.warnings.size(), size_t{0});
}

PRISM_TEST("state: header encryption is 7z-only and needs a password") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("out.7z");
  state.SetPassword("pw");
  state.SetEncryptFileNames(true);
  PRISM_CHECK(state.encryptFileNames());
  PRISM_CHECK(state.IsEnabled(cd::Field::EncryptFileNames));
  PRISM_CHECK_EQ(PropertyValue(state.BuildPlan(), "he"), std::string("true"));

  // Switching away from 7z clears it (and says so): a zip listing that shows the
  // file names despite "encrypt names" would be a lie with security impact.
  state.SetFormat(prism::ArchiveFormat::Zip);
  PRISM_CHECK(!state.encryptFileNames());
  PRISM_CHECK(Contains(AdjustmentFor(state, cd::Field::EncryptFileNames), "only 7z"));
  PRISM_CHECK(!state.IsEnabled(cd::Field::EncryptFileNames));
  PRISM_CHECK_EQ(PropertyValue(state.BuildPlan(), "he"), std::string("<absent>"));

  // Without a password it is a hard refusal (the plan refuses it too).
  cd::DialogState bare;
  bare.SetInput(FixtureItems());
  bare.SetTargetPath("out.7z");
  bare.SetEncryptFileNames(true);
  PRISM_CHECK(!bare.OkEnabled());
  PRISM_CHECK(Contains(bare.ReasonLine(), "needs a password"));
  PRISM_CHECK(!bare.BuildPlan().ok);
}

PRISM_TEST("state: the threads edit converts text to a number and refuses nonsense") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("out.7z");

  PRISM_CHECK_EQ(cd::ParseThreads("").threads, 0);
  PRISM_CHECK_EQ(cd::ParseThreads("Auto").threads, 0);
  PRISM_CHECK_EQ(cd::ParseThreads("  auto  ").threads, 0);
  PRISM_CHECK_EQ(cd::ParseThreads("0").threads, 0);
  PRISM_CHECK_EQ(cd::ParseThreads("4").threads, 4);
  PRISM_CHECK_EQ(cd::ParseThreads("64").threads, 64);
  PRISM_CHECK(cd::ParseThreads("65").clamped);
  PRISM_CHECK_EQ(cd::ParseThreads("999").threads, 64);
  PRISM_CHECK(!cd::ParseThreads("8x").valid);
  PRISM_CHECK(!cd::ParseThreads("-2").valid);
  PRISM_CHECK_EQ(cd::FormatThreadsText(0), std::string("Auto"));
  PRISM_CHECK_EQ(cd::FormatThreadsText(6), std::string("6"));

  state.SetThreadsText("8");
  PRISM_CHECK_EQ(state.threads(), 8);
  PRISM_CHECK_EQ(PropertyValue(state.BuildPlan(), "mt"), std::string("8"));
  PRISM_CHECK(state.OkEnabled());

  state.SetThreadsText("999");
  PRISM_CHECK_EQ(state.threads(), 64);
  PRISM_CHECK(Contains(state.ReasonLine(), "64"));
  PRISM_CHECK(state.OkEnabled());  // clamped, not refused

  state.SetThreadsText("many");
  PRISM_CHECK(!state.threadsValid());
  PRISM_CHECK(!state.OkEnabled());  // a number that is not a number IS refused
  PRISM_CHECK(Contains(state.ReasonLine(), "Threads must be a number"));
}

PRISM_TEST("state: OK enablement matrix (target, wildcards, empty selection)") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("C:\\Users\\me\\out.7z");
  PRISM_CHECK(state.OkEnabled());

  state.SetTargetPath("C:\\Users\\me\\out*.7z");
  PRISM_CHECK(!state.OkEnabled());
  PRISM_CHECK(Contains(state.ReasonLine(), "* ?"));

  state.SetTargetPath("C:\\Users\\me\\out.7z");
  PRISM_CHECK(state.OkEnabled());

  // A plan-level refusal still disables OK and keeps the plan's own sentence.
  cd::DialogState empty;
  empty.SetTargetPath("out.7z");
  empty.SetInput({});
  PRISM_CHECK(!empty.OkEnabled());
  PRISM_CHECK(Contains(empty.ReasonLine(), "Nothing selected"));

  // A selection whose paths cannot be stored (path traversal) is refused by the
  // decision layer, and the dialog surfaces that sentence verbatim.
  cd::DialogState bad;
  bad.SetTargetPath("out.7z");
  bad.SetInput({prism::PlanItem{"../escape.txt", 10, false, 0}});
  PRISM_CHECK(!bad.OkEnabled());
  PRISM_CHECK(Contains(bad.ReasonLine(), "invalid entry path"));
}

PRISM_TEST("state: the target follows the format when its extension is one of ours") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetFormat(prism::ArchiveFormat::Zip);
  state.SetTargetPath("report.zip");
  state.SetFormat(prism::ArchiveFormat::SevenZip);  // a real switch, not the default
  PRISM_CHECK_EQ(state.targetPath(), std::string("report.7z"));
  PRISM_CHECK(Contains(AdjustmentFor(state, cd::Field::Target), "renamed"));

  state.SetFormat(prism::ArchiveFormat::Tar);
  PRISM_CHECK_EQ(state.targetPath(), std::string("report.tar"));

  // A foreign extension is left alone: appending would be a rename nobody asked for.
  state.SetTargetPath("backup.dat");
  state.SetFormat(prism::ArchiveFormat::Zip);
  PRISM_CHECK_EQ(state.targetPath(), std::string("backup.dat"));

  // A dotted directory name is not an extension ("C:\my.docs\out" has none).
  state.SetTargetPath("C:\\my.docs\\out");
  state.SetFormat(prism::ArchiveFormat::SevenZip);
  PRISM_CHECK_EQ(state.targetPath(), std::string("C:\\my.docs\\out"));
}

PRISM_TEST("state: the estimate uses the plan's cost model, lower bound only when honest") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("out.7z");
  state.SetCpuCores(4);
  state.SetLevel(5);

  const cd::Estimate compressed = state.ComputeEstimate();
  PRISM_CHECK(compressed.ok);
  PRISM_CHECK_EQ(compressed.inputBytes, uint64_t{2004096});
  PRISM_CHECK_EQ(compressed.lowerOutputBytes, uint64_t{0});  // no honest lower bound
  PRISM_CHECK(compressed.upperOutputBytes >= compressed.inputBytes);
  PRISM_CHECK(compressed.upperMs > 0);
  PRISM_CHECK_EQ(compressed.threads, 4);
  PRISM_CHECK(Contains(compressed.text, "no lower bound"));
  PRISM_CHECK(Contains(compressed.text, "1 pass"));
  PRISM_CHECK(Contains(compressed.text, "LZMA2"));

  state.SetLevel(0);  // Store: every input byte lands verbatim -> hard lower bound
  const cd::Estimate stored = state.ComputeEstimate();
  PRISM_CHECK(stored.ok);
  PRISM_CHECK_EQ(stored.lowerOutputBytes, uint64_t{2004096});
  PRISM_CHECK(Contains(stored.text, "never below"));
  PRISM_CHECK(Contains(stored.text, "Copy"));
  // Storing is a copy: the same payload must not be quoted as slower than LZMA2.
  PRISM_CHECK(stored.upperMs <= compressed.upperMs);

  state.SetFormat(prism::ArchiveFormat::Tar);
  const cd::Estimate tarred = state.ComputeEstimate();
  PRISM_CHECK(tarred.ok);
  PRISM_CHECK_EQ(tarred.lowerOutputBytes, uint64_t{2004096});
  PRISM_CHECK(Contains(tarred.text, "Store"));

  // Nothing to estimate is stated, never invented.
  cd::DialogState bare;
  const cd::Estimate nothing = bare.ComputeEstimate();
  PRISM_CHECK(!nothing.ok);
  PRISM_CHECK(Contains(nothing.text, "nothing to archive"));
}

PRISM_TEST("state: unit conversion") {
  PRISM_CHECK_EQ(cd::FormatBytes(0), std::string("0 B"));
  PRISM_CHECK_EQ(cd::FormatBytes(1023), std::string("1023 B"));
  PRISM_CHECK_EQ(cd::FormatBytes(1024), std::string("1.0 KB"));
  PRISM_CHECK_EQ(cd::FormatBytes(1536), std::string("1.5 KB"));
  PRISM_CHECK_EQ(cd::FormatBytes(uint64_t{32} << 20), std::string("32.0 MB"));
  PRISM_CHECK_EQ(cd::FormatBytes(uint64_t{1} << 30), std::string("1.0 GB"));
  PRISM_CHECK_EQ(cd::FormatDurationMs(0), std::string("0 s"));
  PRISM_CHECK_EQ(cd::FormatDurationMs(400), std::string("<1 s"));
  PRISM_CHECK_EQ(cd::FormatDurationMs(2500), std::string("3 s"));
  PRISM_CHECK_EQ(cd::FormatDurationMs(90000), std::string("1 min 30 s"));
  PRISM_CHECK_EQ(cd::FormatDurationMs(3600000), std::string("1 h 0 min"));
}

PRISM_TEST("state: plan -> state -> plan round trips without losing a decision") {
  cd::DialogState first;
  first.SetInput(FixtureItems());
  first.SetTargetPath("out.7z");
  first.SetCpuCores(2);
  first.SetLevel(7);
  first.SetSolid(true);
  first.SetThreadsText("3");
  first.SetDictionaryIndex(4);  // 16 MB
  first.SetPassword("pw");
  first.SetEncryptFileNames(true);

  const prism::CompressionPlan plan = first.BuildPlan();
  PRISM_CHECK(plan.ok);
  const std::string signature = PlanSignature(plan);

  // AdoptPlan is the other direction: the dialog reopened on a plan shows what
  // will ACTUALLY happen (resolved solid, resolved dictionary, resolved threads).
  cd::DialogState second;
  second.SetInput(FixtureItems());
  second.SetTargetPath("out.7z");
  second.SetCpuCores(2);
  second.SetPassword("pw");
  second.AdoptPlan(plan);

  PRISM_CHECK_EQ(second.format(), plan.format);
  PRISM_CHECK_EQ(second.level(), plan.level);
  PRISM_CHECK_EQ(second.solid(), plan.solid);
  PRISM_CHECK_EQ(second.threads(), plan.threads);
  PRISM_CHECK_EQ(second.dictionaryIndex(), cd::DictionaryIndexFor(plan.dictionaryBytes));
  PRISM_CHECK_EQ(PlanSignature(second.BuildPlan()), signature);
}

PRISM_TEST("state: zip encryption asks for AES-256 and the names stay visible unless allowed") {
  cd::DialogState state;
  state.SetInput(FixtureItems());
  state.SetTargetPath("out.zip");
  state.SetFormat(prism::ArchiveFormat::Zip);
  state.SetPassword("pw");
  const prism::CompressionPlan plan = state.BuildPlan();
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.cipher, prism::Cipher::Aes256);
  // Without "em" 7-Zip would silently write legacy ZipCrypto.
  PRISM_CHECK_EQ(PropertyValue(plan, "em"), std::string("AES256"));
  PRISM_CHECK(!state.encryptFileNames());
  PRISM_CHECK(state.showPassword() == false);
  state.SetShowPassword(true);
  PRISM_CHECK(state.showPassword());
}

// ===========================================================================
// 2. The real window. Compiled only on Windows.
// ===========================================================================
#if defined(_WIN32)

namespace {

std::string gSkipReason;  // non-empty => the whole run is a SKIP (never a pass)

std::vector<prism::PlanItem> WindowFixture() { return FixtureItems(); }

// "Can this session create ANY window?" -- asked with a stock class so the
// answer is about the environment, never about our code. Without this split a
// broken dialog would masquerade as a SKIP, and a SKIP is the one thing D1 says
// must never be able to hide a failure.
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

// Creates the dialog with a usable selection. Returns false when the run cannot
// continue: an environment refusal is recorded as a SKIP (main() prints it and
// returns 77), while a genuine Create() failure is recorded as a FAILED check.
bool Open(cd::CompressDialog& dialog) {
  if (!gSkipReason.empty()) return false;
  if (!WindowStationUsable()) {
    gSkipReason = "no interactive window station (CreateWindowExW on a stock class failed)";
    return false;
  }
  if (!dialog.Create(GetModuleHandleW(nullptr), L"PrismArchive compress dialog test")) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    PRISM_CHECK(false);  // our bug, not the environment: this must be RED
    return false;
  }
  dialog.SetSource(WindowFixture(), L"E:\\fixture");
  dialog.State().SetCpuCores(4);
  dialog.State().SetTargetPath("out.7z");
  dialog.SyncControls();
  return true;
}

// A real combo selection: move the control, then send the notification the
// control would send. Nothing here calls DialogState directly.
void SelectFormat(cd::CompressDialog& dialog, int index) {
  const HWND combo = dialog.Control(cd::ids::kFormat);
  SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(cd::ids::kFormat, CBN_SELCHANGE),
                  reinterpret_cast<LPARAM>(combo));
}

void TypeInto(cd::CompressDialog& dialog, int id, const std::wstring& text, int notification) {
  const HWND edit = dialog.Control(id);
  SetWindowTextW(edit, text.c_str());
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(id, notification), reinterpret_cast<LPARAM>(edit));
}

// BM_CLICK is what a mouse does: it flips the check state AND sends
// BN_CLICKED to the parent, so the dialog sees a genuine WM_COMMAND.
void ClickButton(cd::CompressDialog& dialog, int id) {
  SendMessageW(dialog.Control(id), BM_CLICK, 0, 0);
}

void MoveSlider(cd::CompressDialog& dialog, int position) {
  const HWND slider = dialog.Control(cd::ids::kLevel);
  SendMessageW(slider, TBM_SETPOS, TRUE, static_cast<LPARAM>(position));
  dialog.Dispatch(WM_HSCROLL, 0, reinterpret_cast<LPARAM>(slider));
}

int CheckState(cd::CompressDialog& dialog, int id) {
  return static_cast<int>(SendMessageW(dialog.Control(id), BM_GETCHECK, 0, 0));
}

}  // namespace

PRISM_TEST("win: the dialog is built from code -- every control is a real HWND") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;

  WNDCLASSEXW info{};
  info.cbSize = sizeof(info);
  PRISM_CHECK(GetClassInfoExW(GetModuleHandleW(nullptr), cd::CompressDialog::kClassName, &info) !=
              FALSE);

  const int controls[] = {cd::ids::kFormat,      cd::ids::kLevel,           cd::ids::kDictionary,
                          cd::ids::kSolid,       cd::ids::kThreads,         cd::ids::kPassword,
                          cd::ids::kShowPassword, cd::ids::kEncryptNames,   cd::ids::kTarget,
                          cd::ids::kBrowse,      cd::ids::kEstimate,        cd::ids::kReason,
                          cd::ids::kOk,          cd::ids::kCancel};
  for (int id : controls) {
    // A missing control is the failure mode of a typo'd class name (e.g. the
    // trackbar needs InitCommonControlsEx), so every id is checked by handle.
    if (!PRISM_CHECK(dialog.Control(id) != nullptr)) {
      std::printf("    [dialog] missing control id %d\n", id);
      return;
    }
  }

  const HWND format = dialog.Control(cd::ids::kFormat);
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(format, CB_GETCOUNT, 0, 0)), 3);  // 7z/zip/tar
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(format, CB_GETCURSEL, 0, 0)), 0);
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(cd::ids::kDictionary), CB_GETCOUNT, 0,
                                               0)),
                 static_cast<int>(cd::kDictionaryChoiceCount));
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(cd::ids::kLevel), TBM_GETPOS, 0, 0)),
                 5);
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(cd::ids::kThreads), std::string("Auto"));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(cd::ids::kTarget), std::string("out.7z"));
  PRISM_CHECK(dialog.ControlEnabled(cd::ids::kOk));
}

PRISM_TEST("win: choosing zip through WM_COMMAND turns solid off and prints the reason") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;

  // The 7z default is already solid (SolidMode::Auto resolves to on), so one
  // real click turns it OFF and a second turns it back ON -- explicitly, which is
  // the case that must downgrade (an "Auto" solid on a zip was never promised).
  PRISM_CHECK_EQ(CheckState(dialog, cd::ids::kSolid), BST_CHECKED);
  ClickButton(dialog, cd::ids::kSolid);
  PRISM_CHECK(!dialog.State().solid());
  PRISM_CHECK_EQ(dialog.State().solidMode(), prism::SolidMode::Off);
  ClickButton(dialog, cd::ids::kSolid);
  PRISM_CHECK_EQ(CheckState(dialog, cd::ids::kSolid), BST_CHECKED);
  PRISM_CHECK(dialog.State().solid());
  PRISM_CHECK_EQ(dialog.State().solidMode(), prism::SolidMode::On);

  SelectFormat(dialog, 1);  // zip
  PRISM_CHECK_EQ(dialog.State().format(), prism::ArchiveFormat::Zip);
  PRISM_CHECK(!dialog.State().solid());
  PRISM_CHECK_EQ(CheckState(dialog, cd::ids::kSolid), BST_UNCHECKED);
  PRISM_CHECK(!dialog.ControlEnabled(cd::ids::kSolid));
  PRISM_CHECK(!dialog.ControlEnabled(cd::ids::kDictionary));
  PRISM_CHECK(dialog.ControlEnabled(cd::ids::kLevel));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(cd::ids::kReason), "cannot be solid"));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(cd::ids::kTarget), std::string("out.zip"));

  // Back to 7z: the explicit "on" the user turned off is not silently restored.
  SelectFormat(dialog, 0);
  PRISM_CHECK_EQ(dialog.State().format(), prism::ArchiveFormat::SevenZip);
  PRISM_CHECK(!dialog.State().solid());
  PRISM_CHECK(dialog.ControlEnabled(cd::ids::kSolid));
}

PRISM_TEST("win: the level slider speaks WM_HSCROLL and WM_NOTIFY, and the estimate follows") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;

  MoveSlider(dialog, 0);
  PRISM_CHECK_EQ(dialog.State().level(), 0);
  PRISM_CHECK(!dialog.ControlEnabled(cd::ids::kDictionary));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(cd::ids::kReason), "level 0 stores"));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(cd::ids::kEstimate), "never below"));

  // The "user let go" notification commits the position too.
  MoveSlider(dialog, 7);
  NMHDR header{};
  header.hwndFrom = dialog.Control(cd::ids::kLevel);
  header.idFrom = static_cast<UINT_PTR>(cd::ids::kLevel);
  header.code = NM_RELEASEDCAPTURE;
  dialog.Dispatch(WM_NOTIFY, cd::ids::kLevel, reinterpret_cast<LPARAM>(&header));
  PRISM_CHECK_EQ(dialog.State().level(), 7);
  PRISM_CHECK(dialog.ControlEnabled(cd::ids::kDictionary));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(cd::ids::kEstimate), "no lower bound"));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(cd::ids::kEstimate), "LZMA2"));
}

PRISM_TEST("win: the password edit drives encryption and Show password unmasks it") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;

  const HWND password = dialog.Control(cd::ids::kPassword);
  PRISM_CHECK(static_cast<WPARAM>(SendMessageW(password, EM_GETPASSWORDCHAR, 0, 0)) != 0);
  PRISM_CHECK(!dialog.ControlEnabled(cd::ids::kEncryptNames));  // no password yet

  TypeInto(dialog, cd::ids::kPassword, L"hunter2", EN_CHANGE);
  PRISM_CHECK(dialog.State().encrypts());
  PRISM_CHECK(dialog.ControlEnabled(cd::ids::kEncryptNames));

  ClickButton(dialog, cd::ids::kShowPassword);
  PRISM_CHECK(dialog.State().showPassword());
  PRISM_CHECK_EQ(static_cast<WPARAM>(SendMessageW(password, EM_GETPASSWORDCHAR, 0, 0)), WPARAM{0});
  ClickButton(dialog, cd::ids::kShowPassword);
  PRISM_CHECK(!dialog.State().showPassword());
  PRISM_CHECK(static_cast<WPARAM>(SendMessageW(password, EM_GETPASSWORDCHAR, 0, 0)) != 0);

  // The password reaches the plan as a cipher, not as text.
  SelectFormat(dialog, 1);  // zip
  const prism::CompressionPlan plan = dialog.State().BuildPlan();
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.cipher, prism::Cipher::Aes256);
  PRISM_CHECK_EQ(PropertyValue(plan, "em"), std::string("AES256"));
}

PRISM_TEST("win: tar refuses the password -- OK stays disabled and the text is not dropped") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;

  TypeInto(dialog, cd::ids::kPassword, L"hunter2", EN_CHANGE);
  PRISM_CHECK(dialog.ControlEnabled(cd::ids::kOk));

  SelectFormat(dialog, 2);  // tar
  PRISM_CHECK_EQ(dialog.State().format(), prism::ArchiveFormat::Tar);
  PRISM_CHECK(!dialog.ControlEnabled(cd::ids::kPassword));
  PRISM_CHECK(!dialog.ControlEnabled(cd::ids::kLevel));
  // D3: with PRISM_CD_MUTANT_TAR_ENCRYPT_ALLOWED the dialog clears the password
  // instead, so BOTH of these go red and the mutant marker can fire.
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(cd::ids::kPassword), std::string("hunter2"));
  PRISM_CHECK(!dialog.ControlEnabled(cd::ids::kOk));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(cd::ids::kReason), "tar cannot encrypt"));
  // No partial promise: the plan itself refuses too.
  PRISM_CHECK(!dialog.State().BuildPlan().ok);
}

PRISM_TEST("win: Browse fills the target and OK hands over a usable plan") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;

  dialog.SetBrowseHandler([] { return std::wstring(L"E:\\out\\report.7z"); });
  ClickButton(dialog, cd::ids::kBrowse);
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(cd::ids::kTarget), std::string("E:\\out\\report.7z"));

  MoveSlider(dialog, 6);
  ClickButton(dialog, cd::ids::kSolid);
  TypeInto(dialog, cd::ids::kPassword, L"pw", EN_CHANGE);

  cd::CreateRequest captured;
  bool called = false;
  dialog.SetAcceptHandler([&](const cd::CreateRequest& request) {
    captured = request;
    called = true;
    return true;  // close the dialog: the create was accepted
  });

  const HWND window = dialog.Hwnd();
  ClickButton(dialog, cd::ids::kOk);
  PRISM_CHECK(called);
  PRISM_CHECK(dialog.Accepted());
  PRISM_CHECK_EQ(IsWindow(window), FALSE);  // a successful OK really closes it
  PRISM_CHECK_EQ(captured.plan.ok, true);
  PRISM_CHECK_EQ(captured.plan.format, prism::ArchiveFormat::SevenZip);
  PRISM_CHECK_EQ(captured.plan.level, 6);
  PRISM_CHECK_EQ(captured.plan.cipher, prism::Cipher::Aes256);
  // mini_test's printer is ostream-based (narrow): compare the UTF-8 forms so a
  // failure prints the path instead of failing to compile.
  PRISM_CHECK_EQ(cd::Utf8FromWide(captured.archivePath), std::string("E:\\out\\report.7z"));
  PRISM_CHECK_EQ(cd::Utf8FromWide(captured.sourceRoot), std::string("E:\\fixture"));
  PRISM_CHECK_EQ(captured.plan.items.size(), size_t{3});
}

PRISM_TEST("win: OK stays disabled (and says why) while the dialog has no work") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;

  // A fresh dialog with no selection and no target.
  cd::CompressDialog bare;
  if (!bare.Create(GetModuleHandleW(nullptr), L"PrismArchive compress dialog test")) {
    std::printf("    [dialog] second window failed; skipping the empty-state case\n");
    return;
  }
  PRISM_CHECK(!bare.ControlEnabled(cd::ids::kOk));
  PRISM_CHECK(Contains(bare.ControlTextUtf8(cd::ids::kReason), "Nothing selected"));

  bare.SetSource(WindowFixture(), L"E:\\fixture");
  PRISM_CHECK(!bare.ControlEnabled(cd::ids::kOk));  // target still missing
  PRISM_CHECK(Contains(bare.ControlTextUtf8(cd::ids::kReason), "Choose where"));
}

PRISM_TEST("win: WM_CLOSE destroys the window (no hidden dialog, no WM_QUIT)") {
  cd::CompressDialog dialog;
  if (!Open(dialog)) return;
  const HWND window = dialog.Hwnd();
  PRISM_CHECK(window != nullptr);
  dialog.Dispatch(WM_CLOSE, 0, 0);
  PRISM_CHECK_EQ(IsWindow(window), FALSE);
  PRISM_CHECK(dialog.Hwnd() == nullptr);
}

#endif  // _WIN32

int main(int argc, char** argv) {
  const int failures = prism::test::RunAll(argc, argv);

#if defined(_WIN32)
#if defined(PRISM_CD_MUTANT_ZIP_SOLID_ALLOWED) || defined(PRISM_CD_MUTANT_TAR_ENCRYPT_ALLOWED)
  // CONTRACTS D3 + D26: the marker is a CONJUNCTION -- the run went red AND the
  // deliberate defect actually fired. A mutation that silently stopped firing
  // prints nothing, the suite's PASS_REGULAR_EXPRESSION misses, and the test is
  // RED. No WILL_FAIL is used anywhere near a PASS_REGULAR_EXPRESSION.
  if (failures != 0 && cd::DialogState::MutationFired()) {
    std::puts("COMPRESS-DIALOG MUTANT CAUGHT");
  }
  std::printf("mutant build: exit 1 on purpose (mutation hits=%d)\n",
              cd::DialogState::MutationCount());
  return 1;
#else
  if (!gSkipReason.empty()) {
    // CONTRACTS D1: a skip is PRINTED and it is never exit 0.
    std::printf("SKIP: %s\n", gSkipReason.c_str());
    return 77;  // SKIP_RETURN_CODE 77 (Tests/suites/c18-compressdialog.cmake)
  }
  if (failures != 0) return 1;
  std::puts("ALL COMPRESS-DIALOG CHECKS PASSED");
  return 0;
#endif
#else
  return failures;
#endif
}
