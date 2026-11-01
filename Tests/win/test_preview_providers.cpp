// PrismArchive -- Windows integration test (card C21)
// Quick Look content providers on the REAL 7z.dll:
//   * text  -- UTF-8 and GBK fixtures are read through the 7-Zip stream (never
//              a temp file) and the encoding decision + line model are asserted
//              by CONTENT (CONTRACTS D2), not by "it decoded".
//   * PDF   -- Windows.Data.Pdf renders page 1 from an in-memory stream; the
//              first BGRA pixels are asserted (black square on a white page)
//              and the temp directory is proven untouched.
//   * audio -- Media Foundation reports duration/bitrate/sample rate for a WAV
//              whose exact values the fixture authored.
//
// Missing 7z.dll/7z.exe is a FAIL (CONTRACTS D1: a silent skip that looks like
// a pass is how the 0.15 s green happened). A missing SYSTEM component
// (Windows.Data.Pdf, Media Foundation) is the only honoured SKIP: it prints its
// reason and returns exit code 77, which CTest maps to SKIP_RETURN_CODE -- never
// to a pass.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "PreviewProviders.hpp"
#include "SolidFixture.hpp"

namespace fs = std::filesystem;
namespace pv = prism::win::preview;

namespace {

constexpr int kSkipExitCode = 77;

bool g_pdfAvailable = false;
bool g_mfAvailable = false;
std::string g_pdfWhy;
std::string g_mfWhy;

std::unique_ptr<prism::win::SevenZipLibrary> g_library;
std::unique_ptr<prism::win::ArchiveReader> g_reader;
std::vector<prism::ArchiveItem> g_catalog;
fs::path g_work;         // fixtures + the archive live here
fs::path g_privateTemp;  // redirected TMP/TEMP: must stay EMPTY (no disk preview)

bool WriteBytes(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

// Minimal one-page PDF authored by hand (offsets computed, not guessed):
// 200x200 pt page with a filled black square at 40,40..160,160. No font, so the
// page renders even on an engine with no font fallback.
std::string BuildMinimalPdf() {
  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  auto object = [&out, &offsets](const std::string& body) {
    offsets.push_back(out.size());
    out += body;
  };
  object("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
  object("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
  object(
      "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
      "/Resources << >> /Contents 4 0 R >>\nendobj\n");
  const std::string content = "q 0 0 0 rg 40 40 120 120 re f Q\n";
  object("4 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
         "endstream\nendobj\n");
  const size_t xrefPosition = out.size();
  out += "xref\n0 5\n0000000000 65535 f \n";
  for (const size_t offset : offsets) {
    char line[32] = {};
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
    out += line;
  }
  out += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefPosition) +
         "\n%%EOF\n";
  return out;
}

// PCM WAV: 8000 Hz, 16-bit, mono, exactly one second. The pure suite proves
// ParseWavHeader derives 10000000 ticks from these same numbers.
std::string BuildWav() {
  const uint32_t dataBytes = 16000;  // 8000 samples * 2 bytes
  std::string wav("RIFF", 4);
  auto put32 = [&wav](uint32_t value) {
    for (int i = 0; i < 4; ++i) wav.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  };
  auto put16 = [&wav](uint16_t value) {
    wav.push_back(static_cast<char>(value & 0xFF));
    wav.push_back(static_cast<char>((value >> 8) & 0xFF));
  };
  put32(36 + dataBytes);
  wav += "WAVEfmt ";
  put32(16);
  put16(1);      // PCM
  put16(1);      // mono
  put32(8000);   // sample rate
  put32(16000);  // byte rate
  put16(2);      // block align
  put16(16);     // bits per sample
  wav += "data";
  put32(dataBytes);
  for (uint32_t i = 0; i < dataBytes / 2; ++i) {
    // A 440 Hz square-ish tone: the amplitude does not matter, the metadata does.
    const uint16_t sample = ((i / 9) % 2 == 0) ? 12000 : 52000;
    put16(sample);
  }
  return wav;
}

uint32_t IndexOfPath(const std::string& needle) {
  for (const prism::ArchiveItem& item : g_catalog) {
    if (item.path.find(needle) != std::string::npos) return item.index;
  }
  return static_cast<uint32_t>(-1);
}

size_t CountFiles(const fs::path& dir) {
  size_t count = 0;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return 0;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) ++count;
  }
  return count;
}

// Fixtures are AUTHORED here and packed by the real 7z.exe; nothing ships in git.
bool MakeFixtures() {
  std::error_code ec;
  fs::create_directories(g_work, ec);
  const std::string longLine(300, 'x');

  std::string utf8Text = "alpha\nsecond line\n" + longLine + "\n" +
                         "\xE4\xB8\xAD\xE6\x96\x87 UTF-8 tail\n";
  if (!WriteBytes(g_work / "notes.txt", utf8Text)) return false;

  // "zhong wen ce shi" + newline + "di er hang" in GBK (D6 D0 CE C4 B2 E2 CA D4).
  std::string gbkText("\xD6\xD0\xCE\xC4\xB2\xE2\xCA\xD4\n", 9);
  gbkText += std::string("\xB5\xDA\xB6\xFE\xD0\xD0\n", 7);
  if (!WriteBytes(g_work / "gbk.txt", gbkText)) return false;

  if (!WriteBytes(g_work / "hello.pdf", BuildMinimalPdf())) return false;
  if (!WriteBytes(g_work / "beep.wav", BuildWav())) return false;

  std::string blob(512, '\0');
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<char>(i & 0xFF);
  if (!WriteBytes(g_work / "blob.bin", blob)) return false;

  const std::wstring exe = prism::fixture::ExePath();
  if (exe.empty()) return false;
  return prism::fixture::PackSolid(exe, g_work.wstring(), L"preview.7z",
                                   {L"notes.txt", L"gbk.txt", L"hello.pdf", L"beep.wav",
                                    L"blob.bin"});
}

}  // namespace

PRISM_TEST("the real archive plans one provider per class") {
  using P = prism::PreviewProvider;
  PRISM_CHECK_EQ(g_catalog.size(), size_t(5));

  const uint32_t textIndex = IndexOfPath("notes.txt");
  const uint32_t pdfIndex = IndexOfPath("hello.pdf");
  const uint32_t wavIndex = IndexOfPath("beep.wav");
  const uint32_t blobIndex = IndexOfPath("blob.bin");
  PRISM_CHECK(textIndex != static_cast<uint32_t>(-1));
  PRISM_CHECK(pdfIndex != static_cast<uint32_t>(-1));
  PRISM_CHECK(wavIndex != static_cast<uint32_t>(-1));
  PRISM_CHECK(blobIndex != static_cast<uint32_t>(-1));

  const auto textPlan = prism::PreviewPlanner::Plan(g_catalog[textIndex]);
  PRISM_CHECK(textPlan.provider == P::TextModel);
  PRISM_CHECK_EQ(textPlan.byteBudget, uint64_t(64) << 10);

  const auto pdfPlan = prism::PreviewPlanner::Plan(g_catalog[pdfIndex]);
  PRISM_CHECK(pdfPlan.provider == P::PdfRender);
  PRISM_CHECK((pdfPlan.capabilities & prism::preview_cap::kWindowsDataPdf) != 0);

  const auto wavPlan = prism::PreviewPlanner::Plan(g_catalog[wavIndex]);
  PRISM_CHECK(wavPlan.provider == P::AudioMetadata);
  PRISM_CHECK((wavPlan.capabilities & prism::preview_cap::kMediaFoundation) != 0);

  // The binary blob must never reach a provider, and it must SAY why.
  const auto blobPlan = prism::PreviewPlanner::Plan(g_catalog[blobIndex]);
  PRISM_CHECK(blobPlan.provider == P::None);
  PRISM_CHECK(blobPlan.refusal == prism::PreviewRefusal::UnsupportedType);
  PRISM_CHECK(!blobPlan.reason.empty());
}

PRISM_TEST("text: UTF-8 payload keeps its lines and clips the 300-char line") {
  const uint32_t index = IndexOfPath("notes.txt");
  prism::TextOptions options;
  options.maxLines = 10;
  options.maxLineChars = 200;
  const pv::TextPreview preview = pv::ReadTextPreview(*g_reader, index, options);
  PRISM_CHECK(preview.ok);
  PRISM_CHECK(preview.encoding == prism::TextEncoding::Utf8);
  PRISM_CHECK_EQ(preview.model.lines.size(), size_t(4));
  PRISM_CHECK_EQ(preview.model.lines[0].text, std::string("alpha"));
  PRISM_CHECK_EQ(preview.model.lines[1].text, std::string("second line"));
  PRISM_CHECK_EQ(preview.model.lines[2].text.size(), size_t(200));
  PRISM_CHECK(preview.model.lines[2].clipped);
  PRISM_CHECK_EQ(preview.model.clippedLines, uint32_t(1));
  PRISM_CHECK_EQ(preview.model.lines[3].text,
                 std::string("\xE4\xB8\xAD\xE6\x96\x87 UTF-8 tail"));
  // The archive reports the REAL size even though the provider only kept a
  // prefix: the card shows the file size, not the read budget.
  PRISM_CHECK_EQ(preview.payloadBytes, g_catalog[index].size);
}

PRISM_TEST("text: GBK payload is transcoded to the exact expected characters") {
  const uint32_t index = IndexOfPath("gbk.txt");
  const pv::TextPreview preview = pv::ReadTextPreview(*g_reader, index);
  PRISM_CHECK(preview.ok);
  PRISM_CHECK(preview.encoding == prism::TextEncoding::Gbk);
  if (!PRISM_CHECK_EQ(preview.model.lines.size(), size_t(2))) return;
  // "zhong wen ce shi" / "di er hang" -- asserted as UTF-8 bytes, so a wrong
  // codepage can never pass as "some text came out".
  PRISM_CHECK_EQ(preview.model.lines[0].text,
                 std::string("\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95"));
  PRISM_CHECK_EQ(preview.model.lines[1].text,
                 std::string("\xE7\xAC\xAC\xE4\xBA\x8C\xE8\xA1\x8C"));
}

PRISM_TEST("text: a binary blob is refused, not rendered as mojibake") {
  const uint32_t index = IndexOfPath("blob.bin");
  const pv::TextPreview preview = pv::ReadTextPreview(*g_reader, index);
  PRISM_CHECK(!preview.ok);
  PRISM_CHECK(preview.encoding == prism::TextEncoding::Binary);
  PRISM_CHECK(!preview.error.empty());
}

PRISM_TEST("pdf: page 1 renders to a BGRA bitmap with the black square in it") {
  if (!g_pdfAvailable) {
    std::cout << "    SKIP pdf: " << g_pdfWhy << std::endl;
    return;
  }
  const uint32_t index = IndexOfPath("hello.pdf");
  const auto payload = g_reader->ExtractToMemory(index, 4u << 20);
  if (!PRISM_CHECK(payload.has_value())) return;
  const pv::PdfPagePreview page =
      pv::RenderPdfFirstPage(std::span<const uint8_t>(payload->data(), payload->size()));
  if (!PRISM_CHECK(page.ok)) {
    std::cout << "    pdf error: " << page.error << std::endl;
    return;
  }
  // The fixture's MediaBox is 200x200 POINTS; Windows.Data.Pdf rasterises at
  // 96 DPI, so the bitmap is 200 * 96/72 = 267 px. Computed, not copied.
  const uint32_t expectedPixels = static_cast<uint32_t>(200.0 * 96.0 / 72.0 + 0.5);
  PRISM_CHECK_EQ(page.pageCount, uint32_t(1));
  PRISM_CHECK_EQ(page.width, expectedPixels);
  PRISM_CHECK_EQ(page.height, expectedPixels);
  PRISM_CHECK_EQ(page.stride, expectedPixels * 4u);
  PRISM_CHECK_EQ(page.pixelsBgra.size(),
                 size_t(expectedPixels) * expectedPixels * size_t(4));

  // Content, not existence (D2): the centre of the page is the black square,
  // the corner is the white background. PBGRA: B G R A.
  const size_t midpoint = expectedPixels / 2u;
  const size_t centre = midpoint * page.stride + midpoint * 4u;
  PRISM_CHECK(page.pixelsBgra[centre + 0] < 16);
  PRISM_CHECK(page.pixelsBgra[centre + 1] < 16);
  PRISM_CHECK(page.pixelsBgra[centre + 2] < 16);
  PRISM_CHECK(page.pixelsBgra[centre + 3] > 200);
  const size_t corner = (4u * page.stride) + 4u * 4u;
  PRISM_CHECK(page.pixelsBgra[corner + 0] > 200);
  PRISM_CHECK(page.pixelsBgra[corner + 1] > 200);
  PRISM_CHECK(page.pixelsBgra[corner + 2] > 200);
}

PRISM_TEST("audio: Media Foundation reports the fixture's exact metadata") {
  if (!g_mfAvailable) {
    std::cout << "    SKIP audio: " << g_mfWhy << std::endl;
    return;
  }
  const uint32_t index = IndexOfPath("beep.wav");
  const auto payload = g_reader->ExtractToMemory(index, 1u << 20);
  if (!PRISM_CHECK(payload.has_value())) return;
  const pv::AudioMetadata info =
      pv::ReadAudioMetadata(std::span<const uint8_t>(payload->data(), payload->size()));
  if (!PRISM_CHECK(info.ok)) {
    std::cout << "    audio error: " << info.error << std::endl;
    return;
  }
  PRISM_CHECK_EQ(info.sampleRate, uint32_t(8000));
  PRISM_CHECK_EQ(info.channels, uint16_t(1));
  PRISM_CHECK_EQ(info.bitsPerSample, uint16_t(16));
  PRISM_CHECK_EQ(info.bitrate, uint32_t(128000));
  // Exactly one second, in 100 ns ticks -- the unit MF_PD_DURATION uses.
  PRISM_CHECK_EQ(info.duration100ns, uint64_t(10000000));
}

PRISM_TEST("no preview path wrote a temp file (the temp dir is still empty)") {
  // TMP/TEMP were redirected to a private empty directory before any provider
  // ran: a text/PDF/audio preview that detoured through disk would land here.
  const size_t files = CountFiles(g_privateTemp);
  if (files != 0) {
    for (const auto& entry : fs::directory_iterator(g_privateTemp)) {
      std::cout << "    unexpected temp artifact: " << entry.path().filename().string()
                << std::endl;
    }
  }
  PRISM_CHECK_EQ(files, size_t(0));
}

int main(int argc, char** argv) {
  const std::wstring dll = prism::fixture::DllPath();
  if (dll.empty() || prism::fixture::ExePath().empty()) {
    std::cout << "FAIL: 7z.dll/7z.exe not found (PRISM_7Z_DLL="
              << prism::win::WideToUtf8(dll) << "); this test does NOT skip on a missing "
              << "engine (CONTRACTS D1)" << std::endl;
    return 1;
  }

  wchar_t tempRoot[MAX_PATH] = {};
  const DWORD written = GetTempPathW(MAX_PATH, tempRoot);
  if (written == 0) {
    std::cout << "FAIL: GetTempPathW failed" << std::endl;
    return 1;
  }
  g_work = fs::path(std::wstring(tempRoot, written)) / L"prism_c21_preview";
  if (!MakeFixtures()) {
    std::cout << "FAIL: fixture creation / 7z packing failed" << std::endl;
    return 1;
  }

  g_library = std::make_unique<prism::win::SevenZipLibrary>();
  if (!g_library->Load(dll)) {
    std::cout << "FAIL: LoadLibrary 7z.dll failed" << std::endl;
    return 1;
  }
  g_reader = std::make_unique<prism::win::ArchiveReader>(*g_library);
  if (!g_reader->Open((g_work / L"preview.7z").wstring())) {
    std::cout << "FAIL: Open failed: " << g_reader->openTrace() << std::endl;
    return 1;
  }
  g_catalog = g_reader->ReadCatalog();
  if (g_catalog.size() != 5) {
    std::cout << "FAIL: expected 5 catalog items, got " << g_catalog.size() << std::endl;
    return 1;
  }

  g_pdfAvailable = pv::PdfRenderAvailable(&g_pdfWhy);
  g_mfAvailable = pv::MediaFoundationAvailable(&g_mfWhy);
  std::cout << "    components: Windows.Data.Pdf=" << (g_pdfAvailable ? "yes" : "no")
            << " MediaFoundation=" << (g_mfAvailable ? "yes" : "no") << std::endl;

  g_privateTemp = g_work / L"no-temp";
  std::error_code ec;
  fs::create_directories(g_privateTemp, ec);
  wchar_t previousTmp[MAX_PATH * 2] = {};
  wchar_t previousTemp[MAX_PATH * 2] = {};
  const bool hadTmp = GetEnvironmentVariableW(L"TMP", previousTmp, ARRAYSIZE(previousTmp)) > 0;
  const bool hadTemp =
      GetEnvironmentVariableW(L"TEMP", previousTemp, ARRAYSIZE(previousTemp)) > 0;
  SetEnvironmentVariableW(L"TMP", g_privateTemp.c_str());
  SetEnvironmentVariableW(L"TEMP", g_privateTemp.c_str());

  const int failures = prism::test::RunAll(argc, argv);

  SetEnvironmentVariableW(L"TMP", hadTmp ? previousTmp : nullptr);
  SetEnvironmentVariableW(L"TEMP", hadTemp ? previousTemp : nullptr);
  g_reader->Close();
  g_reader.reset();
  g_library.reset();

  if (failures != 0) {
    std::cout << "fixtures kept for diagnosis: " << g_work.string() << std::endl;
    return 1;
  }
  if (!g_pdfAvailable || !g_mfAvailable) {
    // D1: a missing system component is a SKIP with a reason and a distinct
    // exit code -- never a green that hides what was not verified.
    if (!g_pdfAvailable) std::cout << "SKIP: " << g_pdfWhy << std::endl;
    if (!g_mfAvailable) std::cout << "SKIP: " << g_mfWhy << std::endl;
    return kSkipExitCode;
  }
  fs::remove_all(g_work, ec);
  std::cout << "ALL PREVIEW-PROVIDER CHECKS PASSED" << std::endl;
  return 0;
}
