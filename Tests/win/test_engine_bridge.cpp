// Windows integration test: loads the REAL 7z.dll and round-trips archives.
//
// This is the guard rail for the hand-written COM vtables in SevenZipAbi.hpp
// and for the PrismCore -> PrismWin path-rewriting contract. It runs on every
// windows-latest CI build; if 7z.dll cannot be found it reports SKIP loudly
// instead of pretending to be green.
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "DpapiVault.hpp"
#include "PrismEngineBridge.hpp"
#include "Win32Transcoder.hpp"

namespace {

// ---------------------------------------------------------------------------
// Minimal ZIP writer (stored, UTF-8 flagged) so the test needs no 7-Zip CLI.
// ---------------------------------------------------------------------------
uint32_t Crc32(const std::string& data) {
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
  for (const char c : data) {
    crc = table[(crc ^ static_cast<uint8_t>(c)) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

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
    Put32(out, 0x04034b50);  // local file header
    Put16(out, 20);          // version needed
    Put16(out, 0x0800);      // general purpose bit 11: UTF-8 names
    Put16(out, 0);           // stored
    Put16(out, 0);           // time
    Put16(out, 0x21);        // date (1980-01-01)
    Put32(out, Crc32(e.data));
    Put32(out, static_cast<uint32_t>(e.data.size()));
    Put32(out, static_cast<uint32_t>(e.data.size()));
    Put16(out, static_cast<uint16_t>(e.name.size()));
    Put16(out, 0);
    for (const char c : e.name) out.push_back(static_cast<uint8_t>(c));
    for (const char c : e.data) out.push_back(static_cast<uint8_t>(c));
  }
  const uint32_t centralOffset = static_cast<uint32_t>(out.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    Put32(out, 0x02014b50);  // central directory
    Put16(out, 20);
    Put16(out, 20);
    Put16(out, 0x0800);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, 0x21);
    Put32(out, Crc32(entries[i].data));
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
  Put32(out, 0x06054b50);  // end of central directory
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, static_cast<uint16_t>(entries.size()));
  Put16(out, static_cast<uint16_t>(entries.size()));
  Put32(out, centralSize);
  Put32(out, centralOffset);
  Put16(out, 0);
  return out;
}

std::string FromHex(const std::string& hex) {
  std::string out;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------
std::wstring MakeTempDir() {
  wchar_t tmp[MAX_PATH]{};
  GetTempPathW(MAX_PATH, tmp);
  std::wstring dir = std::wstring(tmp) + L"prism_it_" + std::to_wstring(GetCurrentProcessId());
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

std::wstring Locate7zDll() {
  // GetEnvironmentVariableW instead of _wgetenv: the latter is C4996-deprecated
  // and MSVC /WX turns that into an error.
  wchar_t envValue[MAX_PATH * 2]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", envValue, ARRAYSIZE(envValue)) > 0) {
    const wchar_t* fromEnv = envValue;
    if (std::filesystem::exists(fromEnv)) return fromEnv;
  }
  for (const wchar_t* candidate : {L"C:\\Program Files\\7-Zip\\7z.dll",
                                   L"C:\\Program Files (x86)\\7-Zip\\7z.dll", L"7z.dll"}) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return {};
}

bool ReadFileEquals(const std::wstring& path, const std::string& expect) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return content == expect;
}

// Shared state: SKIP the whole suite when no 7z.dll is present.
bool g_haveDll = false;
std::wstring g_dllPath;

PRISM_TEST("7z.dll loads and reports its format table") {
  if (!g_haveDll) {
    std::cout << "    SKIP: 7z.dll not found (set PRISM_7Z_DLL)" << std::endl;
    return;
  }
  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  PRISM_CHECK(library.loaded());
  const auto formats = library.FormatCount();
  PRISM_CHECK(formats.has_value());
  std::cout << "    " << *formats << " formats in " << prism::win::WideToUtf8(library.path())
            << std::endl;
  PRISM_CHECK(*formats > 10);  // a real 7z.dll knows 40+ containers
}

PRISM_TEST("catalog scan reads the central directory with zero payload work") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring zip = dir + L"\\pack.zip";
  {
    std::ofstream out(zip, std::ios::binary);
    const auto bytes = BuildZip({{"Assets/logo.txt", "hello prism"},
                                 {"Assets/nested/deep.txt", "deep"},
                                 {"README.md", "readme"}});
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<long>(bytes.size()));
  }

  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::Win32Transcoder transcoder;
  prism::win::ArchiveReader reader(library, &transcoder);
  const bool opened = reader.Open(zip);
  std::cout << "    open trace: " << reader.openTrace() << std::endl;
  if (!PRISM_CHECK(opened)) return;  // never index an empty catalog

  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(3))) return;
  PRISM_CHECK_EQ(catalog[0].path, std::string("Assets/logo.txt"));
  PRISM_CHECK_EQ(catalog[0].size, uint64_t(11));
  PRISM_CHECK(!catalog[0].isDir);
  PRISM_CHECK_EQ(catalog[2].path, std::string("README.md"));

  // PROPID discovery must have found the authoritative ids inside the DLL.
  std::cout << "    discovered propids:";
  for (const uint32_t id : reader.discoveredProperties()) std::cout << ' ' << id;
  std::cout << std::endl;
  if (!PRISM_CHECK(!reader.discoveredProperties().empty())) {
    std::cout << "    prop trace: " << reader.propTrace() << std::endl;
    return;
  }
  // kpidPath = 3, kpidPackSize = 8 must both be exposed by the Zip handler.
  PRISM_CHECK_EQ(reader.PropertyId("Path", 3), uint32_t(3));
  PRISM_CHECK_EQ(reader.PropertyId("PackSize", 8), uint32_t(8));

  // Regression for the 2026-10-31 "every directory reads as a file" bug:
  // kpidIsDir (6) is ANSWERED by the handler but never ADVERTISED in its
  // property-discovery table (7z's kPropMap has no kpidIsDir row). Gating the
  // read on discovery returned pid 0 and GetBool silently said "not a dir".
  // Discovery must never be able to change what we ask for.
  PRISM_CHECK_EQ(reader.PropertyId("IsDir", 6), uint32_t(6));
  if (reader.discoveredProperties().count(6) == 0) {
    std::cout << "    (kpidIsDir is not advertised by this handler -- the"
                 " exact condition that used to break directory listing)"
              << std::endl;
  }

  reader.Close();
  std::filesystem::remove_all(dir);
}

PRISM_TEST("in-memory extraction feeds the thumbnail pipeline") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring zip = dir + L"\\one.zip";
  {
    std::ofstream out(zip, std::ios::binary);
    const auto bytes = BuildZip({{"image.bin", "PNGDATA-1234567890"}});
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<long>(bytes.size()));
  }
  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  const bool opened = reader.Open(zip);
  std::cout << "    open trace: " << reader.openTrace() << std::endl;
  if (!PRISM_CHECK(opened)) return;
  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(1))) return;
  const auto bytes = reader.ExtractToMemory(0, 1024 * 1024);
  PRISM_CHECK(bytes.has_value());
  if (bytes) {
    const std::string text(bytes->begin(), bytes->end());
    PRISM_CHECK_EQ(text, std::string("PNGDATA-1234567890"));
  }
  reader.Close();
  std::filesystem::remove_all(dir);
}

PRISM_TEST("smart extraction writes straight into the wrapped capsule folder") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring zip = dir + L"\\pack.zip";
  {
    std::ofstream out(zip, std::ios::binary);
    const auto bytes = BuildZip({{"Assets/logo.txt", "hello prism"}, {"README.md", "readme"}});
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<long>(bytes.size()));
  }
  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(zip))) { std::cout << "    open trace: " << reader.openTrace() << std::endl; return; }
  const auto catalog = reader.ReadCatalog();

  const std::string outDir = prism::win::WideToUtf8(dir);
  const auto plan = prism::SmartExtractionPlanner::Build(
      prism::win::WideToUtf8(zip), outDir, catalog,
      [](const std::string& p) { return std::filesystem::exists(p); });
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)),
                 std::string("WrapIntoNewNamedSubfolder"));
  PRISM_CHECK_EQ(plan.baseTargetDir, outDir + "/pack");

  prism::BombFuse fuse;
  prism::win::SmartExtractCallback::Result result;
  PRISM_CHECK(reader.ExtractWithPlan(catalog, plan, plan.baseTargetDir, &fuse, &result));
  PRISM_CHECK(!result.guardTripped);
  PRISM_CHECK(!result.fuseTripped);
  PRISM_CHECK_EQ(result.filesWritten, uint64_t(2));

  const std::wstring capsule = dir + L"\\pack";
  PRISM_CHECK(ReadFileEquals(capsule + L"\\Assets\\logo.txt", "hello prism"));
  PRISM_CHECK(ReadFileEquals(capsule + L"\\README.md", "readme"));
  reader.Close();
  std::filesystem::remove_all(dir);
}

PRISM_TEST("nesting-doll archive is flattened to a single folder") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring zip = dir + L"\\Photo.zip";
  {
    std::ofstream out(zip, std::ios::binary);
    const auto bytes = BuildZip({{"Photo/Photo/1.jpg", "JPEGBYTES"}});
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<long>(bytes.size()));
  }
  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(zip))) { std::cout << "    open trace: " << reader.openTrace() << std::endl; return; }
  const auto catalog = reader.ReadCatalog();

  const std::string outDir = prism::win::WideToUtf8(dir);
  const auto plan = prism::SmartExtractionPlanner::Build(
      prism::win::WideToUtf8(zip), outDir, catalog,
      [](const std::string& p) { return std::filesystem::exists(p); });
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)),
                 std::string("StripRedundantNestedRoot"));

  prism::win::SmartExtractCallback::Result result;
  PRISM_CHECK(reader.ExtractWithPlan(catalog, plan, plan.baseTargetDir, nullptr, &result));
  // The whole point: Photo/Photo/1.jpg must land as Photo/1.jpg, not Photo/Photo/1.jpg.
  PRISM_CHECK(ReadFileEquals(dir + L"\\Photo\\1.jpg", "JPEGBYTES"));
  PRISM_CHECK(!std::filesystem::exists(dir + L"\\Photo\\Photo"));
  reader.Close();
  std::filesystem::remove_all(dir);
}

PRISM_TEST("zip slip payload is blocked by the write guard") {
  if (!g_haveDll) return;
  const std::wstring dir = MakeTempDir();
  const std::wstring zip = dir + L"\\evil.zip";
  {
    std::ofstream out(zip, std::ios::binary);
    const auto bytes = BuildZip({{"../../evil.txt", "pwned"}, {"good.txt", "fine"}});
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<long>(bytes.size()));
  }
  prism::win::SevenZipLibrary library;
  PRISM_CHECK(library.Load(g_dllPath));
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(zip))) { std::cout << "    open trace: " << reader.openTrace() << std::endl; return; }
  const auto catalog = reader.ReadCatalog();

  const std::string outDir = prism::win::WideToUtf8(dir) + "/out";
  const auto plan = prism::SmartExtractionPlanner::Build(
      prism::win::WideToUtf8(zip), outDir, catalog,
      [](const std::string& p) { return std::filesystem::exists(p); });
  prism::BombFuse fuse;
  prism::win::SmartExtractCallback::Result result;
  reader.ExtractWithPlan(catalog, plan, plan.baseTargetDir, &fuse, &result);
  // Whatever happens, nothing may be written outside the target root.
  for (const std::string& written : result.writtenPaths) {
    PRISM_CHECK(prism::IsContainedIn(plan.baseTargetDir, written));
  }
  PRISM_CHECK(!std::filesystem::exists(dir + L"\\evil.txt"));
  PRISM_CHECK(!std::filesystem::exists(dir + L"\\..\\evil.txt"));
  reader.Close();
  std::filesystem::remove_all(dir);
}

PRISM_TEST("legacy CJK name is repaired through the Win32 transcoder") {
  prism::win::Win32Transcoder transcoder;
  const std::string gbkBytes = FromHex("cec4b5b5d7cac1cf2e646f6378");
  const auto repaired = prism::RepairArchiveName(gbkBytes, false, &transcoder);
  PRISM_CHECK(repaired.repaired);
  PRISM_CHECK_EQ(repaired.name,
                 std::string("\xe6\x96\x87\xe6\xa1\xa3\xe8\xb5\x84\xe6\x96\x99.docx"));
}

}  // namespace

int main(int argc, char** argv) {
  g_dllPath = Locate7zDll();
  g_haveDll = !g_dllPath.empty();
  if (!g_haveDll) {
    std::cout << "WARNING: 7z.dll not found -- integration checks will SKIP" << std::endl;
  } else {
    std::wcout << L"7z.dll: " << g_dllPath << std::endl;
  }
  const int rc = ::prism::test::RunAll(argc, argv);
  // The marker below is part of the verdict: ctest requires it in stdout, so
  // a silent SKIP can never count as a pass (L2: negative control).
  if (rc == 0 && g_haveDll) std::cout << "ALL ENGINE-BRIDGE CHECKS PASSED" << std::endl;
  return rc;
}
