// PrismArchive -- card C43: format coverage (rar/iso/cab/wim/msi/chm/cpio/arj/
// lzh/dmg and the "engine cannot read it" answer).
//
// WHY THIS SUITE EXISTS
// C26 routed Open() by extension and C28 added magic sniffing, but the handler
// table only listed ten extensions. Every other common container -- .iso, .cab,
// .wim, .msi, .chm, .cpio, .arj, .lzh, .dmg -- fell through to "try the
// candidates in turn", and that probe is not free: measured on this engine, the
// [Zip, 7z] probe reads an ENTIRE 8 MiB file before it declines one whose first
// four bytes already said "MSCF" (see the byte-gate cells below). The gate is a
// BYTE COUNT, not a stopwatch (C28 established that: a wall clock measures the
// runner, a byte count measures the code).
//
// STEP 1 -- THE FACTS, ENUMERATED, NOT REMEMBERED
// 7z.dll publishes its own handler table through GetNumberOfFormats() +
// GetHandlerProperty2(). Dumped on 2026-11-02 from E:\DevTools\7-Zip\7z.dll
// (7-Zip 25.x, 60 handlers). Columns: index | handler name | CLSID |
// extensions it claims. NOTE: kClassID comes back as a VT_BSTR holding the RAW
// 16 GUID bytes (SysAllocStringByteLen in ArchiveExports.cpp), not a printable
// string -- reading it as text yields mojibake, which is how the first dump of
// this card was wrong.
//
//    03 | Arj      | {23170F69-40C1-278A-1000-000110040000} | arj
//    06 | Lzh      | {23170F69-40C1-278A-1000-000110060000} | lzh lha
//    08 | Cab      | {23170F69-40C1-278A-1000-000110080000} | cab
//    E4 | Dmg      | {23170F69-40C1-278A-1000-000110E40000} | dmg
//    E5 | Compound | {23170F69-40C1-278A-1000-000110E50000} | msi msp doc xls ppt
//    E6 | wim      | {23170F69-40C1-278A-1000-000110E60000} | wim swm esd ppkg
//    E7 | Iso      | {23170F69-40C1-278A-1000-000110E70000} | iso img
//    E9 | Chm      | {23170F69-40C1-278A-1000-000110E90000} | chm chi chq chw
//    ED | Cpio     | {23170F69-40C1-278A-1000-000110ED0000} | cpio
//    -- | Rar      | {23170F69-40C1-278A-1000-000110030000} | rar r00
//    CC | Rar5     | {23170F69-40C1-278A-1000-000110CC0000} | rar r00
//    01 | zip      | {23170F69-40C1-278A-1000-000110010000} | zip z01 zipx jar ...
//    07 | 7z       | {23170F69-40C1-278A-1000-000110070000} | 7z
//    EE | tar      | {23170F69-40C1-278A-1000-000110EE0000} | tar ova
//    EF | gzip     | {23170F69-40C1-278A-1000-000110EF0000} | gz gzip tgz tpz apk
//    02 | bzip2    | {23170F69-40C1-278A-1000-000110020000} | bz2 bzip2 tbz2 tbz
//    0C | xz       | {23170F69-40C1-278A-1000-0001100C0000} | xz txz
//    0E | zstd     | {23170F69-40C1-278A-1000-0001100E0000} | zst tzst
//    -- | (lz4, br) -- THIS ENGINE HAS NO HANDLER FOR THEM (0 CLSIDs each).
//       That absence is a FACT this suite exercises, not a gap: it is the only
//       real "engine cannot read it" case available, and Open() must say so by
//       name instead of probing.
//
// Multi-volume: GetHandlerProperty2 exposes name/CLSID/extension/addext ONLY --
// there is no volume property to enumerate, so this table states what the engine
// says and nothing about volumes. The one volume-shaped handler it DOES publish
// is index 34 "Split" (extension "001"), which joins byte-split parts; .001 is
// deliberately NOT routed here (a .001 part is not an archive the user opens).
//
// WHAT IS ASSERTED, PER FORMAT (sparse fixture: head/tail only, 8 MiB hole)
//   (1) the bytes are identified (trace "sniff=<fmt> route=sniffed");
//   (2) Open() reads < 64 KiB of an 8 MiB file (the C28 byte gate);
//   (3) a failure is EXPLICIT -- failureReason() names the format, and no
//       [Zip, 7z] probe was run -- never a silent empty catalog;
//   (4) two containers that CAN be authored here (cpio, cab) are opened for
//       real: 1 item, still under the byte gate.
// Negative control (PRISM_C43_MUTANT_NO_ISO_SNIFF): the iso rule is deleted at
// compile time, so the fixture falls back to route + probe and the byte gate
// must go red. The marker "C43 MUTANT CAUGHT" is printed only when a byte-gate
// assertion really failed AND the injection counter proves the deletion fired
// (CONTRACTS D3/D26 -- no WILL_FAIL next to PASS_REGULAR_EXPRESSION).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "FormatSniffer.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"

namespace {

int g_failures = 0;
int g_byteGateFailures = 0;  // the mutant's verdict keys off THESE failures only

bool CheckTrue(bool ok, const char* expr, const char* file, int line) {
  if (!ok) {
    std::printf("    FAIL %s:%d  expected true: %s\n", file, line, expr);
    ++g_failures;
  }
  return ok;
}

template <class A, class B>
bool CheckEq(const A& a, const B& b, const char* exprA, const char* exprB, const char* file,
             int line) {
  const bool ok = (a == b);
  if (!ok) {
    std::printf("    FAIL %s:%d  expected (%s) == (%s)\n", file, line, exprA, exprB);
    ++g_failures;
  }
  return ok;
}

#define C43_CHECK(cond) ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define C43_CHECK_EQ(a, b) ::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

// The gate the card asks for: identifying a container must never read more than
// 64 KiB of it, whatever its size.
constexpr uint64_t kOpenByteBudget = 64ull << 10;
constexpr uint64_t kFixtureSize = 8ull << 20;  // 8 MiB of HOLE, not of writes

// ---------------------------------------------------------------------------
// Section A: the sniffer rules, as pure logic (no 7z.dll, no files)
// ---------------------------------------------------------------------------
prism::win::sniff::Window MakeWindow(const std::vector<uint8_t>& bytes,
                                     std::string_view extension = {}) {
  prism::win::sniff::Window window;
  window.head = bytes.empty() ? nullptr : bytes.data();
  window.headSize = bytes.size();
  window.tail = window.head;
  window.tailSize = bytes.size();
  window.fileSize = bytes.size();
  window.extensionHint = extension;
  return window;
}

// A window whose head is 'bytes' and whose sector-16 probe is 'sector16'.
prism::win::sniff::Window MakeWindowWithSector16(const std::vector<uint8_t>& head,
                                                 const std::vector<uint8_t>& sector16,
                                                 uint64_t fileSize) {
  prism::win::sniff::Window window = MakeWindow(head);
  window.sector16 = sector16.empty() ? nullptr : sector16.data();
  window.sector16Size = sector16.size();
  window.fileSize = fileSize;
  return window;
}

std::vector<uint8_t> IsoSector16() {
  std::vector<uint8_t> sector(8, 0);
  sector[0] = 0x01;  // primary volume descriptor type
  std::memcpy(sector.data() + 1, "CD001", 5);
  return sector;
}

// A DMG trailer: "koly" opens the LAST 512-byte block.
std::vector<uint8_t> DmgTrailer() {
  std::vector<uint8_t> trailer(512, 0);
  std::memcpy(trailer.data(), "koly", 4);
  return trailer;
}

void RunSnifferChecks() {
  using prism::win::sniff::ArchiveFormat;
  using prism::win::sniff::Confidence;
  using prism::win::sniff::FormatExtension;
  using prism::win::sniff::FormatName;
  using prism::win::sniff::Sniff;

  auto expect = [](const char* label, const prism::win::sniff::Window& window,
                   ArchiveFormat format, Confidence confidence) {
    const auto finding = Sniff(window);
    const bool formatOk = std::strcmp(FormatName(finding.format), FormatName(format)) == 0;
    const bool confidenceOk = finding.confidence == confidence;
    std::printf("    %-30s -> sniff=%-8s (%s)%s\n", label, FormatName(finding.format),
                finding.evidence.c_str(), (formatOk && confidenceOk) ? "" : "   <-- MISMATCH");
    if (!formatOk) {
      std::printf("    FAIL %s: expected format %s, got %s\n", label, FormatName(format),
                  FormatName(finding.format));
      ++g_failures;
    }
    if (!confidenceOk) {
      std::printf("    FAIL %s: expected confidence %s, got %s\n", label,
                  prism::win::sniff::ConfidenceName(confidence),
                  prism::win::sniff::ConfidenceName(finding.confidence));
      ++g_failures;
    }
    return formatOk && confidenceOk;
  };

  // --- cab ------------------------------------------------------------------
  expect("cab MSCF", MakeWindow({'M', 'S', 'C', 'F', 0, 0, 0, 0}), ArchiveFormat::kCab,
         Confidence::kMagic);
  expect("MSC only", MakeWindow({'M', 'S', 'C', 0, 0, 0, 0, 0}), ArchiveFormat::kUnknown,
         Confidence::kNone);

  // --- wim ------------------------------------------------------------------
  expect("wim MSWIM\\0", MakeWindow({'M', 'S', 'W', 'I', 'M', 0, 0, 0}), ArchiveFormat::kWim,
         Confidence::kMagic);
  {
    // "MSWIMX" is not a WIM: the NUL is part of the magic.
    std::vector<uint8_t> fake = {'M', 'S', 'W', 'I', 'M', 'X', 0, 0};
    expect("MSWIMX", MakeWindow(fake), ArchiveFormat::kUnknown, Confidence::kNone);
  }

  // --- msi (OLE compound document) ------------------------------------------
  expect("ole compound (msi)",
         MakeWindow({0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1}), ArchiveFormat::kCompound,
         Confidence::kMagic);
  {
    std::vector<uint8_t> almost = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE2};
    expect("ole with a broken tail", MakeWindow(almost), ArchiveFormat::kUnknown,
           Confidence::kNone);
  }

  // --- chm ------------------------------------------------------------------
  expect("chm ITSF", MakeWindow({'I', 'T', 'S', 'F', 3, 0, 0, 0}), ArchiveFormat::kChm,
         Confidence::kMagic);
  expect("ITSFx", MakeWindow({'I', 'T', 'S', 'F', 'x', 0, 0, 0}), ArchiveFormat::kChm,
         Confidence::kMagic);  // "ITSF" is the whole signature; byte 4 is a version

  // --- cpio -----------------------------------------------------------------
  expect("cpio newc 070701", MakeWindow({'0', '7', '0', '7', '0', '1', 0, 0}),
         ArchiveFormat::kCpio, Confidence::kMagic);
  expect("cpio crc 070702", MakeWindow({'0', '7', '0', '7', '0', '2', 0, 0}),
         ArchiveFormat::kCpio, Confidence::kMagic);
  expect("cpio odc 070707", MakeWindow({'0', '7', '0', '7', '0', '7', 0, 0}),
         ArchiveFormat::kCpio, Confidence::kMagic);
  expect("cpio 070700", MakeWindow({'0', '7', '0', '7', '0', '0', 0, 0}), ArchiveFormat::kUnknown,
         Confidence::kNone);

  // --- arj ------------------------------------------------------------------
  expect("arj 60ea", MakeWindow({0x60, 0xEA, 0x00, 0x00}), ArchiveFormat::kArj,
         Confidence::kMagic);
  expect("arj 60eb", MakeWindow({0x60, 0xEB, 0x00, 0x00}), ArchiveFormat::kUnknown,
         Confidence::kNone);

  // --- lzh ------------------------------------------------------------------
  {
    // Level-0 header: size, checksum, then "-lh5-".
    std::vector<uint8_t> lzh = {0x16, 0x00, '-', 'l', 'h', '5', '-'};
    expect("lzh -lh5-", MakeWindow(lzh), ArchiveFormat::kLzh, Confidence::kMagic);
    // "-lh" without the closing '-' is prose, not a header.
    std::vector<uint8_t> prose = {0x16, 0x00, '-', 'l', 'h', '5', 'x'};
    expect("lzh without the closing '-'", MakeWindow(prose), ArchiveFormat::kUnknown,
           Confidence::kNone);
  }

  // --- iso (sector 16) ------------------------------------------------------
  {
    const auto head = std::vector<uint8_t>(512, 0);
    const auto sector = IsoSector16();
#ifndef PRISM_C43_MUTANT_NO_ISO_SNIFF
    // In the mutant build this rule is DELETED, so asserting it would only
    // re-state the mutation. The mutant's own verdict is the byte gate below.
    expect("iso cd001@32769", MakeWindowWithSector16(head, sector, 8ull << 20),
           ArchiveFormat::kIso, Confidence::kMagic);
#endif
    // No sector-16 probe (a file too small to have one) -> no claim.
    expect("iso without the sector-16 probe", MakeWindow(head), ArchiveFormat::kUnknown,
           Confidence::kNone);
    // A CD002 descriptor is a different volume type, not the primary descriptor.
    std::vector<uint8_t> cd002 = {'0', 'C', 'D', '0', '0', '2'};
    expect("iso cd002", MakeWindowWithSector16(head, cd002, 8ull << 20), ArchiveFormat::kUnknown,
           Confidence::kNone);
  }

  // --- dmg (last 512-byte block) --------------------------------------------
  {
    prism::win::sniff::Window window;
    std::vector<uint8_t> head(512, 0);
    const auto trailer = DmgTrailer();
    window.head = head.data();
    window.headSize = head.size();
    window.tail = trailer.data();
    window.tailSize = trailer.size();
    window.fileSize = 8ull << 20;
    expect("dmg koly@tail", window, ArchiveFormat::kDmg, Confidence::kMagic);

    // The same four bytes anywhere else in the tail is a coincidence.
    std::vector<uint8_t> shifted(1024, 0);
    std::memcpy(shifted.data() + 100, "koly", 4);
    window.tail = shifted.data();
    window.tailSize = shifted.size();
    expect("koly not at the trailer", window, ArchiveFormat::kUnknown, Confidence::kNone);
  }

  // --- the new formats are routed, not probed -------------------------------
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kIso)), std::string("iso"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kCab)), std::string("cab"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kWim)), std::string("wim"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kCompound)), std::string("msi"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kChm)), std::string("chm"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kCpio)), std::string("cpio"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kArj)), std::string("arj"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kLzh)), std::string("lzh"));
  C43_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kDmg)), std::string("dmg"));
  // The extension table must actually route each one (a sniff without a route
  // falls back to the pinned CLSID; a route without a sniff falls back to the
  // probe -- both are needed, and both are asserted here).
  const wchar_t* routed[] = {L"a.iso", L"a.cab", L"a.wim", L"a.msi", L"a.chm",
                             L"a.cpio", L"a.arj", L"a.lzh", L"a.dmg"};
  for (const wchar_t* path : routed) {
    if (!C43_CHECK(prism::win::FindFormatRoute(path) != nullptr)) {
      std::printf("    FAIL no route for %ls\n", path);
    }
  }
}

// ---------------------------------------------------------------------------
// Fixtures: sparse files (head/tail patches only) in a directory of their own
// ---------------------------------------------------------------------------
std::filesystem::path WorkRoot() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_FORMAT_COVERAGE_ROOT", overridePath, MAX_PATH) > 0) {
    return std::filesystem::path(overridePath);
  }
  std::filesystem::path temp = std::filesystem::temp_directory_path();
  if (temp.filename().empty()) temp = temp.parent_path();
  // One directory per card: two agents' fixtures must never share a name.
  return temp / L"prism_format_coverage_c43";
}

struct Patch {
  uint64_t offset = 0;
  std::vector<uint8_t> bytes;
};

bool WriteAt(HANDLE file, uint64_t offset, const uint8_t* data, std::size_t size) {
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
  DWORD written = 0;
  return WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
}

// A file of 'size' bytes that is a HOLE except for the patches: a fixture costs
// the bytes it patches, not the size it claims.
bool MakeSparseFixture(const std::filesystem::path& path, uint64_t size,
                       const std::vector<Patch>& patches) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD returned = 0;
  DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
  LARGE_INTEGER end{};
  end.QuadPart = static_cast<LONGLONG>(size);
  bool ok = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
  for (const Patch& patch : patches) {
    if (!ok) break;
    ok = WriteAt(file, patch.offset, patch.bytes.data(), patch.bytes.size());
  }
  CloseHandle(file);
  return ok;
}

std::string Hex64(uint64_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08llX",
                static_cast<unsigned long long>(value & 0xFFFFFFFFull));
  return buffer;
}

// A genuine POSIX "newc" cpio archive with one 3-byte file. 7-Zip's Cpio
// handler accepts this as-is (measured: 508 bytes read, 1 item).
std::vector<uint8_t> BuildNewcCpio() {
  std::vector<uint8_t> out;
  auto add = [&out](const char* text, std::size_t size) {
    out.insert(out.end(), text, text + size);
  };
  auto pad4 = [&out]() {
    while (out.size() % 4) out.push_back(0);
  };
  auto entry = [&](const std::string& name, const std::string& data, uint32_t mode) {
    std::string header = "070701";
    header += Hex64(1);                  // ino
    header += Hex64(mode);               // mode
    header += Hex64(0);                  // uid
    header += Hex64(0);                  // gid
    header += Hex64(1);                  // nlink
    header += Hex64(0x60000000);         // mtime
    header += Hex64(data.size());        // filesize
    header += Hex64(0);                  // devmajor
    header += Hex64(0);                  // devminor
    header += Hex64(0);                  // rdevmajor
    header += Hex64(0);                  // rdevminor
    header += Hex64(name.size() + 1);    // namesize (includes the NUL)
    header += Hex64(0);                  // check
    add(header.c_str(), header.size());
    add(name.c_str(), name.size());
    out.push_back(0);
    pad4();
    add(data.c_str(), data.size());
    pad4();
  };
  entry("hello.txt", "hi\n", 0x81A4);
  entry("TRAILER!!!", "", 0);
  return out;
}

// A genuine Microsoft cabinet: one folder, one file, typeCompress 0 (stored),
// csum 0 ("no checksum"). 7-Zip's Cab handler accepts it (measured: 81 bytes,
// 1 item) -- and still accepts it when the cabinet is grown into an 8 MiB hole.
std::vector<uint8_t> BuildCabinet() {
  std::vector<uint8_t> cab(36, 0);
  std::memcpy(cab.data(), "MSCF", 4);
  cab[24] = 3;  // versionMinor
  cab[25] = 1;  // versionMajor
  auto put16 = [&cab](std::size_t at, uint16_t value) {
    cab[at] = static_cast<uint8_t>(value & 0xFF);
    cab[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  };
  auto put32 = [&cab](std::size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) cab[at + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  };
  put16(26, 1);  // cFolders
  put16(28, 1);  // cFiles
  const std::size_t folderAt = cab.size();
  cab.resize(cab.size() + 8, 0);  // CFFOLDER
  const std::size_t filesAt = cab.size();
  put32(16, static_cast<uint32_t>(filesAt));  // coffFiles
  cab.resize(cab.size() + 16, 0);             // CFFILE fixed part
  const char payload[] = "hi\n";
  put32(filesAt, 3);              // cbFile
  put32(filesAt + 4, 0);          // uoffFolderStart
  put16(filesAt + 10, 0x6000);    // date
  put16(filesAt + 12, 0x0000);    // time
  put16(filesAt + 14, 0x20);      // attribs = FILE_ATTRIBUTE_ARCHIVE
  const std::string name = "hello.txt";
  cab.insert(cab.end(), name.begin(), name.end());
  cab.push_back(0);
  const std::size_t dataAt = cab.size();
  put32(folderAt, static_cast<uint32_t>(dataAt));  // coffCabStart
  put16(folderAt + 4, 1);                          // cCFData
  put16(folderAt + 6, 0);                          // typeCompress = none
  cab.resize(cab.size() + 8, 0);                   // CFDATA header
  put32(dataAt, 0);                                // csum = 0 -> no checksum
  put16(dataAt + 4, 3);                            // cbData
  put16(dataAt + 6, 3);                            // cbUncomp
  cab.insert(cab.end(), payload, payload + 3);
  put32(8, static_cast<uint32_t>(cab.size()));  // cbCabinet
  return cab;
}

// ---------------------------------------------------------------------------
// Measuring one Open
// ---------------------------------------------------------------------------
struct OpenMeasurement {
  bool opened = false;
  uint64_t bytes = 0;
  uint64_t sniffBytes = 0;
  size_t items = 0;
  std::string trace;
  std::string reason;
  std::string unsupported;
};

OpenMeasurement Measure(prism::win::SevenZipLibrary& library, const std::filesystem::path& file) {
  OpenMeasurement measured;
  prism::win::ResetIoCounters();
  prism::win::ArchiveReader reader(library);
  measured.opened = reader.Open(file.wstring());
  measured.bytes = prism::win::IoCounters().bytesRead.load();
  measured.sniffBytes = prism::win::IoCounters().sniffBytes.load();
  measured.trace = reader.openTrace();
  measured.reason = reader.failureReason();
  measured.unsupported = reader.unsupportedFormat();
  if (measured.opened) measured.items = reader.ReadCatalog().size();
  reader.Close();
  return measured;
}

bool HasToken(const std::string& text, const std::string& token) {
  return text.find(token) != std::string::npos;
}

void PrintMeasurement(const char* label, const OpenMeasurement& measured) {
  std::printf("    %-14s bytes=%-8llu sniff=%-5llu items=%zu opened=%d\n", label,
              static_cast<unsigned long long>(measured.bytes),
              static_cast<unsigned long long>(measured.sniffBytes), measured.items,
              measured.opened ? 1 : 0);
  std::printf("      trace: %s\n", measured.trace.c_str());
  if (!measured.reason.empty()) std::printf("      reason: %s\n", measured.reason.c_str());
}

// The C28 byte gate. ONLY this feeds the negative control's verdict.
bool CheckByteGate(const char* label, const OpenMeasurement& measured) {
  if (measured.bytes <= kOpenByteBudget) return true;
  std::printf("    FAIL %s: Open read %llu bytes of an 8 MiB fixture (budget %llu)\n", label,
              static_cast<unsigned long long>(measured.bytes),
              static_cast<unsigned long long>(kOpenByteBudget));
  ++g_failures;
  ++g_byteGateFailures;
  return false;
}

// A cell that must OPEN: the container is genuine, so the sniffed handler takes
// it and the catalog has to be non-empty.
void CheckOpenedCell(const char* label, const OpenMeasurement& measured, const char* format) {
  PrintMeasurement(label, measured);
  CheckByteGate(label, measured);
  if (!C43_CHECK(measured.opened)) {
    std::printf("    FAIL %s: a genuine %s must open (reason: %s)\n", label, format,
                measured.reason.c_str());
    return;
  }
  if (!C43_CHECK_EQ(measured.items, static_cast<size_t>(1))) {
    std::printf("    FAIL %s: the catalog lost its item\n", label);
  }
  const std::string wanted = std::string("sniff=") + format + " route=sniffed";
  if (!C43_CHECK(HasToken(measured.trace, wanted))) {
    std::printf("    FAIL %s: expected '%s' in the trace\n", label, wanted.c_str());
  }
}

// A cell whose container cannot be parsed by this engine. The contract is the
// SAME in both directions: the bytes are identified, the failure is explicit
// and names the format, and no probe walks the file.
void CheckDeclinedCell(const char* label, const OpenMeasurement& measured, const char* format) {
  PrintMeasurement(label, measured);
  CheckByteGate(label, measured);
  const std::string sniffed = std::string("sniff=") + format + " route=sniffed";
  if (!C43_CHECK(HasToken(measured.trace, sniffed))) {
    std::printf("    FAIL %s: expected '%s' in the trace\n", label, sniffed.c_str());
  }
  const std::string declined = std::string("declined=") + format;
  if (!C43_CHECK(HasToken(measured.trace, declined))) {
    std::printf("    FAIL %s: expected '%s' in the trace (explicit, not a probe)\n", label,
                declined.c_str());
  }
  // CONTRACTS D1: a failure with no reason is the "empty directory" this card
  // exists to remove.
  if (!C43_CHECK(!measured.reason.empty())) {
    std::printf("    FAIL %s: Open() failed with no reason -- that is the empty-catalog bug\n",
                label);
  }
  if (!C43_CHECK(measured.reason.find(format) != std::string::npos)) {
    std::printf("    FAIL %s: the reason does not name '%s': %s\n", label, format,
                measured.reason.c_str());
  }
  // The pre-C26 probe is exactly the whole-file walk this card removes.
  if (!C43_CHECK(!HasToken(measured.trace, "Zip.Open") && !HasToken(measured.trace, "7z.Open"))) {
    std::printf("    FAIL %s: the legacy [Zip, 7z] probe ran after a PROVEN format\n", label);
  }
  if (!C43_CHECK(!measured.opened)) {
    std::printf("    FAIL %s: expected the handler to reject this fixture\n", label);
  }
}

// ---------------------------------------------------------------------------
// The engine's own table vs the table this card pinned (step 1, as a gate)
// ---------------------------------------------------------------------------
struct PinnedFormat {
  const char* handler;   // as the engine spells it
  const char* extension; // the extension that routes to it
  const GUID* clsid;
  const char* claimed;   // extensions the engine claims for it (substring check)
};

void CheckEngineTable(prism::win::SevenZipLibrary& library) {
  std::printf("  -- engine handler table (GetHandlerProperty2) --\n");
  const auto& formats = library.formats();
  std::printf("    this 7z.dll advertises %zu handlers\n", formats.size());
  if (!C43_CHECK(!formats.empty())) return;

  const PinnedFormat pinned[] = {
      {"Arj", "arj", &prism::win::kClsidArj, "arj"},
      {"Lzh", "lzh", &prism::win::kClsidLzh, "lzh"},
      {"Cab", "cab", &prism::win::kClsidCab, "cab"},
      {"Dmg", "dmg", &prism::win::kClsidDmg, "dmg"},
      {"Compound", "msi", &prism::win::kClsidCompound, "msi"},
      {"wim", "wim", &prism::win::kClsidWim, "wim"},
      {"Iso", "iso", &prism::win::kClsidIso, "iso"},
      {"Chm", "chm", &prism::win::kClsidChm, "chm"},
      {"Cpio", "cpio", &prism::win::kClsidCpio, "cpio"},
  };
  for (const PinnedFormat& expected : pinned) {
    const prism::win::EngineFormatInfo* format = library.FormatNamed(expected.handler);
    if (!C43_CHECK(format != nullptr)) {
      std::printf("    FAIL engine has no '%s' handler\n", expected.handler);
      continue;
    }
    // A pinned GUID that disagrees with the engine is a silent degradation to
    // the probe path on every open of that format (CONTRACTS A4).
    if (!C43_CHECK(std::memcmp(&format->clsid, expected.clsid, sizeof(GUID)) == 0)) {
      std::printf("    FAIL pinned CLSID for %s disagrees with the engine\n", expected.handler);
    }
    const auto resolved = library.ClsidsForExtension(expected.extension);
    if (!C43_CHECK(!resolved.empty())) {
      std::printf("    FAIL the engine routes no handler for .%s\n", expected.extension);
    }
    bool claims = false;
    for (const std::string& extension : format->extensions) {
      if (extension == expected.claimed) claims = true;
    }
    if (!C43_CHECK(claims)) {
      std::printf("    FAIL the engine's '%s' does not claim '%s'\n", expected.handler,
                  expected.claimed);
    }
    std::printf("    %-9s clsid ok, claims '%s', %zu handler(s) for .%s\n", expected.handler,
                expected.claimed, resolved.size(), expected.extension);
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("=== format-coverage (C43) ===");
#ifdef PRISM_C43_MUTANT_NO_ISO_SNIFF
  std::printf(" [MUTANT: iso sniff rule deleted]");
#endif
#ifdef PRISM_C43_SNIFF_ONLY
  std::printf(" [sniffer only]");
#endif
  std::printf("\n");

  std::printf("  -- sniffer table (C43 formats) --\n");
  RunSnifferChecks();

#ifdef PRISM_C43_SNIFF_ONLY
  if (g_failures == 0) {
    std::printf("ALL FORMAT-COVERAGE SNIFFER CHECKS PASSED\n");
    return 0;
  }
  std::printf("FORMAT-COVERAGE SNIFFER CHECKS FAILED (%d)\n", g_failures);
  return 1;
#else
  if (g_failures != 0) {
    // Never let a missing engine hide a red sniffer: this is the half that runs
    // everywhere, and a SKIP after a failure would be a false green (D1).
    std::printf("FORMAT-COVERAGE CHECKS FAILED (%d, sniffer)\n", g_failures);
    return 1;
  }
  const std::wstring dllPath = prism::fixture::DllPath();
  if (dllPath.empty()) {
    std::printf("SKIP: PRISM_7Z_DLL is not set -- the byte-gate half needs a real 7z.dll\n");
    return 77;
  }
  prism::win::SevenZipLibrary library;
  if (!C43_CHECK(library.Load(dllPath))) {
    std::printf("FORMAT-COVERAGE CHECKS FAILED: cannot load 7z.dll\n");
    return 1;
  }

  std::error_code ec;
  const std::filesystem::path work = WorkRoot();
  std::filesystem::create_directories(work, ec);

  // ------------------------------------------------------------------ fixtures
  std::printf("  -- sparse fixtures (8 MiB hole, head/tail only) --\n");
  const auto cpio = BuildNewcCpio();
  const auto cab = BuildCabinet();
  struct Fixture {
    const wchar_t* name;
    std::vector<Patch> patches;
  };
  auto head = [](const std::vector<uint8_t>& bytes) {
    return std::vector<Patch>{{0, bytes}};
  };
  const std::vector<Fixture> fixtures = {
      {L"accept.cpio", head(cpio)},
      {L"accept.cab", head(cab)},
      {L"fake.iso", {{0x8000, {0x01, 'C', 'D', '0', '0', '1', 0x01, 0x00}}}},
      {L"fake.wim", head({'M', 'S', 'W', 'I', 'M', 0, 0, 0})},
      {L"fake.msi", head({0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1})},
      {L"fake.chm", head({'I', 'T', 'S', 'F', 3, 0, 0, 0})},
      {L"fake.arj", head({0x60, 0xEA, 0x00, 0x00, 0x00, 0x00})},
      {L"fake.lzh", head({0x16, 0x00, '-', 'l', 'h', '5', '-', 0x00})},
      {L"fake.dmg", {{kFixtureSize - 512, DmgTrailer()}}},
      {L"unsupported.lz4", head({0x04, 0x22, 0x4D, 0x18, 0x64, 0x40, 0xA7, 0x00})},
  };
  for (const Fixture& fixture : fixtures) {
    const std::filesystem::path path = work / fixture.name;
    if (!C43_CHECK(MakeSparseFixture(path, kFixtureSize, fixture.patches))) {
      std::printf("    FAIL cannot author %ls\n", fixture.name);
    }
  }

  CheckEngineTable(library);

  // ------------------------------------------------------------------ accepts
  std::printf("  -- containers this engine can read (accept path) --\n");
  CheckOpenedCell("accept.cpio", Measure(library, work / L"accept.cpio"), "cpio");
  CheckOpenedCell("accept.cab", Measure(library, work / L"accept.cab"), "cab");

  // ------------------------------------------------------------------ declines
  std::printf("  -- headers only: identified, then rejected BY NAME --\n");
  struct DeclineCase {
    const wchar_t* file;
    const char* label;
    const char* format;
  };
  const DeclineCase declineCases[] = {
      {L"fake.iso", "iso", "iso"},       {L"fake.wim", "wim", "wim"},
      {L"fake.msi", "msi", "compound"},  {L"fake.chm", "chm", "chm"},
      {L"fake.arj", "arj", "arj"},       {L"fake.lzh", "lzh", "lzh"},
      {L"fake.dmg", "dmg", "dmg"},
  };
  for (const DeclineCase& item : declineCases) {
    CheckDeclinedCell(item.label, Measure(library, work / item.file), item.format);
  }

  // ------------------------------------------------------------------ no handler
  // The only "engine cannot read this format at all" case this 7z.dll offers:
  // it advertises no lz4 handler (0 CLSIDs, see the table at the top). If a
  // future engine gains one, the arm is skipped WITH THE FACT PRINTED -- never
  // silently.
  std::printf("  -- format the engine has no handler for --\n");
  if (library.ClsidsForExtension("lz4").empty()) {
    const OpenMeasurement measured = Measure(library, work / L"unsupported.lz4");
    PrintMeasurement("lz4", measured);
    CheckByteGate("lz4", measured);
    C43_CHECK(!measured.opened);
    C43_CHECK_EQ(measured.unsupported, std::string("lz4"));
    C43_CHECK(HasToken(measured.trace, "unsupported=lz4 reason=no-handler"));
    C43_CHECK(!measured.reason.empty());
    C43_CHECK(!HasToken(measured.trace, "Zip.Open") && !HasToken(measured.trace, "7z.Open"));
  } else {
    std::printf("    note: this 7z.dll HAS an lz4 handler -- the unsupported arm is skipped\n");
  }

  const uint64_t injections = prism::win::IoCounters().mutantIsoSniffOff.load();
#ifdef PRISM_C43_MUTANT_NO_ISO_SNIFF
  // CONTRACTS D3/D26: the marker demands a RED byte gate AND proof the deletion
  // fired. No WILL_FAIL -- this marker is the verdict.
  if (g_byteGateFailures > 0 && injections > 0) {
    std::printf("C43 MUTANT CAUGHT: iso rule deleted -> %d byte-gate assertion(s) red, "
                "injections=%llu\n",
                g_byteGateFailures, static_cast<unsigned long long>(injections));
    return 0;
  }
  std::printf("C43 MUTANT NOT CAUGHT: the byte gate stayed green (failures=%d, injections=%llu)\n",
              g_byteGateFailures, static_cast<unsigned long long>(injections));
  return 1;
#else
  std::printf("  iso-sniff injections: %llu (0 = the rule was armed)\n",
              static_cast<unsigned long long>(injections));
  if (g_failures == 0) {
    std::printf("ALL FORMAT-COVERAGE CHECKS PASSED\n");
    return 0;
  }
  std::printf("FORMAT-COVERAGE CHECKS FAILED (%d, %d byte-gate)\n", g_failures,
              g_byteGateFailures);
  return 1;
#endif
#endif  // PRISM_C43_SNIFF_ONLY
}
