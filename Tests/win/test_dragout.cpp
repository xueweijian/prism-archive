// Windows integration test (card C20): the drag-out chain, end to end, with the
// REAL 7z.dll -- no human, no Explorer required.
//
// What this proves (each line is an assertion below):
//   1. A catalog selection becomes a real IDataObject that ADVERTISES the two
//      formats Explorer needs (CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS) --
//      enumerated through EnumFormatEtc, not through our own bookkeeping.
//   2. GetData(CFSTR_FILEDESCRIPTORW) parses back into the exact file list:
//      basenames only, honest sizes, one entry per selected item, in the
//      selection's order (lindex order is what the Shell copies in).
//   3. GetData(CFSTR_FILECONTENTS) per lindex yields a byte-exact SHA-256 match
//      against a DIRECT 7z.exe extraction of the same archive -- the file the
//      user would get from "Extract here". Nothing shorter, nothing invented.
//   4. Seek into the middle of a SOLID item and back again is byte-exact, and
//      the solid re-decode budget is real: the third backward jump past the
//      lookback window is REFUSED with STG_E_INVALIDFUNCTION and the cursor
//      does not move (C11 slot + M4 budget, through the production stream).
//   5. ZERO %TEMP%: TMP/TEMP are redirected to an empty trap directory for the
//      whole drag-out read and the trap is still empty afterwards; the archive
//      directory gains no file either (recursive before/after snapshot).
//
// SKIP POLICY (D1): this test needs a real 7z.dll + 7z.exe. When they are
// missing it prints the reason and exits with 77, which the suite registers as
// SKIP_RETURN_CODE -- a skip is a labelled outcome, never a silent pass. The
// success marker below is a CONJUNCTION with exit 0.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "../mini_test.hpp"
#include "DragOutSession.hpp"
#include "PrismDragDecisionBridge.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

namespace {

constexpr int kSkipReturnCode = 77;
constexpr size_t kLookback = 256u * 1024u;

std::wstring g_dllPath;
std::wstring g_exePath;

// ---------------------------------------------------------------------------
// Small helpers (kept local so this TU never depends on another card's fixture)
// ---------------------------------------------------------------------------
std::wstring MakeTempDir(const wchar_t* tag) {
  // Card C36 (Tests/win/UniqueTempDir.hpp): card + pid + per-call COUNTER, and
  // the tree is owned by a process-lifetime list, so a case that returns early
  // still gets its directory removed at exit instead of poisoning the next run.
  static std::vector<std::unique_ptr<prism::testtmp::UniqueDir>> kept;
  kept.push_back(std::make_unique<prism::testtmp::UniqueDir>(L"c36", tag));
  const prism::testtmp::UniqueDir& dir = *kept.back();
  return dir.ok() ? dir.wstr() : std::wstring();
}

bool RemoveDir(const std::wstring& dir) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(dir), ec);
  return !ec;
}

// Offset-sensitive pseudo-random bytes: a read served from the wrong offset can
// never alias the expected content (a repeating pattern could pass at period
// multiples, which is exactly how a "seek is broken" bug hides).
std::string MakeContent(size_t bytes, uint32_t seed) {
  std::string out;
  out.resize(bytes);
  uint32_t state = seed | 1u;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    const uint32_t mix = static_cast<uint32_t>((i * 2654435761u) >> 13);
    out[i] = static_cast<char>(((state >> 16) ^ mix) & 0xFF);
  }
  return out;
}

bool WriteFileBytes(const std::wstring& dir, const std::wstring& name, const std::string& content) {
  std::ofstream out(std::filesystem::path(dir) / std::filesystem::path(name), std::ios::binary);
  if (!out) return false;
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(out);
}

bool ReadFileBytes(const std::wstring& path, std::string* out) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

std::string Hex(const uint8_t* bytes, size_t count) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(count * 2);
  for (size_t i = 0; i < count; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0xF];
  }
  return out;
}

// SHA-256 through CNG (bcrypt.lib): no OpenSSL, no extra dependency, and it is
// the same primitive the CI lane already links for C11.
bool Sha256(const std::string& bytes, std::string* hex) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return false;
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = false;
  do {
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) break;
    if (!bytes.empty() &&
        !BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                      static_cast<ULONG>(bytes.size()), 0))) {
      break;
    }
    uint8_t digest[32]{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) break;
    if (hex) *hex = Hex(digest, sizeof(digest));
    ok = true;
  } while (false);
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

bool ReadAll(IStream* stream, std::string* out, uint64_t expected) {
  out->clear();
  out->reserve(static_cast<size_t>(expected));
  char buffer[64 * 1024];
  for (;;) {
    ULONG got = 0;
    const HRESULT hr = stream->Read(buffer, sizeof(buffer), &got);
    if (FAILED(hr)) {
      std::cout << "    read failed hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
                << " after " << out->size() << "/" << expected << " bytes" << std::endl;
      return false;
    }
    if (got == 0) break;
    out->append(buffer, got);
    if (out->size() > expected) {
      std::cout << "    overdrawn: " << out->size() << " > declared " << expected << std::endl;
      return false;
    }
  }
  return out->size() == expected;
}

bool SeekIs(IStream* stream, int64_t offset, DWORD origin, uint64_t wantPosition) {
  LARGE_INTEGER move{};
  move.QuadPart = offset;
  ULARGE_INTEGER newPos{};
  const HRESULT hr = stream->Seek(move, origin, &newPos);
  if (hr != S_OK) {
    std::cout << "    seek origin=" << origin << " offset=" << offset << " hr=0x" << std::hex
              << static_cast<unsigned long>(hr) << std::dec << std::endl;
    return false;
  }
  return newPos.QuadPart == static_cast<LONGLONG>(wantPosition);
}

bool ReadWindow(IStream* stream, size_t count, std::string* out) {
  out->assign(count, '\0');
  ULONG got = 0;
  const HRESULT hr = stream->Read(out->data(), static_cast<ULONG>(count), &got);
  if (hr != S_OK || got != count) {
    std::cout << "    window read hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
              << " got=" << got << " want=" << count << std::endl;
    return false;
  }
  return true;
}

bool SameBytes(const std::string& got, const std::string& want, size_t offset) {
  if (offset > want.size()) return false;
  if (got.size() > want.size() - offset) return false;
  return std::memcmp(got.data(), want.data() + offset, got.size()) == 0;
}

// Recursive snapshot of a directory (names only). Used to prove the drag-out
// path creates nothing: a spilled temp file, a stray partial, a scratch dir.
std::vector<std::wstring> SnapshotDir(const std::wstring& dir) {
  std::vector<std::wstring> names;
  std::error_code ec;
  if (!std::filesystem::exists(std::filesystem::path(dir), ec)) return names;
  for (auto it = std::filesystem::recursive_directory_iterator(std::filesystem::path(dir), ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    names.push_back(it->path().wstring());
  }
  std::sort(names.begin(), names.end());
  return names;
}

struct SelectionCase {
  const wchar_t* name;   // basename inside the archive
  size_t bytes;
  uint32_t seed;
};

struct Expected {
  std::wstring fileName;
  uint64_t size = 0;
  std::string content;  // the exact bytes (source of truth for offsets)
  std::string sha256;
};

}  // namespace

int main() {
  if (!PRISM_CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))) return 1;

  g_dllPath = prism::fixture::DllPath();
  g_exePath = prism::fixture::ExePath();
  if (g_dllPath.empty() || g_exePath.empty()) {
    // D1: a labelled skip, never a silent pass.
    std::cout << "SKIP: 7z.dll/7z.exe not found (PRISM_7Z_DLL="
              << (g_dllPath.empty() ? "<unset>" : "set") << "); this suite needs the real engine"
              << std::endl;
    std::cout << "SKIP REASON: no real 7z.dll or 7z.exe on this machine -- the drag-out chain "
                 "cannot be verified without the decoder, and a green without it would be a lie"
              << std::endl;
    return kSkipReturnCode;
  }

  const std::wstring dir = MakeTempDir(L"dragout");
  if (!PRISM_CHECK(!dir.empty())) return 1;

  // 3 MiB is big enough that a 64 KiB read cannot buffer the whole item, so the
  // seek cases really walk the frontier; 0 bytes covers the empty-file edge.
  const SelectionCase kFiles[] = {
      {L"alpha.bin", 1024 * 1024, 0x1234u},
      {L"beta.txt", 4096, 0x2222u},
      {L"gamma.bin", 3 * 1024 * 1024, 0x3333u},
      {L"delta.log", 777, 0x4444u},
      {L"empty.bin", 0, 0x5555u},
  };
  constexpr size_t kFileCount = sizeof(kFiles) / sizeof(kFiles[0]);

  std::vector<std::wstring> packNames;
  std::vector<Expected> expectedBySource;  // in kFiles order
  bool wroteAll = true;
  for (const SelectionCase& file : kFiles) {
    Expected expected;
    expected.fileName = file.name;
    expected.size = file.bytes;
    expected.content = MakeContent(file.bytes, file.seed);
    if (!Sha256(expected.content, &expected.sha256)) wroteAll = false;
    wroteAll = wroteAll && WriteFileBytes(dir, file.name, expected.content);
    packNames.push_back(file.name);
    expectedBySource.push_back(std::move(expected));
  }
  if (!PRISM_CHECK(wroteAll)) {
    RemoveDir(dir);
    return 1;
  }

  const std::wstring archive = std::filesystem::path(dir) / L"c20solid.7z";
  if (!PRISM_CHECK(prism::fixture::PackSolid(g_exePath, dir, L"c20solid.7z", packNames))) {
    std::cout << "    PackSolid failed" << std::endl;
    RemoveDir(dir);
    return 1;
  }

  // Ground truth for "the same file a plain extraction produces". 7z.exe writes
  // it to disk ONCE, which is exactly the detour the drag path must not take.
  const std::wstring directDir = std::filesystem::path(dir) / L"direct";
  std::filesystem::create_directories(directDir);
  if (!PRISM_CHECK(prism::fixture::RunSevenZip(
          g_exePath, L"x \"" + archive + L"\" -o\"" + directDir + L"\" -y"))) {
    std::cout << "    direct extraction failed" << std::endl;
    RemoveDir(dir);
    return 1;
  }

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_dllPath))) {
    RemoveDir(dir);
    return 1;
  }

  // The window's reader: used ONLY to read the catalog, never to serve a stream
  // (that is the C11 boundary -- a 7z handler is not thread-safe).
  prism::win::ArchiveReader catalogReader(library);
  if (!PRISM_CHECK(catalogReader.Open(archive))) {
    std::cout << "    open failed: " << catalogReader.openTrace() << std::endl;
    RemoveDir(dir);
    return 1;
  }
  const std::vector<prism::ArchiveItem> catalog = catalogReader.ReadCatalog();
  if (!PRISM_CHECK(catalog.size() == kFileCount)) {
    RemoveDir(dir);
    return 1;
  }

  // Selection in a deliberately non-alphabetical order: lindex order must be
  // the SELECTION order (Explorer copies in descriptor order), which a test
  // that accidentally re-sorts could not distinguish.
  const wchar_t* kSelectionOrder[] = {L"gamma.bin", L"alpha.bin", L"empty.bin", L"delta.log",
                                      L"beta.txt"};
  std::vector<uint32_t> selection;
  std::vector<Expected> expected;
  for (const wchar_t* want : kSelectionOrder) {
    uint32_t found = UINT32_MAX;
    for (const prism::ArchiveItem& item : catalog) {
      if (!item.isDir && item.path == prism::win::WideToUtf8(want)) found = item.index;
    }
    if (!PRISM_CHECK(found != UINT32_MAX)) {
      RemoveDir(dir);
      return 1;
    }
    selection.push_back(found);
    for (const Expected& candidate : expectedBySource) {
      if (candidate.fileName == want) expected.push_back(candidate);
    }
  }
  if (!PRISM_CHECK(expected.size() == kFileCount)) {
    RemoveDir(dir);
    return 1;
  }

  prism::win::dragout::SessionConfig sessionConfig;
  sessionConfig.archivePath = archive;
  sessionConfig.maxHandlers = 3;  // < 5 items on purpose: slots must recycle
  sessionConfig.seekPolicy.useLookback = true;
  sessionConfig.seekPolicy.lookbackBytes = kLookback;
  sessionConfig.seekPolicy.maxRestarts = 2;
  sessionConfig.seekPolicy.restartDecodeBudget = 8u * 1024 * 1024;
  std::string createError;
  auto session = prism::win::dragout::DragOutSession::Create(library, catalog, selection,
                                                             sessionConfig, &createError);
  if (!PRISM_CHECK(session != nullptr)) {
    std::cout << "    session create failed: " << createError << std::endl;
    RemoveDir(dir);
    return 1;
  }

  // -------------------------------------------------------------------------
  // Zero-%TEMP% trap: from here until the trap check, TMP/TEMP point at an
  // empty directory. Any temp spill by the drag path lands there and is caught.
  // The fixture above already finished, so the decoder cannot pollute it.
  // -------------------------------------------------------------------------
  const std::wstring trapDir = std::filesystem::path(dir) / L"traptemp";
  CreateDirectoryW(trapDir.c_str(), nullptr);
  wchar_t oldTmp[MAX_PATH]{};
  wchar_t oldTemp[MAX_PATH]{};
  const bool hadTmp = GetEnvironmentVariableW(L"TMP", oldTmp, MAX_PATH) > 0;
  const bool hadTemp = GetEnvironmentVariableW(L"TEMP", oldTemp, MAX_PATH) > 0;
  SetEnvironmentVariableW(L"TMP", trapDir.c_str());
  SetEnvironmentVariableW(L"TEMP", trapDir.c_str());
  const std::vector<std::wstring> archiveDirBefore = SnapshotDir(dir);

  Microsoft::WRL::ComPtr<IDataObject> dataObject;
  const HRESULT buildHr = session->BuildDataObject(dataObject.GetAddressOf());
  if (!PRISM_CHECK(SUCCEEDED(buildHr) && dataObject != nullptr)) {
    std::cout << "    BuildDataObject hr=0x" << std::hex << static_cast<unsigned long>(buildHr)
              << std::dec << std::endl;
    RemoveDir(dir);
    return 1;
  }

  // (1) The object must ADVERTISE both formats -- enumerated like a real target.
  const CLIPFORMAT cfDescriptor =
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
  const CLIPFORMAT cfContents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
  if (!PRISM_CHECK(cfDescriptor != 0 && cfContents != 0)) {
    RemoveDir(dir);
    return 1;
  }
  {
    Microsoft::WRL::ComPtr<IEnumFORMATETC> formats;
    if (PRISM_CHECK(SUCCEEDED(dataObject->EnumFormatEtc(DATADIR_GET, formats.GetAddressOf())))) {
      FORMATETC got[8]{};
      ULONG fetched = 0;
      const HRESULT hr = formats->Next(8, got, &fetched);
      bool sawDescriptor = false;
      bool sawContents = false;
      for (ULONG i = 0; i < fetched; ++i) {
        if (got[i].cfFormat == cfDescriptor) sawDescriptor = (got[i].tymed & TYMED_HGLOBAL) != 0;
        if (got[i].cfFormat == cfContents) sawContents = (got[i].tymed & TYMED_ISTREAM) != 0;
      }
      PRISM_CHECK(fetched == 2);  // S_OK for a full read; S_FALSE would mean fewer
      PRISM_CHECK(hr == S_OK || hr == S_FALSE);
      PRISM_CHECK(sawDescriptor);
      PRISM_CHECK(sawContents);
      std::cout << "  [formats] enumerated=" << fetched << " descriptor=" << sawDescriptor
                << " contents=" << sawContents << std::endl;
    }
  }

  // (2) GetData(CFSTR_FILEDESCRIPTORW) -> the exact file list.
  {
    FORMATETC request{};
    request.cfFormat = cfDescriptor;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = -1;
    request.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    const HRESULT hr = dataObject->GetData(&request, &medium);
    if (!PRISM_CHECK(hr == S_OK)) {
      std::cout << "    GetData(descriptor) hr=0x" << std::hex
                << static_cast<unsigned long>(hr) << std::dec << std::endl;
    } else {
      PRISM_CHECK(medium.tymed == TYMED_HGLOBAL);
      auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
      if (PRISM_CHECK(group != nullptr)) {
        PRISM_CHECK(group->cItems == kFileCount);
        for (size_t i = 0; i < kFileCount && i < group->cItems; ++i) {
          const FILEDESCRIPTORW& entry = group->fgd[i];
          const std::wstring name(entry.cFileName);
          PRISM_CHECK(name == expected[i].fileName);
          PRISM_CHECK((entry.dwFlags & FD_FILESIZE) != 0 ||
                      expected[i].size == 0);  // a 0-byte file may omit FD_FILESIZE
          PRISM_CHECK((entry.dwFlags & FD_ATTRIBUTES) != 0);
          const uint64_t size =
              (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
          PRISM_CHECK(size == expected[i].size);
          std::cout << "  [descriptor] lindex=" << i << " name="
                    << prism::win::WideToUtf8(name) << " size=" << size
                    << " expected=" << expected[i].size << std::endl;
        }
        GlobalUnlock(medium.hGlobal);
      }
      ReleaseStgMedium(&medium);
    }
  }

  // (3) Per-item payload: byte-exact against the DIRECT extraction.
  {
    for (size_t lindex = 0; lindex < kFileCount; ++lindex) {
      FORMATETC request{};
      request.cfFormat = cfContents;
      request.dwAspect = DVASPECT_CONTENT;
      request.lindex = static_cast<LONG>(lindex);
      request.tymed = TYMED_ISTREAM;
      if (!PRISM_CHECK(dataObject->QueryGetData(&request) == S_OK)) continue;
      STGMEDIUM medium{};
      const HRESULT hr = dataObject->GetData(&request, &medium);
      if (!PRISM_CHECK(hr == S_OK && medium.pstm != nullptr)) {
        std::cout << "    GetData(contents, lindex=" << lindex << ") hr=0x" << std::hex
                  << static_cast<unsigned long>(hr) << std::dec << std::endl;
        continue;
      }
      PRISM_CHECK(medium.tymed == TYMED_ISTREAM);
      // B8: the payload must answer IID_IStream through QueryInterface (the
      // Shell QIs; a complete vtable that QI does not answer is invisible).
      // ISequentialStream is REPORTED, not asserted: the WRL shell declares
      // IStream only, and the TYMED_ISTREAM contract asks for IStream. If a
      // target ever QIs ISequentialStream directly this line is the evidence.
      {
        Microsoft::WRL::ComPtr<IStream> asStream;
        Microsoft::WRL::ComPtr<ISequentialStream> asSequential;
        PRISM_CHECK(SUCCEEDED(medium.pstm->QueryInterface(IID_PPV_ARGS(asStream.GetAddressOf()))));
        if (lindex == 0) {
          std::cout << "  [qi] IStream=ok ISequentialStream="
                    << (SUCCEEDED(medium.pstm->QueryInterface(
                            IID_PPV_ARGS(asSequential.GetAddressOf())))
                            ? "ok (declared)"
                            : "not-answered (IStream is the TYMED_ISTREAM contract)")
                    << std::endl;
        }
      }
      STATSTG stat{};
      if (PRISM_CHECK(SUCCEEDED(medium.pstm->Stat(&stat, STATFLAG_NONAME)))) {
        PRISM_CHECK(stat.type == STGTY_STREAM);
        PRISM_CHECK(stat.cbSize.QuadPart == expected[lindex].size);
      }
      std::string got;
      const bool readOk = ReadAll(medium.pstm, &got, expected[lindex].size);
      PRISM_CHECK(readOk);
      std::string gotSha;
      if (!PRISM_CHECK(Sha256(got, &gotSha))) {
        ReleaseStgMedium(&medium);
        continue;
      }
      // Direct extraction ground truth (the file 7z.exe wrote to disk).
      std::string direct;
      std::string directSha;
      const std::wstring directPath =
          std::filesystem::path(directDir) / std::filesystem::path(expected[lindex].fileName);
      PRISM_CHECK(ReadFileBytes(directPath, &direct));
      PRISM_CHECK(Sha256(direct, &directSha));
      PRISM_CHECK(gotSha == directSha);
      PRISM_CHECK(gotSha == expected[lindex].sha256);  // and the authored bytes
      std::cout << "  [contents] lindex=" << lindex << " bytes=" << got.size() << " sha256="
                << gotSha.substr(0, 16) << "==direct:" << directSha.substr(0, 16)
                << " match=" << (gotSha == directSha) << std::endl;
      ReleaseStgMedium(&medium);
    }
  }

  // (4) Seek into the middle of a solid item / back out, through the SAME
  // production stream factory the data object uses.
  {
    const size_t gammaIndex = 0;  // selection order starts with gamma.bin
    Microsoft::WRL::ComPtr<prism::win::dragdrop::DelayedExtractStream> stream =
        session->OpenStream(gammaIndex);
    if (PRISM_CHECK(static_cast<bool>(stream))) {
      Microsoft::WRL::ComPtr<IStream> raw;
      PRISM_CHECK(SUCCEEDED(stream.As(&raw)));
      const std::string& want = expected[gammaIndex].content;
      std::string window;

      // Forward seek: the decoder was never asked for the skipped bytes.
      PRISM_CHECK(ReadWindow(raw.Get(), 64 * 1024, &window));
      PRISM_CHECK(SameBytes(window, want, 0));
      PRISM_CHECK(SeekIs(raw.Get(), 1024 * 1024, STREAM_SEEK_SET, 1024 * 1024));
      PRISM_CHECK(ReadWindow(raw.Get(), 64 * 1024, &window));
      PRISM_CHECK(SameBytes(window, want, 1024 * 1024));
      const uint64_t restartsAfterForward = stream->restarts();
      PRISM_CHECK(restartsAfterForward == 0);  // forward seeks must not re-decode

      // A small back-step lands inside the lookback ring: zero decoder work.
      PRISM_CHECK(SeekIs(raw.Get(), -32 * 1024, STREAM_SEEK_CUR, 1024 * 1024 + 32 * 1024));
      PRISM_CHECK(ReadWindow(raw.Get(), 32 * 1024, &window));
      PRISM_CHECK(SameBytes(window, want, 1024 * 1024 + 32 * 1024));
      PRISM_CHECK(stream->restarts() == 0);

      // END-origin seek resolves against the honest size.
      PRISM_CHECK(SeekIs(raw.Get(), -4096, STREAM_SEEK_END, want.size() - 4096));
      PRISM_CHECK(ReadWindow(raw.Get(), 4096, &window));
      PRISM_CHECK(SameBytes(window, want, want.size() - 4096));

      // A back-step past the window means the SOLID block has to be re-decoded.
      PRISM_CHECK(SeekIs(raw.Get(), 512 * 1024, STREAM_SEEK_SET, 512 * 1024));
      PRISM_CHECK(ReadWindow(raw.Get(), 4096, &window));
      PRISM_CHECK(SameBytes(window, want, 512 * 1024));
      const uint64_t restartsAfterRestart = stream->restarts();
      PRISM_CHECK(restartsAfterRestart == 1);
      PRISM_CHECK(stream->wastedBytes() >= 512 * 1024);
      std::cout << "  [seek] forward-restarts=0 lookback-ok=1 restart=" << restartsAfterRestart
                << " wasted=" << stream->wastedBytes() << std::endl;

      // Budget: with maxRestarts=2 a third big back-step must be REFUSED, and
      // a refusal must not move the cursor or spend anything.
      PRISM_CHECK(SeekIs(raw.Get(), 200 * 1024, STREAM_SEEK_SET, 200 * 1024));
      PRISM_CHECK(ReadWindow(raw.Get(), 4096, &window));
      PRISM_CHECK(stream->restarts() == 2);
      const uint64_t before = stream->position();
      LARGE_INTEGER move{};
      move.QuadPart = 0;
      ULARGE_INTEGER newPos{};
      const HRESULT refused = raw.Get()->Seek(move, STREAM_SEEK_SET, &newPos);
      PRISM_CHECK(refused == STG_E_INVALIDFUNCTION);
      PRISM_CHECK(stream->position() == before);
      PRISM_CHECK(stream->restarts() == 2);
      PRISM_CHECK(!stream->diagnostics().empty());
      std::cout << "  [budget] third back-step refused hr=0x" << std::hex
                << static_cast<unsigned long>(refused) << std::dec << " restarts="
                << stream->restarts() << " reason-len=" << stream->diagnostics().size()
                << std::endl;

      // Out-of-range refusal, distinct from a budget refusal.
      move.QuadPart = static_cast<LONGLONG>(want.size() + 1);
      PRISM_CHECK(raw.Get()->Seek(move, STREAM_SEEK_SET, &newPos) != S_OK);
      std::cout << "  [seek] past-EOF refused as expected" << std::endl;
    }
  }

  // (5) Refusals are HRESULTs, never zeros: a directory has no contents stream
  // and an out-of-range lindex is rejected by the data object itself.
  {
    PRISM_CHECK(!session->OpenStream(kFileCount));
    FORMATETC request{};
    request.cfFormat = cfContents;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = static_cast<LONG>(kFileCount);
    request.tymed = TYMED_ISTREAM;
    STGMEDIUM medium{};
    PRISM_CHECK(dataObject->GetData(&request, &medium) == DV_E_LINDEX);
    std::cout << "  [refusal] bad lindex -> DV_E_LINDEX" << std::endl;
  }

  // ---- zero-%TEMP% verdict ------------------------------------------------
  const std::vector<std::wstring> trapAfter = SnapshotDir(trapDir);
  const std::vector<std::wstring> archiveDirAfter = SnapshotDir(dir);
  PRISM_CHECK(trapAfter.empty());
  PRISM_CHECK(archiveDirBefore == archiveDirAfter);
  if (!trapAfter.empty()) {
    for (const std::wstring& name : trapAfter) {
      std::cout << "    TRAP HIT: " << prism::win::WideToUtf8(name) << std::endl;
    }
  }

  const prism::win::dragout::SessionStats stats = session->stats();
  std::cout << "  [stats] handlers=" << stats.handlers << " opened=" << stats.streamsOpened
            << " refused=" << stats.streamsRefused << " bytesPiped=" << stats.bytesPiped
            << " solidRestarts=" << stats.solidRestarts
            << " maxConcurrent=" << stats.maxConcurrent
            << " tempFiles=" << stats.tempFilesWritten << std::endl;
  PRISM_CHECK(stats.tempFilesWritten == 0);
  PRISM_CHECK(stats.handlers == 3);          // the configured slot budget, no more
  PRISM_CHECK(stats.maxConcurrent <= 3);     // never a shared handler beyond it
  PRISM_CHECK(stats.streamsOpened >= kFileCount - 1);  // 5 items minus the empty one

  if (hadTmp) SetEnvironmentVariableW(L"TMP", oldTmp);
  if (hadTemp) SetEnvironmentVariableW(L"TEMP", oldTemp);

  // Teardown order: the data object holds streams that hold reader leases, so
  // it must die before the library and the dir (Windows refuses unlink of an
  // open file, which is also why the archive handle is closed explicitly).
  dataObject.Reset();
  session.reset();
  catalogReader.Close();
  RemoveDir(dir);

  if (prism::test::FailureCount() != 0) {
    std::cout << "C20 DRAG-OUT CHECKS FAILED (" << prism::test::FailureCount() << " failures / "
              << prism::test::CheckCount() << " checks)" << std::endl;
    return 1;
  }
  std::cout << "ALL DRAG-OUT CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  return 0;
}
