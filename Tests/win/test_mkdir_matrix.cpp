// Windows mkdir VARIANT MATRIX (batch B1 evidence round): ONE executable,
// EIGHT parameterized "commit a new empty directory into a solid .7z"
// variants, each against its OWN fixture (3 WIC-authored PNGs packed solid
// by the 7-Zip CLI, SolidFixture.hpp style) and its OWN UpdateItems commit +
// independent re-open readback. The point is CI latency: instead of one
// hypothesis per run, every shape of the IArchiveUpdateCallback answer is
// probed in a single execution.
//
// The shipped CommitTree() VIRTUALIZES brand-new empty folders (CONTRACTS
// A10), so this probe deliberately bypasses it and drives
// IOutArchive::UpdateItems() directly through a parameterized clone of
// VirtualTreeExecutor's TreeUpdateCallback (MatrixUpdateCallback below).
// Untouched items ride along exactly as the shipped executor plans them
// (newData=0 newProps=0, properties forwarded from the open handler).
//
// Variant dimensions:
//   newData/newProps  (0,1) metadata-only vs (1,1) fresh-data node
//   kpidIsDir         VT_BOOL(TRUE) vs VT_EMPTY
//   kpidAttrib        FILE_ATTRIBUTE_DIRECTORY vs VT_EMPTY
//   new path          "docs" vs "docs/" (trailing slash)
//   GetStream         (newData=1 nodes only) S_OK + NULL stream
//
// Output contract (one machine-greppable line per variant):
//   VARIANT <name>: committed=[path(dir|file)|docs(file:0B)|docs(DROPPED), ...]
// plus RAW/COMMIT/ASK evidence lines and the final "MATRIX COMPLETE".
//
// EXIT POLICY -- REPORT-ONLY, ALWAYS 1: this run measures, it does not yet
// assert. It exits non-zero ON PURPOSE so `ctest --output-on-failure` prints
// the full table into the CI log (a passing test's stdout is swallowed by
// ctest). The verdicts (which shapes yield dir / file / dropped) are read
// from the log, not from the exit code.
//
// Crash containment: each variant runs in its OWN child process (self-spawn
// with --one <name>); an in-dll access violation kills one variant, not the
// matrix. If spawning is impossible the parent runs the variant in-process.
#include <windows.h>
#include <propidl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "VirtualTreeExecutor.hpp"

namespace {

namespace fs = std::filesystem;
namespace abi = prism::win::abi;
namespace vt = prism::win::vt;

struct VariantSpec {
  const char* name;
  int newData;
  int newProps;
  bool isDirTrue;    // kpidIsDir: VT_BOOL TRUE vs VT_EMPTY
  bool attribDir;    // kpidAttrib: FILE_ATTRIBUTE_DIRECTORY vs VT_EMPTY
  const char* path;  // "docs" or "docs/"
};

// Eight representative cells of the 32-cell factorial space: both
// newData/newProps shapes, each prop dimension toggled on/off against the
// spec-complete baseline, the trailing-slash path form, and the newData=1
// S_OK+NULL GetStream answer.
const VariantSpec kVariants[] = {
    {"meta_spec", 0, 1, true, true, "docs"},        // shipped-callback shape
    {"meta_isdir_only", 0, 1, true, false, "docs"},     // kpidAttrib VT_EMPTY
    {"meta_attrib_only", 0, 1, false, true, "docs"},    // kpidIsDir VT_EMPTY
    {"meta_bare", 0, 1, false, false, "docs"},          // both props empty
    {"meta_slash", 0, 1, true, true, "docs/"},          // trailing slash
    {"nd1_spec", 1, 1, true, true, "docs"},             // + GetStream S_OK+NULL
    {"nd1_slash", 1, 1, true, true, "docs/"},           //   + trailing slash
    {"nd1_bare", 1, 1, false, false, "docs"},           //   + minimal props
};

// Unbuffered-at-the-call-site stdout emitter: endl flushes, so everything
// printed before a crash (or a child's death) still lands in the CI log.
void Emit(const std::string& line) { std::cout << line << std::endl; }

// Parameterized clone of TreeUpdateCallback (VirtualTreeExecutor.hpp): every
// answer the engine requests comes from the VariantSpec instead of the
// shipped hardcoded policy. Old-archive items are forwarded wholesale, and
// kpidMTime is always answered (7zHandlerOut needs an MTime to sort; every
// FS-driven caller would have one) -- MTime is deliberately NOT a matrix
// dimension, rounds 9-10 already pinned it.
class MatrixUpdateCallback final : public vt::IArchiveUpdateCallback,
                                   public vt::ICryptoGetTextPassword2 {
 public:
  MatrixUpdateCallback(std::vector<vt::detail::OutEntry> entries,
                       const VariantSpec& spec, abi::IInArchive* oldArchive)
      : entries_(std::move(entries)), spec_(spec), old_(oldArchive) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == vt::kIID_IArchiveUpdateCallback) {
      *out = static_cast<vt::IArchiveUpdateCallback*>(this);
    } else if (iid == vt::kIID_IProgress) {
      *out = static_cast<abi::IProgress*>(this);
    } else if (iid == vt::kIID_ICryptoGetTextPassword2) {
      *out = static_cast<vt::ICryptoGetTextPassword2*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP SetTotal(abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*) override { return S_OK; }

  STDMETHODIMP GetUpdateItemInfo(abi::UInt32 index, abi::Int32* newData,
                                 abi::Int32* newProps,
                                 abi::UInt32* indexInArchive) override {
    if (!newData || !newProps || !indexInArchive) return E_POINTER;
    if (index >= entries_.size()) return E_INVALIDARG;
    const vt::detail::OutEntry& entry = entries_[index];
    *newData = entry.newData ? 1 : 0;
    *newProps = entry.newProps ? 1 : 0;
    *indexInArchive = static_cast<abi::UInt32>(entry.indexInArchive);  // -1 -> 0xFFFFFFFF
    return S_OK;
  }

  STDMETHODIMP GetProperty(abi::UInt32 index, PROPID propId,
                           PROPVARIANT* value) override {
    if (!value) return E_POINTER;
    PropVariantInit(value);
    if (index >= entries_.size()) return E_INVALIDARG;
    const vt::detail::OutEntry& entry = entries_[index];
    if (propId == abi::kpidPath && !entry.newPath.empty()) {
      value->vt = VT_BSTR;
      value->bstrVal = SysAllocString(entry.newPath.c_str());
      return value->bstrVal ? S_OK : E_OUTOFMEMORY;
    }
    if (entry.indexInArchive >= 0 && old_ != nullptr) {
      return old_->GetProperty(static_cast<abi::UInt32>(entry.indexInArchive),
                               propId, value);
    }
    // Brand-new node: each dimension answers per the variant spec; anything
    // the engine invents beyond that stays VT_EMPTY (recorded in the trace).
    ++asked_[propId];
    if (propId == abi::kpidIsDir) {
      if (spec_.isDirTrue) {
        value->vt = VT_BOOL;
        value->boolVal = VARIANT_TRUE;
      }
    } else if (propId == abi::kpidAttrib) {
      if (spec_.attribDir) {
        value->vt = VT_UI4;
        value->ulVal = FILE_ATTRIBUTE_DIRECTORY;
      }
    } else if (propId == abi::kpidMTime) {
      value->vt = VT_FILETIME;
      value->hVal.QuadPart = static_cast<LONGLONG>(116444736000000000LL);  // 1970-01-01
    }
    return S_OK;
  }

  STDMETHODIMP GetStream(abi::UInt32 index,
                         abi::ISequentialInStream** inStream) override {
    if (!inStream) return E_POINTER;
    *inStream = nullptr;
    ++streamAsks_;
    if (index >= entries_.size()) return E_INVALIDARG;
    if (!entries_[index].newData) {
      if (reason_.empty()) reason_ = "engine asked a stream for a metadata-only item";
      return E_NOTIMPL;  // unreachable by construction; loud on purpose
    }
    // The matrix cell under test: S_OK + NULL. 7zUpdate then treats the item
    // as content-less -- the suspected "silent 0-byte file" path.
    return S_OK;
  }

  STDMETHODIMP SetOperationResult(abi::Int32 operationResult) override {
    if (operationResult != 0 && reason_.empty()) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "SetOperationResult=%d",
                    static_cast<int>(operationResult));
      reason_ = buf;
    }
    return S_OK;
  }

  STDMETHODIMP CryptoGetTextPassword2(abi::Int32* passwordIsDefined,
                                      BSTR* password) override {
    if (!passwordIsDefined || !password) return E_POINTER;
    *passwordIsDefined = 0;  // no password from the UI layer
    *password = nullptr;
    return S_OK;
  }

  STDMETHODIMP CryptoGetTextPassword(BSTR* password) override {
    if (!password) return E_POINTER;
    *password = nullptr;
    return S_OK;
  }

  const std::string& reason() const { return reason_; }
  int streamAsks() const { return streamAsks_; }

  std::string askedSummary() const {
    std::string out;
    char buf[32];
    for (const auto& kv : asked_) {
      std::snprintf(buf, sizeof(buf), "pid%u x%d ",
                    static_cast<unsigned>(kv.first), kv.second);
      out += buf;
    }
    return out;
  }

 private:
  std::vector<vt::detail::OutEntry> entries_;
  VariantSpec spec_;
  abi::IInArchive* old_ = nullptr;
  std::map<PROPID, int> asked_;
  int streamAsks_ = 0;
  std::string reason_;
  LONG refs_ = 1;
};

// One variant end-to-end: fixture -> open -> UpdateItems -> reopen -> table
// line. Returns 0 when the round trip produced a readable archive (whatever
// it decided "docs" means -- dir, file, or dropped is the DATA we came for),
// 1 when evidence could not be produced at all.
int RunOne(const VariantSpec& spec, prism::win::SevenZipLibrary& library) {
  const std::wstring dir = (fs::temp_directory_path() / L"prism_mkdir_matrix" /
                            prism::win::Utf8ToWide(spec.name))
                               .wstring();
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) {
    Emit(std::string("VARIANT ") + spec.name + ": committed=[FIXTURE-FAILED(dir)]");
    return 1;
  }

  struct Png {
    const wchar_t* file;
    UINT w, h;
    uint8_t b, g, r;
  };
  const Png files[] = {
      {L"icon.png", 64, 64, 0, 0, 255},
      {L"photo.png", 120, 90, 0, 255, 0},
      {L"large.png", 300, 200, 255, 0, 0},
  };
  std::vector<std::wstring> names;
  for (const Png& f : files) {
    if (!prism::fixture::WritePng(dir + L"\\" + f.file, f.w, f.h, f.b, f.g, f.r)) {
      Emit(std::string("VARIANT ") + spec.name + ": committed=[FIXTURE-FAILED(png)]");
      return 1;
    }
    names.push_back(f.file);
  }
  const std::wstring archive = dir + L"\\solid.7z";
  if (!prism::fixture::PackSolid(prism::fixture::ExePath(), dir, L"solid.7z", names)) {
    Emit(std::string("VARIANT ") + spec.name + ": committed=[FIXTURE-FAILED(pack)]");
    return 1;
  }

  // Open + mini catalog (7z handler first: the zip tail-scan poisons the 7z
  // parse -- see OpenedArchive::Open in VirtualTreeExecutor.hpp).
  vt::detail::OpenedArchive opened;
  if (!opened.Open(library, archive)) {
    Emit(std::string("VARIANT ") + spec.name + ": committed=[OPEN-FAILED " +
         opened.trace + "]");
    return 1;
  }
  const auto catalog = vt::detail::ReadMiniCatalog(opened.handler);

  // The full new-archive item list: every old item rides along untouched
  // (data + props copied), the matrix node is appended LAST (index 3).
  std::vector<vt::detail::OutEntry> entries;
  entries.reserve(catalog.size() + 1);
  for (size_t i = 0; i < catalog.size(); ++i) {
    vt::detail::OutEntry entry;
    entry.indexInArchive = static_cast<int64_t>(i);
    entry.isDir = catalog[i].isDir;
    entry.expectedPath = catalog[i].path;
    entries.push_back(std::move(entry));
  }
  vt::detail::OutEntry node;
  node.indexInArchive = -1;
  node.newProps = spec.newProps != 0;
  node.newData = spec.newData != 0;
  node.isDir = true;
  node.newPath = prism::win::ToBackslashes(prism::win::Utf8ToWide(spec.path));
  node.expectedPath = spec.path;
  entries.push_back(std::move(node));

  vt::IOutArchive* outArchive = nullptr;
  const HRESULT hrQI = opened.handler->QueryInterface(
      vt::kIID_IOutArchive, reinterpret_cast<void**>(&outArchive));
  if (FAILED(hrQI) || !outArchive) {
    opened.Close();
    char buf[80];
    std::snprintf(buf, sizeof(buf), "VARIANT %s: committed=[QI-FAILED 0x%08lX]",
                  spec.name, static_cast<unsigned long>(hrQI));
    Emit(buf);
    return 1;
  }

  // Temp output in the SAME directory (only a same-volume rename is atomic);
  // the guard deletes it when the evidence has been collected.
  const std::wstring temp = dir + L"\\matrix-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
  vt::detail::TempFileGuard guard;
  guard.path = prism::win::ToBackslashes(temp);

  auto* outStream = new prism::win::DiskOutStream(temp);
  if (!outStream->Open()) {
    outStream->Release();
    outArchive->Release();
    opened.Close();
    Emit(std::string("VARIANT ") + spec.name + ": committed=[TEMP-FAILED]");
    return 1;
  }
  auto* callback = new MatrixUpdateCallback(entries, spec, opened.handler);
  const HRESULT hr = outArchive->UpdateItems(
      outStream, static_cast<abi::UInt32>(entries.size()), callback);
  const uint64_t written = outStream->written();
  const std::string cbReason = callback->reason();
  const int streamAsks = callback->streamAsks();
  const std::string asked = callback->askedSummary();
  callback->Release();
  outStream->Release();
  outArchive->Release();
  opened.Close();

  char hrBuf[16];
  std::snprintf(hrBuf, sizeof(hrBuf), "0x%08lX", static_cast<unsigned long>(hr));
  Emit("COMMIT " + std::string(spec.name) + ": hr=" + hrBuf +
       " bytes=" + std::to_string(written) +
       " streamAsks=" + std::to_string(streamAsks) +
       (cbReason.empty() ? "" : " reason=" + cbReason));
  if (FAILED(hr)) {
    Emit(std::string("VARIANT ") + spec.name + ": committed=[UPDATE-FAILED " + hrBuf + "]");
    return 1;
  }

  // The ANSWER: re-open the committed archive and list what actually landed.
  prism::win::ArchiveReader reader(library);
  if (!reader.Open(temp)) {
    Emit(std::string("VARIANT ") + spec.name + ": committed=[REOPEN-FAILED " +
         reader.openTrace() + "]");
    return 1;
  }
  std::string listed;
  bool targetSeen = false;
  for (const auto& item : reader.ReadCatalog()) {
    if (!listed.empty()) listed += ", ";
    const std::string norm = prism::NormalizeArchivePath(item.path);
    const bool isTarget = (norm == "docs");
    if (isTarget) {
      targetSeen = true;
      listed += item.path + (item.isDir ? "(dir)" : "(file:" + std::to_string(item.size) + "B)");
    } else {
      listed += item.path + (item.isDir ? "(dir)" : "(file)");
    }
  }
  if (!targetSeen) {
    if (!listed.empty()) listed += ", ";
    listed += "docs(DROPPED)";
  }
  Emit("VARIANT " + std::string(spec.name) + ": committed=[" + listed + "]");
  Emit("RAW " + std::string(spec.name) + ": " + reader.RawProbe());
  reader.Close();
  Emit("ASK " + std::string(spec.name) + ": new-node asked=[" + asked + "]");
  fs::remove_all(dir, ec);
  return 0;
}

int RunSingle(const char* name) {
  std::cout << std::unitbuf;
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    Emit(std::string("VARIANT ") + name + ": committed=[COM-FAILED]");
    return 1;
  }
  prism::win::SevenZipLibrary library;
  const std::wstring dll = prism::fixture::DllPath();
  if (dll.empty() || !library.Load(dll)) {
    Emit(std::string("VARIANT ") + name + ": committed=[DLL-LOAD-FAILED]");
    return 1;
  }
  Emit("SETUP " + std::string(name) + ": 7z.dll=" + prism::win::WideToUtf8(dll));
  for (const auto& v : kVariants) {
    if (std::strcmp(v.name, name) == 0) return RunOne(v, library);
  }
  Emit(std::string("VARIANT ") + name + ": committed=[UNKNOWN-VARIANT]");
  return 1;
}

std::wstring SelfPathExe() {
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return n ? std::wstring(buf, n) : std::wstring();
}

// Spawns "self --one <name>" with inherited stdio and waits. Returns the
// child exit code, or -1 when spawning is impossible / std handles are not
// pipes (the caller then runs the variant in-process).
int SpawnOne(const std::wstring& self, const char* name) {
  const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (in == nullptr || out == nullptr || err == nullptr) return -1;
  std::wstring cmd = L"\"" + self + L"\" --one " + prism::win::Utf8ToWide(name);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = out;
  si.hStdError = err;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

int RunMatrix() {
  std::cout << std::unitbuf;
  Emit("MKDIR MATRIX: 8 variants x (own solid.7z fixture + own commit + own readback); "
       "report-only, exit 1 is BY DESIGN");
  if (prism::fixture::DllPath().empty()) {
    Emit("MATRIX COMPLETE (degraded: PRISM_7Z_DLL missing, no evidence produced)");
    return 1;
  }
  const std::wstring self = SelfPathExe();
  int produced = 0, failed = 0, fallbacks = 0;
  for (const auto& v : kVariants) {
    const int code = SpawnOne(self, v.name);
    if (code < 0) {
      ++fallbacks;
      Emit("SPAWN " + std::string(v.name) + ": in-process fallback");
      produced += (RunSingle(v.name) == 0) ? 1 : 0;
      continue;
    }
    char buf[80];
    std::snprintf(buf, sizeof(buf), "SPAWN %s: exit=%d (0x%08lX)", v.name, code,
                  static_cast<unsigned long>(code));
    Emit(buf);
    if (code == 0) {
      ++produced;
    } else {
      ++failed;  // commit refused OR the dll died mid-variant; log tells which
    }
  }
  Emit("MATRIX COMPLETE: variants=8 produced=" + std::to_string(produced) +
       " failed=" + std::to_string(failed) +
       " inProcessFallbacks=" + std::to_string(fallbacks));
  Emit("MATRIX REPORT-ONLY: verdicts are read from the VARIANT lines above; "
       "exit 1 on purpose so --output-on-failure prints the table");
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "--one") == 0) return RunSingle(argv[2]);
  return RunMatrix();
}
