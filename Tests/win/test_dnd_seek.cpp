// Windows integration test (M4): the zero-copy drag stream (channel B) must
// really SEEK. A genuine SOLID 7z (5 files, one block, authored by the real
// 7-Zip CLI) is served through the CFSTR_FILECONTENTS IDataObject and through
// directly-wired DelayedExtractStream instances:
//   * Seek(0) + sequential read -> byte-exact original content;
//   * Seek(SET, 100) -> correct offset; Seek(SET, 0) back -> full re-read is
//     byte-exact via the RestartDecode path (the solid block is re-decoded on
//     the SAME 7z.dll session -- no re-Open, no catalog re-parse);
//   * small back-steps are served from the lookback ring with ZERO restarts;
//   * END-origin seeks resolve against the known total size;
//   * Stat() reports cbSize even under STATFLAG_NONAME (Explorer sizes its
//     progress UI from it; the old skeleton returned early and every file
//     looked zero-length);
//   * BOTH restart budgets are counted, and a spent budget REFUSES with a
//     reason while keeping the cursor untouched (no position lies).
// A missing 7z.dll/7z.exe must be a RED, never a silent no-op (CONTRACTS D1).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "../mini_test.hpp"
#include "PrismDragDecisionBridge.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"

namespace {

std::wstring g_dllPath;
std::wstring g_exePath;

std::wstring MakeTempDir(const wchar_t* tag) {
  wchar_t temp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, temp);
  if (n == 0 || n >= MAX_PATH) return {};
  std::wstring dir = std::wstring(temp) + L"prism_dnd_" + std::wstring(tag) + L"_" +
                     std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool RemoveDir(const std::wstring& dir) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(dir), ec);
  return !ec;
}

// Offset-sensitive pseudo-random bytes: a wrong-offset read can never alias
// the expected content (a repeating pattern could pass at period multiples).
std::string MakeContent(size_t bytes, uint32_t seed) {
  std::string out;
  out.resize(bytes);
  uint32_t state = seed;
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

bool ReadAll(IStream* stream, std::string* out, uint64_t expected) {
  out->clear();
  out->reserve(static_cast<size_t>(expected));
  char buffer[64 * 1024];
  for (;;) {
    ULONG got = 0;
    const HRESULT hr = stream->Read(buffer, sizeof(buffer), &got);
    if (FAILED(hr)) return false;
    if (got == 0) break;
    out->append(buffer, got);
    if (out->size() > expected) return false;
  }
  return out->size() == expected;
}

bool ReadExactly(IStream* stream, ULONG count, std::string* out) {
  out->assign(count, '\0');
  ULONG got = 0;
  const HRESULT hr = stream->Read(out->data(), count, &got);
  if (hr != S_OK || got != count) {
    std::cout << "    read hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
              << " got=" << got << " want=" << count << std::endl;
    return false;
  }
  return true;
}

bool ByteEquals(const std::string& got, const std::string& want, size_t offsetInWant) {
  if (offsetInWant > want.size()) return false;
  if (got.size() > want.size() - offsetInWant) return false;
  return std::memcmp(got.data(), want.data() + offsetInWant, got.size()) == 0;
}

bool SeekTo(IStream* stream, int64_t offset, DWORD origin) {
  LARGE_INTEGER move{};
  move.QuadPart = offset;
  ULARGE_INTEGER newPos{};
  const HRESULT hr = stream->Seek(move, origin, &newPos);
  if (hr != S_OK) {
    std::cout << "    seek hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
              << std::endl;
    return false;
  }
  return true;
}

bool CurrentPositionIs(IStream* stream, uint64_t want) {
  LARGE_INTEGER zero{};
  ULARGE_INTEGER pos{};
  if (stream->Seek(zero, STREAM_SEEK_CUR, &pos) != S_OK) return false;
  return pos.QuadPart == static_cast<LONGLONG>(want);
}

// Tiny ring on purpose: every path (hit, miss, budget refusal) is reachable
// with a 1.2 MiB item instead of a multi-GB block.
// Deliberately tight, but not absurd: 100 bytes of lookback (the first value
// tried here) is smaller than a single Read() of most consumers, so the e2e
// case degraded into "restart for every 100 bytes" and died at the frontier
// while the producer was still ahead of it. The budgets exist to make the
// RESTART path reachable, not to make ordinary sequential reads impossible --
// so they are tight in restart COUNT and REALISTIC in lookback.
//
// Every boundary below is expressed in terms of kLookback, never as a magic
// number: the earlier version hard-coded -101 from the 100-byte era and kept
// asserting a restart for a rewind that was by then comfortably inside the
// window. A constant tied to the policy cannot drift away from it.
constexpr size_t kLookback = 256u * 1024u;

prism::dnd::SeekPolicy TinyPolicy(size_t maxRestarts, size_t byteBudget) {
  prism::dnd::SeekPolicy policy;
  policy.useLookback = true;
  policy.lookbackBytes = kLookback;
  policy.maxRestarts = maxRestarts;
  policy.restartDecodeBudget = byteBudget;
  policy.restartsUsed = 0;
  policy.restartBytesUsed = 0;
  return policy;
}

}  // namespace

int main() {
  if (!PRISM_CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))) return 1;

  g_dllPath = prism::fixture::DllPath();
  g_exePath = prism::fixture::ExePath();
  // No silent skip: without the real DLL and CLI there is nothing to verify,
  // and a "pass" here would be a lie (CONTRACTS D1).
  if (!PRISM_CHECK(!g_dllPath.empty())) {
    std::cout << "    7z.dll missing (PRISM_7Z_DLL) -- FAILING, not skipping" << std::endl;
    return 1;
  }
  if (!PRISM_CHECK(!g_exePath.empty())) {
    std::cout << "    7z.exe missing beside 7z.dll -- FAILING, not skipping" << std::endl;
    return 1;
  }

  const std::wstring dir = MakeTempDir(L"seek");
  if (!PRISM_CHECK(!dir.empty())) return 1;

  // Five distinct files; gamma.bin (the middle one) is the drag target.
  const std::string contentAlpha = MakeContent(700 * 1024, 11);
  const std::string contentBeta = MakeContent(900 * 1024, 22);
  const std::string contentGamma = MakeContent(1200 * 1024, 33);
  const std::string contentDelta = MakeContent(800 * 1024, 44);
  const std::string contentEpsilon = MakeContent(1000 * 1024, 55);
  const uint64_t kGamma = contentGamma.size();
  if (!PRISM_CHECK(WriteFileBytes(dir, L"alpha.bin", contentAlpha))) return 1;
  if (!PRISM_CHECK(WriteFileBytes(dir, L"beta.bin", contentBeta))) return 1;
  if (!PRISM_CHECK(WriteFileBytes(dir, L"gamma.bin", contentGamma))) return 1;
  if (!PRISM_CHECK(WriteFileBytes(dir, L"delta.bin", contentDelta))) return 1;
  if (!PRISM_CHECK(WriteFileBytes(dir, L"epsilon.bin", contentEpsilon))) return 1;

  const std::vector<std::wstring> names = {L"alpha.bin", L"beta.bin", L"gamma.bin",
                                           L"delta.bin", L"epsilon.bin"};
  if (!PRISM_CHECK(prism::fixture::PackSolid(g_exePath, dir, L"solid.7z", names))) return 1;
  const std::wstring archive = dir + L"\\solid.7z";

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_dllPath))) return 1;
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(archive))) {
    std::cout << "    openTrace: " << reader.openTrace() << std::endl;
    RemoveDir(dir);
    return 1;
  }
  const std::vector<prism::ArchiveItem> catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(5))) { RemoveDir(dir); return 1; }

  // Fixture sanity: one genuine solid block holds all five files (A7 says the
  // ENTRY ORDER may be permuted by the solid packer; the BLOCK must be one).
  uint32_t gammaCatalogIndex = UINT32_MAX;
  std::vector<uint32_t> nonDirCatalogIndices;
  uint32_t solidBlock = UINT32_MAX;
  for (const prism::ArchiveItem& item : catalog) {
    if (item.isDir) continue;
    nonDirCatalogIndices.push_back(item.index);
    if (solidBlock == UINT32_MAX) solidBlock = item.block;
    if (item.path.size() >= 9 && item.path.compare(item.path.size() - 9, 9, "gamma.bin") == 0) {
      gammaCatalogIndex = item.index;
    }
  }
  if (!PRISM_CHECK_EQ(nonDirCatalogIndices.size(), size_t(5))) { RemoveDir(dir); return 1; }
  if (!PRISM_CHECK(gammaCatalogIndex != UINT32_MAX)) { RemoveDir(dir); return 1; }
  // Every item must be in the SAME block, and that block must be a real one
  // (block 0 would also be what a silently-zeroed kpidBlock looks like, so the
  // count of distinct blocks is asserted too -- contract A3).
  for (const prism::ArchiveItem& item : catalog) {
    if (item.isDir) continue;
    if (!PRISM_CHECK_EQ(item.block, solidBlock)) { RemoveDir(dir); return 1; }
  }
  std::cout << "    fixture: 5 files in solid block " << solidBlock << std::endl;

  // Restartable stream for one catalog item with a directly observable producer.
  struct Wired {
    HRESULT hr = E_FAIL;
    Microsoft::WRL::ComPtr<prism::win::dragdrop::DelayedExtractStream> stream;
    std::shared_ptr<prism::win::dragdrop::ArchiveItemStreamProducer> producer;
  };
  auto makeStream = [&](size_t maxRestarts, size_t byteBudget = 64u * 1024 * 1024) {
    Wired wired;
    wired.producer = std::make_shared<prism::win::dragdrop::ArchiveItemStreamProducer>(
        &reader, gammaCatalogIndex);
    wired.hr = Microsoft::WRL::MakeAndInitialize<prism::win::dragdrop::DelayedExtractStream>(
        &wired.stream, wired.producer, kGamma, TinyPolicy(maxRestarts, byteBudget));
    return wired;
  };

  // The production path: decision core -> sanitised descriptors -> DataObject
  // whose lindex->IStream factory wires a restartable 7z producer per item.
  const auto group = prism::dnd::BuildGroupDescriptor(catalog);
  if (!PRISM_CHECK_EQ(group.error, prism::dnd::DenyReason::None)) { RemoveDir(dir); return 1; }
  auto streamFactory = [&reader, &catalog](size_t lindex) -> Microsoft::WRL::ComPtr<IStream> {
    if (lindex >= catalog.size()) return nullptr;
    return prism::win::dragdrop::CreateDelayedExtractStream(&reader, catalog[lindex].index,
                                                            catalog[lindex].size,
                                                            TinyPolicy(3, 64u * 1024 * 1024));
  };
  Microsoft::WRL::ComPtr<IDataObject> data;
  if (!PRISM_CHECK(SUCCEEDED(prism::win::dragdrop::CreateVirtualFileDataObject(
          group, streamFactory, &data)))) {
    RemoveDir(dir);
    return 1;
  }

  const CLIPFORMAT cfContents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
  const CLIPFORMAT cfDescriptor =
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
  if (!PRISM_CHECK(cfContents != 0 && cfDescriptor != 0)) { RemoveDir(dir); return 1; }

  // Position of gamma in the descriptor list (BuildGroupDescriptor preserves
  // the catalog order of non-directory items, so the mapping is positional).
  size_t gammaLindex = SIZE_MAX;
  for (size_t i = 0; i < group.files.size(); ++i) {
    if (group.files[i].size == kGamma) gammaLindex = i;
  }
  if (!PRISM_CHECK(gammaLindex != SIZE_MAX)) { RemoveDir(dir); return 1; }

  // ---- E2E: DataObject pulls the MIDDLE file's stream, sequential read ----
  {
    FORMATETC etc{};
    etc.cfFormat = cfContents;
    etc.dwAspect = DVASPECT_CONTENT;
    etc.lindex = static_cast<LONG>(gammaLindex);
    etc.tymed = TYMED_ISTREAM;
    STGMEDIUM medium{};
    if (!PRISM_CHECK_EQ(data->GetData(&etc, &medium), S_OK)) { RemoveDir(dir); return 1; }
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(medium.pstm);  // take ownership from the medium
    STATSTG stat{};
    // Stat under STATFLAG_NONAME must still report cbSize: Explorer sizes its
    // progress UI from it, and the old skeleton returned before setting it.
    if (!PRISM_CHECK_EQ(stream->Stat(&stat, STATFLAG_NONAME), S_OK)) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stat.cbSize.QuadPart, static_cast<LONGLONG>(kGamma))) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK_EQ(stat.type, static_cast<DWORD>(STGTY_STREAM))) { RemoveDir(dir); return 1; }
    std::string got;
    if (!PRISM_CHECK(ReadAll(stream.Get(), &got, kGamma))) {
      std::cout << "    e2e read failed; reproducing on the observable channel" << std::endl;
      Wired w = makeStream(3);
      std::string got2;
      const bool direct = ReadAll(w.stream.Get(), &got2, kGamma);
      std::cout << "    direct read ok=" << direct << " bytes=" << got2.size()
              << " kGamma=" << kGamma
              << " produced=" << w.producer->producedBytes()
              << " (delta=" << (w.producer->producedBytes() - got2.size()) << ")" << std::endl;
    // The decisive question: does the producer say it delivered kGamma or not?
    // If produced == kGamma while the consumer stopped at 1114112, the bytes
    // were produced and then lost in the pipe. If produced == 1114112 too, the
    // ENGINE stopped early and the pipe is innocent.
    std::cout << "    item error code: " << w.producer->itemErrorCode() << std::endl;
    std::cout << "    accounting: produced(" << w.producer->producedBytes()
              << ") vs consumed(" << got2.size() << ") vs expected(" << kGamma
              << ")" << std::endl;
      std::cout << "    stream diag: " << w.stream->diagnostics()
                << " restarts=" << w.stream->restarts()
                << " restartBytes=" << w.stream->restartBytesUsed() << std::endl;
      std::cout << "    producer: failed=" << w.producer->workerFailed()
                << " produced=" << w.producer->producedBytes() << std::endl;
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK(ByteEquals(got, contentGamma, 0))) {
      std::cout << "    E2E sequential read is not byte-exact" << std::endl;
      RemoveDir(dir);
      return 1;
    }
    std::cout << "    e2e: gamma.bin streamed byte-exact (" << got.size() << " bytes)" << std::endl;
  }

  // ---- E2E: the first file of the same solid block is equally exact --------
  {
    size_t alphaLindex = SIZE_MAX;
    for (size_t i = 0; i < group.files.size(); ++i) {
      if (group.files[i].size == contentAlpha.size()) alphaLindex = i;
    }
    if (!PRISM_CHECK(alphaLindex != SIZE_MAX)) { RemoveDir(dir); return 1; }
    FORMATETC etc{};
    etc.cfFormat = cfContents;
    etc.dwAspect = DVASPECT_CONTENT;
    etc.lindex = static_cast<LONG>(alphaLindex);
    etc.tymed = TYMED_ISTREAM;
    STGMEDIUM medium{};
    if (!PRISM_CHECK_EQ(data->GetData(&etc, &medium), S_OK)) { RemoveDir(dir); return 1; }
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(medium.pstm);
    std::string got;
    if (!PRISM_CHECK(ReadAll(stream.Get(), &got, contentAlpha.size()))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(got, contentAlpha, 0))) { RemoveDir(dir); return 1; }
  }

  // ---- E2E: GetDataHere copies into the caller's own IStream medium --------
  {
    FORMATETC etc{};
    etc.cfFormat = cfContents;
    etc.dwAspect = DVASPECT_CONTENT;
    etc.lindex = static_cast<LONG>(gammaLindex);
    etc.tymed = TYMED_ISTREAM;
    STGMEDIUM medium{};
    medium.tymed = TYMED_ISTREAM;
    IStream* callerStream = nullptr;
    if (!PRISM_CHECK(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &callerStream)))) {
      RemoveDir(dir);
      return 1;
    }
    medium.pstm = callerStream;  // caller-owned; we write into it, never free it
    if (!PRISM_CHECK_EQ(data->GetDataHere(&etc, &medium), S_OK)) { RemoveDir(dir); return 1; }
    LARGE_INTEGER zero{};
    if (!PRISM_CHECK_EQ(medium.pstm->Seek(zero, STREAM_SEEK_SET, nullptr), S_OK)) {
      RemoveDir(dir);
      return 1;
    }
    std::string got;
    if (!PRISM_CHECK(ReadAll(medium.pstm, &got, kGamma))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(got, contentGamma, 0))) {
      std::cout << "    GetDataHere copy is not byte-exact" << std::endl;
      RemoveDir(dir);
      return 1;
    }
    // Specific HRESULTs, not a blanket refusal: wrong lindex / wrong medium
    // type / wrong format each name their own failure.
    FORMATETC badIndex = etc;
    badIndex.lindex = 99;
    PRISM_CHECK_EQ(data->GetDataHere(&badIndex, &medium), DV_E_LINDEX);
    FORMATETC badTymed = etc;
    badTymed.tymed = TYMED_HGLOBAL;
    PRISM_CHECK_EQ(data->GetDataHere(&badTymed, &medium), DV_E_TYMED);
    FORMATETC badFormat = etc;
    badFormat.cfFormat = static_cast<CLIPFORMAT>(42);
    PRISM_CHECK_EQ(data->GetDataHere(&badFormat, &medium), DV_E_FORMATETC);
    medium.pstm->Release();  // the caller-owned medium stream: we free our probe
    medium.pstm = nullptr;
    std::cout << "    GetDataHere: byte-exact + specific failure HRESULTs" << std::endl;
  }

  // ---- Seek machinery on a directly-wired stream (producer observable) -----
  {
    Wired made = makeStream(2);
    if (!PRISM_CHECK(SUCCEEDED(made.hr))) { RemoveDir(dir); return 1; }
    auto stream = std::move(made.stream);
    // (a) Seek(0) + full sequential read, byte-exact, no restart needed.
    if (!PRISM_CHECK(SeekTo(stream.Get(), 0, STREAM_SEEK_SET))) { RemoveDir(dir); return 1; }
    std::string got;
    if (!PRISM_CHECK(ReadAll(stream.Get(), &got, kGamma))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(got, contentGamma, 0))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(0))) { RemoveDir(dir); return 1; }

    // (b) Seek(SET, 100): backward across the whole file -> RestartDecode on
    // the SAME session, then a small read at the exact offset.
    if (!PRISM_CHECK(SeekTo(stream.Get(), 100, STREAM_SEEK_SET))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(1))) { RemoveDir(dir); return 1; }
    std::string window;
    if (!PRISM_CHECK(ReadExactly(stream.Get(), 50, &window))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(window, contentGamma, 100))) { RemoveDir(dir); return 1; }

    // (c) Seek(SET, 0): back past the ring again -> second restart, full
    // re-read byte-exact.
    if (!PRISM_CHECK(SeekTo(stream.Get(), 0, STREAM_SEEK_SET))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(2))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ReadAll(stream.Get(), &got, kGamma))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(got, contentGamma, 0))) { RemoveDir(dir); return 1; }
    // The producer really re-ran Extract (session kept, no re-Open): its own
    // restart counter matches, and the reader is still the same open session.
    if (!PRISM_CHECK_EQ(made.producer->restarts(), uint64_t(2))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(reader.opened())) { RemoveDir(dir); return 1; }
    // ...and the wasted re-decode is accounted byte-for-byte: 100 + 0.
    if (!PRISM_CHECK_EQ(stream->wastedBytes(), uint64_t(100))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stream->restartBytesUsed(), uint64_t(100))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(stream->diagnostics().find("restart#2") != std::string::npos)) {
      RemoveDir(dir);
      return 1;
    }

    // (d) The restart COUNT budget (maxRestarts=2) is spent: the next rewind
    // REFUSES with a reason and never moves the cursor.
    LARGE_INTEGER target{};
    target.QuadPart = 100;
    ULARGE_INTEGER newPos{};
    const HRESULT refused = stream->Seek(target, STREAM_SEEK_SET, &newPos);
    if (!PRISM_CHECK(FAILED(refused))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(refused, static_cast<HRESULT>(STG_E_INVALIDFUNCTION))) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK(CurrentPositionIs(stream.Get(), kGamma))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(stream->diagnostics().find("restart budget exhausted") !=
                     std::string::npos)) {
      RemoveDir(dir);
      return 1;
    }
    // (e) The stream stays honest after a refusal: EOF is still EOF.
    char probe[8];
    ULONG got8 = 0;
    if (!PRISM_CHECK_EQ(stream->Read(probe, sizeof(probe), &got8), S_FALSE)) {
      RemoveDir(dir);
      return 1;
    }
    std::cout << "    seek: 2 restarts, wasted=" << stream->wastedBytes()
              << ", refusal after budget kept cursor at " << kGamma << std::endl;
  }

  // ---- Lookback hits: small back-steps re-read with ZERO restarts ----------
  {
    Wired made = makeStream(4);
    if (!PRISM_CHECK(SUCCEEDED(made.hr))) { RemoveDir(dir); return 1; }
    auto stream = std::move(made.stream);
    std::string got;
    if (!PRISM_CHECK(ReadAll(stream.Get(), &got, kGamma))) { RemoveDir(dir); return 1; }
    // CUR, -50: inside the ring -> lookback hit, content exact, no restart.
    if (!PRISM_CHECK(SeekTo(stream.Get(), -50, STREAM_SEEK_CUR))) { RemoveDir(dir); return 1; }
    std::string tail;
    if (!PRISM_CHECK(ReadExactly(stream.Get(), 50, &tail))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(tail, contentGamma, kGamma - 50))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(0))) { RemoveDir(dir); return 1; }
    // CUR, -lookbackBytes: the INCLUSIVE boundary the planner grants. This is
    // the case that used to break: with whole-chunk eviction the ring only
    // held lookbackBytes - pullChunk, so the planner said "hit", the read then
    // found a residency gap, and the stream FAILED where a restart was owed.
    // Zero restarts AND byte-exact content is what "the window is as big as it
    // claims" looks like from the outside.
    if (!PRISM_CHECK(SeekTo(stream.Get(), -static_cast<int64_t>(kLookback), STREAM_SEEK_CUR))) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(0))) { RemoveDir(dir); return 1; }
    std::string boundary;
    if (!PRISM_CHECK(ReadExactly(stream.Get(), 4096, &boundary))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(boundary, contentGamma, kGamma - kLookback))) {
      RemoveDir(dir);
      return 1;
    }
    // ...and the cursor is back at the end, so ONE BYTE past the window needs
    // an honest restart rather than a guess.
    if (!PRISM_CHECK(SeekTo(stream.Get(), 0, STREAM_SEEK_END))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(SeekTo(stream.Get(), -(static_cast<int64_t>(kLookback) + 1),
                             STREAM_SEEK_CUR))) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(1))) { RemoveDir(dir); return 1; }
    std::string again;
    if (!PRISM_CHECK(ReadExactly(stream.Get(), 64, &again))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(again, contentGamma, kGamma - kLookback - 1))) {
      RemoveDir(dir);
      return 1;
    }
    std::cout << "    lookback: -" << kLookback << " hit (0 restarts), -"
              << (kLookback + 1) << " fell through to restart #1"
              << std::endl;
  }

  // ---- The BYTE budget refuses an oversized re-decode up front -------------
  {
    // maxRestarts is generous (4) so only the byte budget can bite: this is
    // the check that the count budget was NOT the whole story. 64 KiB budget,
    // a 1.2 MiB item -> any rewind needing more than 64 KiB of prefix decodes
    // must be refused BEFORE a single byte is pulled.
    Wired made = makeStream(4, 64u * 1024);
    if (!PRISM_CHECK(SUCCEEDED(made.hr))) { RemoveDir(dir); return 1; }
    auto stream = std::move(made.stream);
    std::string got;
    if (!PRISM_CHECK(ReadAll(stream.Get(), &got, kGamma))) { RemoveDir(dir); return 1; }
    const uint64_t producedBefore = made.producer->producedBytes();
    // Absolute target: a rewind this far back is outside the ring (so it is
    // not a lookback hit) and re-decoding its 688 KiB prefix blows the 64 KiB
    // budget. STREAM_SEEK_CUR would have been nonsense here -- the cursor is
    // already at kGamma, so "back by kGamma - 64" resolves BEYOND the end and
    // gets refused by the range check instead, which tests nothing about the
    // budget.
    LARGE_INTEGER back{};
    back.QuadPart = static_cast<LONGLONG>(kGamma - 2 * kLookback);
    const HRESULT refused = stream->Seek(back, STREAM_SEEK_SET, nullptr);
    if (!PRISM_CHECK_EQ(refused, static_cast<HRESULT>(STG_E_INVALIDFUNCTION))) {
      RemoveDir(dir);
      return 1;
    }
    // Nothing was decoded for the refused seek: the budget was checked before
    // the decoder ran (a refusal must never look like free work).
    if (!PRISM_CHECK_EQ(made.producer->producedBytes(), producedBefore)) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(0))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(CurrentPositionIs(stream.Get(), kGamma))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(stream->diagnostics().find("re-decode budget exhausted") !=
                     std::string::npos)) {
      RemoveDir(dir);
      return 1;
    }
    std::cout << "    byte budget: oversized rewind refused before any decode" << std::endl;
  }

  // ---- A non-resident lookback window with a spent budget REFUSES ---------
  {
    // maxRestarts=0: the planner keeps granting LookbackHit, but when the
    // ring no longer holds the window the stream must refuse instead of
    // restarting beyond budget (or worse, silently serving wrong bytes).
    Wired guarded = makeStream(0);
    if (!PRISM_CHECK(SUCCEEDED(guarded.hr))) { RemoveDir(dir); return 1; }
    auto stream = std::move(guarded.stream);
    std::string got;
    if (!PRISM_CHECK(ReadAll(stream.Get(), &got, kGamma))) { RemoveDir(dir); return 1; }
    // CUR, -50 lands inside the ring (no budget needed).
    if (!PRISM_CHECK(SeekTo(stream.Get(), -50, STREAM_SEEK_CUR))) { RemoveDir(dir); return 1; }
    std::string partial;
    if (!PRISM_CHECK(ReadExactly(stream.Get(), 10, &partial))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(partial, contentGamma, kGamma - 50))) { RemoveDir(dir); return 1; }
    // CUR, -(kLookback+1) from kGamma-40: the target is one byte OUTSIDE the
    // retained window, so the planner says restart -- and with zero restarts
    // left that must be a clean refusal, not a decode and not a wrong byte.
    LARGE_INTEGER outside{};
    outside.QuadPart = -static_cast<LONGLONG>(kLookback + 1);
    if (!PRISM_CHECK_EQ(stream->Seek(outside, STREAM_SEEK_CUR, nullptr),
                        static_cast<HRESULT>(STG_E_INVALIDFUNCTION))) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(0))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(CurrentPositionIs(stream.Get(), kGamma - 40))) { RemoveDir(dir); return 1; }
    std::cout << "    non-resident lookback with 0 budget: refused, cursor pinned"
              << std::endl;
  }

  // ---- END-origin seeks resolve against the known total size ---------------
  {
    Wired made = makeStream(4);
    if (!PRISM_CHECK(SUCCEEDED(made.hr))) { RemoveDir(dir); return 1; }
    auto stream = std::move(made.stream);
    // END + 0 = the exact end: reading there is EOF, no decoder work.
    if (!PRISM_CHECK(SeekTo(stream.Get(), 0, STREAM_SEEK_END))) { RemoveDir(dir); return 1; }
    char probe[4];
    ULONG got4 = 0;
    if (!PRISM_CHECK_EQ(stream->Read(probe, sizeof(probe), &got4), S_FALSE)) {
      RemoveDir(dir);
      return 1;
    }
    // END - kLookback on a COLD stream: nothing has been read yet, so the ring
    // is empty and this must be an honest restart -- NOT a free hit. That is
    // the real Explorer opening sequence (Stat, seek to end, seek back), and
    // asserting the restart here proves END-origin resolves to an absolute
    // target that the re-decode path actually consumes. The warm-stream free
    // hit at the same offset is covered in the lookback case above; both sides
    // of the distinction matter, and this one is the one a cold stream sees.
    if (!PRISM_CHECK(SeekTo(stream.Get(), -static_cast<int64_t>(kLookback), STREAM_SEEK_END))) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(1))) { RemoveDir(dir); return 1; }
    std::string window;
    if (!PRISM_CHECK(ReadExactly(stream.Get(), 32, &window))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(window, contentGamma,
                                static_cast<size_t>(static_cast<int64_t>(kGamma) - kLookback)))) {
      RemoveDir(dir);
      return 1;
    }
    // END + 0 is a pure forward no-op from anywhere: it must not spend another
    // restart, and the next byte-past-the-window seek pays exactly restart #2.
    if (!PRISM_CHECK(SeekTo(stream.Get(), 0, STREAM_SEEK_END))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(1))) { RemoveDir(dir); return 1; }
    const int64_t kBack = static_cast<int64_t>(kLookback) + 1;
    if (!PRISM_CHECK(SeekTo(stream.Get(), -kBack, STREAM_SEEK_END))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK_EQ(stream->restarts(), uint64_t(2))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ReadExactly(stream.Get(), 32, &window))) { RemoveDir(dir); return 1; }
    if (!PRISM_CHECK(ByteEquals(window, contentGamma,
                                static_cast<size_t>(static_cast<int64_t>(kGamma) - kBack)))) {
      RemoveDir(dir);
      return 1;
    }
    // Fresh stream: past-EOF and negative targets are refused distinctly and
    // the cursor stays put.
    Wired guarded = makeStream(4);
    if (!PRISM_CHECK(SUCCEEDED(guarded.hr))) { RemoveDir(dir); return 1; }
    LARGE_INTEGER beyond{};
    beyond.QuadPart = static_cast<LONGLONG>(kGamma) + 1;
    if (!PRISM_CHECK_EQ(guarded.stream->Seek(beyond, STREAM_SEEK_SET, nullptr),
                        static_cast<HRESULT>(STG_E_INVALIDFUNCTION))) {
      RemoveDir(dir);
      return 1;
    }
    LARGE_INTEGER negative{};
    negative.QuadPart = -1;
    if (!PRISM_CHECK_EQ(guarded.stream->Seek(negative, STREAM_SEEK_SET, nullptr),
                        static_cast<HRESULT>(STG_E_INVALIDFUNCTION))) {
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK(CurrentPositionIs(guarded.stream.Get(), 0))) { RemoveDir(dir); return 1; }
    std::cout << "    END-origin + out-of-range refusals: cursor never lied" << std::endl;
  }

  // ---- MEASUREMENT: restart cost vs ring cost on THIS machine ---------------
  // The default policy numbers came from Linux (Tests/test_dnd_seek.cpp); this
  // prints the same shape of number from the Windows runner so a future policy
  // change has a real datapoint on the platform that ships. REPORT-ONLY: the
  // assertions above are the gate, this line is the evidence (METHODOLOGY L5).
  {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const auto secondsSince = [&freq](const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
      return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(freq.QuadPart);
    };
    Wired made = makeStream(4, 1u << 30);
    if (!PRISM_CHECK(SUCCEEDED(made.hr))) { RemoveDir(dir); return 1; }
    auto stream = std::move(made.stream);
    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    // Sequential read of the whole item: the solid prefix decode, once.
    QueryPerformanceCounter(&t0);
    std::string got;
    const bool seqOk = ReadAll(stream.Get(), &got, kGamma);
    QueryPerformanceCounter(&t1);
    const double seqSec = secondsSince(t0, t1);
    // Back-step served by the ring (inside the lookback window).
    QueryPerformanceCounter(&t0);
    const bool ringSeekOk = SeekTo(stream.Get(), -50, STREAM_SEEK_CUR);
    std::string probeTail;
    const bool ringReadOk = ReadExactly(stream.Get(), 50, &probeTail);
    QueryPerformanceCounter(&t1);
    const double ringSec = secondsSince(t0, t1);
    // Back-step served by an honest restart (rewind to 0, decode 50 bytes).
    QueryPerformanceCounter(&t0);
    const bool restartSeekOk = SeekTo(stream.Get(), 0, STREAM_SEEK_SET);
    std::string probeHead;
    const bool restartReadOk = ReadExactly(stream.Get(), 50, &probeHead);
    QueryPerformanceCounter(&t1);
    const double restartSec = secondsSince(t0, t1);
    if (!PRISM_CHECK(seqOk && ringSeekOk && ringReadOk && restartSeekOk && restartReadOk)) {
      RemoveDir(dir);
      return 1;
    }
    const double mib = static_cast<double>(kGamma) / (1024.0 * 1024.0);
    std::cout << "    BENCH {\"item_mib\":"       << mib
              << ",\"seq_ms\":"                  << seqSec * 1e3
              << ",\"seq_mib_s\":"               << (seqSec > 0.0 ? mib / seqSec : 0.0)
              << ",\"ring_seek_us\":"            << ringSec * 1e6
              << ",\"restart_seek_ms\":"         << restartSec * 1e3
              << ",\"restarts\":"                << stream->restarts()
              << ",\"wasted_bytes\":"            << stream->wastedBytes() << "}" << std::endl;
  }

  reader.Close();
  data.Reset();
  RemoveDir(dir);
  // The stdout marker is part of the verdict (CONTRACTS D1): a run that fails
  // an assertion never reaches the marker, and ctest requires BOTH exit 0 and
  // the marker, so an early exit can never pass.
  if (prism::test::FailureCount() != 0) {
    std::cout << "    " << prism::test::FailureCount() << " DND-SEEK CHECKS FAILED"
              << std::endl;
    return 1;
  }
  std::cout << "    ALL DND-SEEK CHECKS PASSED" << std::endl;
  return 0;
}
