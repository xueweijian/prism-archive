// PrismArchive -- C94 (Windows lane): ONE conflict dialog per extraction, on
// REAL 7z bytes.
//
// MEASURED BEFORE THIS CARD: 14 entries extracted over 14 existing targets
// raised the yes/no/cancel box 14 times, each answer covering exactly one file.
//
// WHAT THIS SUITE PROVES, IN COUNTS AND BYTES
//   1. 全部覆盖  -- the dialog is CREATED ONCE (OverwriteDialog::Creations() is
//                  a count of real HWNDs) and all 14 targets carry the
//                  archive payload afterwards;
//   2. 全部跳过  -- created once, contents AND last-write times are unchanged,
//                  filesWritten == 0, skippedFiles == 14;
//   3. 全部重命名 -- created once, "itemNN (1).txt" holds the payload and the
//                  original target is untouched;
//   4. 仅这一个  -- NOT remembered: 14 conflicts -> 14 dialogs (the scope is
//                  explicit, never assumed);
//   5. 取消(Esc) -- created once and NOTHING is written (no conflict is
//                  overwritten by a dismissal);
//   6. the dialog's own controls carry the six scope answers + the remember box,
//                  and the window is visible; the PNG written here is the card's
//                  screenshot (docs/assets/ui/c94-conflict.png).
//
// NEGATIVE CONTROL (D3/D26/D27): win_c94_overwrite_all_mutant_per_file compiles
// THIS source with PRISM_C94_MUTANT_PER_FILE, which removes the session's
// memory. "14 conflicts -> 1 dialog" must go red, and mini_test prints exactly
// "C94 MUTATION CAUGHT: per_file" at the first failing assertion (armed on the
// mutation counter, so an unrelated red cannot produce a false CAUGHT).
//
// D1: no 7z.dll / 7z.exe -> print WHY and exit 77 (SKIP stays visible).
#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/OverwriteSession.hpp"
#include "../../Src/PrismWin/OverwriteDialog.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace ow = prism::overwrite;
namespace owdlg = prism::win::owdlg;

namespace {

constexpr int kFiles = 14;
bool g_skip = false;

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
  std::wstring out = dir;
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out += L'\\';
  return out + name;
}

std::string Name(int i) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "item%02d.txt", i);
  return buffer;
}

bool WriteText(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

std::string ReadText(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string ArchivePayload(int i) { return "archive-payload-" + Name(i); }
std::string UserPayload(int i) { return "user-payload-" + Name(i); }

bool MtimeOf(const fs::path& path, FILETIME* out, uint64_t* sizeOut) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) return false;
  *out = data.ftLastWriteTime;
  *sizeOut = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  return true;
}

// ---------------------------------------------------------------------------
// Screenshot: the dialog's own pixels, encoded by WIC. PW_RENDERFULLCONTENT
// asks the window to draw itself (controls included) into our DC, so this works
// even when the window overlaps the desktop.
// ---------------------------------------------------------------------------
bool WriteBgraPng(WORD width, WORD height, std::vector<BYTE> pixels, const std::wstring& path) {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(wic->CreateStream(stream.ReleaseAndGetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                encoder.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(), nullptr))) return false;
  if (FAILED(frame->Initialize(nullptr))) return false;
  if (FAILED(frame->SetSize(width, height))) return false;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) return false;
  const UINT stride = static_cast<UINT>(width) * 4;
  const UINT size = stride * height;
  if (FAILED(frame->WritePixels(height, stride, size, pixels.data()))) return false;
  if (FAILED(frame->Commit())) return false;
  return SUCCEEDED(encoder->Commit());
}

bool CaptureWindow(HWND hwnd, const std::wstring& path) {
  RECT rect{};
  if (GetWindowRect(hwnd, &rect) == FALSE) return false;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;
  HDC screen = GetDC(nullptr);
  if (screen == nullptr) return false;
  HDC memory = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap == nullptr || bits == nullptr) {
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return false;
  }
  const HGDIOBJ old = SelectObject(memory, bitmap);
  const BOOL printed = PrintWindow(hwnd, memory, 2 /* PW_RENDERFULLCONTENT */);
  if (printed == FALSE) {
    // Some builds/sessions refuse PrintWindow; the visible window's own DC is
    // the fallback (it only works on a real desktop, which is the only place a
    // screenshot is meaningful anyway).
    HDC windowDc = GetWindowDC(hwnd);
    if (windowDc == nullptr || BitBlt(memory, 0, 0, width, height, windowDc, 0, 0, SRCCOPY) == FALSE) {
      if (windowDc != nullptr) ReleaseDC(hwnd, windowDc);
      SelectObject(memory, old);
      DeleteObject(bitmap);
      DeleteDC(memory);
      ReleaseDC(nullptr, screen);
      return false;
    }
    ReleaseDC(hwnd, windowDc);
  }
  SelectObject(memory, old);
  std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
  std::memcpy(pixels.data(), bits, pixels.size());
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  return WriteBgraPng(static_cast<WORD>(width), static_cast<WORD>(height), std::move(pixels),
                      path);
}

// ---------------------------------------------------------------------------
// The scripted presenter: it creates the REAL dialog (the product's own window
// class), answers it through the same Finish() a click reaches, and counts the
// creations. In the extraction cases it is never shown -- a modal loop would
// hang a test.
// ---------------------------------------------------------------------------
struct Script {
  ow::Answer answer = ow::Answer::ReplaceAll;
  bool remember = false;
  bool cancel = false;
  bool capture = false;
  std::wstring shotPath;
};

struct PresenterLog {
  int calls = 0;
  int creations = 0;  // dialogs this suite built
  std::vector<std::string> paths;
};

ow::Reply Present(const Script& script, const std::string& path, PresenterLog* log) {
  ++log->calls;
  log->paths.push_back(path);
  owdlg::OverwriteDialog dialog;
  dialog.SetModel(owdlg::BuildConflictModel(path));
  if (!dialog.Create(GetModuleHandleW(nullptr))) {
    // A window could not be created: consent was never given.
    return ow::ReplyOf(ow::Answer::SkipAll);
  }
  ++log->creations;
  if (script.capture) {
    dialog.Show();
    if (!CaptureWindow(dialog.Hwnd(), script.shotPath)) {
      std::printf("    [warn] screenshot failed: %ls\n", script.shotPath.c_str());
    } else {
      std::printf("    [shot] %ls\n", script.shotPath.c_str());
    }
  }
  if (script.cancel) {
    dialog.Cancel();
  } else {
    dialog.SetRememberChecked(script.remember);
    dialog.Choose(script.answer);
  }
  const owdlg::ConflictResult& result = dialog.Result();
  return ow::ReplyOf(result.answer, result.remember);
}

// ---------------------------------------------------------------------------
// Engine fixture (same shape as C71: archive authored by the 7-Zip CLI).
// ---------------------------------------------------------------------------
struct EngineFixture {
  std::unique_ptr<prism::testtmp::UniqueDir> dir;
  std::wstring root;
  std::wstring exe;
  std::wstring dll;
  bool ready = false;
  std::string why;
};

EngineFixture& Engine() {
  static EngineFixture fixture;
  return fixture;
}

bool RequireEngine() {
  EngineFixture& fx = Engine();
  if (fx.ready) return true;
  if (!fx.why.empty()) {
    std::printf("    [skip] %s\n", fx.why.c_str());
    g_skip = true;
    return false;
  }
  fx.dll = prism::fixture::DllPath();
  fx.exe = prism::fixture::ExePath();
  if (fx.dll.empty() || fx.exe.empty()) {
    fx.why = prism::fixture::ExePathWhy();
    std::printf("    [skip] %s\n", fx.why.c_str());
    g_skip = true;
    return false;
  }
  fx.dir = std::make_unique<prism::testtmp::UniqueDir>(L"c94", L"engine");
  if (!fx.dir->ok()) {
    fx.why = "cannot create the scratch directory: " + fx.dir->error();
    std::printf("    [skip] %s\n", fx.why.c_str());
    g_skip = true;
    return false;
  }
  fx.root = fx.dir->path().wstring();
  fx.ready = true;
  return true;
}

struct ArchiveFixture {
  std::wstring archivePath;
  std::vector<prism::ArchiveItem> catalog;
  prism::win::SevenZipLibrary library;
  bool ready = false;
};

bool OpenFixture(ArchiveFixture* out, const wchar_t* name) {
  if (!RequireEngine()) return false;
  EngineFixture& engine = Engine();
  out->archivePath = Join(engine.root, name);
  if (!out->library.Load(engine.dll)) return false;
  prism::win::ArchiveReader probe(out->library);
  if (!probe.Open(out->archivePath)) {
    std::printf("    [fixture] open failed: %s\n", probe.openTrace().c_str());
    return false;
  }
  out->catalog = probe.ReadCatalog();
  probe.Close();
  out->ready = !out->catalog.empty();
  return out->ready;
}

std::shared_ptr<prism::app::OperationHandles> MakeOps(const std::wstring& archive,
                                                     prism::win::SevenZipLibrary& library) {
  prism::app::OperationHandles::Config config;
  config.archivePath = archive;
  config.handlerSlots = 2;
  config.backlog = 4;
  std::string why;
  auto ops = prism::app::OperationHandles::Create(library, config, &why);
  if (!ops) std::printf("    [ops] create failed: %s\n", why.c_str());
  return ops;
}

prism::app::ExtractionPlan FlatPlan(const std::wstring& targetDir) {
  prism::app::ExtractionPlan plan;
  plan.mode = prism::app::LaunchMode::kExtractHere;
  plan.plan.strategy = prism::SmartStrategy::FlatDirectly;
  plan.plan.baseTargetDir = prism::win::WideToUtf8(targetDir);
  plan.plan.reason = "C94 test: extract the archive tree verbatim";
  return plan;
}

// Packs the 14 files with the CLI's working directory INSIDE "in", so the
// archive stores "item01.txt" (not "in/item01.txt") and FlatDirectly lands each
// entry exactly on the pre-existing target.
bool PackFixture(const std::wstring& exe, const fs::path& in, const std::wstring& archive) {
  const DWORD old = GetCurrentDirectoryW(0, nullptr);
  std::vector<wchar_t> previous(old > 0 ? old : 0);
  if (!previous.empty()) GetCurrentDirectoryW(old, previous.data());
  if (SetCurrentDirectoryW(in.c_str()) == FALSE) return false;
  std::wstring args = L"a -t7z -mx=0 -ms=off \"" + archive + L"\"";
  for (int i = 1; i <= kFiles; ++i) {
    args += L" \"" + prism::win::Utf8ToWide(Name(i)) + L"\"";
  }
  const bool ok = prism::fixture::RunSevenZip(exe, args);
  if (!previous.empty()) SetCurrentDirectoryW(previous.data());
  return ok;
}

// One extraction over 14 existing targets.
prism::app::ExtractionOutcome RunCase(ArchiveFixture& fixture, const fs::path& out,
                                      const Script& script, PresenterLog* log) {
  auto ops = MakeOps(fixture.archivePath, fixture.library);
  prism::app::ExtractionOutcome outcome;
  if (!ops) return outcome;
  prism::app::ExtractionOptions options;
  options.overwrite = ow::Mode::Ask;  // the default: never silent
  options.verifyAfterExtract = false;
  options.promptAll = [&script, log](const std::string& path) {
    return Present(script, path, log);
  };
  outcome = prism::app::RunExtraction(*ops, fixture.catalog, FlatPlan(out.wstring()),
                                      fixture.archivePath, /*scanExecutables=*/false,
                                      prism::win::amsi::InitMode::LoadAmsi, options);
  ops->Close();
  return outcome;
}

// The 14 pre-existing targets, freshly written for one case.
bool ResetTargets(const fs::path& out) {
  std::error_code ec;
  fs::remove_all(out, ec);
  fs::create_directories(out, ec);
  if (ec) return false;
  for (int i = 1; i <= kFiles; ++i) {
    if (!WriteText(out / Name(i), UserPayload(i))) return false;
  }
  return true;
}

}  // namespace

// ===========================================================================
// 6. The dialog itself: real controls, real labels, visible.
// ===========================================================================
PRISM_TEST("dialog: six answers + the remember box, on real controls") {
  owdlg::OverwriteDialog dialog;
  const std::string target = "C:\\Users\\prism\\out\\item01.txt";
  dialog.SetModel(owdlg::BuildConflictModel(target));
  PRISM_CHECK(dialog.Create(GetModuleHandleW(nullptr)));
  if (dialog.Hwnd() == nullptr) return;  // no window station: the later cases skip

  auto text = [&dialog](int id) {
    HWND control = dialog.Control(id);
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring out(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, out.data(), length + 1);
    out.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return out;
  };
  const owdlg::ConflictModel& model = dialog.Model();
  // std::wstring has no narrow operator<<, so the comparison states the label
  // that disagreed instead of relying on CheckEq's formatter.
  auto same = [&text](int id, const std::wstring& expected, const char* what) {
    const std::wstring got = text(id);
    if (got == expected) return true;
    std::printf("    [dialog] %s label mismatch\n", what);
    return false;
  };
  PRISM_CHECK(same(owdlg::ids::kReplaceAll, model.replaceAll, "replace-all"));
  PRISM_CHECK(same(owdlg::ids::kSkipAll, model.skipAll, "skip-all"));
  PRISM_CHECK(same(owdlg::ids::kRenameAll, model.renameAll, "rename-all"));
  PRISM_CHECK(same(owdlg::ids::kReplaceOne, model.replaceOne, "replace-one"));
  PRISM_CHECK(same(owdlg::ids::kSkipOne, model.skipOne, "skip-one"));
  PRISM_CHECK(same(owdlg::ids::kRenameOne, model.renameOne, "rename-one"));
  PRISM_CHECK(same(owdlg::ids::kRemember, model.rememberLabel, "remember"));
  // The body names the conflicting file -- the user must know WHICH file the
  // "just this one" answers are about.
  PRISM_CHECK(text(owdlg::ids::kBody).find(L"item01.txt") != std::wstring::npos);

  // 本操作内记住我的选择 is a real checkbox: its state is what the reply carries.
  PRISM_CHECK(!dialog.RememberChecked());
  dialog.SetRememberChecked(true);
  PRISM_CHECK(dialog.RememberChecked());

  dialog.Show();
  PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
  // The card's screenshot: the real dialog, rendered by itself.
  wchar_t cwd[MAX_PATH] = {};
  if (GetCurrentDirectoryW(MAX_PATH, cwd) != 0) {
    const std::wstring path = Join(cwd, L"c94-conflict.png");
    if (CaptureWindow(dialog.Hwnd(), path)) {
      std::printf("    [shot] %ls\n", path.c_str());
    } else {
      std::printf("    [warn] screenshot not captured (no desktop?)\n");
    }
  }

  dialog.Choose(ow::Answer::ReplaceAll);
  PRISM_CHECK(dialog.Finished());
  PRISM_CHECK(dialog.Result().answer == ow::Answer::ReplaceAll);
  PRISM_CHECK(owdlg::OverwriteDialog::Creations().load() >= 1u);
}

// ===========================================================================
// 1. 全部覆盖 -- one dialog, 14 correct payloads.
// ===========================================================================
PRISM_TEST("replace-all: 14 conflicts -> ONE dialog, 14 correct files") {
  if (!RequireEngine()) return;
  const fs::path root = fs::path(Engine().root);
  const fs::path in = root / L"in";
  const fs::path out = root / L"out";
  fs::create_directories(in);
  for (int i = 1; i <= kFiles; ++i) {
    if (!PRISM_CHECK(WriteText(in / Name(i), ArchivePayload(i)))) return;
  }
  if (!PRISM_CHECK(PackFixture(Engine().exe, in, Join(Engine().root, L"c94.7z")))) return;
  ArchiveFixture fixture;
  if (!PRISM_CHECK(OpenFixture(&fixture, L"c94.7z"))) return;
  PRISM_CHECK_EQ(fixture.catalog.size(), static_cast<size_t>(kFiles));
  if (!PRISM_CHECK(ResetTargets(out))) return;

  Script script;
  script.answer = ow::Answer::ReplaceAll;
  PresenterLog log;
  const std::uint64_t dialogsBefore = owdlg::OverwriteDialog::Creations().load();
  const prism::app::ExtractionOutcome outcome = RunCase(fixture, out, script, &log);

  // THE CARD'S ASSERTION 1: one dialog for 14 conflicts.
  PRISM_CHECK_EQ(log.calls, 1);
  PRISM_CHECK_EQ(log.creations, 1);
  // The card's counter: real windows created by the product's own dialog class.
  PRISM_CHECK_EQ(owdlg::OverwriteDialog::Creations().load() - dialogsBefore, 1u);
  PRISM_CHECK(outcome.ok);
  PRISM_CHECK_EQ(outcome.filesWritten, static_cast<uint64_t>(kFiles));
  int correct = 0;
  for (int i = 1; i <= kFiles; ++i) {
    if (ReadText(out / Name(i)) == ArchivePayload(i)) ++correct;
  }
  PRISM_CHECK_EQ(correct, kFiles);
}

// ===========================================================================
// 2. 全部跳过 -- one dialog, bytes AND timestamps untouched.
// ===========================================================================
PRISM_TEST("skip-all: 14 conflicts -> ONE dialog, targets untouched") {
  if (!RequireEngine()) return;
  const fs::path root = fs::path(Engine().root);
  const fs::path in = root / L"in";
  const fs::path out = root / L"out";
  fs::create_directories(in);
  for (int i = 1; i <= kFiles; ++i) {
    if (!PRISM_CHECK(WriteText(in / Name(i), ArchivePayload(i)))) return;
  }
  if (!PRISM_CHECK(PackFixture(Engine().exe, in, Join(Engine().root, L"c94.7z")))) return;
  ArchiveFixture fixture;
  if (!PRISM_CHECK(OpenFixture(&fixture, L"c94.7z"))) return;
  if (!PRISM_CHECK(ResetTargets(out))) return;

  std::vector<FILETIME> before(static_cast<size_t>(kFiles));
  std::vector<uint64_t> sizeBefore(static_cast<size_t>(kFiles));
  for (int i = 1; i <= kFiles; ++i) {
    if (!PRISM_CHECK(MtimeOf(out / Name(i), &before[i - 1], &sizeBefore[i - 1]))) return;
  }
  // A sleep is not evidence; a DIFFERENT timestamp is. The targets were written
  // just now, and a skip must not touch them at all.

  Script script;
  script.answer = ow::Answer::SkipAll;
  PresenterLog log;
  const std::uint64_t dialogsBefore = owdlg::OverwriteDialog::Creations().load();
  const prism::app::ExtractionOutcome outcome = RunCase(fixture, out, script, &log);

  PRISM_CHECK_EQ(log.calls, 1);
  PRISM_CHECK_EQ(log.creations, 1);
  // The card's counter: real windows created by the product's own dialog class.
  PRISM_CHECK_EQ(owdlg::OverwriteDialog::Creations().load() - dialogsBefore, 1u);
  PRISM_CHECK(outcome.ok);  // skipping an existing file is not a failure
  PRISM_CHECK_EQ(outcome.filesWritten, static_cast<uint64_t>(0));
  PRISM_CHECK_EQ(outcome.skippedFiles, static_cast<uint64_t>(kFiles));
  int untouched = 0;
  for (int i = 1; i <= kFiles; ++i) {
    FILETIME after{};
    uint64_t sizeAfter = 0;
    if (!MtimeOf(out / Name(i), &after, &sizeAfter)) return;
    if (CompareFileTime(&before[i - 1], &after) == 0 && sizeAfter == sizeBefore[i - 1] &&
        ReadText(out / Name(i)) == UserPayload(i)) {
      ++untouched;
    }
  }
  PRISM_CHECK_EQ(untouched, kFiles);
}

// ===========================================================================
// 3. 全部重命名 -- one dialog, "itemNN (1).txt", originals untouched.
// ===========================================================================
PRISM_TEST("rename-all: 14 conflicts -> ONE dialog, name (1).ext beside each") {
  if (!RequireEngine()) return;
  const fs::path root = fs::path(Engine().root);
  const fs::path in = root / L"in";
  const fs::path out = root / L"out";
  fs::create_directories(in);
  for (int i = 1; i <= kFiles; ++i) {
    if (!PRISM_CHECK(WriteText(in / Name(i), ArchivePayload(i)))) return;
  }
  if (!PRISM_CHECK(PackFixture(Engine().exe, in, Join(Engine().root, L"c94.7z")))) return;
  ArchiveFixture fixture;
  if (!PRISM_CHECK(OpenFixture(&fixture, L"c94.7z"))) return;
  if (!PRISM_CHECK(ResetTargets(out))) return;

  Script script;
  script.answer = ow::Answer::RenameAll;
  PresenterLog log;
  const std::uint64_t dialogsBefore = owdlg::OverwriteDialog::Creations().load();
  const prism::app::ExtractionOutcome outcome = RunCase(fixture, out, script, &log);

  PRISM_CHECK_EQ(log.calls, 1);
  PRISM_CHECK_EQ(log.creations, 1);
  // The card's counter: real windows created by the product's own dialog class.
  PRISM_CHECK_EQ(owdlg::OverwriteDialog::Creations().load() - dialogsBefore, 1u);
  PRISM_CHECK(outcome.ok);
  PRISM_CHECK_EQ(outcome.filesWritten, static_cast<uint64_t>(kFiles));
  int renamedCorrect = 0;
  int originalsKept = 0;
  for (int i = 1; i <= kFiles; ++i) {
    const std::string leaf = Name(i).substr(0, Name(i).size() - 4) + " (1).txt";
    if (ReadText(out / leaf) == ArchivePayload(i)) ++renamedCorrect;
    if (ReadText(out / Name(i)) == UserPayload(i)) ++originalsKept;
  }
  PRISM_CHECK_EQ(renamedCorrect, kFiles);
  PRISM_CHECK_EQ(originalsKept, kFiles);
}

// ===========================================================================
// 4. 仅这一个 (no remember) -- the scope is explicit: 14 conflicts, 14 dialogs.
// ===========================================================================
PRISM_TEST("one-only: not remembered -- 14 conflicts raise 14 dialogs") {
  if (!RequireEngine()) return;
  const fs::path root = fs::path(Engine().root);
  const fs::path in = root / L"in";
  const fs::path out = root / L"out";
  fs::create_directories(in);
  for (int i = 1; i <= kFiles; ++i) {
    if (!PRISM_CHECK(WriteText(in / Name(i), ArchivePayload(i)))) return;
  }
  if (!PRISM_CHECK(PackFixture(Engine().exe, in, Join(Engine().root, L"c94.7z")))) return;
  ArchiveFixture fixture;
  if (!PRISM_CHECK(OpenFixture(&fixture, L"c94.7z"))) return;
  if (!PRISM_CHECK(ResetTargets(out))) return;

  Script script;
  script.answer = ow::Answer::ReplaceOne;
  script.remember = false;
  PresenterLog log;
  const std::uint64_t dialogsBefore = owdlg::OverwriteDialog::Creations().load();
  const prism::app::ExtractionOutcome outcome = RunCase(fixture, out, script, &log);

  // This is the negative case for the SCOPE: an answer that says "this one"
  // must not be stretched into "all of them".
  PRISM_CHECK_EQ(log.calls, kFiles);
  PRISM_CHECK_EQ(log.creations, kFiles);
  PRISM_CHECK_EQ(owdlg::OverwriteDialog::Creations().load() - dialogsBefore,
                 static_cast<std::uint64_t>(kFiles));
  PRISM_CHECK(outcome.ok);
  PRISM_CHECK_EQ(outcome.filesWritten, static_cast<uint64_t>(kFiles));
}

// ===========================================================================
// 5. 取消 (Esc/close) -- one dialog, nothing written.
// ===========================================================================
PRISM_TEST("cancel: one dialog, no conflict is written") {
  if (!RequireEngine()) return;
  const fs::path root = fs::path(Engine().root);
  const fs::path in = root / L"in";
  const fs::path out = root / L"out";
  fs::create_directories(in);
  for (int i = 1; i <= kFiles; ++i) {
    if (!PRISM_CHECK(WriteText(in / Name(i), ArchivePayload(i)))) return;
  }
  if (!PRISM_CHECK(PackFixture(Engine().exe, in, Join(Engine().root, L"c94.7z")))) return;
  ArchiveFixture fixture;
  if (!PRISM_CHECK(OpenFixture(&fixture, L"c94.7z"))) return;
  if (!PRISM_CHECK(ResetTargets(out))) return;

  Script script;
  script.cancel = true;
  PresenterLog log;
  const std::uint64_t dialogsBefore = owdlg::OverwriteDialog::Creations().load();
  const prism::app::ExtractionOutcome outcome = RunCase(fixture, out, script, &log);

  PRISM_CHECK_EQ(log.calls, 1);
  PRISM_CHECK_EQ(log.creations, 1);
  // The card's counter: real windows created by the product's own dialog class.
  PRISM_CHECK_EQ(owdlg::OverwriteDialog::Creations().load() - dialogsBefore, 1u);
  PRISM_CHECK_EQ(outcome.filesWritten, static_cast<uint64_t>(0));
  int untouched = 0;
  for (int i = 1; i <= kFiles; ++i) {
    if (ReadText(out / Name(i)) == UserPayload(i)) ++untouched;
  }
  PRISM_CHECK_EQ(untouched, kFiles);
}

int main() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed");
    return 1;
  }
#if defined(PRISM_C94_MUTANT_PER_FILE)
  std::puts("C94 MUTANT ACTIVE: per_file");
  prism::test::SetMutationVerdict("C94 MUTATION CAUGHT: per_file", [] {
    return prism::overwrite::MutantPerFileHits() > 0;
  });
#endif
  const int failures = prism::test::RunAll(0, nullptr);
  if (g_skip) {
    std::printf("SKIP: %s\n", Engine().why.c_str());
    return 77;
  }
  if (failures != 0) return 1;
  std::puts("ALL C94 OVERWRITE-ALL CHECKS PASSED");
  return 0;
}
