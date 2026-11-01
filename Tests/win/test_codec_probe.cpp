// Windows integration test: does a 7-Zip ZS 7z.dll drop into OUR bridge?
//
// Card C17 spike (blueprint ch.2: "merge 7-Zip ZS's Zstd/LZ4/Brotli"). The
// question is not "does ZS exist" (it does) but "does the DLL we already drive
// answer the same ABI, and what does it actually add". Three layers, increasing
// cost:
//
//   1. PURE classification (no DLL): feed both DLLs' codec tables as strings and
//      check the verdict. The tables below are not hand-written: they were
//      dumped from the two real DLLs through GetHandlerProperty2 /
//      GetMethodProperty (see docs/ENGINE-ZS.md for the dump program).
//      The negative control (CONTRACTS D3) lives here: the stock table must
//      classify as "zstd handler only, no zstd method, no lz4/brotli".
//   2. LIVE probe of both real DLLs through CodecProbe.hpp (format + method
//      tables, method ids, file VERSIONINFO).
//   3. DIFFERENTIAL extraction: a zstd-compressed .7z opens and extracts through
//      the UNCHANGED ArchiveReader/7z handler with the ZS DLL, while the same
//      file on the stock DLL parses but yields no payload (no ZSTD method) --
//      so the positive case cannot be a false pass (CONTRACTS D18). A bare .zst
//      decodes on BOTH builds through handler CLSID 0x0E, which is what makes
//      "same ABI, different codec set" concrete instead of a claim.
//
// The ZS DLL is opt-in (PRISM_7Z_ZS_DLL). Absent, the suite prints the exact
// reason and exits 77 -> CTest "Skipped", never a silent green (CONTRACTS D1).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "CodecProbe.hpp"
#include "PrismEngineBridge.hpp"

#ifndef PRISM_ZS_FIXTURE_DIR
#define PRISM_ZS_FIXTURE_DIR "Tests/fixtures/zs"
#endif

namespace {

using prism::win::abi::IArchiveExtractCallback;
using prism::win::abi::ISequentialOutStream;
using prism::win::abi::UInt32;

// ---------------------------------------------------------------------------
// Fixtures / DLL discovery
// ---------------------------------------------------------------------------
std::wstring FixtureDir() {
  wchar_t envValue[MAX_PATH * 2]{};
  if (GetEnvironmentVariableW(L"PRISM_ZS_FIXTURES", envValue, ARRAYSIZE(envValue)) > 0) {
    return envValue;
  }
  return prism::win::Utf8ToWide(PRISM_ZS_FIXTURE_DIR);
}

std::wstring Fixture(const wchar_t* name) { return FixtureDir() + L"\\" + name; }

// The expected payload is BUILT here, not read from a committed text fixture: a
// .txt in the repo would be CRLF-normalised by core.autocrlf on checkout while
// the archives keep the LF bytes they were built from, and the equality
// assertions would then fail on a clean clone for a reason that has nothing to do
// with codecs. Same 40 lines, byte for byte, as the fixtures were packed from.
std::string ExpectedPayload() {
  std::string out;
  for (int i = 1; i <= 40; ++i) {
    if (i > 1) out += "\n";
    out += "PrismArchive codec probe line " + std::to_string(i) + " / zstd-lz4-brotli spike";
  }
  return out;
}

std::wstring FindZsDll() {
  wchar_t envValue[MAX_PATH * 2]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_ZS_DLL", envValue, ARRAYSIZE(envValue)) > 0 &&
      std::filesystem::exists(envValue)) {
    return envValue;
  }
  for (const wchar_t* candidate : {L"E:\\DevTools\\7zzs\\7z.dll", L"7z.dll"}) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return {};
}

std::wstring FindStockDll() {
  wchar_t envValue[MAX_PATH * 2]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", envValue, ARRAYSIZE(envValue)) > 0 &&
      std::filesystem::exists(envValue)) {
    return envValue;
  }
  for (const wchar_t* candidate : {L"C:\\Program Files\\7-Zip\\7z.dll",
                                   L"C:\\Program Files (x86)\\7-Zip\\7z.dll"}) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return {};
}

// ---------------------------------------------------------------------------
// 1. The two real codec tables, dumped from the DLLs (not transcribed by hand)
// ---------------------------------------------------------------------------
// 7-Zip 25.01 (x64, 2025-08-03), 7z.dll VERSIONINFO 25.1.0.0. 60 handlers, 25
// methods. "zstd" is a HANDLER (unpack only) -- no ZSTD method.
std::vector<std::string> StockFormats() {
  return {"APFS",  "APM",   "Ar",    "Arj",    "Base64", "bzip2", "Compound", "Cpio",
          "CramFS", "Dmg",  "ELF",   "Ext",    "FAT",    "FLV",   "gzip",     "GPT",
          "HFS",   "IHex", "LP",     "Lzh",    "lzma",   "lzma86", "MachO",   "MBR",
          "MsLZ",  "Mub",  "NTFS",  "PE",     "COFF",   "TE",    "Ppmd",     "QCOW",
          "Rpm",   "Sparse", "Split", "SquashFS", "SWFc", "SWF",   "UEFIc",    "UEFIf",
          "VDI",   "VHD",  "VHDX",  "VMDK",   "Xar",    "xz",    "Z",        "zstd",
          "7z",    "Cab",  "Chm",   "Hxs",    "Iso",    "Nsis",  "Rar",      "Rar5",
          "tar",   "Udf",  "wim",   "zip"};
}

std::vector<std::string> StockMethods() {
  return {"BCJ2", "BCJ",     "PPC",    "IA64",  "ARM",  "ARMT", "SPARC",  "ARM64",
          "RISCV", "Swap2",  "Swap4",  "BZip2", "Copy", "Deflate64", "Deflate",
          "Delta", "LZMA2",  "LZMA",   "PPMD",  "Rar1", "Rar2", "Rar3",   "Rar5",
          "7zAES", "AES256CBC"};
}

// 7-Zip ZS 26.02 v1.5.7 R1 (x64, 2026-06-27), 7z.dll VERSIONINFO 26.2.0.0.
// Everything stock has, plus 5 handlers and 6 methods.
std::vector<std::string> ZsExtraFormats() { return {"brotli", "lzip", "lz4", "lz5", "lizard"}; }
std::vector<std::string> ZsExtraMethods() {
  return {"BROTLI", "LZ4", "LIZARD", "LZ5", "ZSTD", "FLZMA2"};
}

// ---------------------------------------------------------------------------
// Minimal single-item extract callback for the raw-ABI path.
// ---------------------------------------------------------------------------
class SingleItemSink final : public IArchiveExtractCallback {
 public:
  explicit SingleItemSink(std::size_t capacity) : capacity_(capacity) {}
  ~SingleItemSink() {
    if (sink_) sink_->Release();
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    // IProgress has no IID pinned in SevenZipAbi.hpp (nothing QIs it for
    // extraction), so this answers the two identities it can honestly claim.
    if (iid == IID_IUnknown || iid == prism::win::abi::IID_IArchiveExtractCallback) {
      *out = static_cast<IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP SetTotal(prism::win::abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const prism::win::abi::UInt64*) override { return S_OK; }

  STDMETHODIMP GetStream(UInt32 index, ISequentialOutStream** outStream,
                         prism::win::abi::Int32 askExtractMode) override {
    if (outStream) *outStream = nullptr;
    if (askExtractMode != prism::win::abi::kExtract || index != 0 || !outStream) return S_OK;
    auto* sink = new prism::win::MemoryOutStream(capacity_);
    sink_ = sink;
    // D15: the out parameter transfers one reference. The constructor's ref goes
    // to the engine; the AddRef below is the one this sink keeps to read bytes.
    sink->AddRef();
    *outStream = sink;
    return S_OK;
  }
  STDMETHODIMP PrepareOperation(prism::win::abi::Int32) override { return S_OK; }
  STDMETHODIMP SetOperationResult(prism::win::abi::Int32 result) override {
    operationResult_ = result;
    return S_OK;
  }

  const std::vector<std::uint8_t>* bytes() const { return sink_ ? &sink_->bytes() : nullptr; }
  bool tripped() const { return sink_ && sink_->tripped(); }
  prism::win::abi::Int32 operationResult() const { return operationResult_; }

 private:
  std::size_t capacity_;
  prism::win::MemoryOutStream* sink_ = nullptr;
  prism::win::abi::Int32 operationResult_ = -1;
  LONG refs_ = 1;
};

struct SingleStreamResult {
  bool created = false;
  bool opened = false;
  UInt32 items = 0;
  bool extracted = false;
  std::string payload;
  std::string trace;
};

// Opens one archive through an explicitly chosen handler CLSID and extracts item
// 0 -- exactly what product code would do once ArchiveReader::Open() grows these
// candidates.
SingleStreamResult ExtractSingleStream(prism::win::SevenZipLibrary& library, unsigned char handlerId,
                                       const std::wstring& file) {
  SingleStreamResult result;
  char buf[160];
  prism::win::abi::IInArchive* archive = nullptr;
  const HRESULT hr = library.CreateObject(prism::win::HandlerClsid(handlerId),
                                          prism::win::abi::IID_IInArchive,
                                          reinterpret_cast<void**>(&archive));
  std::snprintf(buf, sizeof(buf), "CreateObject(id=0x%02X) hr=0x%08lX; ", handlerId,
                static_cast<unsigned long>(hr));
  result.trace += buf;
  if (FAILED(hr) || !archive) return result;
  result.created = true;

  prism::win::InFileStream stream(file);
  auto* openCallback = new prism::win::OpenCallback(std::wstring());
  const HRESULT opened = archive->Open(&stream, nullptr, openCallback);
  openCallback->Release();
  std::snprintf(buf, sizeof(buf), "Open hr=0x%08lX; ", static_cast<unsigned long>(opened));
  result.trace += buf;
  if (opened != S_OK) {
    archive->Release();
    return result;
  }
  result.opened = true;
  archive->GetNumberOfItems(&result.items);

  auto* sink = new SingleItemSink(4u << 20);
  const HRESULT extracted = archive->Extract(nullptr, static_cast<UInt32>(-1), 0, sink);
  if (SUCCEEDED(extracted) && !sink->tripped() && sink->bytes() != nullptr) {
    result.payload.assign(sink->bytes()->begin(), sink->bytes()->end());
    result.extracted = !result.payload.empty();
  }
  std::snprintf(buf, sizeof(buf), "Extract hr=0x%08lX opResult=%d bytes=%zu",
                static_cast<unsigned long>(extracted), static_cast<int>(sink->operationResult()),
                result.payload.size());
  result.trace += buf;
  // The sink's own reference (taken in GetStream) is dropped by ~SingleItemSink.
  sink->Release();
  sink = nullptr;
  archive->Close();
  archive->Release();
  return result;
}

bool g_haveZs = false;
std::wstring g_zsDll;

}  // namespace

// ---------------------------------------------------------------------------
// Layer 1: pure classification (no DLL)
// ---------------------------------------------------------------------------
PRISM_TEST("stock 7-Zip table: zstd HANDLER yes, zstd METHOD no, no lz4/brotli") {
  const auto inv = prism::win::ClassifyCodecs(StockFormats(), StockMethods());
  std::cout << "    stock 25.01: " << inv.Describe() << std::endl;
  PRISM_CHECK_EQ(inv.formatCount, static_cast<std::uint32_t>(60));
  PRISM_CHECK_EQ(inv.methodCount, static_cast<std::uint32_t>(25));
  // The measured shape of the stock engine: a .zst decoder handler...
  PRISM_CHECK(inv.zstdHandler);
  // ...and no zstd METHOD, i.e. no 7z/zip zstd payload, no encoder. This pair is
  // the negative control for every positive assertion below (CONTRACTS D3): a
  // classifier that collapsed the two halves would go red right here.
  PRISM_CHECK(!inv.zstdMethod);
  PRISM_CHECK(!inv.lz4);
  PRISM_CHECK(!inv.brotli);
  PRISM_CHECK(!inv.IsZsBuild());
  // ...while the baselines it DOES have must still be visible, so a classifier
  // that degraded to "always false" is caught too (a vacuous negative).
  PRISM_CHECK(inv.lzma2);
  PRISM_CHECK(inv.deflate);
  PRISM_CHECK(inv.HasFormat("7z"));
  PRISM_CHECK(inv.HasFormat("zip"));
}

PRISM_TEST("ZS table adds handler+method for zstd/lz4/brotli, case-insensitively") {
  auto formats = StockFormats();
  auto methods = StockMethods();
  // Bind the extra rows to locals: ZsExtraFormats() called twice yields two
  // DIFFERENT temporaries, so begin()/end() from separate calls would be
  // iterators into different containers (that UB crashed this suite on first run).
  const std::vector<std::string> extraFormats = ZsExtraFormats();
  const std::vector<std::string> extraMethods = ZsExtraMethods();
  formats.insert(formats.end(), extraFormats.begin(), extraFormats.end());
  methods.insert(methods.end(), extraMethods.begin(), extraMethods.end());

  const auto inv = prism::win::ClassifyCodecs(formats, methods);
  std::cout << "    zs 26.02: " << inv.Describe() << std::endl;
  PRISM_CHECK_EQ(inv.formatCount, static_cast<std::uint32_t>(65));
  PRISM_CHECK_EQ(inv.methodCount, static_cast<std::uint32_t>(31));
  PRISM_CHECK(inv.zstdHandler);
  PRISM_CHECK(inv.zstdMethod);
  PRISM_CHECK(inv.lz4Handler);
  PRISM_CHECK(inv.lz4Method);
  PRISM_CHECK(inv.brotliHandler);
  PRISM_CHECK(inv.brotliMethod);
  PRISM_CHECK(inv.HasMethod("zstd"));  // "ZSTD" folded down
  PRISM_CHECK(inv.IsZsBuild());
  // An empty table is "nothing", not "everything".
  const auto empty = prism::win::ClassifyCodecs({}, {});
  PRISM_CHECK(!empty.IsZsBuild());
  PRISM_CHECK_EQ(empty.formatCount, static_cast<std::uint32_t>(0));
}

PRISM_TEST("handler CLSID construction matches the two constants we already drive") {
  // The rule the raw-ABI path depends on:
  // {23170F69-40C1-278A-1000-000110<id>0000}. CLSID_CFormat7z / CLSID_CFormatZip
  // are proven by every other extraction test in this repo, so agreeing with
  // them validates the construction without trusting a table index.
  PRISM_CHECK(IsEqualGUID(prism::win::HandlerClsid(0x07), prism::win::abi::CLSID_CFormat7z) != FALSE);
  PRISM_CHECK(IsEqualGUID(prism::win::HandlerClsid(0x01), prism::win::abi::CLSID_CFormatZip) != FALSE);
  // The trap this documents: the id is NOT the position in the handler table
  // (on 25.01 "7z" sits at index 48 and still carries id 0x07), so an
  // index-derived GUID is a different, wrong GUID -- CreateObject rejects it.
  PRISM_CHECK(IsEqualGUID(prism::win::HandlerClsid(0x00),
                          prism::win::abi::CLSID_CFormat7z) == FALSE);
}

// ---------------------------------------------------------------------------
// Layer 2: live probe of both DLLs
// ---------------------------------------------------------------------------
PRISM_TEST("ZS 7z.dll answers our ABI and reports zstd/lz4/brotli methods") {
  if (!g_haveZs) {
    std::cout << "    SKIP: no ZS 7z.dll (set PRISM_7Z_ZS_DLL)" << std::endl;
    return;
  }
  const auto probe = prism::win::ProbeCodecs(g_zsDll);
  std::cout << "    " << probe.Describe() << std::endl;
  std::cout << "    already-mapped=" << (probe.alreadyLoaded ? "yes" : "no")
            << " formatTable=" << (probe.hasFormatTable ? "yes" : "no")
            << " methodTable=" << (probe.hasMethodTable ? "yes" : "no") << std::endl;
  if (!PRISM_CHECK(probe.loaded)) {
    std::cout << "    error: " << probe.error << std::endl;
    return;
  }
  PRISM_CHECK(probe.hasFormatTable);
  PRISM_CHECK(probe.hasMethodTable);
  PRISM_CHECK_EQ(probe.inventory.formatCount, static_cast<std::uint32_t>(65));
  PRISM_CHECK_EQ(probe.inventory.methodCount, static_cast<std::uint32_t>(31));
  PRISM_CHECK(probe.inventory.zstdMethod);
  PRISM_CHECK(probe.inventory.lz4Method);
  PRISM_CHECK(probe.inventory.brotliMethod);
  PRISM_CHECK(probe.inventory.zstdHandler);
  // Baselines: the tables were really read, not guessed.
  PRISM_CHECK(probe.inventory.lzma2);
  PRISM_CHECK(probe.inventory.deflate);
  PRISM_CHECK(probe.FormatIndex("zstd") >= 0);
  // Numeric ids from NMethodPropID::kID: these are what a .7z header carries.
  std::cout << "    method ids:";
  for (std::size_t i = 0; i < probe.inventory.methods.size() &&
                          i < probe.inventory.methodIds.size();
       ++i) {
    if (probe.inventory.methods[i] == "zstd" || probe.inventory.methods[i] == "lz4" ||
        probe.inventory.methods[i] == "brotli" || probe.inventory.methods[i] == "lzma2") {
      char id[32];
      std::snprintf(id, sizeof(id), " %s=0x%llX", probe.inventory.methods[i].c_str(),
                    static_cast<unsigned long long>(probe.inventory.methodIds[i]));
      std::cout << id;
    }
  }
  std::cout << std::endl;
  PRISM_CHECK(probe.inventory.HasMethod("zstd"));
}

// ---------------------------------------------------------------------------
// Layer 3a: zstd-compressed .7z through the UNCHANGED bridge
// ---------------------------------------------------------------------------
PRISM_TEST("zstd-compressed .7z extracts through the unchanged 7z handler") {
  if (!g_haveZs) {
    std::cout << "    SKIP: no ZS 7z.dll" << std::endl;
    return;
  }
  const std::string expected = ExpectedPayload();

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_zsDll))) return;

  // (i) LZMA2 baseline: the harness itself is healthy.
  {
    prism::win::ArchiveReader reader(library);
    if (!PRISM_CHECK(reader.Open(Fixture(L"lzma2-baseline.7z")))) {
      std::cout << "    open trace: " << reader.openTrace() << std::endl;
      return;
    }
    const auto bytes = reader.ExtractToMemory(0, 4u << 20);
    PRISM_CHECK(bytes.has_value());
    if (bytes) PRISM_CHECK_EQ(bytes->size(), expected.size());
    reader.Close();
  }

  // (ii) The same payload packed with -m0=zstd. No product change: the 7z
  // container handler is the one we already select; the codec lives inside the
  // DLL. This is the ABI-compatibility proof.
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(Fixture(L"zstd-method.7z")))) {
    std::cout << "    open trace: " << reader.openTrace() << std::endl;
    return;
  }
  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(1))) {
    reader.Close();
    return;
  }
  PRISM_CHECK_EQ(catalog[0].path, std::string("note.txt"));
  PRISM_CHECK_EQ(catalog[0].size, static_cast<std::uint64_t>(expected.size()));
  const auto bytes = reader.ExtractToMemory(0, 4u << 20);
  PRISM_CHECK(bytes.has_value());
  if (bytes) {
    PRISM_CHECK_EQ(bytes->size(), expected.size());
    PRISM_CHECK(std::string(bytes->begin(), bytes->end()) == expected);
  }
  reader.Close();
}

// ---------------------------------------------------------------------------
// Layer 3b: bare .zst / .lz4 / .br through the raw ABI with an explicit CLSID
// ---------------------------------------------------------------------------
PRISM_TEST("bare .zst/.lz4/.br decode through their handler CLSIDs") {
  if (!g_haveZs) {
    std::cout << "    SKIP: no ZS 7z.dll" << std::endl;
    return;
  }
  const std::string expected = ExpectedPayload();

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_zsDll))) return;

  for (const prism::win::KnownCodecHandler& handler : prism::win::KnownCodecHandlers()) {
    const std::wstring name = prism::win::Utf8ToWide(std::string("note.txt.") + handler.extension);
    const std::wstring file = FixtureDir() + L"\\" + name;
    if (!std::filesystem::exists(file)) {
      std::cout << "    (no fixture for " << handler.codec << ")" << std::endl;
      continue;
    }
    const auto result = ExtractSingleStream(library, handler.id, file);
    std::cout << "    " << handler.codec << ": " << result.trace << std::endl;
    PRISM_CHECK(result.created);
    if (!result.created) continue;
    PRISM_CHECK(result.opened);
    PRISM_CHECK_EQ(result.items, static_cast<UInt32>(1));
    PRISM_CHECK(result.extracted);
    PRISM_CHECK_EQ(result.payload.size(), expected.size());
    PRISM_CHECK(result.payload == expected);
  }

  // Product routing, re-measured after card C26 added the extension -> handler
  // table: a bare .zst now opens through ArchiveReader, because the reader no
  // longer guesses between Zip and 7z only. C17 originally asserted the
  // OPPOSITE (a documented product gap); that assertion was correct then and
  // is wrong now. It is UPDATED here rather than deleted: "the product can open
  // this" is the property worth keeping locked, and a deleted assertion would
  // let the routing table regress silently.
  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(Fixture(L"note.txt.zst"));
  std::cout << "    ArchiveReader(.zst) opened=" << (opened ? "yes" : "no")
            << " trace: " << reader.openTrace() << std::endl;
  PRISM_CHECK(opened);
  reader.Close();
}

// ---------------------------------------------------------------------------
// Layer 3c: the same fixtures on the STOCK DLL (D18/D3)
// ---------------------------------------------------------------------------
PRISM_TEST("stock 7z.dll: same .zst CLSID works, zstd .7z has no payload") {
  const std::wstring stockDll = FindStockDll();
  if (stockDll.empty()) {
    std::cout << "    SKIP: no stock 7z.dll found (PRISM_7Z_DLL / Program Files)" << std::endl;
    return;
  }
  const std::string expected = ExpectedPayload();

  const auto probe = prism::win::ProbeCodecs(stockDll);
  std::cout << "    stock probe: " << probe.Describe() << std::endl;
  PRISM_CHECK(probe.loaded);
  // Reversing the ZS claim precisely: handler yes, method no.
  PRISM_CHECK(probe.inventory.zstdHandler);
  PRISM_CHECK(!probe.inventory.zstdMethod);
  PRISM_CHECK(!probe.inventory.lz4);
  PRISM_CHECK(!probe.inventory.brotli);
  PRISM_CHECK(!probe.inventory.IsZsBuild());

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(stockDll))) return;

  // (i) LZMA2 baseline works, so the engine is healthy on these fixtures...
  {
    prism::win::ArchiveReader reader(library);
    if (PRISM_CHECK(reader.Open(Fixture(L"lzma2-baseline.7z")))) {
      const auto bytes = reader.ExtractToMemory(0, 4u << 20);
      PRISM_CHECK(bytes.has_value());
      if (bytes) PRISM_CHECK(std::string(bytes->begin(), bytes->end()) == expected);
    }
    reader.Close();
  }

  // (ii) ...and the bare .zst decodes here too, on the SAME handler CLSID 0x0E.
  // That is the ABI half of the story: identical COM surface, different codec set.
  {
    const auto bare = ExtractSingleStream(library, 0x0E, Fixture(L"note.txt.zst"));
    std::cout << "    stock bare .zst: " << bare.trace << std::endl;
    PRISM_CHECK(bare.created);
    PRISM_CHECK(bare.extracted);
    PRISM_CHECK(bare.payload == expected);
  }

  // (iii) The zstd-compressed .7z: the container parses (the 7z handler is
  // happy), the payload does not materialise -- no ZSTD method in this DLL.
  const auto packed = ExtractSingleStream(library, 0x07, Fixture(L"zstd-method.7z"));
  std::cout << "    stock zstd .7z (raw 7z handler): " << packed.trace << std::endl;
  PRISM_CHECK(packed.created);
  PRISM_CHECK(packed.payload != expected);
  PRISM_CHECK(!packed.extracted);

  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(Fixture(L"zstd-method.7z"));
  std::cout << "    stock ArchiveReader open=" << (opened ? "accepted" : "rejected")
            << " trace: " << reader.openTrace() << std::endl;
  bool produced = false;
  if (opened) {
    const auto bytes = reader.ExtractToMemory(0, 4u << 20);
    produced = bytes.has_value() && !bytes->empty();
    std::cout << "    stock catalog=" << reader.ReadCatalog().size()
              << " bytes=" << (bytes ? std::to_string(bytes->size()) : std::string("failed"))
              << std::endl;
  }
  PRISM_CHECK(!produced);
  reader.Close();
}

int main(int argc, char** argv) {
  g_zsDll = FindZsDll();
  // The archives are the only fixtures (see ExpectedPayload for the text):
  // if they are missing, the live half of this suite has nothing to prove.
  g_haveZs = !g_zsDll.empty() && std::filesystem::exists(Fixture(L"zstd-method.7z"));
  if (g_haveZs) {
    std::wcout << L"ZS 7z.dll: " << g_zsDll << std::endl;
    std::wcout << L"fixtures:  " << FixtureDir() << std::endl;
  } else {
    std::cout << "WARNING: ZS 7z.dll or fixtures unavailable -- live checks SKIP" << std::endl;
  }

  const int rc = ::prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  if (!g_haveZs) {
    // D1: an explicit, non-zero SKIP carrying its reason. CTest maps 77 to
    // "Skipped" via SKIP_RETURN_CODE, so this can never be mistaken for a pass.
    std::cout << "SKIP: no 7-Zip ZS 7z.dll (set PRISM_7Z_ZS_DLL) or no fixtures "
                 "(Tests/fixtures/zs)" << std::endl;
    return 77;
  }
  std::cout << "ALL CODEC-PROBE CHECKS PASSED" << std::endl;
  std::cout.flush();
  return 0;
}
