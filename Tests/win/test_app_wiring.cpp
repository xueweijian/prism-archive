// PrismArchive -- Windows integration test, card C25 (app assembly, wave 2).
//
// THIS SUITE TESTS THE WIRING, NOT THE MODULES. Every module below already has
// its own suite (C11 scheduler/pool, C5 security, C8 keys, C12 sidebar, C13
// list models); what was never tested is that the EXE uses them. So the object
// under test is Src/App/AppWiring.hpp -- the exact header Main.cpp calls --
// driven against a REAL 7z.dll, a REAL archive and a REAL HWND:
//
//   (1) sweep and extract run at the same time on DIFFERENT handler slots and
//       do not cross-talk (the debt docs/PROGRESS recorded: one shared reader).
//       The overlap is joined INSIDE the engine callback (GetStream), per D24 --
//       "two threads started" would prove nothing;
//   (2) every file an extraction writes inherits the archive's :Zone.Identifier
//       BYTE-FOR-BYTE, and an archive with no mark stamps nothing (the second
//       half needs a mark-free premise, built by writing BYTES, not by copying:
//       CopyFileW clones the ADS -- D18);
//   (3) the four context-menu verbs (C4's ArgsForVerb output) parse into four
//       DIFFERENT plans, the three extracting ones really produce three different
//       on-disk layouts, and --test writes nothing while decoding everything;
//   (4) the key table (C8) changes real window state -- list mode, palette,
//       history back/forward, smart extract -- through the same router
//       WM_KEYDOWN uses;
//   (5) the list view is C13's sorted/filtered view and the selection still
//       resolves to the right folder row (the bug class that makes "sort by
//       size" activate a random entry).
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26). The suite is compiled TWICE more, each
// with one deliberate regression injected at COMPILE TIME (a definition cannot
// silently fail to apply -- D3's own lesson):
//   * PRISM_C25_MUTANT_SHARED_READER -- the old shape: one ArchiveReader for
//     every concurrent operation. (1) must go red.
//   * PRISM_C25_MUTANT_NO_MOTW      -- stamping skipped. (2) must go red.
// The verdict shape is D26's: NO WILL_FAIL. The mutant prints a banner (proof
// the injection compiled in) and, per D27, arms the marker BEFORE the first
// assertion; mini_test's ReportFailure prints "C25 MUTATION CAUGHT: <name>" at
// the first failure and leaves with 0 immediately. That marker is the target's
// PASS_REGULAR_EXPRESSION, and teardown cannot swallow it. If the injection ever
// stopped applying, the suite would stay green, the marker would be absent and
// the target would fail.
//
// D1: no silent skip. A missing 7z.dll/7z.exe prints WHY and exits 77, which the
// suite maps to SKIP_RETURN_CODE, and the stdout marker is required all the same.

#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
namespace sched = prism::core::sched;
namespace win = prism::win;
using prism::ArchiveItem;

namespace {

constexpr size_t kItemBytes = 64u * 1024u;
#ifdef PRISM_C25_MUTANT_SHARED_READER
constexpr bool kSharedReaderMutant = true;
#else
constexpr bool kSharedReaderMutant = false;
#endif
#ifdef PRISM_C25_MUTANT_NO_MOTW
constexpr bool kNoMotwMutant = true;
#else
constexpr bool kNoMotwMutant = false;
#endif

constexpr char kZoneBlob[] =
    "[ZoneTransfer]\r\n"
    "ZoneId=3\r\n"
    "HostUrl=https://example.com/payload.zip\r\n"
    "ReferrerUrl=https://example.com/\r\n";

std::string Sha256Hex(const std::vector<uint8_t>& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
    unsigned char digest[32] = {};
    if (BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >=
            0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
      static const char* kHex = "0123456789abcdef";
      hex.reserve(64);
      for (const unsigned char byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0F]);
      }
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

std::vector<uint8_t> MakePayload(uint32_t seed, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  uint32_t state = seed * 2654435761u + 1013904223u;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
  std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

// Reads any file (or ADS) through the same extended-path form MoTW uses (B4).
std::string ReadStreamBytes(const std::wstring& path) {
  const std::wstring full = win::motw::detail::ToExtendedPath(path);
  HANDLE file = CreateFileW(full.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  std::string out;
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t total = 0;
    while (total < out.size()) {
      DWORD got = 0;
      if (!ReadFile(file, out.data() + total, static_cast<DWORD>(out.size() - total), &got,
                    nullptr) ||
          got == 0) {
        out.clear();
        break;
      }
      total += got;
    }
  }
  CloseHandle(file);
  return out;
}

bool WriteAds(const std::wstring& file, const std::string& blob) {
  const std::wstring stream = win::motw::detail::ToExtendedPath(file) + L":Zone.Identifier";
  HANDLE handle = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(handle, blob.data(), static_cast<DWORD>(blob.size()), &written,
                            nullptr) != FALSE &&
                  written == blob.size();
  CloseHandle(handle);
  return ok;
}

std::string ReadAds(const std::wstring& file) {
  return ReadStreamBytes(file + L":Zone.Identifier");
}

// 7z.exe with an explicit working directory: the STORED paths are relative to
// it, which is how the fixture gets "pkg/a.bin" (PackSolid's absolute paths
// would store the whole temp path).
bool Run7z(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 60'000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

struct Fixture {
  // First member = destroyed last: the reader and the library are closed before
  // the fixture tree goes away (card C36, Tests/win/UniqueTempDir.hpp).
  std::unique_ptr<prism::testtmp::UniqueDir> scratch;
  std::wstring dllPath;
  std::wstring exePath;
  std::wstring root;
  std::wstring archivePath;
  win::SevenZipLibrary library;
  std::vector<ArchiveItem> catalog;
  std::string aSha;
  std::string bSha;
  bool ready = false;
  std::string why;
};

Fixture& World() {
  static Fixture fixture;
  return fixture;
}

bool SetupFixture() {
  Fixture& fx = World();
  if (fx.ready) return true;
  if (!fx.why.empty()) {
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  fx.dllPath = prism::fixture::DllPath();
  fx.exePath = prism::fixture::ExePath();
  if (fx.dllPath.empty() || fx.exePath.empty()) {
    fx.why = "7z.dll/7z.exe not available";  // main() turns this into a SKIP
    return false;
  }
  // Unique per process (card C36): "%TEMP%\prism_c25_wiring" was a constant, so
  // two parallel ctest processes packed and wiped the same directory.
  fx.scratch = std::make_unique<prism::testtmp::UniqueDir>(L"c36", L"wiring");
  if (!fx.scratch->ok()) {
    fx.why = "cannot create the fixture directory: " + fx.scratch->error();
    return false;
  }
  const fs::path& root = fx.scratch->path();
  std::error_code ec;
  fs::create_directories(root / "pkg", ec);
  fx.root = root.wstring();
  fx.archivePath = (root / "pool.7z").wstring();

  const std::vector<uint8_t> a = MakePayload(0xC25A, kItemBytes);
  const std::vector<uint8_t> b = MakePayload(0xC25B, kItemBytes);
  fx.aSha = Sha256Hex(a);
  fx.bSha = Sha256Hex(b);
  const std::string aBytes(reinterpret_cast<const char*>(a.data()), a.size());
  const std::string bBytes(reinterpret_cast<const char*>(b.data()), b.size());
  if (!WriteBytes((root / "pkg" / "a.bin").wstring(), aBytes) ||
      !WriteBytes((root / "pkg" / "b.bin").wstring(), bBytes) ||
      !WriteBytes((root / "download.html").wstring(), "<html>visit example</html>")) {
    fx.why = "cannot write the fixture payloads";
    return false;
  }
  // Stored paths come from the working directory: "pkg/a.bin", "download.html".
  if (!Run7z(fx.exePath, L"a -t7z -ms=on \"" + fx.archivePath + L"\" pkg download.html",
             fx.root)) {
    fx.why = "7z.exe could not author the fixture archive";
    return false;
  }
  // The archive must carry a download mark: that is what extraction inherits.
  if (!WriteAds(fx.archivePath, kZoneBlob)) {
    fx.why = "cannot write :Zone.Identifier on the fixture archive (NTFS ADS required)";
    return false;
  }
  if (!fx.library.Load(fx.dllPath)) {
    fx.why = "LoadLibrary(7z.dll) failed";
    return false;
  }
  win::ArchiveReader probe(fx.library);
  if (!probe.Open(fx.archivePath)) {
    fx.why = "probe reader could not open the fixture: " + probe.openTrace();
    return false;
  }
  fx.catalog = probe.ReadCatalog();
  size_t files = 0;
  for (const ArchiveItem& item : fx.catalog) {
    if (!item.isDir) ++files;
  }
  if (files != 3) {
    fx.why = "fixture catalog should hold 3 files, holds " + std::to_string(files);
    return false;
  }
  fx.ready = true;
  return true;
}

std::shared_ptr<app::OperationHandles> MakeOps(size_t slots) {
  app::OperationHandles::Config config;
  config.archivePath = World().archivePath;
  config.handlerSlots = slots;
  std::string error;
  auto ops = app::OperationHandles::Create(World().library, config, &error);
  if (!ops) std::cout << "    OperationHandles::Create failed: " << error << std::endl;
  return ops;
}

std::wstring FreshDir(const wchar_t* name) {
  // A per-case subdirectory of a SHARED constant is still a shared path; hang
  // the case directory off this process's own fixture root instead (card C36).
  if (World().root.empty()) return {};
  const fs::path dir = fs::path(World().root) / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir.wstring();
}

// Relative, '/'-separated file list under a directory -- the on-disk layout,
// which is what a plan actually MEANS (D2: assert content, not existence).
std::vector<std::string> LayoutOf(const std::wstring& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(fs::path(dir), ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string relative = entry.path().lexically_relative(fs::path(dir)).generic_string();
    if (relative.empty()) continue;
    out.push_back(relative);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string JoinList(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    if (!out.empty()) out += ",";
    out += item;
  }
  return out;
}

bool HasFile(const std::vector<std::string>& layout, const std::string& relative) {
  for (const std::string& item : layout) {
    if (item == relative) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------- concurrency
// Both workers park INSIDE their engine call (GetStream), so "these operations
// overlapped" becomes a fact about 7z.dll rather than about two threads that
// merely started. Bounded: a shared-handler regression must FAIL with a
// readable verdict, not hang the lane.
class BeginGate {
 public:
  explicit BeginGate(int expected) : expected_(expected) {}
  void ArriveAndWait() {
    arrived.fetch_add(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (arrived.load() < expected_) {
      if (std::chrono::steady_clock::now() > deadline) {
        timedOut.store(true);
        return;
      }
      std::this_thread::yield();
    }
  }
  std::atomic<int> arrived{0};
  std::atomic<bool> timedOut{false};

 private:
  int expected_;
};

class GateCollector final : public win::abi::IArchiveExtractCallback {
 public:
  GateCollector(size_t itemCount, BeginGate* gate)
      : sinks_(itemCount, nullptr), sha_(itemCount), bytes_(itemCount), gate_(gate) {}
  ~GateCollector() {
    for (auto* sink : sinks_) {
      if (sink) sink->Release();
    }
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == win::abi::IID_IArchiveExtractCallback) {
      *out = static_cast<win::abi::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP SetTotal(win::abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const win::abi::UInt64*) override { return S_OK; }
  STDMETHODIMP PrepareOperation(win::abi::Int32) override { return S_OK; }

  STDMETHODIMP GetStream(win::abi::UInt32 index, win::abi::ISequentialOutStream** outStream,
                         win::abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != win::abi::kExtract) return S_OK;
    if (index >= sinks_.size()) return E_INVALIDARG;
    if (gate_ != nullptr && !gateHit_) {
      gateHit_ = true;
      gate_->ArriveAndWait();
    }
    auto* sink = new win::MemoryOutStream(4u << 20);
    sink->AddRef();
    sinks_[index] = sink;
    *outStream = sink;
    pending_ = index;
    return S_OK;
  }

  STDMETHODIMP SetOperationResult(win::abi::Int32 operationResult) override {
    if (operationResult != 0) failed_ = true;
    if (pending_ < sinks_.size() && sinks_[pending_] != nullptr) {
      auto* sink = sinks_[pending_];
      bytes_[pending_] = sink->bytes().size();
      sha_[pending_] = Sha256Hex(sink->bytes());
      sink->Release();
      sinks_[pending_] = nullptr;
      ++itemsSeen_;
    }
    pending_ = static_cast<size_t>(-1);
    return S_OK;
  }

  bool failed() const { return failed_; }
  int itemsSeen() const { return itemsSeen_; }
  const std::string& sha(size_t index) const { return sha_[index]; }
  size_t bytes(size_t index) const { return bytes_[index]; }

 private:
  std::vector<win::MemoryOutStream*> sinks_;
  std::vector<std::string> sha_;
  std::vector<size_t> bytes_;
  BeginGate* gate_ = nullptr;
  bool gateHit_ = false;
  size_t pending_ = static_cast<size_t>(-1);
  int itemsSeen_ = 0;
  bool failed_ = false;
  LONG refs_ = 1;
};

struct DecodeOutcome {
  bool ok = false;
  int itemsSeen = 0;
  std::vector<std::string> sha;
  std::vector<size_t> bytes;
};

DecodeOutcome DecodeAll(win::ArchiveReader& reader, const std::vector<ArchiveItem>& catalog,
                        BeginGate* gate) {
  DecodeOutcome out;
  out.sha.assign(catalog.size(), std::string());
  out.bytes.assign(catalog.size(), size_t(0));
  auto* collector = new GateCollector(catalog.size(), gate);
  std::vector<uint32_t> indices;
  for (const ArchiveItem& item : catalog) {
    if (!item.isDir) indices.push_back(item.index);
  }
  const bool callOk = reader.ExtractBatch(indices, collector);
  out.itemsSeen = collector->itemsSeen();
  for (size_t i = 0; i < catalog.size(); ++i) {
    out.sha[i] = collector->sha(i);
    out.bytes[i] = collector->bytes(i);
  }
  out.ok = callOk && !collector->failed();
  collector->Release();
  return out;
}

// Copies the fixture archive (bytes AND its :Zone.Identifier) into a directory
// of its own, so an extraction's on-disk layout can be read without the
// fixture's own payload files confusing it.
std::wstring ArchiveCopyIn(const std::wstring& dirName) {
  if (World().root.empty()) return {};
  const fs::path dir = fs::path(World().root) / dirName;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::wstring target = (dir / "pool.7z").wstring();
  std::error_code copyError;
  fs::copy_file(fs::path(World().archivePath), fs::path(target),
                fs::copy_options::overwrite_existing, copyError);
  if (copyError) return {};
  WriteAds(target, ReadAds(World().archivePath));
  return target;
}

int main(int argc, char** argv) {
  if (kSharedReaderMutant) {
    std::cout << "C25 MUTANT ACTIVE: PRISM_C25_MUTANT_SHARED_READER -- one ArchiveReader for "
                 "every concurrent operation (the pre-C11 shape)"
              << std::endl;
  }
  if (kNoMotwMutant) {
    std::cout << "C25 MUTANT ACTIVE: PRISM_C25_MUTANT_NO_MOTW -- Zone.Identifier stamping "
                 "skipped"
              << std::endl;
  }
  if (kSharedReaderMutant || kNoMotwMutant) {
    // CONTRACTS D27: the verdict is armed before the first assertion; the first
    // failed check prints the marker and exits 0 on the spot, so a mutant that
    // walks into the shared-reader premise cannot lose its verdict to teardown.
    // D3: only a mutant build has an injection to claim (the banner above is
    // the compiled-in proof); the predicate keeps a clean build untouched.
    prism::test::SetMutationVerdict(
        kSharedReaderMutant ? "C25 MUTATION CAUGHT: shared_reader"
                            : "C25 MUTATION CAUGHT: no_motw",
        [shared = kSharedReaderMutant, motw = kNoMotwMutant] { return shared || motw; });
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)" << std::endl;
    return 77;
  }
  if (!SetupFixture()) {
    const std::string why = World().why;
    CoUninitialize();
    if (why == "7z.dll/7z.exe not available") {
      // D1: a skip must say WHY, and exit with the code CTest maps to "Skipped".
      std::cout << "SKIP: 7z.dll/7z.exe not available -- set PRISM_7Z_DLL to a 7z.dll that has "
                   "7z.exe beside it (windows-latest ships 7-Zip)"
                << std::endl;
      return 77;
    }
    std::cout << "FAIL: fixture setup: " << why << std::endl;
    return 1;
  }
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();

  // CONTRACTS D26 + D27: a mutation build's verdict is a CONJUNCTION printed by
  // the suite itself, never WILL_FAIL (which cannot be combined with a pass
  // marker, and which would turn a SKIP on a 7z-less runner into a green).
  //   * marker printed  <=> the injection was really compiled in (banner above)
  //                       AND the suite went red (printed at the FIRST failure);
  //   * if the mutation silently stopped applying, the suite stays green, the
  //     marker is absent and this target goes RED -- the injection proves itself.
  // Reaching here with a red suite means a failure bypassed ReportFailure; the
  // normal verdict already left with 0 from inside the first failed check.
  if (kSharedReaderMutant || kNoMotwMutant) {
    if (rc == 0) {
      std::cout << "C25 MUTATION NOT CAUGHT: the suite stayed green with the regression "
                   "compiled in -- the injection is stale"
                << std::endl;
      return 1;
    }
    std::cout << "C25 MUTATION CAUGHT: " << (kSharedReaderMutant ? "shared_reader" : "no_motw")
              << " (suite red, banner above; see the FAIL lines for the contract it broke)"
              << std::endl;
    return 0;
  }

  if (rc != 0) return rc;
  std::cout << "ALL APP-WIRING CHECKS PASSED" << std::endl;
  return 0;
}

// ---------------------------------------------------------------------------
// (1) One handler per concurrent operation (C11 debt fix).
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.sweep_and_extract_own_distinct_handlers") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto ops = MakeOps(2);
  if (!PRISM_CHECK(ops != nullptr)) return;
  PRISM_CHECK_EQ(ops->capacity(), size_t(2));
  PRISM_CHECK_EQ(ops->poolStats().sharedHandlerInjected, false);

  BeginGate gate(2);
  std::atomic<int> live{0};
  std::atomic<int> peak{0};
  DecodeOutcome sweep;
  DecodeOutcome extract;
  size_t sweepSlot = 999;
  size_t extractSlot = 999;
  win::ArchiveReader* sweepReader = nullptr;
  win::ArchiveReader* extractReader = nullptr;

  std::thread sweepThread([&] {
    const int now = ++live;
    int seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
    std::string why;
    auto task = ops->Begin(sched::Kind::kSweep, sched::Priority::kBackground, "sweep:c25",
                           "sweep", &why);
    if (task) {
      sweepSlot = task->slot;
      sweepReader = task->reader;
      sweep = DecodeAll(*task->reader, fx.catalog, &gate);
      ops->Finish(*task, sweep.ok);
    } else {
      std::cout << "    sweep Begin refused: " << why << std::endl;
    }
    --live;
  });
  std::thread extractThread([&] {
    const int now = ++live;
    int seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
    std::string why;
    auto task = ops->Begin(sched::Kind::kExtract, sched::Priority::kUser, "extract:c25",
                           "extract", &why);
    if (task) {
      extractSlot = task->slot;
      extractReader = task->reader;
      extract = DecodeAll(*task->reader, fx.catalog, &gate);
      ops->Finish(*task, extract.ok);
    } else {
      std::cout << "    extract Begin refused: " << why << std::endl;
    }
    --live;
  });
  sweepThread.join();
  extractThread.join();

  // THE assertion of the card: two operations, two slots, two handlers.
  PRISM_CHECK(sweepSlot != extractSlot);
  PRISM_CHECK(sweepReader != nullptr && extractReader != nullptr);
  PRISM_CHECK(sweepReader != extractReader);
  // ... and they really were simultaneous inside the engine, not serialized.
  PRISM_CHECK(!gate.timedOut.load());
  PRISM_CHECK_EQ(peak.load(), 2);
  PRISM_CHECK_EQ(ops->poolStats().maxConcurrent, size_t(2));
  PRISM_CHECK_EQ(ops->schedulerStats().peakRunning, size_t(2));
  PRISM_CHECK_EQ(ops->distinctHandlersInUse(), size_t(0));  // both finished

  // No cross-talk: every byte each side decoded hashes to its own payload.
  if (!PRISM_CHECK(sweep.ok)) return;
  if (!PRISM_CHECK(extract.ok)) return;
  for (const ArchiveItem& item : fx.catalog) {
    if (item.isDir) continue;
    const std::string expected =
        item.path == "pkg/a.bin" ? fx.aSha : (item.path == "pkg/b.bin" ? fx.bSha : std::string());
    if (expected.empty()) continue;
    if (!PRISM_CHECK_EQ(sweep.sha[item.index], expected)) return;
    if (!PRISM_CHECK_EQ(extract.sha[item.index], expected)) return;
    PRISM_CHECK_EQ(sweep.bytes[item.index], kItemBytes);
    PRISM_CHECK_EQ(extract.bytes[item.index], kItemBytes);
  }

  // Backpressure is an answer, not a block: both slots are free again, so a
  // third lease succeeds, and a fourth does not.
  auto first = ops->Begin(sched::Kind::kExtract, sched::Priority::kUser, "probe:1", "probe");
  auto second = ops->Begin(sched::Kind::kExtract, sched::Priority::kUser, "probe:2", "probe");
  if (!PRISM_CHECK(first.has_value() && second.has_value())) return;
  PRISM_CHECK(first->slot != second->slot);
  const auto third = ops->Begin(sched::Kind::kExtract, sched::Priority::kUser, "probe:3", "probe");
  PRISM_CHECK(!third.has_value());
  ops->Finish(*first, true);
  ops->Finish(*second, true);
  ops->Close();
}

// ---------------------------------------------------------------------------
// (2) :Zone.Identifier inheritance (C5) through the REAL extraction path.
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.extracted_files_inherit_the_zone_identifier_verbatim") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto ops = MakeOps(2);
  if (!PRISM_CHECK(ops != nullptr)) return;

  const std::wstring archive = ArchiveCopyIn(L"motw");
  if (!PRISM_CHECK(!archive.empty())) return;
  const app::ExtractionPlan plan = app::BuildExtractionPlan(
      app::LaunchMode::kSmartExtract, win::WideToUtf8(archive), fx.catalog,
      [](const std::string&) { return false; });
  // The promo file (download.html) is filtered by the smart planner: two writes.
  const app::ExtractionOutcome outcome =
      app::RunExtraction(*ops, fx.catalog, plan, archive, /*scanExecutables=*/false);
  if (!PRISM_CHECK(outcome.ok)) {
    std::cout << "    extract failed: " << outcome.error << std::endl;
    return;
  }
  PRISM_CHECK_EQ(outcome.filesWritten, uint64_t(2));
  PRISM_CHECK_EQ(static_cast<int>(outcome.security.motwSource),
                 static_cast<int>(win::motw::SourceState::Present));
  PRISM_CHECK_EQ(outcome.security.filesStamped, size_t(2));
  PRISM_CHECK_EQ(outcome.security.stampFailed, size_t(0));

  const std::string expectedBlob = ReadAds(archive);
  if (!PRISM_CHECK_EQ(expectedBlob.size(), std::string(kZoneBlob).size())) return;
  if (!PRISM_CHECK_EQ(expectedBlob, std::string(kZoneBlob))) return;

  const fs::path dir = fs::path(archive).parent_path();
  size_t checked = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (entry.path().extension() == ".7z") continue;
    const std::string blob = ReadAds(entry.path().wstring());
    // D2: the CONTENT must match, not merely "a stream exists".
    if (!PRISM_CHECK_EQ(blob, expectedBlob)) {
      std::cout << "    " << entry.path().filename().string() << " carries " << blob.size()
                << " bytes, the source mark has " << expectedBlob.size() << std::endl;
    }
    ++checked;
  }
  PRISM_CHECK_EQ(checked, size_t(2));

  // The other half of the contract: an archive WITHOUT a mark must not invent
  // one (a stamp test that only ever sees a source mark cannot tell).
  // NOTE: CopyFileW (and therefore fs::copy_file) copies alternate data streams
  // too, so a copy would still carry the mark. Write the BYTES into a new file:
  // only then is "the archive has no mark" a real premise (CONTRACTS D18).
  const std::string archiveBytes = ReadStreamBytes(fx.archivePath);
  if (!PRISM_CHECK(!archiveBytes.empty())) return;
  const fs::path bare = dir / "bare.7z";
  if (!PRISM_CHECK(WriteBytes(bare.wstring(), archiveBytes))) return;
  const fs::path target = dir / "stamp-probe.bin";
  PRISM_CHECK(WriteBytes(target.wstring(), "probe"));
  const app::SecuritySummary plain =
      app::ApplySecurityPass(bare.wstring(), {win::WideToUtf8(target.wstring())}, false);
  PRISM_CHECK_EQ(static_cast<int>(plain.motwSource),
                 static_cast<int>(win::motw::SourceState::Absent));
  PRISM_CHECK_EQ(plain.filesStamped, size_t(0));
  PRISM_CHECK_EQ(plain.stampFailed, size_t(0));
  PRISM_CHECK(ReadAds(target.wstring()).empty());
  ops->Close();
}

// ---------------------------------------------------------------------------
// (3) The four context-menu verbs reach four DIFFERENT plans (C4 contract).
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.four_context_menu_verbs_reach_four_plans") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto ops = MakeOps(2);
  if (!PRISM_CHECK(ops != nullptr)) return;

  // 3a. C4's verb table and this parser agree, verb for verb. The arguments are
  // built by ArgsForVerb (what the shell DLL really launches) and parsed by
  // ParseLaunch (what the exe really reads), so neither half can drift alone.
  struct VerbCase {
    const char* verb;
    app::LaunchMode mode;
  };
  const VerbCase cases[] = {
      {"smart.extract.here", app::LaunchMode::kSmartExtract},
      {"extract.here", app::LaunchMode::kExtractHere},
      {"extract.to.subfolder", app::LaunchMode::kExtractSub},
      {"test.integrity", app::LaunchMode::kTest},
      {"open.in.prism", app::LaunchMode::kGallery},
  };
  for (const VerbCase& one : cases) {
    const std::wstring rendered = win::shell_launch::ArgsForVerb(one.verb, {fx.archivePath});
    if (!PRISM_CHECK(!rendered.empty())) continue;
    const std::wstring command = L"PrismArchive.exe " + rendered;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command.c_str(), &argc);
    if (!PRISM_CHECK(argv != nullptr)) continue;
    std::vector<std::wstring> argvVec;
    for (int i = 1; i < argc; ++i) argvVec.emplace_back(argv[i]);
    LocalFree(argv);
    const app::LaunchRequest request = app::ParseLaunch(argvVec);
    PRISM_CHECK_EQ(static_cast<int>(request.mode), static_cast<int>(one.mode));
    PRISM_CHECK(!request.unknownFlag);
    PRISM_CHECK(request.archive == fx.archivePath);
  }
  // A verb we do not implement is REFUSED, never guessed into a command line.
  PRISM_CHECK(win::shell_launch::ArgsForVerb("frobnicate.everything", {fx.archivePath}).empty());
  {
    const app::LaunchRequest refused = app::ParseLaunch({L"--compress-7z", L"x.7z"});
    PRISM_CHECK(refused.unknownFlag);
    PRISM_CHECK(refused.mode == app::LaunchMode::kGallery);  // no silent fallback plan
  }

  // 3b. The three extracting verbs produce three different LAYOUTS on disk.
  struct LayoutCase {
    const wchar_t* dir;
    app::LaunchMode mode;
    const char* expected;  // sorted, '/'-separated, archive excluded
  };
  const LayoutCase layouts[] = {
      {L"mode-smart", app::LaunchMode::kSmartExtract, "a.bin,b.bin"},
      {L"mode-here", app::LaunchMode::kExtractHere,
       "download.html,pkg/a.bin,pkg/b.bin"},
      {L"mode-sub", app::LaunchMode::kExtractSub,
       "pool/download.html,pool/pkg/a.bin,pool/pkg/b.bin"},
  };
  for (const LayoutCase& one : layouts) {
    const std::wstring archive = ArchiveCopyIn(one.dir);
    if (!PRISM_CHECK(!archive.empty())) return;
    const app::ExtractionPlan plan = app::BuildExtractionPlan(
        one.mode, win::WideToUtf8(archive), fx.catalog,
        [](const std::string&) { return false; });
    PRISM_CHECK_EQ(static_cast<int>(plan.mode), static_cast<int>(one.mode));
    PRISM_CHECK(!plan.verifyOnly);
    const app::ExtractionOutcome outcome =
        app::RunExtraction(*ops, fx.catalog, plan, archive, /*scanExecutables=*/false);
    if (!PRISM_CHECK(outcome.ok)) {
      std::cout << "    " << win::WideToUtf8(one.dir) << " failed: " << outcome.error
                << std::endl;
      return;
    }
    std::vector<std::string> produced;
    for (const std::string& relative : LayoutOf(fs::path(archive).parent_path().wstring())) {
      if (relative.size() > 3 && relative.substr(relative.size() - 3) == ".7z") continue;
      produced.push_back(relative);
    }
    if (!PRISM_CHECK_EQ(JoinList(produced), std::string(one.expected))) {
      std::cout << "    " << win::WideToUtf8(one.dir) << " layout: " << JoinList(produced)
                << std::endl;
    }
  }

  // 3c. --test is verify-only: it decodes everything and writes NOTHING.
  {
    const std::wstring archive = ArchiveCopyIn(L"mode-test");
    if (!PRISM_CHECK(!archive.empty())) return;
    const app::ExtractionPlan plan = app::BuildExtractionPlan(
        app::LaunchMode::kTest, win::WideToUtf8(archive), fx.catalog,
        [](const std::string&) { return false; });
    PRISM_CHECK(plan.verifyOnly);
    PRISM_CHECK(plan.plan.baseTargetDir.empty());
    uint64_t expectedBytes = 0;  // every non-dir entry, promo noise included
    for (const ArchiveItem& item : fx.catalog) {
      if (!item.isDir) expectedBytes += item.size;
    }
    const app::VerifyOutcome outcome = app::VerifyIntegrity(*ops, fx.catalog, archive);
    if (!PRISM_CHECK(outcome.ok)) return;
    PRISM_CHECK_EQ(outcome.items, uint64_t(3));
    PRISM_CHECK_EQ(outcome.bytes, expectedBytes);
    std::vector<std::string> produced;
    for (const std::string& relative :
         LayoutOf(fs::path(archive).parent_path().wstring())) {
      if (relative.size() > 3 && relative.substr(relative.size() - 3) == ".7z") continue;
      produced.push_back(relative);
    }
    PRISM_CHECK_EQ(produced.size(), size_t(0));
  }
  ops->Close();
}

// ---------------------------------------------------------------------------
// (4)+(5) The key table and the list view, on a REAL HWND.
// ---------------------------------------------------------------------------
namespace {

using prism::win::gallery::GalleryWindow;

std::vector<ArchiveItem> KeyCatalog() {
  std::vector<ArchiveItem> items;
  const auto add = [&items](const char* path, bool isDir, uint64_t size) {
    ArchiveItem item;
    item.index = static_cast<uint32_t>(items.size());
    item.path = path;
    item.isDir = isDir;
    item.size = size;
    items.push_back(item);
  };
  add("docs", true, 0);              // catalog 0
  add("pack", true, 0);              // catalog 1
  add("pack/img1.png", false, 120);  // catalog 2
  add("pack/note.txt", false, 30);   // catalog 3
  add("root.png", false, 500);       // catalog 4
  return items;
}

std::vector<ArchiveItem> SortCatalog() {
  std::vector<ArchiveItem> items;
  const auto add = [&items](const char* path, bool isDir, uint64_t size) {
    ArchiveItem item;
    item.index = static_cast<uint32_t>(items.size());
    item.path = path;
    item.isDir = isDir;
    item.size = size;
    items.push_back(item);
  };
  add("docs", true, 0);        // catalog 0
  add("big.bin", false, 900);  // catalog 1
  add("small.bin", false, 10); // catalog 2
  add("mid.png", false, 300);  // catalog 3
  return items;
}

int RowOf(const GalleryWindow& window, const char* path) {
  const std::vector<ArchiveItem>& rows = window.Items();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == path) return static_cast<int>(i);
  }
  return -1;
}

// Ctrl/Shift/Alt for the REAL WM_KEYDOWN path: the window reads GetKeyState, so
// the thread's keyboard state is what has to hold the chord (test_keys proved
// this works on the CI window station).
bool SetCtrlDown(bool down) {
  BYTE state[256] = {};
  if (!GetKeyboardState(state)) return false;
  state[VK_CONTROL] = down ? 0x80 : 0x00;
  state[VK_SHIFT] = 0x00;
  state[VK_MENU] = 0x00;
  return SetKeyboardState(state) != FALSE;
}

}  // namespace

PRISM_TEST("wiring.key_table_changes_window_state") {
  std::vector<ArchiveItem> items = KeyCatalog();
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C25 keys", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.SetRootLabel(L"keys.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));

  int activated = -1;
  window.SetActivateHandler([&activated](int catalogIndex) { activated = catalogIndex; });

  // (a) Ctrl+2 / Ctrl+1 through the real WM_KEYDOWN (the table, not an if-chain).
  if (!PRISM_CHECK(SetCtrlDown(true))) return;
  window.Dispatch(WM_KEYDOWN, static_cast<WPARAM>('2'), 0);
  PRISM_CHECK(window.ListMode());
  window.Dispatch(WM_KEYDOWN, static_cast<WPARAM>('1'), 0);
  PRISM_CHECK(!window.ListMode());
  PRISM_CHECK(SetCtrlDown(false));
  // A bare digit is NOT the chord: no sticky mode switching.
  window.Dispatch(WM_KEYDOWN, static_cast<WPARAM>('2'), 0);
  PRISM_CHECK(!window.ListMode());

  // (b) Ctrl+K opens the palette; Escape closes it (the palette owns Escape).
  window.DispatchKeyStroke({static_cast<uint32_t>('K'), true, false, false});
  PRISM_CHECK(window.PaletteOpen());
  PRISM_CHECK(window.Palette().RowCount() > 0);
  window.DispatchKeyStroke({prism::keys::kKeyEscape, false, false, false});
  PRISM_CHECK(!window.PaletteOpen());

  // (c) Ctrl+E = smart extract of the selection: the app's activate door with the
  // CATALOG index (4 == root.png), so the app can plan over it.
  window.SetSelection(RowOf(window, "root.png"));
  window.DispatchKeyStroke({static_cast<uint32_t>('E'), true, false, false});
  PRISM_CHECK_EQ(activated, 4);

  // (d) Enter drills into a folder; Alt+Left walks the history back; Alt+Right
  // returns; Backspace (a bare key) is still GoUp.
  window.SetSelection(RowOf(window, "pack"));
  window.Dispatch(WM_KEYDOWN, VK_RETURN, 0);
  PRISM_CHECK_EQ(window.Folder(), std::string("pack"));
  PRISM_CHECK(window.HistoryBackCount() >= 1);
  window.DispatchKeyStroke({prism::keys::kKeyLeft, false, false, true});
  PRISM_CHECK_EQ(window.Folder(), std::string(""));
  PRISM_CHECK(window.HistoryForwardCount() >= 1);
  window.DispatchKeyStroke({prism::keys::kKeyRight, false, false, true});
  PRISM_CHECK_EQ(window.Folder(), std::string("pack"));
  window.Dispatch(WM_KEYDOWN, VK_BACK, 0);
  PRISM_CHECK_EQ(window.Folder(), std::string(""));

  // (e) The sidebar is fed from the same catalog (C12): a root row plus the
  // folders, and a status pill that knows the totals.
  const prism::win::sidebar::Layout& sidebar = window.SidebarLayout();
  PRISM_CHECK(!sidebar.commands.empty());
  PRISM_CHECK(window.SidebarState().Tree().size() >= 3);  // root + docs + pack
  const prism::sidebar::Status status = window.SidebarState().ComputeStatus();
  PRISM_CHECK_EQ(status.fileCount, size_t(3));

  window.Dispatch(WM_DESTROY, 0, 0);
}

PRISM_TEST("wiring.list_view_is_the_sorted_filtered_model") {
  std::vector<ArchiveItem> items = SortCatalog();
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C25 list", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.SetRootLabel(L"sort.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));
  window.SetListMode(true);

  // Default order: folders first, then name ascending -- the navigator's order,
  // so switching the view on changes no pixels.
  PRISM_CHECK_EQ(window.ListRowCount(), 4);
  if (!PRISM_CHECK_EQ(window.ListRows().size(), size_t(4))) return;
  PRISM_CHECK(window.ListRows()[0].isDir);
  PRISM_CHECK_EQ(window.ListRows()[0].name, std::string("docs"));
  PRISM_CHECK_EQ(window.ListRows()[1].name, std::string("big.bin"));
  PRISM_CHECK_EQ(window.ListRows()[2].name, std::string("mid.png"));
  PRISM_CHECK_EQ(window.ListRows()[3].name, std::string("small.bin"));

  // Sort by size, descending: the view re-orders, the folder rows do not.
  prism::list::MultiSortSpec spec;
  spec.keys.push_back({prism::list::SortKey::kOriginalSize, prism::list::SortDirection::kDescending});
  spec.dirsFirst = true;
  window.SetSortSpec(spec);
  PRISM_CHECK_EQ(window.ListRows().size(), size_t(4));
  PRISM_CHECK_EQ(window.ListRows()[0].name, std::string("docs"));       // dirs first
  PRISM_CHECK_EQ(window.ListRows()[1].name, std::string("big.bin"));    // 900
  PRISM_CHECK_EQ(window.ListRows()[2].name, std::string("mid.png"));    // 300
  PRISM_CHECK_EQ(window.ListRows()[3].name, std::string("small.bin"));  // 10
  PRISM_CHECK_EQ(window.ItemCount(), 4);  // the grid is untouched

  // Filter: glob "*.png" leaves exactly one row, and the VIEW row maps back to
  // the folder row that holds it -- this is what stops "sort by size" from
  // activating a random file.
  prism::list::FilterSpec filter;
  filter.glob = "*.png";
  window.SetFilter(filter);
  PRISM_CHECK_EQ(window.ListRowCount(), 1);
  PRISM_CHECK_EQ(window.ListRows().size(), size_t(4));  // model unchanged
  const int midFolderRow = RowOf(window, "mid.png");
  PRISM_CHECK_EQ(window.FolderRowForListRow(0), midFolderRow);

  // Activating through the view still reaches the right entry.
  int activated = -1;
  window.SetActivateHandler([&activated](int catalogIndex) { activated = catalogIndex; });
  window.SetSelection(midFolderRow);
  window.Dispatch(WM_KEYDOWN, VK_RETURN, 0);
  PRISM_CHECK_EQ(activated, 3);  // catalog index of mid.png

  // A filtered-out row has no view row (and therefore no selection pill).
  PRISM_CHECK_EQ(window.ListRowForFolderRow(RowOf(window, "big.bin")), -1);

  // Clearing the filter restores every row, in the active sort order.
  window.SetFilter(prism::list::FilterSpec{});
  PRISM_CHECK_EQ(window.ListRowCount(), 4);
  window.Dispatch(WM_DESTROY, 0, 0);
}
