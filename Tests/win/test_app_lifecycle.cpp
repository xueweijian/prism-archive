// Windows integration test: APPLICATION LIFECYCLE (card C1).
//
// What this proves, against REAL objects (L2: nothing here is a mock):
//   * 7z.dll discovery resolves in the documented order and reports every path
//     it tried (env -> app dir -> 7-Zip's own registry InstallDir -> the two
//     Program Files probes), so "7z.dll not found" is diagnosable;
//   * a REAL GalleryWindow on a REAL message loop, painted once through a REAL
//     WM_PAINT, returns from Run() within 500 ms of a REAL WM_CLOSE, and the
//     window is gone afterwards. This is the regression guard for the bug that
//     shipped in batch A: the gallery renderer was never Initialize()d, the
//     first paint dereferenced a null DirectWrite factory, and the UI thread
//     died inside DispatchMessageW without ever seeing the WM_QUIT that closing
//     the window had posted -- the window disappeared, the process lived
//     forever;
//   * the whole-archive thumbnail scan is CANCELLABLE: arm ArchiveReader::
//     RequestBatchAbort() from inside the sweep (at the first decoded thumb)
//     and one 7z Extract over a real solid archive stops early on the real
//     7z.dll, with the remaining items never decoded.
//
// The stdout marker is part of the verdict (CONTRACTS D1): a silent SKIP cannot
// pass -- this file returns non-zero when it cannot run, and
// Tests/CMakeLists.txt registers PASS_REGULAR_EXPRESSION on the marker.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wincodec.h>

#pragma comment(lib, "advapi32.lib")  // RegOpenKeyExW inside Resolve7zDll

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ThumbStore.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "../../Src/PrismWin/SolidSweeper.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

using prism::ArchiveItem;
using prism::win::gallery::GalleryWindow;

namespace {

uint64_t NowMs() { return GalleryWindow::NowMs(); }

// Joins in the destructor, and ONLY joins (never detaches): every early return
// below has to leave the message-loop thread finished, or std::thread's own
// destructor would call std::terminate and hide the real failure behind
// 0xC0000409.
class JoinOnExit {
 public:
  explicit JoinOnExit(std::thread& thread) : thread_(thread) {}
  ~JoinOnExit() {
    if (thread_.joinable()) thread_.join();
  }
  JoinOnExit(const JoinOnExit&) = delete;
  JoinOnExit& operator=(const JoinOnExit&) = delete;

 private:
  std::thread& thread_;
};

std::wstring JoinPath(const std::wstring& dir, const wchar_t* name) {
  return prism::win::JoinFsPath(dir, name);
}

// ---------------------------------------------------------------- 1. discovery
PRISM_TEST("7z.dll discovery: env wins, order is the documented one") {
  const prism::win::SevenZipDllLookup baseline = prism::win::Resolve7zDll();
  // The suite needs a real engine for the rest of the file; failing here is a
  // hard FAIL (never a skip) and it prints what was tried.
  if (!PRISM_CHECK(!baseline.path.empty())) {
    std::printf("    resolved nothing; candidates:\n");
    for (const auto& c : baseline.candidates) std::wprintf(L"      %ls\n", c.c_str());
    return;
  }

  // (a) an explicit override wins over everything -- that is the escape hatch a
  //     user with a portable 7-Zip relies on.
  const std::wstring realDll = baseline.path;
  if (SetEnvironmentVariableW(L"PRISM_7Z_DLL", realDll.c_str())) {
    const prism::win::SevenZipDllLookup pinned = prism::win::Resolve7zDll();
    if (!PRISM_CHECK(pinned.path == realDll)) return;
    if (!PRISM_CHECK(pinned.source == L"env")) return;
    // (b) a bogus override is REPORTED but not accepted: the search continues
    //     instead of failing with the wrong explanation.
    const std::wstring bogus = L"C:\\__prism_no_such_dir__\\7z.dll";
    if (!PRISM_CHECK(SetEnvironmentVariableW(L"PRISM_7Z_DLL", bogus.c_str()) != 0)) return;
    const prism::win::SevenZipDllLookup skipped = prism::win::Resolve7zDll();
    if (!PRISM_CHECK(!skipped.candidates.empty())) return;
    if (!PRISM_CHECK(skipped.candidates.front() == bogus)) return;  // tried, and said so
    if (!PRISM_CHECK(skipped.path != bogus)) return;                 // but not accepted
    if (!PRISM_CHECK(skipped.path.empty() || prism::win::ExistsAsFile(skipped.path))) return;
    SetEnvironmentVariableW(L"PRISM_7Z_DLL", realDll.c_str());
  }

  // (c) order with no override in the way: app dir first, then 7-Zip's own
  //     registry answer (when the machine has one), then the two probes.
  //
  // Restore the override on EVERY exit path below. The rest of this file (and
  // every later case in the same process) needs PRISM_7Z_DLL, because the
  // fixture locates 7z.exe BESIDE the dll. Clearing it here and not restoring
  // made "closing cancels the whole-archive scan" fail on the runner with an
  // empty fixture path while passing locally -- the dev box re-set the variable
  // by way of case (a), the runner did not. CI caught it on 2026-11-01.
  struct EnvRestore {
    std::wstring value;
    ~EnvRestore() {
      if (!value.empty()) SetEnvironmentVariableW(L"PRISM_7Z_DLL", value.c_str());
    }
  } env_restore{realDll};
  SetEnvironmentVariableW(L"PRISM_7Z_DLL", nullptr);
  const prism::win::SevenZipDllLookup ordered = prism::win::Resolve7zDll();
  if (!PRISM_CHECK(ordered.candidates.size() >= 2)) return;
  if (!PRISM_CHECK(ordered.candidates.front() ==
                   JoinPath(prism::win::ExecutableDirectory(), L"7z.dll"))) {
    return;
  }
  // The Program Files probes are the LAST resort (they are also the only
  // candidates a registry-less install can match), and the x64 probe is tried
  // before the x86 one.
  //
  // Assert the CONTRACT, never a literal index and never "both must exist": a
  // machine may provide only one of the two (the CI runner installs 7-Zip x64
  // only), and a machine whose 7-Zip IS under Program Files gets a registry
  // answer equal to the x64 probe, which the dedupe collapses. Two rounds of
  // main-red on 2026-11-01 came from exactly those two assumptions.
  const std::wstring kProbe64 = L"C:\\Program Files\\7-Zip\\7z.dll";
  const std::wstring kProbe86 = L"C:\\Program Files (x86)\\7-Zip\\7z.dll";
  const std::vector<std::wstring>& c = ordered.candidates;
  auto index_of = [&c](const std::wstring& value) {
    for (std::size_t i = 0; i < c.size(); ++i) {
      if (c[i] == value) return i;
    }
    return c.size();
  };
  const std::size_t probe64 = index_of(kProbe64);
  const std::size_t probe86 = index_of(kProbe86);
  // At least one probe is always tried, and it is always the last resort.
  if (!PRISM_CHECK(probe64 < c.size() || probe86 < c.size())) return;
  if (!PRISM_CHECK(c.back() == kProbe64 || c.back() == kProbe86)) return;
  // Documented order between them, when the machine has both.
  if (probe64 < c.size() && probe86 < c.size()) {
    if (!PRISM_CHECK(probe64 < probe86)) return;
  }
  // If this machine DOES register 7-Zip, its own InstallDir must be among the
  // candidates and must win over the probes -- that is the non-default-drive
  // case this card exists for (a 7-Zip at E:\DevTools\7-Zip used to be
  // invisible to the two hard-coded Program Files paths).
  std::wstring installDir;
  bool registered = prism::win::RegistryInstallDir(HKEY_LOCAL_MACHINE, L"SOFTWARE\\7-Zip",
                                                  &installDir);
  if (!registered) {
    registered = prism::win::RegistryInstallDir(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\7-Zip", &installDir);
  }
  if (registered) {
    const std::wstring fromRegistry = JoinPath(installDir, L"7z.dll");
    bool present = false;
    for (const auto& candidate : c) present = present || candidate == fromRegistry;
    if (!PRISM_CHECK(present)) return;
    if (prism::win::ExistsAsFile(fromRegistry)) {
      if (!PRISM_CHECK(ordered.path == fromRegistry)) return;
      if (!PRISM_CHECK(ordered.source.find(L"registry") == 0)) return;
    }
  } else {
    std::printf("    NOTE: this machine registers no 7-Zip (portable install); the "
                "registry branch is exercised on the CI runner\n");
  }
  SetEnvironmentVariableW(L"PRISM_7Z_DLL", realDll.c_str());
}

// ------------------------------------------------------- 2. window lifecycle
// A REAL window, a REAL message loop on its own thread, and a REAL WM_CLOSE.
// The paint happens first on purpose: that is exactly the message the shipped
// build crashed in.
PRISM_TEST("closing the window returns Run() in <=500ms and destroys the HWND") {
  std::vector<ArchiveItem> items;
  auto add = [&items](const char* path, bool dir, uint64_t size) {
    ArchiveItem item;
    item.index = static_cast<uint32_t>(items.size());
    item.path = path;
    item.isDir = dir;
    item.size = size;
    items.push_back(item);
  };
  add("a.png", false, 1200);
  add("b.png", false, 2400);
  add("notes.txt", false, 12);
  add("folder", true, 0);

  std::mutex mutex;
  std::condition_variable cv;
  HWND hwnd = nullptr;         // published by the window thread
  bool runReturned = false;    // set when Run() came back
  uint64_t runReturnedMs = 0;  // stamped by the window thread itself

  std::thread windowThread([&]() {
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)coInit;
    {
      GalleryWindow window;
      const bool created =
          window.Create(L"PrismArchive -- lifecycle test", GetModuleHandleW(nullptr));
      {
        std::lock_guard<std::mutex> lock(mutex);
        hwnd = created ? window.Hwnd() : nullptr;
      }
      cv.notify_all();
      if (created) {
        window.SetItems(&items);
        window.SetRootLabel(L"lifecycle.7z");
        window.SetStatus(L"4 items");
        window.Run();  // returns on WM_QUIT, which WM_DESTROY posts
        std::lock_guard<std::mutex> lock(mutex);
        runReturnedMs = NowMs();  // measured ON the loop's own thread
        runReturned = true;
      }
    }
    cv.notify_all();
  });
  JoinOnExit joinWindow(windowThread);

  HWND target = nullptr;
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(15), [&] { return hwnd != nullptr; })) {
      lock.unlock();
      PRISM_CHECK(false);  // no window, no lifecycle (JoinOnExit finishes it)
      return;
    }
    target = hwnd;
  }
  // Run() shows the window before pumping, so wait for it to be visible.
  for (int i = 0; i < 500 && !IsWindowVisible(target); ++i) Sleep(10);
  if (!PRISM_CHECK(IsWindowVisible(target))) {
    PostMessageW(target, WM_CLOSE, 0, 0);
    return;
  }

  // Force the paint path the crash lived in: UpdateWindow sends WM_PAINT
  // synchronously on the window thread. If that faults, this process dies and
  // ctest reports a crash -- a red that cannot be mistaken for a hang.
  InvalidateRect(target, nullptr, FALSE);
  if (!PRISM_CHECK(UpdateWindow(target) != 0)) return;

  // The contract: from WM_CLOSE to Run() returning, <= 500 ms (the product
  // budget is 1 s for process exit; the loop must be far quicker).
  const uint64_t closeAt = NowMs();
  if (!PRISM_CHECK(PostMessageW(target, WM_CLOSE, 0, 0) != 0)) return;
  uint64_t returnedAt = 0;
  {
    std::unique_lock<std::mutex> lock(mutex);
    // Wait LONGER than the budget on purpose: a timeout here means the loop is
    // wedged, which is a different (worse) failure than "too slow", and the
    // stamp written by the loop thread below is what grades the budget.
    const bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return runReturned; });
    if (!PRISM_CHECK(ok)) {
      std::printf("    Run() never returned after WM_CLOSE (wedged loop)\n");
      lock.unlock();
      // Do not hang the suite on a regression: ask again and let JoinOnExit
      // finish it; a really stuck loop is killed by ctest's TIMEOUT (also red).
      // No detach anywhere: a detached thread would outlive the stack objects it
      // uses, which is the defect class this card exists to remove.
      PostMessageW(target, WM_CLOSE, 0, 0);
      return;
    }
    returnedAt = runReturnedMs;  // stamped by the loop thread, not by this one
  }
  const uint64_t elapsed = returnedAt >= closeAt ? returnedAt - closeAt : 0;
  std::printf("    WM_CLOSE -> Run() returned in %llu ms (budget 500)\n",
              static_cast<unsigned long long>(elapsed));
  if (!PRISM_CHECK(elapsed <= 500)) return;
  // And the window is really gone: a live HWND here would mean "process exits
  // with a window still on screen", the other half of the reported bug.
  if (!PRISM_CHECK(IsWindow(target) == FALSE)) return;
  if (!PRISM_CHECK(runReturned)) return;
}

// ------------------------------------------------------------ 3. scan abort
// One real Extract over a real solid archive. The abort is armed from INSIDE
// the sweep (at the first decoded thumbnail), so the decoded count is what
// proves it: a full pass decodes every image, an aborted pass cannot.
class ArmOnFirstThumb final : public prism::gallery::IThumbSink {
 public:
  ArmOnFirstThumb(prism::gallery::IThumbSink& inner, prism::win::ArchiveReader& reader,
                  bool arm)
      : inner_(inner), reader_(reader), arm_(arm) {}

  bool OnThumb(uint32_t itemIndex, const prism::gallery::DecodedThumb& thumb) override {
    ++seen_;
    if (arm_ && seen_ == 1) {
      armed_ = true;
      reader_.RequestBatchAbort();  // "the user closed the window"
    }
    return inner_.OnThumb(itemIndex, thumb);
  }
  int seen() const { return seen_; }
  bool armed() const { return armed_; }

 private:
  prism::gallery::IThumbSink& inner_;
  prism::win::ArchiveReader& reader_;
  bool arm_;
  int seen_ = 0;
  bool armed_ = false;
};

PRISM_TEST("closing cancels the whole-archive scan on the real 7z.dll") {
  const prism::win::SevenZipDllLookup lookup = prism::win::Resolve7zDll();
  if (!PRISM_CHECK(!lookup.path.empty())) return;
  const std::wstring sevenZip = prism::fixture::ExePath();
  if (!PRISM_CHECK(!sevenZip.empty())) {
    std::puts("    7z.exe not found beside 7z.dll -- the fixture cannot be packed");
    return;
  }

  // Unique per process (card C36, Tests/win/UniqueTempDir.hpp): the constant
  // "%TEMP%\prism_app_lifecycle" was shared by every parallel copy of this
  // binary, so one run could wipe another run's shots.7z mid-pack.
  prism::testtmp::UniqueDir scratch(L"c36", L"lifecycle");
  if (!PRISM_CHECK(scratch.ok())) {
    std::puts("    cannot create the fixture directory");
    return;
  }
  const std::filesystem::path& dir = scratch.path();

  constexpr int kImages = 6;
  std::vector<std::wstring> names;
  for (int i = 0; i < kImages; ++i) {
    wchar_t name[32];
    swprintf_s(name, L"shot%02d.png", i);
    if (!PRISM_CHECK(prism::fixture::WritePng((dir / name).wstring(), 96, 64,
                                              static_cast<uint8_t>(i * 20), 120, 200))) {
      return;
    }
    names.emplace_back(name);
  }
  if (!PRISM_CHECK(prism::fixture::PackSolid(sevenZip, dir.wstring(), L"shots.7z", names))) {
    return;
  }

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(lookup.path))) return;
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open((dir / L"shots.7z").wstring()))) {
    std::printf("    open trace: %s\n", reader.openTrace().c_str());
    return;
  }
  const std::vector<ArchiveItem> catalog = reader.ReadCatalog();
  std::vector<uint32_t> indices;
  std::vector<uint64_t> sizes;
  for (const ArchiveItem& item : catalog) {
    if (item.isDir) continue;
    if (prism::QuickLookPlanner::KindByName(item.path) != prism::PreviewKind::Image) continue;
    indices.push_back(item.index);
    sizes.push_back(item.size);
  }
  if (!PRISM_CHECK_EQ(static_cast<int>(indices.size()), kImages)) return;

  prism::win::sweep::WicThumbDecoder wic;
  if (!PRISM_CHECK(wic.Initialize())) return;

  // (a) baseline: with no abort, ONE Extract decodes every image. This is also
  //     the D18 guard -- the "cut short" count below is only meaningful against
  //     a pass that really does complete.
  {
    auto store = std::make_shared<prism::gallery::ThumbStore>(16ull << 20);
    const prism::win::sweep::SweepResult full =
        prism::win::sweep::RunSolidSweep(reader, indices, sizes, wic, *store);
    std::printf("    full sweep: ok=%d decoded=%d skipped=%d\n", static_cast<int>(full.ok),
                full.decoded, full.skipped);
    if (!PRISM_CHECK(full.ok)) return;
    if (!PRISM_CHECK_EQ(full.decoded, kImages)) return;
  }

  // (b) the close path: the flag is armed WHILE the sweep runs, exactly like
  //     Main.cpp does when Run() returns -- and 7-Zip has to stop.
  {
    reader.ClearBatchAbort();
    auto store = std::make_shared<prism::gallery::ThumbStore>(16ull << 20);
    ArmOnFirstThumb sink(*store, reader, /*arm=*/true);
    const uint64_t startMs = NowMs();
    const prism::win::sweep::SweepResult cut =
        prism::win::sweep::RunSolidSweep(reader, indices, sizes, wic, sink);
    const uint64_t elapsed = NowMs() - startMs;
    std::printf("    aborted sweep: ok=%d decoded=%d armed=%d in %llu ms\n",
                static_cast<int>(cut.ok), cut.decoded, static_cast<int>(sink.armed()),
                static_cast<unsigned long long>(elapsed));
    if (!PRISM_CHECK(sink.armed())) return;  // the flag was really set mid-pass
    if (!PRISM_CHECK(!cut.ok)) return;       // the Extract came back aborted
    if (!PRISM_CHECK(cut.decoded >= 1)) return;
    if (!PRISM_CHECK(cut.decoded < kImages)) return;  // it stopped early
    if (!PRISM_CHECK(elapsed < 2000)) return;
    reader.ClearBatchAbort();
  }

  reader.Close();
  // The scratch tree is removed by UniqueDir's destructor, which runs here --
  // after the reader is closed, the same ordering this line used to hand-roll
  // (7-Zip opens archives without FILE_SHARE_DELETE).
}

}  // namespace

// The marker is part of the verdict (D1): it prints only when every case passed,
// and Tests/CMakeLists.txt pins it with PASS_REGULAR_EXPRESSION, so a silent
// skip, an early return or a crash after printing all stay red.
int main(int argc, char** argv) {
  // WIC (SolidFixture::WritePng) activates a COM object on the calling thread;
  // without this the scan fixture cannot even be authored, and the scan case
  // would fail for a reason that has nothing to do with cancellation.
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int rc = prism::test::RunAll(argc, argv);
  if (SUCCEEDED(coInit)) CoUninitialize();
  if (rc != 0) return rc;
  std::puts("ALL APP-LIFECYCLE CHECKS PASSED");
  return 0;
}
