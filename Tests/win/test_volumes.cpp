// PrismArchive -- Windows integration test for multi-volume archives (card C10).
//
// REAL 7z.dll, REAL 7z.exe, REAL three-volume archives authored at runtime.
// Nothing here is committed as a fixture (the task's rule): the test builds the
// volume set, then proves it by sha256 per file.
//
// The negative controls are the point:
//   * a missing MIDDLE volume must be refused before the engine sees a stream
//     (a manager that "reads what is there" produces a shifted, corrupt read);
//   * swapped middle contents keep the names valid, so the ENGINE's CRC is the
//     authority -- the test requires that it does not pass as success.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "PrismEngineBridge.hpp"
#include "SevenZipAbi.hpp"
#include "VolumePlan.hpp"
#include "VolumeStream.hpp"
#include "../mini_test.hpp"

namespace {

namespace fs = std::filesystem;
using prism::VolumeFamily;
using prism::VolumePlan;
using prism::win::VolumeOpenCallback;
using prism::win::VolumeSplitWriter;
using prism::win::VolumeStream;

std::wstring g_dllPath;
std::wstring g_exePath;

std::wstring Locate7zDll() {
  wchar_t value[MAX_PATH * 2]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", value, ARRAYSIZE(value)) > 0) {
    if (fs::exists(value)) return value;
  }
  for (const wchar_t* candidate : {L"C:\\Program Files\\7-Zip\\7z.dll",
                                   L"C:\\Program Files (x86)\\7-Zip\\7z.dll"}) {
    if (fs::exists(candidate)) return candidate;
  }
  return {};
}

std::wstring MakeTempDir() {
  wchar_t tmp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring dir = std::wstring(tmp) + L"prism_vol_" +
                           std::to_wstring(GetCurrentProcessId());
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

bool RunSevenZip(const std::wstring& args) {
  std::wstring cmd = L"\"" + g_exePath + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120'000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

std::vector<std::uint8_t> ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

bool WriteAll(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

std::vector<std::uint8_t> Payload(std::size_t size, std::uint32_t seed) {
  std::vector<std::uint8_t> bytes(size);
  std::uint32_t state = seed * 2654435761u + 1u;
  for (std::size_t i = 0; i < size; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    bytes[i] = static_cast<std::uint8_t>(state >> 24);
  }
  return bytes;
}

// SHA-256 through CNG. Checking against the same bytes the archive was built
// from makes byte equality (not existence) the verdict (CONTRACTS D2).
std::string Sha256Hex(const std::uint8_t* data, std::size_t size) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    return hex;
  }
  DWORD objectSize = 0;
  DWORD digestSize = 0;
  DWORD written = 0;
  BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                    sizeof(objectSize), &written, 0);
  BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestSize),
                    sizeof(digestSize), &written, 0);
  std::vector<std::uint8_t> object(objectSize);
  std::vector<std::uint8_t> digest(digestSize);
  if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0) {
    BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0);
    BCryptFinishHash(hash, digest.data(), digestSize, 0);
    static const char* digits = "0123456789abcdef";
    for (const std::uint8_t byte : digest) {
      hex.push_back(digits[byte >> 4]);
      hex.push_back(digits[byte & 0x0F]);
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return hex;
}

std::string BasenameUtf8(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<std::string> ListSiblings(const std::wstring& dir) {
  std::vector<std::string> names;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    names.push_back(prism::win::WideToUtf8Volume(entry.path().filename().wstring()));
  }
  return names;
}

// Minimal extract sink: one item, owned by the engine with our own reference.
class BufferOut final : public prism::win::abi::ISequentialOutStream {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == prism::win::abi::IID_ISequentialOutStream) {
      *out = static_cast<prism::win::abi::ISequentialOutStream*>(this);
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
  STDMETHODIMP Write(const void* data, prism::win::abi::UInt32 size,
                     prism::win::abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (!data || size == 0) return S_OK;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    bytes_.insert(bytes_.end(), bytes, bytes + size);
    if (processed) *processed = size;
    return S_OK;
  }
  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
  LONG refs_ = 1;
};

class OneItemCallback final : public prism::win::abi::IArchiveExtractCallback {
 public:
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
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP SetTotal(prism::win::abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const prism::win::abi::UInt64*) override { return S_OK; }

  STDMETHODIMP GetStream(prism::win::abi::UInt32 index,
                         prism::win::abi::ISequentialOutStream** outStream,
                         prism::win::abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != prism::win::abi::kExtract) return S_OK;
    if (index != wantedIndex_) return E_INVALIDARG;
    auto* sink = new BufferOut;
    sink->AddRef();  // our reference, so SetOperationResult can read the bytes
    sink_ = sink;
    *outStream = sink;  // the engine's reference
    return S_OK;
  }
  STDMETHODIMP PrepareOperation(prism::win::abi::Int32) override { return S_OK; }
  STDMETHODIMP SetOperationResult(prism::win::abi::Int32 operationResult) override {
    result_ = operationResult;
    if (sink_) {
      if (operationResult == 0) bytes_ = sink_->bytes();
      sink_->Release();
      sink_ = nullptr;
    }
    return S_OK;
  }

  void setWantedIndex(std::uint32_t index) { wantedIndex_ = index; }
  prism::win::abi::Int32 result() const { return result_; }
  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

 private:
  std::uint32_t wantedIndex_ = 0;
  BufferOut* sink_ = nullptr;
  prism::win::abi::Int32 result_ = -1;
  std::vector<std::uint8_t> bytes_;
  LONG refs_ = 1;
};

struct OpenedArchive {
  prism::win::abi::IInArchive* handler = nullptr;
  HRESULT openHr = E_FAIL;
  OpenedArchive() = default;
  OpenedArchive(const OpenedArchive&) = delete;
  OpenedArchive& operator=(const OpenedArchive&) = delete;
  OpenedArchive(OpenedArchive&& other) noexcept : handler(other.handler), openHr(other.openHr) {
    other.handler = nullptr;  // ownership moves; the temporary must not Close/Release
  }
  ~OpenedArchive() {
    if (handler) {
      handler->Close();
      handler->Release();
    }
  }
};

OpenedArchive OpenWithSevenZip(prism::win::SevenZipLibrary& library,
                               prism::win::abi::IInStream* stream) {
  OpenedArchive opened;
  if (FAILED(library.CreateObject(prism::win::abi::CLSID_CFormat7z,
                                  prism::win::abi::IID_IInArchive,
                                  reinterpret_cast<void**>(&opened.handler))) ||
      !opened.handler) {
    return opened;
  }
  prism::win::abi::UInt64 pos = 0;
  stream->Seek(0, 0, &pos);
  opened.openHr = opened.handler->Open(stream, nullptr, nullptr);
  return opened;
}

std::string ItemPath(prism::win::abi::IInArchive* handler, std::uint32_t index) {
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string out;
  if (handler->GetProperty(index, prism::win::abi::kpidPath, &value) == S_OK &&
      value.vt == VT_BSTR && value.bstrVal) {
    out = prism::win::WideToUtf8Volume(value.bstrVal);
  }
  PropVariantClear(&value);
  return out;
}

struct ExtractOutcome {
  HRESULT hr = E_FAIL;
  std::int32_t result = -1;
  std::vector<std::uint8_t> bytes;
};

ExtractOutcome ExtractItem(prism::win::abi::IInArchive* handler, std::uint32_t index) {
  ExtractOutcome outcome;
  auto* callback = new OneItemCallback;
  callback->setWantedIndex(index);
  const prism::win::abi::UInt32 indices[1] = {index};
  outcome.hr = handler->Extract(indices, 1, 0, callback);
  outcome.result = callback->result();
  outcome.bytes = callback->bytes();
  callback->Release();
  return outcome;
}

}  // namespace

PRISM_TEST("7z.exe builds a real three-volume archive and VolumePlan orders it") {
  const std::wstring dir = MakeTempDir();
  const fs::path root(dir);
  std::map<std::string, std::string> expected;  // file name -> sha256
  for (int i = 1; i <= 3; ++i) {
    const std::string name = "part" + std::to_string(i) + ".bin";
    const std::vector<std::uint8_t> bytes = Payload(40'000, static_cast<std::uint32_t>(i));
    PRISM_CHECK(WriteAll(root / name, bytes));
    expected[name] = Sha256Hex(bytes.data(), bytes.size());
  }
  const std::wstring args = L"a -t7z -m0=LZMA2 -v40k \"" + dir + L"\\volset.7z\" \"" + dir +
                            L"\\part1.bin\" \"" + dir + L"\\part2.bin\" \"" + dir +
                            L"\\part3.bin\"";
  if (!PRISM_CHECK(RunSevenZip(args))) {
    std::cout << "    FAIL: 7z.exe could not create the volume fixture" << std::endl;
    return;
  }

  const VolumePlan plan =
      prism::MakeVolumePlan("volset.7z.001", ListSiblings(dir));
  if (!PRISM_CHECK(plan.ok())) {
    std::cout << "    plan error: " << plan.error << std::endl;
    return;
  }
  PRISM_CHECK(plan.family == VolumeFamily::NumericSplit);
  PRISM_CHECK_EQ(plan.stem, std::string("volset.7z"));
  PRISM_CHECK(plan.volumes.size() >= 3);
  // 7-Zip names volumes .001, .002, ... -- never .01 and never out of order.
  PRISM_CHECK_EQ(BasenameUtf8(plan.volumes.front().name), std::string("volset.7z.001"));
  std::cout << "    volumes: " << plan.volumes.size() << " ("
            << plan.volumes.front().name << " .. " << plan.volumes.back().name << ")"
            << std::endl;
  for (const auto& entry : plan.volumes) {
    PRISM_CHECK(fs::file_size(root / entry.name) > 0);
  }
  fs::remove_all(dir);
}

PRISM_TEST("VolumeStream is the byte-exact concatenation of the volumes") {
  const std::wstring dir = MakeTempDir();
  const fs::path root(dir);
  for (int i = 1; i <= 3; ++i) {
    PRISM_CHECK(WriteAll(root / ("p" + std::to_string(i) + ".bin"),
                         Payload(30'000, static_cast<std::uint32_t>(i))));
  }
  const std::wstring args = L"a -t7z -m0=Copy -v30k \"" + dir + L"\\cat.7z\" \"" + dir +
                            L"\\p1.bin\" \"" + dir + L"\\p2.bin\" \"" + dir + L"\\p3.bin\"";
  if (!PRISM_CHECK(RunSevenZip(args))) {
    fs::remove_all(dir);
    return;
  }
  const VolumePlan plan = prism::MakeVolumePlan("cat.7z.001", ListSiblings(dir));
  if (!PRISM_CHECK(plan.ok())) {
    std::cout << "    plan error: " << plan.error << std::endl;
    fs::remove_all(dir);
    return;
  }

  // Calibration (CONTRACTS D4): the expectation is built by reading the volume
  // files directly, so a broken VolumeStream cannot define its own truth.
  std::vector<std::uint8_t> direct;
  for (const auto& entry : plan.volumes) {
    const std::vector<std::uint8_t> bytes = ReadAll(root / entry.name);
    direct.insert(direct.end(), bytes.begin(), bytes.end());
  }

  VolumeStream stream;
  std::string error;
  if (!PRISM_CHECK(stream.Open(dir, plan, &error))) {
    std::cout << "    open error: " << error << std::endl;
    fs::remove_all(dir);
    return;
  }
  PRISM_CHECK_EQ(stream.size(), direct.size());

  std::vector<std::uint8_t> viaStream(static_cast<size_t>(stream.size()));
  prism::win::abi::UInt32 read = 0;
  PRISM_CHECK(stream.Read(viaStream.data(), static_cast<prism::win::abi::UInt32>(viaStream.size()),
                          &read) == S_OK);
  PRISM_CHECK_EQ(size_t(read), viaStream.size());
  PRISM_CHECK_EQ(Sha256Hex(viaStream.data(), viaStream.size()),
                 Sha256Hex(direct.data(), direct.size()));

  // Seek across a volume boundary: the last byte of volume 1 must be readable
  // when the cursor starts one byte before the boundary.
  prism::win::abi::UInt64 pos = 0;
  const std::uint64_t firstSize = fs::file_size(root / plan.volumes.front().name);
  PRISM_CHECK(stream.Seek(static_cast<prism::win::abi::Int64>(firstSize) - 1, 0, &pos) == S_OK);
  std::uint8_t two[2] = {0, 0};
  read = 0;
  PRISM_CHECK(stream.Read(two, 2, &read) == S_OK);
  PRISM_CHECK_EQ(read, prism::win::abi::UInt32(2));
  PRISM_CHECK_EQ(two[0], direct[static_cast<size_t>(firstSize) - 1]);
  PRISM_CHECK_EQ(two[1], direct[static_cast<size_t>(firstSize)]);
  stream.Close();
  fs::remove_all(dir);
}

PRISM_TEST("real 7z.dll opens the volume set and every file matches its sha256") {
  const std::wstring dir = MakeTempDir();
  const fs::path root(dir);
  std::map<std::string, std::string> expected;
  for (int i = 1; i <= 3; ++i) {
    const std::string name = "f" + std::to_string(i) + ".bin";
    const auto bytes = Payload(40'000, static_cast<std::uint32_t>(100 + i));
    PRISM_CHECK(WriteAll(root / name, bytes));
    expected[name] = Sha256Hex(bytes.data(), bytes.size());
  }
  const std::wstring args = L"a -t7z -m0=LZMA2 -v40k \"" + dir + L"\\real.7z\" \"" + dir +
                            L"\\f1.bin\" \"" + dir + L"\\f2.bin\" \"" + dir + L"\\f3.bin\"";
  if (!PRISM_CHECK(RunSevenZip(args))) {
    fs::remove_all(dir);
    return;
  }
  const VolumePlan plan = prism::MakeVolumePlan("real.7z.001", ListSiblings(dir));
  if (!PRISM_CHECK(plan.ok())) {
    std::cout << "    plan error: " << plan.error << std::endl;
    fs::remove_all(dir);
    return;
  }

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_dllPath))) {
    fs::remove_all(dir);
    return;
  }
  VolumeStream stream;
  std::string error;
  if (!PRISM_CHECK(stream.Open(dir, plan, &error))) {
    std::cout << "    open error: " << error << std::endl;
    fs::remove_all(dir);
    return;
  }
  OpenedArchive opened = OpenWithSevenZip(library, &stream);
  if (!PRISM_CHECK_EQ(opened.openHr, S_OK)) {
    std::cout << "    7z handler rejected the concatenated volume stream: hr=0x"
              << std::hex << opened.openHr << std::dec << std::endl;
    fs::remove_all(dir);
    return;
  }
  prism::win::abi::UInt32 count = 0;
  PRISM_CHECK(opened.handler->GetNumberOfItems(&count) == S_OK);
  if (!PRISM_CHECK_EQ(count, prism::win::abi::UInt32(3))) {
    fs::remove_all(dir);
    return;
  }
  for (prism::win::abi::UInt32 index = 0; index < count; ++index) {
    const std::string raw = ItemPath(opened.handler, index);
    const std::string name = BasenameUtf8(raw);
    const auto found = expected.find(name);
    if (!PRISM_CHECK(found != expected.end())) {
      std::cout << "    unexpected item: " << raw << std::endl;
      continue;
    }
    const ExtractOutcome outcome = ExtractItem(opened.handler, index);
    PRISM_CHECK_EQ(outcome.hr, S_OK);
    PRISM_CHECK_EQ(outcome.result, std::int32_t(0));
    if (!PRISM_CHECK_EQ(Sha256Hex(outcome.bytes.data(), outcome.bytes.size()), found->second)) {
      std::cout << "    sha256 mismatch for " << name << std::endl;
    }
  }
  stream.Close();
  fs::remove_all(dir);
}

PRISM_TEST("a missing MIDDLE volume is refused before the engine reads anything") {
  const std::wstring dir = MakeTempDir();
  const fs::path root(dir);
  for (int i = 1; i <= 3; ++i) {
    PRISM_CHECK(WriteAll(root / ("m" + std::to_string(i) + ".bin"),
                         Payload(40'000, static_cast<std::uint32_t>(200 + i))));
  }
  const std::wstring args = L"a -t7z -m0=Copy -v40k \"" + dir + L"\\gap.7z\" \"" + dir +
                            L"\\m1.bin\" \"" + dir + L"\\m2.bin\" \"" + dir + L"\\m3.bin\"";
  if (!PRISM_CHECK(RunSevenZip(args))) {
    fs::remove_all(dir);
    return;
  }
  const VolumePlan full = prism::MakeVolumePlan("gap.7z.001", ListSiblings(dir));
  if (!PRISM_CHECK(full.ok()) || full.volumes.size() < 3) {
    fs::remove_all(dir);
    return;
  }

  // The second volume disappears (simulating an interrupted download).
  PRISM_CHECK(fs::remove(root / full.volumes[1].name));
  const VolumePlan holey = prism::MakeVolumePlan("gap.7z.001", ListSiblings(dir));
  PRISM_CHECK(!holey.ok());
  PRISM_CHECK(!holey.missing.empty());
  std::cout << "    planner says: " << holey.error << std::endl;

  VolumeStream refused;
  std::string error;
  PRISM_CHECK(!refused.Open(dir, holey, &error));
  PRISM_CHECK(error.find("gap.7z.002") != std::string::npos);
  std::cout << "    stream refuses with: " << error << std::endl;

  // And the disaster this guard prevents: hand the engine the volumes that ARE
  // there and see whether it can still "succeed". It must not.
  VolumePlan holeyButForced = full;
  holeyButForced.volumes.erase(holeyButForced.volumes.begin() + 1);
  holeyButForced.error.clear();
  VolumeStream naive;
  if (naive.Open(dir, holeyButForced, &error)) {
    prism::win::SevenZipLibrary library;
    PRISM_CHECK(library.Load(g_dllPath));
    OpenedArchive opened = OpenWithSevenZip(library, &naive);
    bool allMatched = false;
    if (opened.openHr == S_OK) {
      prism::win::abi::UInt32 count = 0;
      opened.handler->GetNumberOfItems(&count);
      std::map<std::string, std::string> expected;
      for (int i = 1; i <= 3; ++i) {
        const std::string name = "m" + std::to_string(i) + ".bin";
        const auto bytes = ReadAll(root / name);
        expected[name] = Sha256Hex(bytes.data(), bytes.size());
      }
      allMatched = true;
      for (prism::win::abi::UInt32 index = 0; index < count; ++index) {
        const std::string name = BasenameUtf8(ItemPath(opened.handler, index));
        const ExtractOutcome outcome = ExtractItem(opened.handler, index);
        const auto found = expected.find(name);
        if (outcome.hr != S_OK || outcome.result != 0 || found == expected.end() ||
            Sha256Hex(outcome.bytes.data(), outcome.bytes.size()) != found->second) {
          allMatched = false;
        }
      }
    }
    std::cout << "    engine over the naive concatenation: openHr=0x" << std::hex
              << opened.openHr << std::dec << ", all files matched=" << allMatched
              << std::endl;
    // A manager without the planner guard would need this to be true. It is not.
    PRISM_CHECK(!allMatched);
  } else {
    std::cout << "    (naive concatenation also refused: " << error << ")" << std::endl;
  }
  fs::remove_all(dir);
}

PRISM_TEST("swapped middle volume contents do not pass as a successful extract") {
  const std::wstring dir = MakeTempDir();
  const fs::path root(dir);
  std::map<std::string, std::string> expected;
  for (int i = 1; i <= 3; ++i) {
    const std::string name = "s" + std::to_string(i) + ".bin";
    const auto bytes = Payload(40'000, static_cast<std::uint32_t>(300 + i));
    PRISM_CHECK(WriteAll(root / name, bytes));
    expected[name] = Sha256Hex(bytes.data(), bytes.size());
  }
  const std::wstring args = L"a -t7z -m0=Copy -v40k \"" + dir + L"\\swap.7z\" \"" + dir +
                            L"\\s1.bin\" \"" + dir + L"\\s2.bin\" \"" + dir + L"\\s3.bin\"";
  if (!PRISM_CHECK(RunSevenZip(args))) {
    fs::remove_all(dir);
    return;
  }
  const VolumePlan plan = prism::MakeVolumePlan("swap.7z.001", ListSiblings(dir));
  if (!PRISM_CHECK(plan.ok()) || plan.volumes.size() < 3) {
    fs::remove_all(dir);
    return;
  }
  // Names stay valid; only the bytes are in the wrong order (a "sorted by
  // modified time" bug or a hand-assembled set looks exactly like this).
  const auto second = ReadAll(root / plan.volumes[1].name);
  const auto third = ReadAll(root / plan.volumes[2].name);
  PRISM_CHECK(WriteAll(root / plan.volumes[1].name, third));
  PRISM_CHECK(WriteAll(root / plan.volumes[2].name, second));

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  VolumeStream stream;
  std::string error;
  if (!PRISM_CHECK(stream.Open(dir, plan, &error))) {
    std::cout << "    open error: " << error << std::endl;
    fs::remove_all(dir);
    return;
  }
  OpenedArchive opened = OpenWithSevenZip(library, &stream);
  bool allMatched = false;
  if (opened.openHr == S_OK) {
    prism::win::abi::UInt32 count = 0;
    opened.handler->GetNumberOfItems(&count);
    allMatched = count == 3;
    for (prism::win::abi::UInt32 index = 0; index < count; ++index) {
      const std::string name = BasenameUtf8(ItemPath(opened.handler, index));
      const ExtractOutcome outcome = ExtractItem(opened.handler, index);
      const auto found = expected.find(name);
      if (outcome.hr != S_OK || outcome.result != 0 || found == expected.end() ||
          Sha256Hex(outcome.bytes.data(), outcome.bytes.size()) != found->second) {
        allMatched = false;
      }
    }
  }
  std::cout << "    engine over swapped volumes: openHr=0x" << std::hex << opened.openHr
            << std::dec << ", all files matched=" << allMatched << std::endl;
  PRISM_CHECK(!allMatched);
  stream.Close();
  fs::remove_all(dir);
}

PRISM_TEST("IArchiveOpenVolumeCallback answers its IID and serves named volumes") {
  const std::wstring dir = MakeTempDir();
  const fs::path root(dir);
  for (int i = 1; i <= 2; ++i) {
    PRISM_CHECK(WriteAll(root / ("v" + std::to_string(i) + ".bin"), Payload(20'000, 400 + i)));
  }
  const std::wstring args = L"a -t7z -m0=Copy -v20k \"" + dir + L"\\cb.7z\" \"" + dir +
                            L"\\v1.bin\" \"" + dir + L"\\v2.bin\"";
  if (!PRISM_CHECK(RunSevenZip(args))) {
    fs::remove_all(dir);
    return;
  }
  const VolumePlan plan = prism::MakeVolumePlan("cb.7z.001", ListSiblings(dir));
  if (!PRISM_CHECK(plan.ok())) {
    fs::remove_all(dir);
    return;
  }

  auto* callback = new VolumeOpenCallback(dir, plan, "cb.7z.001");
  void* query = nullptr;
  // CONTRACTS D10: QI must answer the volume IID -- a complete vtable that the
  // engine cannot reach is the 18-CI-round defect.
  PRISM_CHECK(callback->QueryInterface(prism::win::IID_IArchiveOpenVolumeCallback, &query) == S_OK);
  PRISM_CHECK(query == static_cast<prism::win::IArchiveOpenVolumeCallback*>(callback));
  if (query) static_cast<IUnknown*>(query)->Release();

  PROPVARIANT value;
  PropVariantInit(&value);
  PRISM_CHECK(callback->GetProperty(4 /* kpidName */, &value) == S_OK);
  PRISM_CHECK(value.vt == VT_BSTR);
  if (value.vt == VT_BSTR) {
    PRISM_CHECK_EQ(prism::win::WideToUtf8Volume(value.bstrVal), std::string("cb.7z.001"));
  }
  PropVariantClear(&value);

  prism::win::abi::IInStream* stream = nullptr;
  const std::wstring firstName = prism::win::Utf8ToWideVolume(plan.volumes.front().name);
  PRISM_CHECK(callback->GetStream(firstName.c_str(), &stream) == S_OK);
  if (stream) {
    auto* sized = static_cast<prism::win::abi::IStreamGetSize*>(nullptr);
    PRISM_CHECK(stream->QueryInterface(prism::win::abi::IID_IStreamGetSize,
                                       reinterpret_cast<void**>(&sized)) == S_OK);
    if (sized) {
      prism::win::abi::UInt64 size = 0;
      PRISM_CHECK(sized->GetSize(&size) == S_OK);
      PRISM_CHECK_EQ(size, static_cast<prism::win::abi::UInt64>(
                               fs::file_size(root / plan.volumes.front().name)));
      sized->Release();
    }
    stream->Release();
  }
  prism::win::abi::IInStream* missing = nullptr;
  PRISM_CHECK(callback->GetStream(L"cb.7z.999", &missing) == S_FALSE);
  PRISM_CHECK(missing == nullptr);
  std::cout << "    volume callback: " << callback->streamRequests() << " request(s), "
            << callback->streamsServed() << " served" << std::endl;
  callback->Release();
  fs::remove_all(dir);
}

PRISM_TEST("VolumeSplitWriter splits exactly like 7-Zip's CMultiOutStream") {
  const std::wstring dir = MakeTempDir();
  const fs::path root(dir);
  const auto payload = Payload(100'000, 77);

  VolumeSplitWriter writer;
  std::string error;
  const std::wstring prefix = dir + L"\\out.7z";
  if (!PRISM_CHECK(writer.Open(prefix, 40'000, &error))) {
    std::cout << "    writer error: " << error << std::endl;
    fs::remove_all(dir);
    return;
  }
  prism::win::abi::UInt32 processed = 0;
  PRISM_CHECK(writer.Write(payload.data(), static_cast<prism::win::abi::UInt32>(payload.size()),
                           &processed) == S_OK);
  PRISM_CHECK_EQ(size_t(processed), payload.size());
  PRISM_CHECK_EQ(writer.volumeCount(), std::uint32_t(3));
  if (!PRISM_CHECK_EQ(writer.paths().size(), size_t(3))) {
    fs::remove_all(dir);
    return;
  }
  // The naming rule is 7-Zip's: minimum three digits, ordinal starting at 1.
  PRISM_CHECK(writer.paths()[0].substr(writer.paths()[0].size() - 4) == L".001");
  PRISM_CHECK(writer.paths()[2].substr(writer.paths()[2].size() - 4) == L".003");
  const std::uint64_t sizes[3] = {fs::file_size(writer.paths()[0]),
                                  fs::file_size(writer.paths()[1]),
                                  fs::file_size(writer.paths()[2])};
  PRISM_CHECK_EQ(sizes[0], std::uint64_t(40'000));
  PRISM_CHECK_EQ(sizes[1], std::uint64_t(40'000));
  PRISM_CHECK_EQ(sizes[2], std::uint64_t(20'000));
  PRISM_CHECK_EQ(writer.totalBytes(), std::uint64_t(payload.size()));

  std::vector<std::uint8_t> joined;
  for (const std::wstring& path : writer.paths()) {
    const auto bytes = ReadAll(path);
    joined.insert(joined.end(), bytes.begin(), bytes.end());
  }
  PRISM_CHECK_EQ(Sha256Hex(joined.data(), joined.size()),
                 Sha256Hex(payload.data(), payload.size()));

  // In-place patch inside the first volume (the 7z writer rewrites its start
  // header after the payload): same total length, changed leading bytes.
  const std::uint8_t patch[8] = {9, 8, 7, 6, 5, 4, 3, 2};
  prism::win::abi::UInt64 pos = 0;
  PRISM_CHECK(writer.Seek(0, 0, &pos) == S_OK);
  processed = 0;
  PRISM_CHECK(writer.Write(patch, 8, &processed) == S_OK);
  PRISM_CHECK_EQ(processed, prism::win::abi::UInt32(8));
  PRISM_CHECK_EQ(writer.totalBytes(), std::uint64_t(payload.size()));
  const auto patched = ReadAll(writer.paths()[0]);
  PRISM_CHECK_EQ(patched[0], std::uint8_t(9));
  PRISM_CHECK_EQ(patched[7], std::uint8_t(2));
  PRISM_CHECK_EQ(patched[8], payload[8]);
  writer.Finish();
  fs::remove_all(dir);
}

int main(int argc, char** argv) {
  g_dllPath = Locate7zDll();
  if (g_dllPath.empty()) {
    std::cout << "FAIL: 7z.dll not found -- set PRISM_7Z_DLL (this is a hard "
                 "failure, not a silent pass)" << std::endl;
    return 1;
  }
  g_exePath = (fs::path(g_dllPath).parent_path() / L"7z.exe").wstring();
  if (!fs::exists(g_exePath)) {
    std::cout << "FAIL: 7z.exe not found beside 7z.dll" << std::endl;
    return 1;
  }
  std::wcout << L"7z.dll: " << g_dllPath << std::endl;

  const int rc = ::prism::test::RunAll(argc, argv);
  // The marker is part of the verdict (CONTRACTS D1): CTest also requires it.
  if (rc == 0) {
    std::cout << "ALL VOLUME CHECKS PASSED" << std::endl;
    std::cout.flush();
  }
  return rc;
}
