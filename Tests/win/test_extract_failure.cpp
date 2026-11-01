// PrismArchive -- Tests/win
// Card C26: the ENGINE'S FAILURE SEMANTICS + the extension -> CLSID route table.
//
// What C17 measured and this file locks down: 7z.dll answers IInArchive::Extract()
// with S_OK even when an item could not be decoded. The real verdict arrives out
// of band, in IArchiveExtractCallback::SetOperationResult(opResult): 1 means
// kUnsupportedMethod. The pre-C26 bridge looked only at the HRESULT, so a .7z
// compressed with zstd (a method the official engine does not implement) was
// reported as a successful extraction of ZERO bytes -- an empty file on disk and
// no reason anywhere.
//
// The fixtures are built here, byte by byte, because they have to be:
//   * the zstd-method .7z cannot be produced by the very engine that cannot
//     decode it, so its header is written by hand (validated against 7z.exe);
//   * the raw .zst payload is a zstd frame made of one RAW block (RFC 8878),
//     which needs no compressor at all.
//
// Verdict shape (CONTRACTS D1/D26): the stdout marker is part of the verdict and
// a missing 7z.dll exits 77 (SKIP_RETURN_CODE), never 0.
//
// Negative control (CONTRACTS D3): built with PRISM_C26_MUTANT_IGNORE_OPRESULT
// the bridge restores the pre-C26 defect (opResult read and thrown away). The
// opResult checks MUST then go red, and this binary prints
// "EXTRACT-FAILURE MUTANT CAUGHT" only when they did AND the injection counter
// proves the mutation actually fired. Per D26 the mutant is registered on that
// marker alone -- never PASS_REGULAR_EXPRESSION + WILL_FAIL.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "PrismEngineBridge.hpp"
#include "UniqueTempDir.hpp"

namespace {

std::wstring MakeTempDir() {
  // Card C36 (Tests/win/UniqueTempDir.hpp): card + pid + per-call COUNTER. Every
  // case gets its own tree, and the process owns them all, so an early return
  // removes them at exit instead of leaving debris for the next run.
  static std::vector<std::unique_ptr<prism::testtmp::UniqueDir>> kept;
  kept.push_back(std::make_unique<prism::testtmp::UniqueDir>(L"c36", nullptr));
  const prism::testtmp::UniqueDir& dir = *kept.back();
  return dir.ok() ? dir.wstr() : std::wstring();
}

std::wstring Locate7zDll() {
  wchar_t envValue[MAX_PATH * 2]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", envValue, ARRAYSIZE(envValue)) > 0) {
    if (std::filesystem::exists(envValue)) return envValue;
  }
  for (const wchar_t* candidate : {L"C:\\Program Files\\7-Zip\\7z.dll",
                                   L"C:\\Program Files (x86)\\7-Zip\\7z.dll", L"7z.dll"}) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return {};
}

void WriteFile(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

uint32_t Crc32(const uint8_t* data, size_t size) {
  static uint32_t table[256] = {0};
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// 7z's variable-length number (7zFormat.txt): the high bits of the first byte
// say how many bytes follow.
std::vector<uint8_t> SevenZipNumber(uint64_t value) {
  std::vector<uint8_t> out;
  for (int extra = 0; extra < 8; ++extra) {
    if (value < (1ull << (7 * (extra + 1)))) {
      if (extra == 0) {
        out.push_back(static_cast<uint8_t>(value));
        return out;
      }
      const uint8_t first = static_cast<uint8_t>(((0xFFu << (8 - extra)) & 0xFFu) |
                                                 ((value >> (8 * extra)) & 0xFFu));
      out.push_back(first);
      for (int i = extra - 1; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
      }
      return out;
    }
  }
  return out;
}

void Append(std::vector<uint8_t>& out, const std::vector<uint8_t>& more) {
  out.insert(out.end(), more.begin(), more.end());
}

// A minimal, VALID .7z container: one packed stream, one file, plain (not
// encoded) header. methodId is the coder id written verbatim, which is how a
// file compressed with a method this engine lacks is manufactured. Verified
// byte-for-byte against 7z.exe 25.01: it lists the archive and extracts it when
// the method is Copy, and fails with "Unsupported Method" when it is not.
std::vector<uint8_t> Build7z(const std::string& payload, const std::string& name,
                             const std::vector<uint8_t>& methodId, uint64_t unpackSize) {
  enum : uint8_t {
    kEnd = 0x00,
    kHeader = 0x01,
    kMainStreams = 0x04,
    kFilesInfo = 0x05,
    kPackInfo = 0x06,
    kUnpackInfo = 0x07,
    kSize = 0x09,
    kFolder = 0x0B,
    kCodersUnpackSize = 0x0C,
    kName = 0x11,
  };

  std::vector<uint8_t> folder;
  Append(folder, SevenZipNumber(1));                        // one coder
  folder.push_back(static_cast<uint8_t>(methodId.size()));  // flags: idSize only
  folder.insert(folder.end(), methodId.begin(), methodId.end());

  std::vector<uint8_t> header;
  header.push_back(kHeader);
  header.push_back(kMainStreams);
  header.push_back(kPackInfo);
  Append(header, SevenZipNumber(0));  // packPos
  Append(header, SevenZipNumber(1));  // numPackStreams
  header.push_back(kSize);
  Append(header, SevenZipNumber(payload.size()));
  header.push_back(kEnd);
  header.push_back(kUnpackInfo);
  header.push_back(kFolder);
  Append(header, SevenZipNumber(1));  // numFolders
  header.push_back(0);                // external = false
  Append(header, folder);
  header.push_back(kCodersUnpackSize);
  Append(header, SevenZipNumber(unpackSize));
  header.push_back(kEnd);  // end unpack info
  header.push_back(kEnd);  // end main streams
  header.push_back(kFilesInfo);
  Append(header, SevenZipNumber(1));  // numFiles
  std::vector<uint8_t> wideName;
  for (const char c : name) {
    wideName.push_back(static_cast<uint8_t>(c));
    wideName.push_back(0);
  }
  wideName.push_back(0);
  wideName.push_back(0);  // UTF-16 NUL
  header.push_back(kName);
  Append(header, SevenZipNumber(1 + wideName.size()));  // external flag + name
  header.push_back(0);                                 // external = false
  Append(header, wideName);
  header.push_back(kEnd);  // end files info
  header.push_back(kEnd);  // end header

  std::vector<uint8_t> startHeader;
  auto put64 = [&startHeader](uint64_t v) {
    for (int i = 0; i < 8; ++i) startHeader.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  };
  put64(payload.size());  // next header offset (payload follows the signature)
  put64(header.size());   // next header size
  const uint32_t headerCrc = Crc32(header.data(), header.size());
  for (int i = 0; i < 4; ++i) {
    startHeader.push_back(static_cast<uint8_t>((headerCrc >> (8 * i)) & 0xFF));
  }

  std::vector<uint8_t> out = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04};
  const uint32_t startCrc = Crc32(startHeader.data(), startHeader.size());
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((startCrc >> (8 * i)) & 0xFF));
  Append(out, startHeader);
  Append(out, std::vector<uint8_t>(payload.begin(), payload.end()));
  Append(out, header);
  return out;
}

// RFC 8878: magic + frame header descriptor 0 + window descriptor 0 + ONE raw
// (uncompressed) block -- a valid zstd frame that needs no zstd compressor,
// which is the point: the engine can decode it, we cannot encode it.
std::vector<uint8_t> BuildZstdRawFrame(const std::string& payload) {
  std::vector<uint8_t> out = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00};
  const uint32_t blockHeader = (static_cast<uint32_t>(payload.size()) << 3) | 0x1u;
  out.push_back(static_cast<uint8_t>(blockHeader & 0xFF));
  out.push_back(static_cast<uint8_t>((blockHeader >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((blockHeader >> 16) & 0xFF));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// Minimal stored (method 0) ZIP writer -- the same shape test_engine_bridge uses.
struct ZipEntry {
  std::string name;
  std::string data;
};

void Put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

std::vector<uint8_t> BuildZip(const std::vector<ZipEntry>& entries) {
  std::vector<uint8_t> out;
  std::vector<uint32_t> offsets;
  for (const auto& e : entries) {
    offsets.push_back(static_cast<uint32_t>(out.size()));
    Put32(out, 0x04034b50);
    Put16(out, 20);
    Put16(out, 0x0800);  // UTF-8 names
    Put16(out, 0);       // stored
    Put16(out, 0);
    Put16(out, 0x21);
    Put32(out, Crc32(reinterpret_cast<const uint8_t*>(e.data.data()), e.data.size()));
    Put32(out, static_cast<uint32_t>(e.data.size()));
    Put32(out, static_cast<uint32_t>(e.data.size()));
    Put16(out, static_cast<uint16_t>(e.name.size()));
    Put16(out, 0);
    for (const char c : e.name) out.push_back(static_cast<uint8_t>(c));
    for (const char c : e.data) out.push_back(static_cast<uint8_t>(c));
  }
  const uint32_t centralOffset = static_cast<uint32_t>(out.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    Put32(out, 0x02014b50);
    Put16(out, 20);
    Put16(out, 20);
    Put16(out, 0x0800);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, 0x21);
    Put32(out, Crc32(reinterpret_cast<const uint8_t*>(entries[i].data.data()),
                     entries[i].data.size()));
    Put32(out, static_cast<uint32_t>(entries[i].data.size()));
    Put32(out, static_cast<uint32_t>(entries[i].data.size()));
    Put16(out, static_cast<uint16_t>(entries[i].name.size()));
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, 0);
    Put32(out, 0);
    Put32(out, offsets[i]);
    for (const char c : entries[i].name) out.push_back(static_cast<uint8_t>(c));
  }
  const uint32_t centralSize = static_cast<uint32_t>(out.size()) - centralOffset;
  Put32(out, 0x06054b50);
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, static_cast<uint16_t>(entries.size()));
  Put16(out, static_cast<uint16_t>(entries.size()));
  Put32(out, centralSize);
  Put32(out, centralOffset);
  Put16(out, 0);
  return out;
}

// zstd's method id in 7-Zip (7-Zip Zstandard / 7-Zip 25.x handler table).
// MAINTENANCE: 7-Zip 25.01 ships a .zst ARCHIVE handler but no zstd CODEC, so
// this id is unsupported inside a .7z (verified: opResult=1, 0 bytes). If a
// future engine learns the codec, the premise test below goes red on purpose --
// regenerate the fixture with another unknown method id instead of weakening
// the assertion.
const std::vector<uint8_t> kZstdMethodId = {0x04, 0xF7, 0x11, 0x01};
const std::vector<uint8_t> kCopyMethodId = {0x00};

// No 7z.dll -> the whole binary reports SKIP (exit 77), never a silent pass.
bool g_haveDll = false;
std::wstring g_dllPath;

bool EngineHasZstd() {
  prism::win::SevenZipLibrary library;
  if (!library.Load(g_dllPath)) return false;
  return library.FormatNamed("zstd") != nullptr;
}

// ---------------------------------------------------------------------------
// 1. The route table is a contract, and the engine is the authority on CLSIDs.
// ---------------------------------------------------------------------------
struct ExpectedRoute {
  const wchar_t* extension;  // with the dot
  const char* handler;       // engine handler name
  const GUID* pinned;        // nullptr = engine-only (fork formats)
};

// The ten extensions this card promises to route. .zst/.lz4/.br are not in the
// official Guid.txt, which is exactly why the CLSID is read back from the
// engine instead of guessed (CONTRACTS A4).
const ExpectedRoute kExpectedRoutes[] = {
    {L".7z", "7z", &prism::win::abi::CLSID_CFormat7z},
    {L".zip", "zip", &prism::win::abi::CLSID_CFormatZip},
    {L".rar", "rar", &prism::win::kClsidRar},
    {L".tar", "tar", &prism::win::kClsidTar},
    {L".gz", "gzip", &prism::win::kClsidGzip},
    {L".bz2", "bzip2", &prism::win::kClsidBzip2},
    {L".xz", "xz", &prism::win::kClsidXz},
    {L".zst", "zstd", nullptr},
    {L".lz4", "lz4", nullptr},
    {L".br", "br", nullptr},
};

PRISM_TEST("extension lookup ignores case and dots inside directory names") {
  PRISM_CHECK(prism::win::PathExtensionLower(L"C:\\a.b\\file.ZST") == L".zst");
  PRISM_CHECK(prism::win::PathExtensionLower(L"C:\\a.b\\file") == L"");
  PRISM_CHECK(prism::win::PathExtensionLower(L"noext") == L"");
  PRISM_CHECK(prism::win::FindFormatRoute(L"C:\\x.y\\pack.tar") != nullptr);
  PRISM_CHECK(prism::win::FindFormatRoute(L"C:\\x.y\\pack.unknown") == nullptr);
  for (const ExpectedRoute& expected : kExpectedRoutes) {
    const std::wstring probe = std::wstring(L"C:\\dir.with.dot\\payload") + expected.extension;
    const prism::win::FormatRoute* route = prism::win::FindFormatRoute(probe);
    if (!PRISM_CHECK(route != nullptr)) continue;
    PRISM_CHECK_EQ(std::string(route->handler), std::string(expected.handler));
  }
}

PRISM_TEST("pinned CLSIDs agree with the engine's own handler table") {
  if (!g_haveDll) return;
  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  const auto& formats = library.formats();
  std::cout << "    engine advertises " << formats.size() << " handlers" << std::endl;
  if (!PRISM_CHECK(!formats.empty())) return;

  for (const ExpectedRoute& expected : kExpectedRoutes) {
    const std::string extension = prism::win::WideToUtf8(expected.extension + 1);
    const prism::win::EngineFormatInfo* format = library.FormatNamed(expected.handler);
    if (!format) {
      // An older engine legitimately lacks the fork formats. The route itself
      // is still required (checked above); only the CLSID check is moot.
      std::cout << "    (engine has no '" << expected.handler << "' handler)" << std::endl;
      continue;
    }
    if (expected.pinned) {
      // A typo in a pinned GUID would otherwise silently degrade every open of
      // that format to the two-handler probe.
      PRISM_CHECK(IsEqualGUID(*expected.pinned, format->clsid));
    }
    const auto resolved = library.ClsidsForExtension(extension);
    PRISM_CHECK(!resolved.empty());
    std::cout << "    " << extension << " -> " << expected.handler << " ("
              << resolved.size() << " handler(s))" << std::endl;
  }
}

// ---------------------------------------------------------------------------
// 2. The premise, proven directly: the engine answers S_OK for an item it could
//    not decode. If that ever stops being true, the fixture no longer exercises
//    the bug and the card's main assertion would be vacuous (CONTRACTS D18).
// ---------------------------------------------------------------------------
class OpResultProbe final : public prism::win::abi::IArchiveExtractCallback {
 public:
  explicit OpResultProbe(prism::win::MemoryOutStream* sink) : sink_(sink) { sink_->AddRef(); }
  ~OpResultProbe() { sink_->Release(); }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == prism::win::abi::IID_IArchiveExtractCallback) {
      *out = static_cast<prism::win::abi::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = InterlockedDecrement(&refs_);
    if (remaining == 0) delete this;
    return remaining;
  }
  STDMETHODIMP SetTotal(prism::win::abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const prism::win::abi::UInt64*) override { return S_OK; }
  STDMETHODIMP GetStream(prism::win::abi::UInt32, prism::win::abi::ISequentialOutStream** out,
                         prism::win::abi::Int32 mode) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (mode == prism::win::abi::kExtract) {
      *out = sink_;
      sink_->AddRef();
    }
    return S_OK;
  }
  STDMETHODIMP PrepareOperation(prism::win::abi::Int32) override { return S_OK; }
  STDMETHODIMP SetOperationResult(prism::win::abi::Int32 operationResult) override {
    ++opResultCalls;
    if (opResultCalls == 1) firstOpResult = operationResult;
    return S_OK;
  }

  int opResultCalls = 0;
  int32_t firstOpResult = 0;

 private:
  prism::win::MemoryOutStream* sink_ = nullptr;
  LONG refs_ = 1;
};

PRISM_TEST("the engine really answers S_OK while reporting kUnsupportedMethod") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring archive = dir + L"\\zstd_method.7z";
  const std::string payload = "hello prism zstd\n";
  WriteFile(archive, Build7z(payload, "hello.txt", kZstdMethodId, payload.size()));

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  // Deliberately the 7z handler directly: this test is about the ENGINE, not
  // about ArchiveReader's routing.
  prism::win::abi::IInArchive* raw = nullptr;
  PRISM_CHECK(SUCCEEDED(library.CreateObject(prism::win::abi::CLSID_CFormat7z,
                                             prism::win::abi::IID_IInArchive,
                                             reinterpret_cast<void**>(&raw))));
  if (!PRISM_CHECK(raw != nullptr)) return;
  auto* stream = new prism::win::InFileStream(archive);
  if (!PRISM_CHECK(stream->valid())) {
    stream->Release();
    raw->Release();
    return;
  }
  auto* openCallback = new prism::win::OpenCallback();
  const HRESULT opened = raw->Open(stream, nullptr, openCallback);
  openCallback->Release();
  if (!PRISM_CHECK_EQ(opened, S_OK)) {
    stream->Release();
    raw->Release();
    return;
  }
  auto* sink = new prism::win::MemoryOutStream(1u << 20);
  auto* probe = new OpResultProbe(sink);
  const prism::win::abi::UInt32 index = 0;
  const HRESULT extracted = raw->Extract(&index, 1, 0, probe);
  std::cout << "    Extract hr=0x" << std::hex << static_cast<unsigned long>(extracted) << std::dec
            << " opResultCalls=" << probe->opResultCalls
            << " firstOpResult=" << probe->firstOpResult << " bytes=" << sink->bytes().size()
            << std::endl;
  // The premise: HRESULT says success, opResult says "unsupported method", and
  // nothing was produced.
  PRISM_CHECK_EQ(extracted, S_OK);
  PRISM_CHECK_EQ(probe->opResultCalls, 1);
  PRISM_CHECK_EQ(probe->firstOpResult, 1);  // NOperationResult::kUnsupportedMethod
  PRISM_CHECK_EQ(sink->bytes().size(), size_t(0));

  probe->Release();
  sink->Release();
  raw->Close();
  raw->Release();
  stream->Release();
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 3. The product path: ExtractWithPlan must report FAILURE with an actionable
//    reason -- not "success, 0 bytes".
// ---------------------------------------------------------------------------
PRISM_TEST("unsupported method is a failure, not a 0-byte success") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring archive = dir + L"\\zstd_method.7z";
  const std::string payload = "hello prism zstd\n";
  WriteFile(archive, Build7z(payload, "hello.txt", kZstdMethodId, payload.size()));

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(archive);
  std::cout << "    open trace: " << reader.openTrace() << std::endl;
  if (!PRISM_CHECK(opened)) {
    std::filesystem::remove_all(dir);
    return;
  }
  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(1))) {
    reader.Close();
    std::filesystem::remove_all(dir);
    return;
  }

  const std::string outDir = prism::win::WideToUtf8(dir) + "/out";
  const auto plan = prism::SmartExtractionPlanner::Build(
      prism::win::WideToUtf8(archive), outDir, catalog,
      [](const std::string& p) { return std::filesystem::exists(p); });

  prism::win::SmartExtractCallback::Result result;
  const bool ok = reader.ExtractWithPlan(catalog, plan, plan.baseTargetDir, nullptr, &result);
  std::cout << "    ExtractWithPlan ok=" << (ok ? "true" : "false")
            << " filesWritten=" << result.filesWritten << " failedItems=" << result.failedItems
            << " firstFailure=" << prism::win::ToString(result.firstFailure) << std::endl;
  std::cout << "    reason: " << result.failureReason << std::endl;

  // The card's core assertion: S_OK from Extract() must NOT be reported as a
  // successful extraction. Before C26 this returned true with zero files.
  PRISM_CHECK(!ok);
  PRISM_CHECK(result.failed());
  PRISM_CHECK_EQ(result.failedItems, uint64_t(1));
  PRISM_CHECK_EQ(result.filesWritten, uint64_t(0));
  PRISM_CHECK_EQ(result.bytesWritten, uint64_t(0));
  PRISM_CHECK(result.firstFailure == prism::win::OpResult::kUnsupportedMethod);
  PRISM_CHECK_EQ(result.failedIndices.size(), size_t(1));
  PRISM_CHECK_EQ(result.failedPaths.size(), size_t(1));
  // Actionable text: names the method and says what to do about it.
  const std::string& reason = result.failureReason;
  PRISM_CHECK(reason.find("unsupported compression method") != std::string::npos);
  PRISM_CHECK(reason.find("zstd") != std::string::npos);
  PRISM_CHECK(reason.find("7-Zip ZS") != std::string::npos);
  PRISM_CHECK(reason.find("hello.txt") != std::string::npos);

  reader.Close();
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 4. The fake path (no engine at all): a callback handed opResult != 0 must
//    record a failure even though every HRESULT on the way was S_OK.
// ---------------------------------------------------------------------------
PRISM_TEST("a callback handed opResult != 0 reports failure") {
  std::vector<prism::ArchiveItem> catalog(1);
  catalog[0].index = 0;
  catalog[0].path = "hello.txt";
  catalog[0].size = 17;
  const std::string outDir = prism::win::WideToUtf8(MakeTempDir()) + "/out";
  const auto plan = prism::SmartExtractionPlanner::Build(
      "fake.7z", outDir, catalog, [](const std::string& p) { return std::filesystem::exists(p); });

  auto* callback = new prism::win::SmartExtractCallback(
      catalog, plan, plan.baseTargetDir, nullptr,
      [](uint32_t) { return std::string("04F71101"); });
  prism::win::abi::ISequentialOutStream* stream = nullptr;
  PRISM_CHECK_EQ(callback->GetStream(0, &stream, prism::win::abi::kExtract), S_OK);
  if (stream) stream->Release();
  // HRESULT S_OK here means "the report was handled", not "it worked".
  PRISM_CHECK_EQ(callback->SetOperationResult(1), S_OK);
  const auto& result = callback->result();
  PRISM_CHECK(result.failed());
  PRISM_CHECK_EQ(result.failedItems, uint64_t(1));
  PRISM_CHECK_EQ(result.filesWritten, uint64_t(0));
  PRISM_CHECK(result.firstFailure == prism::win::OpResult::kUnsupportedMethod);
  PRISM_CHECK(result.failureReason.find("zstd") != std::string::npos);
  callback->Release();
}

// ---------------------------------------------------------------------------
// 5. Unknown extension: the pre-C26 candidate probe must still open the file.
// ---------------------------------------------------------------------------
PRISM_TEST("unknown extension still opens through the candidate probe") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring archive = dir + L"\\payload.dat";
  WriteFile(archive, BuildZip({{"a.txt", "alpha"}, {"b.txt", "beta"}}));

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(archive);
  std::cout << "    open trace: " << reader.openTrace() << std::endl;
  PRISM_CHECK(opened);
  PRISM_CHECK(reader.openTrace().find("route=probe") != std::string::npos);
  if (opened) PRISM_CHECK_EQ(reader.ReadCatalog().size(), size_t(2));
  reader.Close();
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 6. No regression: a real .7z (Copy coder) and a real .zip still open through
//    their routed handler and still hand back the exact bytes.
// ---------------------------------------------------------------------------
PRISM_TEST("existing .7z behaviour does not regress") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring archive = dir + L"\\plain.7z";
  const std::string payload = "hello prism archive\n";
  WriteFile(archive, Build7z(payload, "hello.txt", kCopyMethodId, payload.size()));

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(archive);
  std::cout << "    open trace: " << reader.openTrace() << std::endl;
  if (!PRISM_CHECK(opened)) {
    std::filesystem::remove_all(dir);
    return;
  }
  PRISM_CHECK(reader.openTrace().find("route=7z") != std::string::npos);
  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(1))) {
    reader.Close();
    std::filesystem::remove_all(dir);
    return;
  }
  PRISM_CHECK_EQ(catalog[0].path, std::string("hello.txt"));
  PRISM_CHECK_EQ(catalog[0].size, static_cast<uint64_t>(payload.size()));
  const auto bytes = reader.ExtractToMemory(0);
  if (PRISM_CHECK(bytes.has_value())) {
    PRISM_CHECK_EQ(std::string(bytes->begin(), bytes->end()), payload);
  }
  reader.Close();
  std::filesystem::remove_all(dir);
}

PRISM_TEST("existing .zip behaviour does not regress") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring archive = dir + L"\\pack.zip";
  WriteFile(archive, BuildZip({{"Assets/logo.txt", "hello prism"}, {"README.md", "readme"}}));

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(archive);
  std::cout << "    open trace: " << reader.openTrace() << std::endl;
  if (!PRISM_CHECK(opened)) {
    std::filesystem::remove_all(dir);
    return;
  }
  PRISM_CHECK(reader.openTrace().find("route=zip") != std::string::npos);
  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(2))) {
    reader.Close();
    std::filesystem::remove_all(dir);
    return;
  }
  PRISM_CHECK_EQ(catalog[0].path, std::string("Assets/logo.txt"));
  const auto bytes = reader.ExtractToMemory(0);
  if (PRISM_CHECK(bytes.has_value())) {
    PRISM_CHECK_EQ(std::string(bytes->begin(), bytes->end()), std::string("hello prism"));
  }
  reader.Close();
  std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 7. Bare .zst. It has its own executable on purpose: an engine without a zstd
//    handler reports a visible SKIP instead of a fake green (D1/D21).
// ---------------------------------------------------------------------------
PRISM_TEST("bare .zst opens through the route table and extracts the payload") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring archive = dir + L"\\payload.ZST";  // uppercase on purpose
  const std::string payload = "hello prism zstd\n";
  WriteFile(archive, BuildZstdRawFrame(payload));

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(archive);
  std::cout << "    open trace: " << reader.openTrace() << std::endl;
  if (!PRISM_CHECK(opened)) {
    std::filesystem::remove_all(dir);
    return;
  }
  PRISM_CHECK(reader.openTrace().find("route=zstd") != std::string::npos);
  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(1))) {
    reader.Close();
    std::filesystem::remove_all(dir);
    return;
  }
  std::cout << "    catalog[0] path=" << catalog[0].path << " size=" << catalog[0].size
            << std::endl;
  const auto bytes = reader.ExtractToMemory(0);
  if (PRISM_CHECK(bytes.has_value())) {
    PRISM_CHECK_EQ(std::string(bytes->begin(), bytes->end()), payload);
  }
  reader.Close();
  std::filesystem::remove_all(dir);
}

}  // namespace

int main(int argc, char** argv) {
  g_dllPath = Locate7zDll();
  g_haveDll = !g_dllPath.empty();
  if (!g_haveDll) {
    std::cout << "SKIP: 7z.dll not found (set PRISM_7Z_DLL) -- no engine, no verdict"
              << std::endl;
    return 77;  // visible Skip via SKIP_RETURN_CODE, never a silent pass
  }
  std::wcout << L"7z.dll: " << g_dllPath << std::endl;
#ifdef PRISM_C26_ZST_ONLY
  if (!EngineHasZstd()) {
    std::cout << "SKIP: this engine has no zstd handler (7-Zip < 25.00) -- the"
                 " raw-.zst capability cannot be exercised on this machine"
              << std::endl;
    return 77;
  }
#endif
  const int rc = ::prism::test::RunAll(argc, argv);
  if (rc == 0) {
#ifdef PRISM_C26_ZST_ONLY
    std::cout << "ALL EXTRACT-FAILURE AND RAW-ZST CHECKS PASSED" << std::endl;
#else
    std::cout << "ALL EXTRACT-FAILURE CHECKS PASSED" << std::endl;
#endif
    std::cout.flush();
  }
#ifdef PRISM_C26_MUTANT_IGNORE_OPRESULT
  // CONTRACTS D3/D26: the marker is the conjunction -- red checks AND a proven
  // injection. A mutation that no longer reaches the code cannot print it, and
  // a mutant that somehow stays green prints nothing either.
  if (rc != 0 && prism::win::MutantIgnoredOpResultHits() > 0) {
    std::cout << "EXTRACT-FAILURE MUTANT CAUGHT (injections="
              << prism::win::MutantIgnoredOpResultHits() << ")" << std::endl;
    std::cout.flush();
  }
#endif
  return rc;
}
