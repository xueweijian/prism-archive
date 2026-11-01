// PrismArchive -- archive CREATION against the real 7z.dll (blueprint ch.2).
//
// The Linux suite (Tests/test_compression_plan.cpp) proves the DECISIONS; this
// one proves the ENGINE ACCEPTS them and that what lands on disk is what the
// plan promised -- entry kinds, sizes and payload bytes, re-read with a second
// handler instance and (for 7z/zip) cross-checked with 7z.exe.
//
// Negative controls (CONTRACTS D3) are compiled into this same source:
//   PRISM_CW_MUTANT_DIR_AS_FILE  -> the callback tells 7z folders are files
//   PRISM_CW_MUTANT_NO_SIZE      -> the callback refuses to answer kpidSize
// Each is registered WILL_FAIL by Tests/suites/c9-compress.cmake, and each
// build asserts that the deliberate defect ACTUALLY reached the wire
// (injectedDirAsFile / injectedMissingSize counters), so a mutation that
// silently stopped firing can never masquerade as a sharp test.
//
// Exit policy (CONTRACTS D1): the marker line is printed only when the exit code
// is 0, ctest requires BOTH; a missing 7z.dll prints a SKIP line and returns 77
// (SKIP_RETURN_CODE 77), never a fake green.
#include <windows.h>
#include <propidl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ArchiveWriter.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "../mini_test.hpp"

namespace fs = std::filesystem;
namespace abi = prism::win::abi;
namespace cw = prism::win::cw;
using prism::ArchiveFormat;
using prism::CompressionOptions;
using prism::PlanItem;

namespace {

std::wstring gRoot;     // <temp>\prism_write_archive
std::wstring gSource;   // <root>\src
std::wstring gOut;      // <root>\out
prism::win::SevenZipLibrary gLibrary;

void Emit(const std::string& line) { std::cout << line << std::endl; }

// CONTRACTS D3, second half: a negative control is only worth something when
// the deliberate defect ACTUALLY reached 7z.dll. Each mutant build accumulates
// the callback's injection counters here, and main() prints the expected-failure
// marker ONLY when the run went red AND at least one defect was injected -- the
// two conditions are a conjunction, so a mutation whose guard silently stopped
// firing prints the plain failure line instead (regex mismatch -> RED) rather
// than masquerading as a caught one.
#if defined(PRISM_CW_MUTANT_DIR_AS_FILE) || defined(PRISM_CW_MUTANT_NO_SIZE)
int gInjectedTotal = 0;
void NoteInjection(const cw::WriteResult& res) {
  gInjectedTotal += res.injectedDirAsFile + res.injectedMissingSize;
}
#else
void NoteInjection(const cw::WriteResult&) {}
#endif

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> bytes(n);
  for (size_t i = 0; i < n; ++i) {
    bytes[i] = static_cast<uint8_t>((i * 31 + seed * 7 + (i >> 8)) & 0xFF);
  }
  return bytes;
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  size_t done = 0;
  while (done < bytes.size()) {
    DWORD wrote = 0;
    const DWORD want = static_cast<DWORD>((std::min)(size_t{1} << 16, bytes.size() - done));
    if (!WriteFile(h, bytes.data() + done, want, &wrote, nullptr) || wrote == 0) {
      CloseHandle(h);
      return false;
    }
    done += wrote;
  }
  CloseHandle(h);
  return true;
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (a.back() == L'\\' || a.back() == L'/') return a + b;
  return a + L"\\" + b;
}

// Runs 7z.exe and captures stdout+stderr into a file. Returns the exit code, or
// -1 when the CLI is unavailable (caller treats that as a hard failure for the
// assertions that depend on it -- but PRISM_7Z_DLL is always set in the lane).
int RunSevenZipCapture(const std::wstring& exe, const std::wstring& args,
                       const std::wstring& logPath) {
  if (exe.empty()) return -1;
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return -1;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = log;
  si.hStdError = log;
  si.hStdInput = INVALID_HANDLE_VALUE;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(log);
  if (!started) return -1;
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

std::string ReadTextFile(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  std::string out(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  if (!out.empty() && !ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
    out.clear();
  }
  CloseHandle(h);
  return out;
}

std::string ToUtf8(const std::wstring& w) { return prism::win::WideToUtf8(w); }

// One planned entry, with the payload the source file must hold.
struct SourceEntry {
  std::string path;
  bool isDir;
  std::vector<uint8_t> bytes;
};

std::vector<SourceEntry> BuildSourceTree() {
  std::error_code ec;
  fs::remove_all(gRoot, ec);
  fs::create_directories(gSource, ec);
  fs::create_directories(fs::path(gSource) / L"sub" / L"deep", ec);
  fs::create_directories(gOut, ec);
  std::vector<SourceEntry> entries;
  const auto add = [&](const char* rel, const std::vector<uint8_t>& bytes) {
    const std::wstring full = Join(gSource, prism::win::Utf8ToWide(rel));
    if (!WriteBytes(full, bytes)) {
      Emit(std::string("FIXTURE: cannot write ") + rel);
      return;
    }
    entries.push_back(SourceEntry{rel, false, bytes});
  };
  add("alpha.txt", Pattern(4096, 1));
  add("beta.bin", Pattern(255, 2));
  add("empty.txt", {});
  add("sub/gamma.txt", Pattern(8192, 3));
  add("sub/deep/delta.txt", Pattern(2048, 4));
  // Folder nodes are part of the plan on purpose: the kpidIsDir answer is what
  // the DIR_AS_FILE mutation breaks, and without them that mutation would be
  // unobservable.
  entries.push_back(SourceEntry{"sub", true, {}});
  entries.push_back(SourceEntry{"sub/deep", true, {}});
  return entries;
}

prism::CompressionPlan MakePlan(const std::vector<SourceEntry>& entries, ArchiveFormat format,
                                int level, bool encrypt, bool encryptNames) {
  std::vector<PlanItem> items;
  for (const auto& e : entries) {
    PlanItem item;
    item.path = e.path;
    item.size = e.bytes.size();
    item.isDir = e.isDir;
    items.push_back(std::move(item));
  }
  CompressionOptions options;
  options.format = format;
  options.level = level;
  options.encrypt = encrypt;
  options.encryptFileNames = encryptNames;
  options.cpuCores = 4;
  return prism::MakeCompressionPlan(items, options);
}

// Reads the created archive back with a SECOND handler instance: paths, kinds,
// sizes, and every payload compared byte for byte.
struct ReadBack {
  bool ok = false;
  std::string error;
  size_t items = 0;
};

ReadBack ReadBackAndCompare(const std::wstring& archive, const std::vector<SourceEntry>& entries,
                            const std::wstring& password, bool expectEncryptedCatalog,
                            bool comparePayloads = true) {
  ReadBack result;
  prism::win::ArchiveReader reader(gLibrary);
  if (!reader.Open(archive, password)) {
    result.error = "re-open failed (" + reader.openTrace() + ")";
    return result;
  }
  const auto catalog = reader.ReadCatalog();
  if (expectEncryptedCatalog && catalog.empty()) {
    reader.Close();
    result.error = "catalog is empty although a password was supplied";
    return result;
  }
  std::map<std::string, std::pair<bool, uint64_t>> got;
  std::map<std::string, uint32_t> indexByPath;
  for (const auto& item : catalog) {
    const std::string norm = prism::NormalizeArchivePath(item.path);
    got[norm] = {item.isDir, item.size};
    indexByPath[norm] = item.index;
  }
  for (const auto& e : entries) {
    const auto it = got.find(e.path);
    if (it == got.end()) {
      result.error = "missing entry '" + e.path + "'";
      reader.Close();
      return result;
    }
    if (it->second.first != e.isDir) {
      result.error = "'" + e.path + "' landed as the wrong kind";
      reader.Close();
      return result;
    }
    if (!e.isDir && it->second.second != e.bytes.size()) {
      result.error = "'" + e.path + "' size mismatch";
      reader.Close();
      return result;
    }
    if (!e.isDir && comparePayloads) {
      const auto bytes = reader.ExtractToMemory(indexByPath[e.path]);
      if (!bytes.has_value()) {
        result.error = "cannot re-extract '" + e.path + "'";
        reader.Close();
        return result;
      }
      if (*bytes != e.bytes) {
        result.error = "payload mismatch for '" + e.path + "'";
        reader.Close();
        return result;
      }
    }
  }
  result.items = catalog.size();
  reader.Close();
  result.ok = true;
  return result;
}

// Header of a zip written by 7-Zip must be a real local-file header and must
// carry an End Of Central Directory record -- that pair is what Explorer needs.
bool LooksLikeZip(const std::wstring& path) {
  std::vector<uint8_t> bytes;
  if (!cw::ReadWholeFile(path, &bytes)) return false;
  if (bytes.size() < 22) return false;
  if (!(bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 3 && bytes[3] == 4)) return false;
  // Walk every candidate EOCD offset, starting with the last possible one.
  for (size_t i = bytes.size() - 21; i-- > 0;) {
    if (bytes[i] == 'P' && bytes[i + 1] == 'K' && bytes[i + 2] == 5 && bytes[i + 3] == 6) {
      return true;
    }
  }
  return false;
}

// Minimal ustar reader: an INDEPENDENT check of the tar bytes (7z.dll wrote
// them; this parser only knows the public format). Verifies that each planned
// entry appears with the right size and payload.
bool TarHolds(const std::wstring& path, const std::vector<SourceEntry>& entries,
              std::string* why) {
  std::vector<uint8_t> bytes;
  if (!cw::ReadWholeFile(path, &bytes)) {
    *why = "cannot read tar";
    return false;
  }
  size_t off = 0;
  std::map<std::string, std::pair<uint64_t, size_t>> seen;  // name -> (size, dataOffset)
  while (off + 512 <= bytes.size()) {
    const uint8_t* block = bytes.data() + off;
    bool allZero = true;
    for (size_t i = 0; i < 512; ++i) {
      if (block[i] != 0) {
        allZero = false;
        break;
      }
    }
    if (allZero) break;
    std::string name(reinterpret_cast<const char*>(block), 100);
    name = name.c_str();  // trim at NUL
    const std::string magic(reinterpret_cast<const char*>(block + 257), 5);
    if (magic != "ustar") {
      *why = "missing ustar magic at offset " + std::to_string(off + 257);
      return false;
    }
    uint64_t size = 0;
    for (int i = 124; i < 136; ++i) {
      const char c = static_cast<char>(block[i]);
      if (c == 0 || c == ' ') break;
      if (c < '0' || c > '7') break;
      size = size * 8 + static_cast<uint64_t>(c - '0');
    }
    seen[name] = {size, off + 512};
    off += 512 + ((size + 511) / 512) * 512;
  }
  for (const auto& e : entries) {
    if (e.isDir) continue;
    const auto it = seen.find(e.path);
    if (it == seen.end()) {
      *why = "tar is missing '" + e.path + "'";
      return false;
    }
    if (it->second.first != e.bytes.size()) {
      *why = "tar entry '" + e.path + "' has the wrong size";
      return false;
    }
    if (it->second.second + e.bytes.size() > bytes.size()) {
      *why = "tar entry '" + e.path + "' runs past the end of the file";
      return false;
    }
    if (std::memcmp(bytes.data() + it->second.second, e.bytes.data(), e.bytes.size()) != 0) {
      *why = "tar entry '" + e.path + "' has different bytes";
      return false;
    }
  }
  return true;
}

unsigned Log2(uint64_t v) {
  unsigned n = 0;
  while (v > 1) {
    v >>= 1;
    ++n;
  }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
PRISM_TEST("7z solid: LZMA2 with the planned dictionary, verified byte-for-byte") {
  const auto entries = BuildSourceTree();
  PRISM_CHECK_EQ(entries.size(), static_cast<size_t>(7));
  const auto plan = MakePlan(entries, ArchiveFormat::SevenZip, 9, false, false);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK(plan.solid);
  const std::wstring archive = Join(gOut, L"solid.7z");

  cw::WriteRequest req;
  req.archivePath = archive;
  req.sourceRoot = gSource;
  req.plan = plan;
  const cw::WriteResult res = cw::CreateArchive(req);
  NoteInjection(res);
  char line[512];
  std::snprintf(line, sizeof(line),
                "CREATE 7z-solid: ok=%d verified=%d verifiedItems=%llu items=%llu streams=%llu "
                "bytes=%llu dirAsFile=%d missingSize=%d elapsed=%lldms err=%s",
                res.ok ? 1 : 0, res.verified ? 1 : 0,
                static_cast<unsigned long long>(res.verifiedItems),
                static_cast<unsigned long long>(res.itemsWritten),
                static_cast<unsigned long long>(res.streamsServed),
                static_cast<unsigned long long>(res.bytesWritten), res.injectedDirAsFile,
                res.injectedMissingSize, static_cast<long long>(res.elapsedMs),
                res.error.c_str());
  Emit(line);
  Emit("TRACE " + res.trace);

  // CONTRACTS D3: in a mutant build the defect must have reached the engine.
#if defined(PRISM_CW_MUTANT_DIR_AS_FILE)
  PRISM_CHECK(res.injectedDirAsFile > 0);
#elif defined(PRISM_CW_MUTANT_NO_SIZE)
  PRISM_CHECK(res.injectedMissingSize > 0);
#else
  PRISM_CHECK_EQ(res.injectedDirAsFile, 0);
  PRISM_CHECK_EQ(res.injectedMissingSize, 0);
#endif
  if (!PRISM_CHECK(res.ok)) return;
  PRISM_CHECK(res.verified);
  PRISM_CHECK_EQ(res.verifiedItems, plan.items.size());

  // 1) The archive must re-open through a fresh handler and hold the payload.
  const ReadBack read = ReadBackAndCompare(archive, entries, L"", true);
  PRISM_CHECK(read.ok);
  if (!read.ok) Emit("READBACK: " + read.error);
  PRISM_CHECK_EQ(read.items, plan.items.size());

  // 2) The engine must have used the dictionary the plan chose: 7-Zip records
  //    it in kpidMethod as "LZMA2:<log2 dict>", which 7z.exe prints verbatim.
  const std::wstring log = Join(gOut, L"solid-list.txt");
  const int code = RunSevenZipCapture(prism::fixture::ExePath(),
                                      L"l -slt \"" + archive + L"\"", log);
  PRISM_CHECK_EQ(code, 0);
  const std::string listing = ReadTextFile(log);
  const std::string wantMethod = "Method = LZMA2:" + std::to_string(Log2(plan.dictionaryBytes));
  if (listing.find(wantMethod) == std::string::npos) {
    Emit("EXPECTED method line missing: " + wantMethod);
  }
  PRISM_CHECK(listing.find(wantMethod) != std::string::npos);
  PRISM_CHECK(listing.find("Path = empty.txt") != std::string::npos);
  PRISM_CHECK(listing.find("Method = LZMA2") != std::string::npos);
}

PRISM_TEST("7z level 0: Copy, and the archive is never smaller than the payload") {
  const auto entries = BuildSourceTree();
  const auto plan = MakePlan(entries, ArchiveFormat::SevenZip, 0, false, false);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.methodName, std::string("Copy"));
  const std::wstring archive = Join(gOut, L"stored.7z");

  cw::WriteRequest req;
  req.archivePath = archive;
  req.sourceRoot = gSource;
  req.plan = plan;
  const cw::WriteResult res = cw::CreateArchive(req);
  NoteInjection(res);
  char line[256];
  std::snprintf(line, sizeof(line), "CREATE 7z-store: ok=%d bytes=%llu payload=%llu err=%s",
                res.ok ? 1 : 0, static_cast<unsigned long long>(res.bytesWritten),
                static_cast<unsigned long long>(plan.inputBytes), res.error.c_str());
  Emit(line);
  if (!PRISM_CHECK(res.ok)) return;
  // The honest cost model promised this bound; a stored archive must honour it.
  PRISM_CHECK(res.bytesWritten >= plan.inputBytes);

  const ReadBack read = ReadBackAndCompare(archive, entries, L"", true);
  PRISM_CHECK(read.ok);
  if (!read.ok) Emit("READBACK: " + read.error);

  const std::wstring log = Join(gOut, L"stored-list.txt");
  PRISM_CHECK_EQ(RunSevenZipCapture(prism::fixture::ExePath(), L"l -slt \"" + archive + L"\"", log),
                 0);
  const std::string listing = ReadTextFile(log);
  PRISM_CHECK(listing.find("Method = Copy") != std::string::npos);
}

PRISM_TEST("zip: a container Explorer can open, not just one 7z likes") {
  const auto entries = BuildSourceTree();
  const auto plan = MakePlan(entries, ArchiveFormat::Zip, 6, false, false);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK(!plan.solid);
  const std::wstring archive = Join(gOut, L"pack.zip");

  cw::WriteRequest req;
  req.archivePath = archive;
  req.sourceRoot = gSource;
  req.plan = plan;
  const cw::WriteResult res = cw::CreateArchive(req);
  NoteInjection(res);
  Emit("CREATE zip: ok=" + std::to_string(res.ok ? 1 : 0) + " bytes=" +
       std::to_string(res.bytesWritten) + " err=" + res.error);
  if (!PRISM_CHECK(res.ok)) return;

  // Structural check: local file header + End Of Central Directory. Explorer
  // refuses a zip without the latter even if 7-Zip happens to still read it.
  PRISM_CHECK(LooksLikeZip(archive));
  const ReadBack read = ReadBackAndCompare(archive, entries, L"", true);
  PRISM_CHECK(read.ok);
  if (!read.ok) Emit("READBACK: " + read.error);

  // Independent integrity test through the CLI (same dll, different code path:
  // it CRC-checks every entry).
  const std::wstring log = Join(gOut, L"zip-test.txt");
  const int code =
      RunSevenZipCapture(prism::fixture::ExePath(), L"t \"" + archive + L"\"", log);
  Emit("ZIP TEST: exit=" + std::to_string(code));
  PRISM_CHECK_EQ(code, 0);
  const std::string out = ReadTextFile(log);
  PRISM_CHECK(out.find("Everything is Ok") != std::string::npos);
}

PRISM_TEST("7z AES-256 with encrypted file names hides the catalog") {
  const auto entries = BuildSourceTree();
  const auto plan = MakePlan(entries, ArchiveFormat::SevenZip, 5, true, true);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK(plan.encryptFileNames);
  const std::wstring archive = Join(gOut, L"secret.7z");
  const std::wstring password = L"prism-c9-passphrase";

  cw::WriteRequest req;
  req.archivePath = archive;
  req.sourceRoot = gSource;
  req.plan = plan;
  req.password = password;
  const cw::WriteResult res = cw::CreateArchive(req);
  NoteInjection(res);
  Emit("CREATE 7z-aes: ok=" + std::to_string(res.ok ? 1 : 0) + " bytes=" +
       std::to_string(res.bytesWritten) + " err=" + res.error);
  if (!PRISM_CHECK(res.ok)) return;

  // With the password the catalog reads back. Payload bytes are cross-checked
  // through the CLI instead of ArchiveReader: its extract callback does not
  // answer ICryptoGetTextPassword (it lives in PrismEngineBridge, owned by
  // another card), so decrypting through it is not a fair test of THIS writer.
  const ReadBack good = ReadBackAndCompare(archive, entries, password, true, false);
  PRISM_CHECK(good.ok);
  if (!good.ok) Emit("READBACK(password): " + good.error);

  const std::wstring log = Join(gOut, L"secret-test.txt");
  const int code = RunSevenZipCapture(prism::fixture::ExePath(),
                                      L"t -p" + password + L" \"" + archive + L"\"", log);
  Emit("AES TEST: exit=" + std::to_string(code));
  PRISM_CHECK_EQ(code, 0);
  PRISM_CHECK(ReadTextFile(log).find("Everything is Ok") != std::string::npos);

  // Without it: header encryption means no names at all. Opening must fail --
  // that is the difference between "encrypted" and "really encrypted".
  prism::win::ArchiveReader withoutPassword(gLibrary);
  const bool opened = withoutPassword.Open(archive);
  std::size_t visible = 0;
  if (opened) visible = withoutPassword.ReadCatalog().size();
  withoutPassword.Close();
  Emit("AES WITHOUT PASSWORD: opened=" + std::to_string(opened ? 1 : 0) +
       " visible=" + std::to_string(visible));
  PRISM_CHECK(!opened || visible == 0);
}

PRISM_TEST("tar: store-only container, verified by an independent ustar parser") {
  const auto entries = BuildSourceTree();
  const auto plan = MakePlan(entries, ArchiveFormat::Tar, 5, false, false);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.methodName, std::string("Store"));
  const std::wstring archive = Join(gOut, L"bundle.tar");

  cw::WriteRequest req;
  req.archivePath = archive;
  req.sourceRoot = gSource;
  req.plan = plan;
  const cw::WriteResult res = cw::CreateArchive(req);
  NoteInjection(res);
  Emit("CREATE tar: ok=" + std::to_string(res.ok ? 1 : 0) + " bytes=" +
       std::to_string(res.bytesWritten) + " err=" + res.error);
  if (!PRISM_CHECK(res.ok)) return;

  std::string why;
  if (!TarHolds(archive, entries, &why)) Emit("TAR CHECK: " + why);
  PRISM_CHECK(TarHolds(archive, entries, &why));
  // tar stores: the bytes are on disk verbatim, so the payload bound is exact.
  PRISM_CHECK(res.bytesWritten >= plan.inputBytes);
}

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) {
    Emit("WRITE-ARCHIVE SKIPPED: CoInitializeEx failed");
    return 77;
  }
  const std::wstring dll = prism::fixture::DllPath();
  if (dll.empty() || !gLibrary.Load(dll)) {
    // CONTRACTS D1: a SKIP is exit 77 + SKIP_RETURN_CODE, never a silent pass.
    Emit("WRITE-ARCHIVE SKIPPED: PRISM_7Z_DLL is not set or 7z.dll cannot be loaded");
    CoUninitialize();
    return 77;
  }
  wchar_t temp[MAX_PATH]{};
  const DWORD n = GetTempPathW(MAX_PATH, temp);
  if (n == 0) {
    Emit("WRITE-ARCHIVE SKIPPED: GetTempPathW failed");
    CoUninitialize();
    return 77;
  }
  gRoot = Join(std::wstring(temp), L"prism_write_archive");
  gSource = Join(gRoot, L"src");
  gOut = Join(gRoot, L"out");
  Emit("FIXTURE root=" + ToUtf8(gRoot) + " dll=" + ToUtf8(dll));

  const int rc = prism::test::RunAll(argc, argv);
  std::error_code ec;
  fs::remove_all(gRoot, ec);
  CoUninitialize();
  if (rc == 0) {
    Emit("ALL WRITE-ARCHIVE CHECKS PASSED");
    return 0;
  }
#if defined(PRISM_CW_MUTANT_DIR_AS_FILE) || defined(PRISM_CW_MUTANT_NO_SIZE)
  // A mutant build is supposed to go red: that is the whole point. It may only
  // claim success when the injected defect is PROVEN to have reached the engine
  // (gInjectedTotal > 0), so the marker cannot be produced by a mutation that
  // stopped firing. Exit 0 because the expected outcome happened; the ctest
  // PASS_REGULAR_EXPRESSION pins the marker as well (CONTRACTS D1/D3 -- measured
  // 2026-11-01: with a matching regex ctest ignores the exit code, and WILL_FAIL
  // would then invert the correct pass into a failure).
  if (gInjectedTotal > 0) {
    Emit("WRITE-ARCHIVE MUTANT CAUGHT: " + std::to_string(gInjectedTotal) +
         " injected answer(s) reached 7z.dll and the checks went red as required");
    return 0;
  }
#endif
  Emit("WRITE-ARCHIVE CHECKS FAILED");
  return 1;
}
