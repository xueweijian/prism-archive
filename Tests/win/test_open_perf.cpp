// PrismArchive -- card C28: open-performance gates.
//
// C7 measured a 4 GiB archive Open at 78.5 s (~55 MB/s of sequential read, the
// shape of a whole-file scan) and C17 cross-checked it. C28's claim is that the
// cost is in the ROUTING, not in 7-Zip: a container's own bytes name it, so
// Open() must ask one handler instead of handing the file to every candidate.
//
// The gate is a BYTE COUNT, not a stopwatch (a wall-clock threshold on a shared
// runner measures the machine; a byte count is reproducible and says what the
// code did). Three fixtures make it sharp:
//
//   sparse4g.zip      a genuine 4 GiB zip  -> sniff=zip, the Zip handler reads
//                     the tail + central directory, never the payload.
//   sparse4g.7z       a genuine 4 GiB 7z   -> sniff=7z, the 7z handler reads
//                     the end header only.
//   sparse4g-lie.tar  a genuine 4 GiB 7z WEARING A .tar NAME. The extension
//                     route sends the TAR handler at it, and tar has no index:
//                     it walks every 512-byte block before declining. This is
//                     the cell the negative control turns red -- sniffing the
//                     magic picks 7z and the whole-file walk never happens.
//
// Everything is authored SPARSE (a hole, not 4 GiB of writes), so the whole
// binary costs a few hundred KB of disk and a couple of seconds.
//
// Two more binaries share this source:
//   * PRISM_C28_SNIFF_ONLY  -- the pure sniffer table, no 7z.dll needed;
//   * PRISM_C28_MUTANT_PROBE_ALL -- the pre-C28 defect (sniff thrown away).
//     Its own ctest entry requires the marker "C28 MUTANT CAUGHT", which is
//     printed only when a byte-gate assertion really went red AND the injection
//     counter proves the mutation fired (CONTRACTS D3/D26: the marker is the
//     conjunction, WILL_FAIL is deliberately not used).
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

#define C28_CHECK(cond) ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define C28_CHECK_EQ(a, b) ::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

// The budget the card asks for: identifying + indexing a 4 GiB archive must
// never touch more than 2 MiB of it.
constexpr uint64_t kOpenByteBudget = 2ull << 20;
constexpr uint64_t kFourGiB = 4ull << 30;

// ---------------------------------------------------------------------------
// Section A: the sniffer, as pure logic (no 7z.dll, no files)
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

// A minimal POSIX tar header block: only offset 257 matters to the sniffer.
std::vector<uint8_t> TarBytes(char flavour) {
  std::vector<uint8_t> block(512, 0);
  std::memcpy(block.data() + 257, "ustar", 5);
  block[262] = static_cast<uint8_t>(flavour);  // 0x00 = POSIX, ' ' = GNU
  return block;
}

// A zip tail that really terminates at EOF: 22 fixed bytes + a comment.
std::vector<uint8_t> ZipTailEndingAtEof(std::size_t prefix, std::size_t commentLength,
                                        bool validLength = true) {
  std::vector<uint8_t> bytes(prefix, 0);
  const std::size_t eocd = bytes.size();
  bytes.resize(eocd + 22 + commentLength, 0);
  const uint8_t signature[4] = {0x50, 0x4B, 0x05, 0x06};
  std::memcpy(bytes.data() + eocd, signature, 4);
  // The comment-length field must account for every byte after the record.
  const std::size_t claimed = validLength ? commentLength : commentLength + 1;
  bytes[eocd + 20] = static_cast<uint8_t>(claimed & 0xFF);
  bytes[eocd + 21] = static_cast<uint8_t>((claimed >> 8) & 0xFF);
  return bytes;
}

void RunSnifferChecks() {
  using prism::win::sniff::ArchiveFormat;
  using prism::win::sniff::BrotliWindowBits;
  using prism::win::sniff::Confidence;
  using prism::win::sniff::DescribeRoute;
  using prism::win::sniff::FormatExtension;
  using prism::win::sniff::FormatName;
  using prism::win::sniff::Sniff;

  auto expect = [](const char* label, const prism::win::sniff::Window& window,
                   ArchiveFormat format, Confidence confidence) {
    const auto finding = Sniff(window);
    const bool formatOk = std::strcmp(FormatName(finding.format), FormatName(format)) == 0;
    const bool confidenceOk = finding.confidence == confidence;
    std::printf("    %-28s -> sniff=%-7s (%s)%s\n", label, FormatName(finding.format),
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

  // --- signatures ---------------------------------------------------------
  expect("7z magic", MakeWindow({0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04}),
         ArchiveFormat::kSevenZip, Confidence::kMagic);
  expect("zip local header", MakeWindow({0x50, 0x4B, 0x03, 0x04, 0x14, 0x00}),
         ArchiveFormat::kZip, Confidence::kMagic);
  expect("zip empty archive", MakeWindow({0x50, 0x4B, 0x05, 0x06, 0x00, 0x00}),
         ArchiveFormat::kZip, Confidence::kMagic);
  expect("zip spanned marker", MakeWindow({0x50, 0x4B, 0x07, 0x08, 0x00, 0x00}),
         ArchiveFormat::kZip, Confidence::kMagic);
  expect("rar5", MakeWindow({0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00}),
         ArchiveFormat::kRar5, Confidence::kMagic);
  expect("rar4", MakeWindow({0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00, 0x00}),
         ArchiveFormat::kRar4, Confidence::kMagic);
  expect("tar (POSIX ustar)", MakeWindow(TarBytes(0x00)), ArchiveFormat::kTar,
         Confidence::kMagic);
  expect("tar (GNU ustar)", MakeWindow(TarBytes(' ')), ArchiveFormat::kTar, Confidence::kMagic);
  {
    // The word "ustar" at the right offset is not enough: the flavour byte has
    // to be one tar actually writes, or a text file would route to the tar
    // handler.
    std::vector<uint8_t> fake = TarBytes('X');
    expect("text that spells ustar", MakeWindow(fake), ArchiveFormat::kUnknown,
           Confidence::kNone);
  }
  expect("gzip", MakeWindow({0x1F, 0x8B, 0x08, 0x00}), ArchiveFormat::kGzip,
         Confidence::kMagic);
  expect("xz", MakeWindow({0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00}), ArchiveFormat::kXz,
         Confidence::kMagic);
  expect("bzip2", MakeWindow({0x42, 0x5A, 0x68, '9'}), ArchiveFormat::kBzip2,
         Confidence::kMagic);
  {
    std::vector<uint8_t> invalid = {'B', 'Z', 'h', '0'};
    expect("bzip2 with block size 0", MakeWindow(invalid), ArchiveFormat::kUnknown,
           Confidence::kNone);
  }
  expect("zstd", MakeWindow({0x28, 0xB5, 0x2F, 0xFD}), ArchiveFormat::kZstd,
         Confidence::kMagic);
  expect("lz4 frame", MakeWindow({0x04, 0x22, 0x4D, 0x18}), ArchiveFormat::kLz4,
         Confidence::kMagic);
  expect("lz4 legacy", MakeWindow({0x02, 0x21, 0x4C, 0x18}), ArchiveFormat::kLz4,
         Confidence::kMagic);

  // --- structural evidence is a HINT, never a commitment -------------------
  {
    std::vector<uint8_t> sfx = {0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00};
    const auto tail = ZipTailEndingAtEof(64 * 1024, 12);
    sfx.insert(sfx.end(), tail.begin(), tail.end());
    expect("self-extracting exe (MZ+eocd)", MakeWindow(sfx), ArchiveFormat::kZip,
           Confidence::kHint);
    C28_CHECK(!Sniff(MakeWindow(sfx)).singleHandler());  // must keep probing
  }
  {
    // No PK at offset 0 but a well-formed EOCD at EOF: a zip behind a prefix.
    const auto prefixed = ZipTailEndingAtEof(4096, 0);
    expect("zip behind a prefix", MakeWindow(prefixed), ArchiveFormat::kZip,
           Confidence::kHint);
    C28_CHECK(!Sniff(MakeWindow(prefixed)).singleHandler());
  }
  {
    // The same signature with a comment length that does NOT reach EOF is junk
    // in a payload, not an end-of-central-directory record.
    const auto broken = ZipTailEndingAtEof(4096, 8, /*validLength=*/false);
    expect("eocd whose comment overruns", MakeWindow(broken), ArchiveFormat::kUnknown,
           Confidence::kNone);
  }

  // --- brotli: no signature at all, so the name may only CONFIRM it --------
  {
    std::vector<uint8_t> byte = {0x00};
    expect("brotli byte, no extension", MakeWindow(byte), ArchiveFormat::kUnknown,
           Confidence::kNone);
    expect("brotli byte, .br name", MakeWindow(byte, "br"), ArchiveFormat::kBrotli,
           Confidence::kHint);
    C28_CHECK(!Sniff(MakeWindow(byte, "br")).singleHandler());
  }
  {
    struct WbitsCase { uint8_t byte; int expected; };
    const WbitsCase cases[] = {
        {0x00, 16},   // bit0 = 0                     -> 16
        {0x01, 17},   // 1 then 000 then 000          -> 17
        {0x07, 20},   // 1 then 011                   -> 17 + 3
        {0x0F, 24},   // 1 then 111                   -> 17 + 7
        {0x21, 10},   // 1 then 000 then 010          -> 8 + 2
        {0x10, 16},   // bit0 = 0
    };
    for (const WbitsCase& item : cases) {
      const auto bits = BrotliWindowBits(item.byte);
      const bool ok = bits.has_value() && *bits == static_cast<unsigned>(item.expected);
      if (!ok) {
        std::printf("    FAIL brotli window bits for 0x%02x: expected %d, got %d\n", item.byte,
                    item.expected, bits ? static_cast<int>(*bits) : -1);
        ++g_failures;
      }
      std::printf("    brotli byte 0x%02x -> wbits %d\n", item.byte,
                  bits ? static_cast<int>(*bits) : -1);
    }
    C28_CHECK(!BrotliWindowBits(0x11).has_value());  // large-window marker
  }

  // --- the lie the old probe could not see --------------------------------
  {
    // A 7z named .zip: the magic must win, because the extension is exactly the
    // thing that made the pre-C28 probe read the file.
    std::vector<uint8_t> bytes = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04};
    expect("7z wearing a .zip name", MakeWindow(bytes, "zip"), ArchiveFormat::kSevenZip,
           Confidence::kMagic);
  }
  expect("zeros", MakeWindow(std::vector<uint8_t>(64, 0)), ArchiveFormat::kUnknown,
         Confidence::kNone);
  {
    std::vector<uint8_t> tiny = {0x50, 0x4B};
    expect("two-byte head", MakeWindow(tiny), ArchiveFormat::kUnknown, Confidence::kNone);
  }
  {
    prism::win::sniff::Window empty;
    expect("empty window", empty, ArchiveFormat::kUnknown, Confidence::kNone);
  }

  // --- the diagnostic line Open() prints ----------------------------------
  C28_CHECK_EQ(DescribeRoute([] {
                 auto f = prism::win::sniff::Finding{};
                 f.format = ArchiveFormat::kZip;
                 f.confidence = Confidence::kMagic;
                 return f;
               }()),
               std::string("sniff=zip route=sniffed"));
  C28_CHECK_EQ(DescribeRoute([] {
                 auto f = prism::win::sniff::Finding{};
                 f.format = ArchiveFormat::kZip;
                 f.confidence = Confidence::kHint;
                 return f;
               }()),
               std::string("sniff=zip(hint) route=probe"));
  C28_CHECK_EQ(DescribeRoute(prism::win::sniff::Finding{}),
               std::string("sniff=unknown route=probe"));

  // --- the mapping the router uses ---------------------------------------
  C28_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kSevenZip)), std::string("7z"));
  C28_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kZip)), std::string("zip"));
  C28_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kRar5)), std::string("rar"));
  C28_CHECK_EQ(std::string(FormatExtension(ArchiveFormat::kGzip)), std::string("gz"));
  C28_CHECK(FormatExtension(ArchiveFormat::kUnknown) == nullptr);
}

// ---------------------------------------------------------------------------
// Fixtures: real archives from the 7-Zip CLI, then the same containers grown to
// 4 GiB with a HOLE (sparse), not with 4 GiB of writes
// ---------------------------------------------------------------------------
std::filesystem::path WorkRoot() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_OPEN_PERF_ROOT", overridePath, MAX_PATH) > 0) {
    return std::filesystem::path(overridePath);
  }
  // temp_directory_path() may carry a trailing separator, in which case
  // filename() is empty and parent_path() is the temp dir itself.
  std::filesystem::path temp = std::filesystem::temp_directory_path();
  if (temp.filename().empty()) temp = temp.parent_path();
  return temp / L"prism_open_perf";
}

uint32_t Le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t Le64(const uint8_t* p) {
  return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

void PutLe32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}

void PutLe64(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}

// IEEE CRC-32: the 7z start-header check field is this over the 20 bytes that
// follow it (7zFormat.txt).
uint32_t Crc32(const uint8_t* data, std::size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

bool ReadAllBytes(const std::filesystem::path& path, std::vector<uint8_t>* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0;
  if (ok) {
    out->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) &&
         read == out->size();
  }
  CloseHandle(file);
  return ok;
}

// A file whose 4 GiB are a HOLE: FSCTL_SET_SPARSE first, so the gap costs no
// disk and reads back as zeros at memory speed.
HANDLE CreateSparseFile(const std::filesystem::path& path, uint64_t size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
  DWORD returned = 0;
  // Best effort: on a volume without sparse support the hole becomes real
  // zeros, which still works -- it just costs the space.
  DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
  LARGE_INTEGER end{};
  end.QuadPart = static_cast<LONGLONG>(size);
  if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
    CloseHandle(file);
    return INVALID_HANDLE_VALUE;
  }
  return file;
}

bool WriteAt(HANDLE file, uint64_t offset, const uint8_t* data, std::size_t size) {
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
  DWORD written = 0;
  return WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
}

bool RunSevenZipIn(const std::wstring& exe, const std::wstring& args,
                   const std::filesystem::path& cwd) {
  std::wstring command = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

// Packs the tiny payload into work/name with the given -t type. Returns false
// when this 7z.exe cannot write that type (vanilla builds write no zstd).
bool PackSmall(const std::wstring& exe, const std::filesystem::path& work, const wchar_t* type,
               const wchar_t* name) {
  const std::filesystem::path payload = work / L"payload.txt";
  if (!std::filesystem::exists(payload)) {
    HANDLE file = CreateFileW(payload.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char text[] = "prism archive payload\n";
    DWORD written = 0;
    WriteFile(file, text, sizeof(text) - 1, &written, nullptr);
    CloseHandle(file);
  }
  const std::filesystem::path target = work / name;
  if (std::filesystem::exists(target)) {
    std::error_code ec;
    std::filesystem::remove(target, ec);
  }
  const std::wstring args = std::wstring(L"a -t") + type + L" \"" + target.wstring() + L"\" \"" +
                            payload.filename().wstring() + L"\"";
  return RunSevenZipIn(exe, args, work);
}

// A genuine zip grown to totalSize by a hole between the last local entry and
// the central directory. Only the EOCD's central-directory offset has to move;
// every local-header offset stays valid, which is a legal zip layout.
// NOTE: the parameter is NOT called "small" -- windows.h pulls rpcndr.h, which
// still defines the legacy macro  #define small char.  (Cost of a Windows TU.)
bool BuildSparseZip(const std::filesystem::path& source,
                    const std::filesystem::path& target, uint64_t totalSize,
                    std::string* error) {
  std::vector<uint8_t> bytes;
  if (!ReadAllBytes(source, &bytes)) {
    *error = "cannot read the small zip";
    return false;
  }
  std::size_t eocd = std::string::npos;
  for (std::size_t i = bytes.size() - 22 + 1; i-- > 0;) {
    if (std::memcmp(bytes.data() + i, "PK\x05\x06", 4) == 0) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) {
    *error = "the small zip has no EOCD";
    return false;
  }
  const uint32_t cdSize = Le32(bytes.data() + eocd + 12);
  const uint32_t cdOffset = Le32(bytes.data() + eocd + 16);
  if (cdOffset == 0xFFFFFFFFu || static_cast<uint64_t>(cdOffset) + cdSize > bytes.size()) {
    *error = "the small zip is zip64 or not sane";
    return false;
  }
  const uint64_t newCdOffset = totalSize - 22 - cdSize;
  HANDLE file = CreateSparseFile(target, totalSize);
  if (file == INVALID_HANDLE_VALUE) {
    *error = "cannot create the sparse file";
    return false;
  }
  bool ok = WriteAt(file, 0, bytes.data(), cdOffset) &&
            WriteAt(file, newCdOffset, bytes.data() + cdOffset, cdSize);
  if (ok) {
    std::vector<uint8_t> tail(bytes.begin() + static_cast<std::ptrdiff_t>(eocd),
                              bytes.begin() + static_cast<std::ptrdiff_t>(eocd + 22));
    PutLe32(tail.data() + 16, static_cast<uint32_t>(newCdOffset));
    ok = WriteAt(file, totalSize - 22, tail.data(), tail.size());
  }
  CloseHandle(file);
  if (!ok) *error = "cannot write the sparse zip";
  return ok;
}

// A genuine 7z grown to totalSize by moving only its END header to EOF: the
// start header's nextHeaderOffset is repointed and its CRC recomputed, the
// packed stream stays where it was. 7z reads the start header (32 B) and the
// end header, so this is a container of 4 GiB that costs ~300 bytes to open.
bool BuildSparse7z(const std::filesystem::path& source,
                   const std::filesystem::path& target, uint64_t totalSize,
                   std::string* error) {
  std::vector<uint8_t> bytes;
  if (!ReadAllBytes(source, &bytes)) {
    *error = "cannot read the small 7z";
    return false;
  }
  const uint8_t magic[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
  if (bytes.size() < 32 || std::memcmp(bytes.data(), magic, 6) != 0) {
    *error = "the small 7z has no signature header";
    return false;
  }
  const uint64_t headerOffset = Le64(bytes.data() + 12);
  const uint64_t headerSize = Le64(bytes.data() + 20);
  if (32 + headerOffset + headerSize != bytes.size()) {
    *error = "the small 7z layout is not start-header/pack/header";
    return false;
  }
  const uint64_t newHeaderOffset = totalSize - 32 - headerSize;
  HANDLE file = CreateSparseFile(target, totalSize);
  if (file == INVALID_HANDLE_VALUE) {
    *error = "cannot create the sparse file";
    return false;
  }
  bool ok = WriteAt(file, 0, bytes.data(), static_cast<std::size_t>(32 + headerOffset));
  if (ok) {
    std::vector<uint8_t> start(bytes.begin(), bytes.begin() + 32);
    PutLe64(start.data() + 12, newHeaderOffset);
    // 7z signature header layout: 0 signature, 6 version, 8 StartHeaderCRC,
    // 12 NextHeaderOffset, 20 NextHeaderSize, 28 NextHeaderCRC. The start CRC
    // covers bytes 12..31 and lives at offset 8 -- writing it at 28 (which is
    // the untouched NextHeaderCRC) makes 7z reject the file and SCAN the whole
    // archive looking for a signature, which is the 4 GiB read this card is
    // about. Measured the hard way.
    PutLe32(start.data() + 8, Crc32(start.data() + 12, 20));
    ok = WriteAt(file, 0, start.data(), start.size()) &&
         WriteAt(file, 32 + newHeaderOffset, bytes.data() + 32 + headerOffset,
                 static_cast<std::size_t>(headerSize));
  }
  CloseHandle(file);
  if (!ok) *error = "cannot write the sparse 7z";
  return ok;
}

// ---------------------------------------------------------------------------
// Measuring one Open
// ---------------------------------------------------------------------------
struct OpenMeasurement {
  bool opened = false;
  uint64_t bytes = 0;
  uint64_t sniffBytes = 0;
  uint64_t reads = 0;
  std::string trace;
  size_t items = 0;
};

OpenMeasurement Measure(prism::win::SevenZipLibrary& library, const std::filesystem::path& file) {
  OpenMeasurement measured;
  prism::win::ResetIoCounters();
  prism::win::ArchiveReader reader(library);
  measured.opened = reader.Open(file.wstring());
  // Snapshot BEFORE ReadCatalog: the cell is about Open, not about the catalog.
  measured.bytes = prism::win::IoCounters().bytesRead.load();
  measured.sniffBytes = prism::win::IoCounters().sniffBytes.load();
  measured.reads = prism::win::IoCounters().reads.load();
  measured.trace = reader.openTrace();
  if (measured.opened) measured.items = reader.ReadCatalog().size();
  reader.Close();
  return measured;
}

std::string Narrow(const wchar_t* text) {
  if (!text || !*text) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

void PrintMeasurement(const std::string& label, const OpenMeasurement& measured) {
  std::printf("    %-22s bytes=%-10llu sniff=%-6llu reads=%-5llu items=%zu\n", label.c_str(),
              static_cast<unsigned long long>(measured.bytes),
              static_cast<unsigned long long>(measured.sniffBytes),
              static_cast<unsigned long long>(measured.reads), measured.items);
  std::printf("      trace: %s\n", measured.trace.c_str());
}

// The byte gate. Only THIS assertion feeds the negative control's verdict: the
// mutant claims that removing the sniff turns a 4 GiB archive into a
// whole-file walk, and that is what has to go red.
void CheckOpenBytes(const char* label, const OpenMeasurement& measured, const char* sniffed) {
  if (!C28_CHECK(measured.opened)) return;
  const std::string wanted = std::string("sniff=") + sniffed + " route=sniffed";
  if (measured.trace.find(wanted) == std::string::npos) {
    std::printf("    FAIL %s: routing did not sniff (%s)\n", label, wanted.c_str());
    ++g_failures;
  }
  if (measured.bytes > kOpenByteBudget) {
    std::printf("    FAIL %s: Open read %llu bytes of a 4 GiB archive (budget %llu)\n", label,
                static_cast<unsigned long long>(measured.bytes),
                static_cast<unsigned long long>(kOpenByteBudget));
    ++g_failures;
    ++g_byteGateFailures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  // Unbuffered: a run that dies mid-fixture must still show how far it got.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("=== open-perf ===");
#ifdef PRISM_C28_MUTANT_PROBE_ALL
  std::printf(" [MUTANT: sniff disabled]");
#endif
#ifdef PRISM_C28_SNIFF_ONLY
  std::printf(" [sniffer only]");
#endif
  std::printf("\n");

  std::printf("  -- sniffer table --\n");
  RunSnifferChecks();

#ifdef PRISM_C28_SNIFF_ONLY
  if (g_failures == 0) {
    std::printf("ALL FORMAT-SNIFFER CHECKS PASSED\n");
    return 0;
  }
  std::printf("FORMAT-SNIFFER CHECKS FAILED (%d)\n", g_failures);
  return 1;
#else
  const std::wstring dllPath = prism::fixture::DllPath();
  const std::wstring exePath = prism::fixture::ExePath();
  if (dllPath.empty() || exePath.empty()) {
    // CONTRACTS D1/D21: a visible Skip, never a fake green.
    std::printf("SKIP: 7z.dll/7z.exe not found (set PRISM_7Z_DLL)\n");
    return 77;
  }
  prism::win::SevenZipLibrary library;
  if (!C28_CHECK(library.Load(dllPath))) {
    std::printf("OPEN-PERF CHECKS FAILED: cannot load 7z.dll\n");
    return 1;
  }

  std::error_code ec;
  const std::filesystem::path work = WorkRoot();
  std::filesystem::create_directories(work, ec);

  // ------------------------------------------------------------------ B. routing
  // Small real archives: the sniffed handler is the FIRST candidate, opens the
  // file, and the catalog is unchanged (no regression against C26).
  std::printf("  -- routing (small fixtures) --\n");
  struct RouteCase {
    const wchar_t* type;
    const wchar_t* name;
    const char* sniffed;
    const char* engineExtension;
  };
  const RouteCase routeCases[] = {
      {L"7z", L"small.7z", "7z", "7z"},
      {L"zip", L"small.zip", "zip", "zip"},
      {L"tar", L"small.tar", "tar", "tar"},
      {L"zst", L"small.zst", "zstd", "zst"},
  };
  for (const RouteCase& item : routeCases) {
    if (!PackSmall(exePath, work, item.type, item.name)) {
      std::printf("    note: this 7z.exe cannot write -t%ls -- routing check skipped\n",
                  item.type);
      continue;
    }
    // A format with no handler in THIS engine cannot be opened by anyone; the
    // sniffer's verdict is asserted by the sniff-only binary regardless.
    if (library.ClsidsForExtension(item.engineExtension).empty()) {
      std::printf("    note: this 7z.dll has no '%s' handler -- routing check skipped\n",
                  item.engineExtension);
      continue;
    }
    const OpenMeasurement measured = Measure(library, work / item.name);
    PrintMeasurement(Narrow(item.name), measured);
    if (!C28_CHECK(measured.opened)) continue;
    const std::string wanted = std::string("sniff=") + item.sniffed + " route=sniffed";
    if (measured.trace.find(wanted) == std::string::npos) {
      std::printf("    FAIL %ls: expected '%s' in the trace\n", item.name, wanted.c_str());
      ++g_failures;
    }
    if (!C28_CHECK_EQ(measured.items, static_cast<size_t>(1))) {
      std::printf("    FAIL %ls: the catalog lost its item\n", item.name);
    }
    if (!C28_CHECK(measured.bytes <= kOpenByteBudget)) {
      std::printf("    FAIL %ls: a small archive must not cost more than the budget\n",
                  item.name);
      ++g_failures;
      ++g_byteGateFailures;
    }
  }

  // ------------------------------------------------------------------ C. 4 GiB
  std::printf("  -- 4 GiB byte gate --\n");
  const std::filesystem::path smallZip = work / L"small.zip";
  const std::filesystem::path small7z = work / L"small.7z";
  if (!std::filesystem::exists(smallZip)) PackSmall(exePath, work, L"zip", L"small.zip");
  if (!std::filesystem::exists(small7z)) PackSmall(exePath, work, L"7z", L"small.7z");

  const std::filesystem::path bigZip = work / L"sparse4g.zip";
  const std::filesystem::path big7z = work / L"sparse4g.7z";
  // The sharp one: a genuine 7z wearing a .tar name. The extension route sends
  // the TAR handler at it, and tar has no index -- it walks the file.
  const std::filesystem::path lyingTar = work / L"sparse4g-lie.tar";
  std::string error;

  // Rebuilt on EVERY run, deliberately: authoring a sparse 4 GiB container is
  // milliseconds, while a cached fixture that no longer matches the builder
  // turns a real assertion failure into a fixture failure (bench_scale learned
  // this the hard way and validates its cache before reuse).
  if (!BuildSparseZip(smallZip, bigZip, kFourGiB, &error)) {
    std::printf("SKIP: %s\n", error.c_str());
    return 77;
  }
  if (!BuildSparse7z(small7z, big7z, kFourGiB, &error)) {
    std::printf("SKIP: %s\n", error.c_str());
    return 77;
  }
  if (!BuildSparse7z(small7z, lyingTar, kFourGiB, &error)) {
    std::printf("SKIP: %s\n", error.c_str());
    return 77;
  }

  struct BigCase {
    const char* label;
    std::filesystem::path file;
    const char* sniffed;
  };
  const BigCase bigCases[] = {
      {"sparse4g.zip", bigZip, "zip"},
      {"sparse4g.7z", big7z, "7z"},
      {"sparse4g-lie.tar", lyingTar, "7z"},
  };
  for (const BigCase& item : bigCases) {
    const OpenMeasurement measured = Measure(library, item.file);
    PrintMeasurement(item.label, measured);
    CheckOpenBytes(item.label, measured, item.sniffed);
    if (measured.opened && measured.items != 1) {
      std::printf("    FAIL %s: expected 1 item, got %zu\n", item.label, measured.items);
      ++g_failures;
    }
  }

  const uint64_t injections = prism::win::IoCounters().mutantProbeAll.load();
#ifdef PRISM_C28_MUTANT_PROBE_ALL
  // CONTRACTS D3: the marker demands BOTH a red byte gate AND proof the
  // injection fired. CONTRACTS D26: no WILL_FAIL -- this marker IS the verdict.
  if (g_byteGateFailures > 0 && injections > 0) {
    std::printf(
        "C28 MUTANT CAUGHT: sniff disabled -> %d byte-gate assertion(s) red, injections=%llu\n",
        g_byteGateFailures, static_cast<unsigned long long>(injections));
    return 0;
  }
  std::printf(
      "C28 MUTANT NOT CAUGHT: the byte gate stayed green (failures=%d, injections=%llu)\n",
      g_byteGateFailures, static_cast<unsigned long long>(injections));
  return 1;
#else
  std::printf("  sniff injections: %llu (0 = the sniff was armed)\n",
              static_cast<unsigned long long>(injections));
  if (g_failures == 0) {
    std::printf("ALL OPEN-PERF CHECKS PASSED\n");
    return 0;
  }
  std::printf("OPEN-PERF CHECKS FAILED (%d)\n", g_failures);
  return 1;
#endif  // PRISM_C28_MUTANT_PROBE_ALL
#endif  // PRISM_C28_SNIFF_ONLY
}
