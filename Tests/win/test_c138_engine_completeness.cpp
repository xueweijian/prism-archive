// PrismArchive -- Card C138 integration test:
//   1. All-family multi-volume via ArchiveReader::Open + VolumeOpenCallback:
//      SplitZip (.z01/.zip), RarNew (.part1.rar/.part2.rar), RarOld (.rar/.r00/.r01),
//      plus clear missing-volume diagnostics.
//   2. Encrypted ExtractToMemory (ICryptoGetTextPassword on MemoryExtractCallback),
//      eliminating D30 silent 0-byte extraction on encrypted archives.
//   3. CharsetRepair ambiguity preservation on ArchiveItem/ArchiveCatalog +
//      ReopenWithCodePage on ArchiveReader and ArchiveSession.
//   4. Negative controls (D3/D26/D27):
//      PRISM_C138_MUTANT_NO_VOLUME_CALLBACK
//      PRISM_C138_MUTANT_NO_MEM_CRYPTO
//      PRISM_C138_MUTANT_NO_AMBIGUITY
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "../../Src/PrismWin/Win32Transcoder.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
namespace win = prism::win;

namespace {

#if defined(PRISM_C138_MUTANT_NO_VOLUME_CALLBACK)
// MUTANT ACTIVE (CONTRACTS D3/D27): VolumeOpenCallback IID disabled in OpenCallback.
constexpr bool kMutantNoVolumeCb = true;
#else
constexpr bool kMutantNoVolumeCb = false;
#endif

#if defined(PRISM_C138_MUTANT_NO_MEM_CRYPTO)
// MUTANT ACTIVE (CONTRACTS D3/D27): ICryptoGetTextPassword + opResult disabled in ExtractToMemory.
constexpr bool kMutantNoMemCrypto = true;
#else
constexpr bool kMutantNoMemCrypto = false;
#endif

#if defined(PRISM_C138_MUTANT_NO_AMBIGUITY)
// MUTANT ACTIVE (CONTRACTS D3/D27): CharsetRepair ambiguity/alternatives dropped in ReadCatalog.
constexpr bool kMutantNoAmbiguity = true;
#else
constexpr bool kMutantNoAmbiguity = false;
#endif

constexpr bool kAnyMutant = kMutantNoVolumeCb || kMutantNoMemCrypto || kMutantNoAmbiguity;

std::wstring gDllPath;
std::wstring gExePath;

void Put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void Put16At(std::vector<uint8_t>& out, size_t at, uint16_t v) {
  out[at] = static_cast<uint8_t>(v & 0xFF);
  out[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void Put32At(std::vector<uint8_t>& out, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

uint32_t Crc32Bytes(const std::string& data) {
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

bool WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

bool Run7z(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &si, &pi)) {
    return false;
  }
  const DWORD waited = WaitForSingleObject(pi.hProcess, 60'000);
  DWORD code = 1;
  if (waited == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return waited == WAIT_OBJECT_0 && code == 0;
}

// Legacy non-UTF-8 (bit 11 = 0) ZIP builder.
std::vector<uint8_t> BuildLegacyZip(const std::string& rawName, const std::string& payload) {
  std::vector<uint8_t> out;
  Put32(out, 0x04034b50);
  Put16(out, 20);
  Put16(out, 0);  // NO UTF-8 flag (bit 11 = 0)
  Put16(out, 0);  // stored
  Put16(out, 0);
  Put16(out, 0x21);
  Put32(out, Crc32Bytes(payload));
  Put32(out, static_cast<uint32_t>(payload.size()));
  Put32(out, static_cast<uint32_t>(payload.size()));
  Put16(out, static_cast<uint16_t>(rawName.size()));
  Put16(out, 0);
  for (char c : rawName) out.push_back(static_cast<uint8_t>(c));
  for (char c : payload) out.push_back(static_cast<uint8_t>(c));

  const uint32_t cdOffset = static_cast<uint32_t>(out.size());
  Put32(out, 0x02014b50);
  Put16(out, 20);
  Put16(out, 20);
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, 0x21);
  Put32(out, Crc32Bytes(payload));
  Put32(out, static_cast<uint32_t>(payload.size()));
  Put32(out, static_cast<uint32_t>(payload.size()));
  Put16(out, static_cast<uint16_t>(rawName.size()));
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, 0);
  Put32(out, 0);
  Put32(out, 0);
  for (char c : rawName) out.push_back(static_cast<uint8_t>(c));
  const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdOffset;

  Put32(out, 0x06054b50);
  Put16(out, 0);
  Put16(out, 0);
  Put16(out, 1);
  Put16(out, 1);
  Put32(out, cdSize);
  Put32(out, cdOffset);
  Put16(out, 0);
  return out;
}

// Generates a real 3-volume SplitZip (.z01, .z02, .zip) where both files cross
// volume boundaries so 7z.dll MUST call IArchiveOpenVolumeCallback::GetStream.
void WriteSplitZipSet(const fs::path& dir, const std::string& stem,
                      const std::string& fileA, const std::string& dataA,
                      const std::string& fileB, const std::string& dataB) {
  std::vector<uint8_t> z01;
  Put32(z01, 0x08074b50);  // split archive signature
  const uint32_t offA = static_cast<uint32_t>(z01.size());
  Put32(z01, 0x04034b50);
  Put16(z01, 20);
  Put16(z01, 0x0800);
  Put16(z01, 0);
  Put16(z01, 0);
  Put16(z01, 0x21);
  Put32(z01, Crc32Bytes(dataA));
  Put32(z01, static_cast<uint32_t>(dataA.size()));
  Put32(z01, static_cast<uint32_t>(dataA.size()));
  Put16(z01, static_cast<uint16_t>(fileA.size()));
  Put16(z01, 0);
  for (char c : fileA) z01.push_back(static_cast<uint8_t>(c));
  const size_t splitA = dataA.size() / 2;
  for (size_t i = 0; i < splitA; ++i) z01.push_back(static_cast<uint8_t>(dataA[i]));

  std::vector<uint8_t> z02;
  for (size_t i = splitA; i < dataA.size(); ++i) z02.push_back(static_cast<uint8_t>(dataA[i]));
  const uint32_t offB = static_cast<uint32_t>(z02.size());
  Put32(z02, 0x04034b50);
  Put16(z02, 20);
  Put16(z02, 0x0800);
  Put16(z02, 0);
  Put16(z02, 0);
  Put16(z02, 0x21);
  Put32(z02, Crc32Bytes(dataB));
  Put32(z02, static_cast<uint32_t>(dataB.size()));
  Put32(z02, static_cast<uint32_t>(dataB.size()));
  Put16(z02, static_cast<uint16_t>(fileB.size()));
  Put16(z02, 0);
  for (char c : fileB) z02.push_back(static_cast<uint8_t>(c));
  const size_t splitB = dataB.size() / 2;
  for (size_t i = 0; i < splitB; ++i) z02.push_back(static_cast<uint8_t>(dataB[i]));

  std::vector<uint8_t> zip;
  for (size_t i = splitB; i < dataB.size(); ++i) zip.push_back(static_cast<uint8_t>(dataB[i]));
  const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
  auto appendCd = [&](const std::string& name, const std::string& data,
                      uint16_t diskStart, uint32_t localOff) {
    Put32(zip, 0x02014b50);
    Put16(zip, 20);
    Put16(zip, 20);
    Put16(zip, 0x0800);
    Put16(zip, 0);
    Put16(zip, 0);
    Put16(zip, 0x21);
    Put32(zip, Crc32Bytes(data));
    Put32(zip, static_cast<uint32_t>(data.size()));
    Put32(zip, static_cast<uint32_t>(data.size()));
    Put16(zip, static_cast<uint16_t>(name.size()));
    Put16(zip, 0);
    Put16(zip, 0);
    Put16(zip, diskStart);
    Put16(zip, 0);
    Put32(zip, 0);
    Put32(zip, localOff);
    for (char c : name) zip.push_back(static_cast<uint8_t>(c));
  };
  appendCd(fileA, dataA, 0, offA);
  appendCd(fileB, dataB, 1, offB);
  const uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;

  Put32(zip, 0x06054b50);
  Put16(zip, 2);  // number of this disk (0-based: z01=0, z02=1, zip=2)
  Put16(zip, 2);  // disk where CD starts
  Put16(zip, 2);  // entries on this disk
  Put16(zip, 2);  // total entries
  Put32(zip, cdSize);
  Put32(zip, cdOffset);
  Put16(zip, 0);

  WriteBytes(dir / (stem + ".z01"), z01);
  WriteBytes(dir / (stem + ".z02"), z02);
  WriteBytes(dir / (stem + ".zip"), zip);
}

// Generates a real stored RAR4 multi-volume set (either .part1.rar/.part2.rar
// with MHD_NEWNUMBERING or .rar/.r00/.r01) splitting a single file across the
// volumes. Per RAR4 format, intermediate split chunks carry that chunk's CRC.
std::vector<uint8_t> BuildRar4Volume(const std::string& name, const std::string& fullData,
                                     size_t sliceOffset, size_t sliceLen,
                                     bool isFirstVol, bool splitBefore, bool splitAfter,
                                     bool newNumbering) {
  std::vector<uint8_t> out = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};

  auto finalizeHeaderCrc = [&](size_t hdrStart) {
    const std::string slice(reinterpret_cast<const char*>(out.data() + hdrStart + 2),
                            out.size() - (hdrStart + 2));
    const uint16_t crc = static_cast<uint16_t>(Crc32Bytes(slice) & 0xFFFFu);
    Put16At(out, hdrStart, crc);
  };

  // Main header (0x73)
  const size_t mainStart = out.size();
  Put16(out, 0);
  out.push_back(0x73);
  uint16_t mainFlags = 0x0001u;  // MHD_VOLUME
  if (newNumbering) mainFlags |= 0x0010u;  // MHD_NEWNUMBERING
  if (isFirstVol) mainFlags |= 0x0100u;    // MHD_FIRSTVOLUME
  Put16(out, mainFlags);
  Put16(out, 13);
  Put16(out, 0);
  Put32(out, 0);
  finalizeHeaderCrc(mainStart);

  // File header (0x74)
  const size_t fileStart = out.size();
  Put16(out, 0);
  out.push_back(0x74);
  uint16_t fileFlags = 0x8000u;  // LHD_LONG_BLOCK
  if (splitBefore) fileFlags |= 0x0001u;
  if (splitAfter) fileFlags |= 0x0002u;
  Put16(out, fileFlags);
  const uint16_t fileHdrSize = static_cast<uint16_t>(32 + name.size());
  Put16(out, fileHdrSize);
  Put32(out, static_cast<uint32_t>(sliceLen));
  Put32(out, static_cast<uint32_t>(fullData.size()));
  out.push_back(2);  // HostOS: Win32
  const uint32_t chunkCrc = splitAfter
                                ? Crc32Bytes(fullData.substr(sliceOffset, sliceLen))
                                : Crc32Bytes(fullData);
  Put32(out, chunkCrc);
  Put32(out, 0x50000000u);  // DOS time
  out.push_back(29);        // UNP_VER
  out.push_back(0x30);      // METHOD: Store
  Put16(out, static_cast<uint16_t>(name.size()));
  Put32(out, 0x20u);        // FILE_ATTR
  for (char c : name) out.push_back(static_cast<uint8_t>(c));
  finalizeHeaderCrc(fileStart);

  for (size_t i = 0; i < sliceLen; ++i) {
    out.push_back(static_cast<uint8_t>(fullData[sliceOffset + i]));
  }

  // End-of-archive header (0x7B)
  const size_t endStart = out.size();
  Put16(out, 0);
  out.push_back(0x7B);
  Put16(out, splitAfter ? 0x0001u : 0x0000u);  // EARC_NEXT_VOLUME
  Put16(out, 7);
  finalizeHeaderCrc(endStart);
  return out;
}

PRISM_TEST("c138.split_zip_and_missing_volume_diagnostics") {
  prism::testtmp::UniqueDir scratch(L"c138", L"szip");
  if (!PRISM_CHECK(scratch.ok())) return;
  const fs::path dir = scratch.path();

  const std::string dataA = std::string(128, 'A') + "-alpha-tail";
  const std::string dataB = std::string(128, 'B') + "-beta-tail";
  WriteSplitZipSet(dir, "bundle", "alpha.txt", dataA, "beta.txt", dataB);

  win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(gDllPath))) return;

  // Open via .z01 AND via .zip; both must resolve the set via VolumeOpenCallback.
  for (const std::wstring entry : {L"bundle.z01", L"bundle.zip"}) {
    win::ArchiveReader reader(library);
    const bool opened = reader.Open((dir / entry).wstring());
    if (!PRISM_CHECK(opened)) {
      std::cout << "    open failed trace=" << reader.openTrace()
                << " reason=" << reader.failureReason() << std::endl;
      return;
    }
    const auto catalog = reader.ReadCatalog();
    PRISM_CHECK_EQ(catalog.size(), static_cast<size_t>(2));
    const auto bytesA = reader.ExtractToMemory(0);
    const auto bytesB = reader.ExtractToMemory(1);
    if (PRISM_CHECK(bytesA.has_value())) {
      PRISM_CHECK_EQ(std::string(bytesA->begin(), bytesA->end()), dataA);
    }
    if (PRISM_CHECK(bytesB.has_value())) {
      PRISM_CHECK_EQ(std::string(bytesB->begin(), bytesB->end()), dataB);
    }
    reader.Close();
  }

  // Missing middle volume diagnostic (.z02 removed).
  PRISM_CHECK(fs::remove(dir / "bundle.z02"));
  {
    win::ArchiveReader missingMid(library);
    PRISM_CHECK(!missingMid.Open((dir / "bundle.zip").wstring()));
    PRISM_CHECK(missingMid.failureReason().find("bundle.z02") != std::string::npos);
  }

  // Missing final volume diagnostic (.zip removed).
  PRISM_CHECK(fs::remove(dir / "bundle.zip"));
  {
    win::ArchiveReader missingFinal(library);
    PRISM_CHECK(!missingFinal.Open((dir / "bundle.z01").wstring()));
    PRISM_CHECK(missingFinal.failureReason().find("bundle.zip") != std::string::npos);
  }
}

PRISM_TEST("c138.rar_new_and_rar_old_volumes_and_missing_diagnostics") {
  prism::testtmp::UniqueDir scratch(L"c138", L"rar");
  if (!PRISM_CHECK(scratch.ok())) return;
  const fs::path dirNew = scratch.path() / "new";
  const fs::path dirOld = scratch.path() / "old";
  std::error_code ec;
  fs::create_directories(dirNew, ec);
  fs::create_directories(dirOld, ec);

  const std::string payload = std::string(150, 'R') + "-rar-multi-volume-payload";
  const size_t half = payload.size() / 2;
  WriteBytes(dirNew / "clip.part1.rar",
             BuildRar4Volume("movie.bin", payload, 0, half, true, false, true, true));
  WriteBytes(dirNew / "clip.part2.rar",
             BuildRar4Volume("movie.bin", payload, half, payload.size() - half, false, true, false, true));

  const size_t third = payload.size() / 3;
  WriteBytes(dirOld / "classic.rar",
             BuildRar4Volume("old.bin", payload, 0, third, true, false, true, false));
  WriteBytes(dirOld / "classic.r00",
             BuildRar4Volume("old.bin", payload, third, third, false, true, true, false));
  WriteBytes(dirOld / "classic.r01",
             BuildRar4Volume("old.bin", payload, third * 2, payload.size() - third * 2, false, true, false, false));

  win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(gDllPath))) return;

  // RarNew: open from .part1.rar AND .part2.rar.
  for (const std::wstring entry : {L"clip.part1.rar", L"clip.part2.rar"}) {
    win::ArchiveReader reader(library);
    if (!PRISM_CHECK(reader.Open((dirNew / entry).wstring()))) {
      std::cout << "    RarNew open failed trace=" << reader.openTrace()
                << " reason=" << reader.failureReason() << std::endl;
      return;
    }
    const auto catalog = reader.ReadCatalog();
    PRISM_CHECK_EQ(catalog.size(), static_cast<size_t>(1));
    const auto extracted = reader.ExtractToMemory(0);
    if (PRISM_CHECK(extracted.has_value())) {
      PRISM_CHECK_EQ(std::string(extracted->begin(), extracted->end()), payload);
    }
    reader.Close();
  }

  // Missing RarNew tail volume: removing part2 makes part1 refuse cleanly by name.
  PRISM_CHECK(fs::remove(dirNew / "clip.part2.rar"));
  {
    win::ArchiveReader missingTail(library);
    const bool ok = missingTail.Open((dirNew / "clip.part1.rar").wstring());
    if (!ok) {
      PRISM_CHECK(missingTail.failureReason().find("clip.part2.rar") != std::string::npos);
    } else {
      // If Open reads part1 header only, ExtractToMemory must fail and not emit partial bytes.
      const auto bytes = missingTail.ExtractToMemory(0);
      PRISM_CHECK(!bytes.has_value() || bytes->size() != payload.size());
    }
  }

  // RarOld: open from classic.rar AND classic.r00.
  for (const std::wstring entry : {L"classic.rar", L"classic.r00"}) {
    win::ArchiveReader reader(library);
    if (!PRISM_CHECK(reader.Open((dirOld / entry).wstring()))) {
      std::cout << "    RarOld open failed trace=" << reader.openTrace()
                << " reason=" << reader.failureReason() << std::endl;
      return;
    }
    const auto catalog = reader.ReadCatalog();
    PRISM_CHECK_EQ(catalog.size(), static_cast<size_t>(1));
    const auto extracted = reader.ExtractToMemory(0);
    if (PRISM_CHECK(extracted.has_value())) {
      PRISM_CHECK_EQ(std::string(extracted->begin(), extracted->end()), payload);
    }
    reader.Close();
  }

  // Missing RarOld middle volume (.r00 removed).
  PRISM_CHECK(fs::remove(dirOld / "classic.r00"));
  {
    win::ArchiveReader missingR00(library);
    PRISM_CHECK(!missingR00.Open((dirOld / "classic.rar").wstring()));
    PRISM_CHECK(!missingR00.failureReason().empty());
  }
}

PRISM_TEST("c138.encrypted_extract_to_memory_implements_crypto_and_rejects_missing_password") {
  prism::testtmp::UniqueDir scratch(L"c138", L"memcrypto");
  if (!PRISM_CHECK(scratch.ok())) return;
  const fs::path root = scratch.path();
  const fs::path plain = root / "secret.txt";
  const std::string expected = "PrismArchive-C138-Encrypted-Memory-Payload-12345";
  {
    std::ofstream out(plain, std::ios::binary);
    out.write(expected.data(), static_cast<std::streamsize>(expected.size()));
  }
  const fs::path dataEnc = root / "data_enc.7z";
  const fs::path hdrEnc = root / "hdr_enc.7z";
  const std::wstring pass = L"PrismC138Secret";
  if (!PRISM_CHECK(Run7z(gExePath, L"a -t7z -p" + pass + L" -mhe=off \"" + dataEnc.wstring() + L"\" secret.txt",
                         root.wstring()))) {
    return;
  }
  if (!PRISM_CHECK(Run7z(gExePath, L"a -t7z -p" + pass + L" -mhe=on \"" + hdrEnc.wstring() + L"\" secret.txt",
                         root.wstring()))) {
    return;
  }

  win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(gDllPath))) return;

  // 1. Data-encrypted opened WITHOUT password: ExtractToMemory MUST return nullopt
  //    (never a silent 0-byte vector, eliminating D30).
  {
    win::ArchiveReader noPass(library);
    if (!PRISM_CHECK(noPass.Open(dataEnc.wstring()))) return;
    const auto emptyRead = noPass.ExtractToMemory(0);
    PRISM_CHECK(!emptyRead.has_value());

    // Passing passwordOverride succeeds!
    const auto withOverride = noPass.ExtractToMemory(0, 16ull << 20, pass);
    if (PRISM_CHECK(withOverride.has_value())) {
      PRISM_CHECK_EQ(std::string(withOverride->begin(), withOverride->end()), expected);
    }
    noPass.Close();
  }

  // 2. Data-encrypted and header-encrypted opened WITH password: ExtractToMemory uses stored password.
  for (const fs::path& p : {dataEnc, hdrEnc}) {
    win::ArchiveReader reader(library);
    if (!PRISM_CHECK(reader.Open(p.wstring(), pass))) return;
    const auto bytes = reader.ExtractToMemory(0);
    if (PRISM_CHECK(bytes.has_value())) {
      PRISM_CHECK_EQ(std::string(bytes->begin(), bytes->end()), expected);
    }
    reader.Close();
  }
}

PRISM_TEST("c138.charset_ambiguity_preserved_and_reopen_with_codepage") {
  prism::testtmp::UniqueDir scratch(L"c138", L"charset");
  if (!PRISM_CHECK(scratch.ok())) return;
  const fs::path root = scratch.path();

  // EUC-KR bytes for "문서자료.txt" (tied with GB18030 in DetectEncodings -> ambiguous=true).
  const std::string eucKrRaw = FromHex("b9aebcadc0dab7e12e747874");
  const std::string eucKrUtf8 = "\xeb\xac\xb8\xec\x84\x9c\xec\x9e\x90\xeb\xa3\x8c.txt";
  const fs::path eucKrZip = root / "euckr.zip";
  PRISM_CHECK(WriteBytes(eucKrZip, BuildLegacyZip(eucKrRaw, "korean-payload")));

  // Shift-JIS bytes for "資料フォルダ.txt".
  const std::string sjisRaw = FromHex("8e9197bf83748348838b835f2e747874");
  const std::string sjisUtf8 =
      "\xe8\xb3\x87\xe6\x96\x99\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80.txt";
  const fs::path sjisZip = root / "sjis.zip";
  PRISM_CHECK(WriteBytes(sjisZip, BuildLegacyZip(sjisRaw, "japanese-payload")));

  win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(gDllPath))) return;
  win::Win32Transcoder transcoder;

  // 1. ArchiveReader preserves ambiguous=true & alternatives, then ReopenWithCodePage(949)
  //    decodes EUC-KR cleanly and clears ambiguity.
  {
    win::ArchiveReader reader(library, &transcoder);
    if (!PRISM_CHECK(reader.Open(eucKrZip.wstring()))) return;
    const auto autoItems = reader.ReadCatalog();
    if (!PRISM_CHECK_EQ(autoItems.size(), static_cast<size_t>(1))) return;
    PRISM_CHECK(autoItems[0].pathAmbiguous);
    PRISM_CHECK(std::find(autoItems[0].pathAlternatives.begin(),
                          autoItems[0].pathAlternatives.end(),
                          prism::Encoding::EucKr) != autoItems[0].pathAlternatives.end());

    prism::ArchiveCatalog autoCatalog;
    autoCatalog.Add(autoItems[0]);
    PRISM_CHECK(autoCatalog.HasAmbiguousPaths());

    if (!PRISM_CHECK(reader.ReopenWithCodePage(949))) return;
    PRISM_CHECK_EQ(reader.codePage(), static_cast<uint32_t>(949));
    const auto switched = reader.ReadCatalog();
    if (PRISM_CHECK_EQ(switched.size(), static_cast<size_t>(1))) {
      PRISM_CHECK_EQ(switched[0].path, eucKrUtf8);
      PRISM_CHECK(!switched[0].pathAmbiguous);
      PRISM_CHECK(switched[0].pathEncoding == prism::Encoding::EucKr);
    }
    reader.Close();
  }

  // 2. AppWiring session ReopenWithCodePage switches the live session cleanly.
  {
    std::shared_ptr<app::OperationHandles> ops;
    std::vector<prism::ArchiveItem> catalog;
    std::vector<prism::ArchiveItem> items;
    std::wstring archivePath = sjisZip.wstring();

    app::OperationHandles::Config cfg;
    cfg.archivePath = archivePath;
    cfg.handlerSlots = 1;
    ops = app::OperationHandles::Create(library, cfg);
    if (!PRISM_CHECK(ops != nullptr)) return;

    app::ArchiveSession session{&library, &ops, &catalog, &items, &archivePath, 1, &transcoder};
    const app::ArchiveSwitch switched = app::ReopenWithCodePage(session, 932);
    PRISM_CHECK(switched.ok);
    PRISM_CHECK(switched.switched);
    PRISM_CHECK_EQ(switched.codePage, static_cast<uint32_t>(932));
    if (PRISM_CHECK_EQ(catalog.size(), static_cast<size_t>(1))) {
      PRISM_CHECK_EQ(catalog[0].path, sjisUtf8);
      PRISM_CHECK(catalog[0].pathEncoding == prism::Encoding::ShiftJis);
    }
    if (ops) ops->Close();
  }
}

}  // namespace

int main(int argc, char** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  gDllPath = prism::fixture::DllPath();
  gExePath = prism::fixture::ExePath();
  if (gDllPath.empty() || gExePath.empty()) {
    std::cout << "SKIP: 7z.dll/7z.exe missing: " << prism::fixture::ExePathWhy() << std::endl;
    CoUninitialize();
    return 77;
  }
  if (kMutantNoVolumeCb) {
    std::cout << "C138 MUTANT ACTIVE: PRISM_C138_MUTANT_NO_VOLUME_CALLBACK=1" << std::endl;
    prism::test::SetMutationVerdict("C138 MUTATION CAUGHT: no_volume_callback", []() {
      return win::IoCounters().mutantNoVolumeCallback.load(std::memory_order_relaxed) > 0;
    });
  } else if (kMutantNoMemCrypto) {
    std::cout << "C138 MUTANT ACTIVE: PRISM_C138_MUTANT_NO_MEM_CRYPTO=1" << std::endl;
    prism::test::SetMutationVerdict("C138 MUTATION CAUGHT: no_mem_crypto", []() {
      return win::IoCounters().mutantNoMemCrypto.load(std::memory_order_relaxed) > 0;
    });
  } else if (kMutantNoAmbiguity) {
    std::cout << "C138 MUTANT ACTIVE: PRISM_C138_MUTANT_NO_AMBIGUITY=1" << std::endl;
    prism::test::SetMutationVerdict("C138 MUTATION CAUGHT: no_ambiguity", []() {
      return win::IoCounters().mutantNoAmbiguity.load(std::memory_order_relaxed) > 0;
    });
  }
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (rc == 0 && !kAnyMutant) {
    std::cout << "ALL C138 CHECKS PASSED" << std::endl;
  }
  return rc;
}
