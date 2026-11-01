// TDD: Quick Look CONTENT plan -- six classes, provider + system capability +
// refusal reason, text encoding/model, WAV oracle (blueprint 9.1, ADR-001).
//
// First principles:
//  * a preview card is only useful if it can name WHAT it will render and WHAT
//    the machine needs to render it. "It's media" cannot answer either, which
//    is why QuickLookPlanner's five kinds grew a six-class table here.
//  * the encoding decision is the whole text preview: rename a GBK file to
//    .txt and a wrong guess shows mojibake, which is worse than a refusal.
//  * the WAV parser exists to be an ORACLE: the Windows suite asks Media
//    Foundation for duration/bitrate and compares against these numbers, so a
//    silently wrong MF answer cannot pass as "it returned something".
//
// Negative controls (CONTRACTS D3): two deliberate mutations are compiled into
// this same file by Tests/suites/c21-preview.cmake (WILL_FAIL). The first one
// ignores the long-line clip, the second one calls every payload UTF-8. Both
// must turn this suite RED -- and the "mutation injection is absent" case below
// proves the define actually reached the header (a mutation that does not
// compile in would otherwise look like a blunt test).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "Preview.hpp"
#include "PreviewPlan.hpp"
#include "mini_test.hpp"

namespace {

#if defined(PRISM_PREVIEW_MUTANT_KEEP_LONG_LINES)
constexpr bool kMutantKeepLongLines = true;
#else
constexpr bool kMutantKeepLongLines = false;
#endif
#if defined(PRISM_PREVIEW_MUTANT_UTF8_ALWAYS)
constexpr bool kMutantUtf8Always = true;
#else
constexpr bool kMutantUtf8Always = false;
#endif

std::span<const uint8_t> Bytes(std::string_view text) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

prism::ArchiveItem Item(const std::string& path, uint64_t size, bool isDir = false,
                        bool encrypted = false) {
  prism::ArchiveItem item;
  item.path = path;
  item.size = size;
  item.isDir = isDir;
  item.encrypted = encrypted;
  return item;
}

}  // namespace

PRISM_TEST("the six classes are decided by extension") {
  using C = prism::PreviewClass;
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("Scans/photo.PNG") == C::Image);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("docs/manual.pdf") == C::Pdf);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("src/main.cpp") == C::Text);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("notes/README.md") == C::Text);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("audio/take.flac") == C::Audio);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("audio/beep.wav") == C::Audio);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("clips/holiday.mkv") == C::Video);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("clips/holiday.mp4") == C::Video);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("bin/tool.exe") == C::Binary);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("Makefile") == C::Text);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("LICENSE") == C::Text);
  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("noidea.qqq") == C::Binary);
}

PRISM_TEST("magic sniffing recognises every container we preview") {
  using M = prism::MagicKind;
  PRISM_CHECK(prism::SniffMagic(Bytes("%PDF-1.7\n")) == M::Pdf);
  PRISM_CHECK(prism::SniffMagic(Bytes(std::string("\x89PNG\r\n\x1a\n", 8))) == M::Png);
  PRISM_CHECK(prism::SniffMagic(Bytes(std::string("\xFF\xD8\xFF\xE0", 4))) == M::Jpeg);
  PRISM_CHECK(prism::SniffMagic(Bytes(std::string("RIFF\0\0\0\0WAVEfmt ", 16))) == M::Wav);
  PRISM_CHECK(prism::SniffMagic(Bytes(std::string("RIFF\0\0\0\0AVI LIST", 16))) == M::Avi);
  PRISM_CHECK(prism::SniffMagic(Bytes("OggS\0\2")) == M::Ogg);
  PRISM_CHECK(prism::SniffMagic(Bytes("7z\xBC\xAF\x27\x1C\x00\x04")) == M::SevenZip);
  PRISM_CHECK(prism::SniffMagic(Bytes("MZ\x90\x00")) == M::Pe);
  PRISM_CHECK(prism::SniffMagic(Bytes("just some plain text\n")) == M::Text);
  PRISM_CHECK(prism::SniffMagic(Bytes(std::string("\0\1\2\3", 4))) == M::Unknown);
}

PRISM_TEST("magic outranks a lying extension") {
  using C = prism::PreviewClass;
  // A .jpg that is really a PDF is a normal archive accident; the decoder sees
  // bytes, not the name, so the plan has to follow the bytes.
  PRISM_CHECK(prism::PreviewPlanner::Classify("weird/photo.jpg", Bytes("%PDF-1.4\n")) == C::Pdf);
  PRISM_CHECK(prism::PreviewPlanner::Classify(
                  "weird/clip.txt", Bytes(std::string("RIFF\0\0\0\0WAVEfmt ", 16))) == C::Audio);
  // A known binary extension stays binary even when the head is printable (the
  // DOS stub of a real .exe is "MZ" + ASCII).
  PRISM_CHECK(prism::PreviewPlanner::Classify("weird/setup.exe", Bytes("MZ\x90\x00")) == C::Binary);
  // No head at all: the name is all we have.
  PRISM_CHECK(prism::PreviewPlanner::Classify("docs/manual.pdf") == C::Pdf);
}

PRISM_TEST("each previewable class names its provider, capability and budget") {
  using C = prism::PreviewClass;
  using P = prism::PreviewProvider;

  const auto image = prism::PreviewPlanner::Plan(Item("a/b.png", 1024));
  PRISM_CHECK(image.klass == C::Image);
  PRISM_CHECK(image.provider == P::WicImage);
  PRISM_CHECK((image.capabilities & prism::preview_cap::kWic) != 0);
  PRISM_CHECK_EQ(image.byteBudget, uint64_t(32) << 20);

  const auto text = prism::PreviewPlanner::Plan(Item("a/b.md", 4096));
  PRISM_CHECK(text.provider == P::TextModel);
  PRISM_CHECK((text.capabilities & prism::preview_cap::kDirectWrite) != 0);
  PRISM_CHECK_EQ(text.byteBudget, uint64_t(64) << 10);

  const auto pdf = prism::PreviewPlanner::Plan(Item("a/b.pdf", 200000));
  PRISM_CHECK(pdf.provider == P::PdfRender);
  PRISM_CHECK((pdf.capabilities & prism::preview_cap::kWindowsDataPdf) != 0);
  PRISM_CHECK((pdf.capabilities & prism::preview_cap::kWic) != 0);

  const auto audio = prism::PreviewPlanner::Plan(Item("a/b.wav", 16000));
  PRISM_CHECK(audio.provider == P::AudioMetadata);
  PRISM_CHECK((audio.capabilities & prism::preview_cap::kMediaFoundation) != 0);

  const auto video = prism::PreviewPlanner::Plan(Item("a/b.mp4", 16000));
  PRISM_CHECK(video.klass == C::Video);
  PRISM_CHECK(video.provider == P::None);
  PRISM_CHECK(!video.previewable());
  PRISM_CHECK(!video.reason.empty());
  PRISM_CHECK(video.refusal == prism::PreviewRefusal::UnsupportedType);

  const auto binary = prism::PreviewPlanner::Plan(Item("a/b.dat", 16000));
  PRISM_CHECK(binary.refusal == prism::PreviewRefusal::UnsupportedType);
  PRISM_CHECK(binary.provider == P::None);
  PRISM_CHECK(!binary.reason.empty());
}

PRISM_TEST("refusals carry a user-facing reason, not a silent blank card") {
  const auto folder = prism::PreviewPlanner::Plan(Item("Assets/", 0, true));
  PRISM_CHECK(folder.refusal == prism::PreviewRefusal::IsDirectory);
  PRISM_CHECK(folder.provider == prism::PreviewProvider::None);

  const auto locked = prism::PreviewPlanner::Plan(Item("secret.pdf", 4000, false, true));
  PRISM_CHECK(locked.refusal == prism::PreviewRefusal::Encrypted);
  PRISM_CHECK(locked.reason.find("password") != std::string::npos);

  const auto empty = prism::PreviewPlanner::Plan(Item("empty.txt", 0));
  PRISM_CHECK(empty.refusal == prism::PreviewRefusal::Empty);
}

PRISM_TEST("text preview is bounded, whole-payload providers refuse instead") {
  // A 900 MB log is a normal archive member: preview the first 64 KiB.
  const auto log = prism::PreviewPlanner::Plan(Item("huge/server.log", 900ull << 20));
  PRISM_CHECK(log.previewable());
  PRISM_CHECK(log.bounded);
  PRISM_CHECK(log.exceedsLimit);
  PRISM_CHECK_EQ(log.byteBudget, uint64_t(64) << 10);

  // Windows.Data.Pdf needs the whole document: a slice renders a corrupt page,
  // so the plan refuses with the limit in the sentence.
  const auto pdf = prism::PreviewPlanner::Plan(Item("huge/manual.pdf", 200ull << 20));
  PRISM_CHECK(!pdf.previewable());
  PRISM_CHECK(pdf.refusal == prism::PreviewRefusal::OverBudget);
  PRISM_CHECK(pdf.reason.find("16 MiB") != std::string::npos);
}

PRISM_TEST("encoding decision: BOM, NUL, UTF-8, GBK, binary") {
  using E = prism::TextEncoding;
  PRISM_CHECK(prism::DetectEncoding(Bytes("plain ascii\n")) == E::Utf8);
  PRISM_CHECK(prism::DetectEncoding(Bytes("\xEF\xBB\xBFhello")) == E::Utf8);
  PRISM_CHECK(prism::DetectEncoding(Bytes(std::string("\xFF\xFEh\0i\0", 6))) == E::Utf16Le);
  PRISM_CHECK(prism::DetectEncoding(Bytes(std::string("\xFE\xFF\0h\0i", 6))) == E::Utf16Be);
  // UTF-8 CJK stays UTF-8; GBK bytes for the same characters do not.
  PRISM_CHECK(prism::DetectEncoding(Bytes("\xE4\xB8\xAD\xE6\x96\x87")) == E::Utf8);
  PRISM_CHECK(prism::DetectEncoding(Bytes("\xD6\xD0\xCE\xC4")) == E::Gbk);
  // A NUL in the first page: not text, no guessing.
  PRISM_CHECK(prism::DetectEncoding(Bytes(std::string("ab\0cd", 5))) == E::Binary);
  // UTF-32LE BOM is explicitly refused instead of rendered as mojibake.
  PRISM_CHECK(prism::DetectEncoding(Bytes(std::string("\xFF\xFE\0\0", 4))) == E::Binary);
  // 0x80 is never a GBK lead byte, so this is not "some legacy text".
  PRISM_CHECK(prism::DetectEncoding(Bytes(std::string("\x80\x80\x80", 3))) == E::Binary);
}

PRISM_TEST("text model numbers lines, handles CRLF/CR and clips long lines") {
  prism::TextOptions options;
  options.maxBytes = 4096;
  options.maxLines = 10;
  options.maxLineChars = 5;

  const std::string wire = "alpha\r\nbeta\rgamma\n0123456789\ndelta";
  const auto model = prism::BuildTextModel(Bytes(wire), prism::TextEncoding::Utf8, options);
  PRISM_CHECK_EQ(model.lines.size(), size_t(5));
  PRISM_CHECK_EQ(model.lines[0].number, uint32_t(1));
  PRISM_CHECK_EQ(model.lines[0].text, std::string("alpha"));
  PRISM_CHECK_EQ(model.lines[1].number, uint32_t(2));
  PRISM_CHECK_EQ(model.lines[1].text, std::string("beta"));
  PRISM_CHECK_EQ(model.lines[2].text, std::string("gamma"));
  // Line 3 is ten chars: the clip keeps five and says so.
  PRISM_CHECK_EQ(model.lines[3].text, std::string("01234"));
  PRISM_CHECK(model.lines[3].clipped);
  PRISM_CHECK_EQ(model.clippedLines, uint32_t(1));
  PRISM_CHECK_EQ(model.lines[4].text, std::string("delta"));
  PRISM_CHECK(!model.lineLimited);
}

PRISM_TEST("a trailing newline does not invent a phantom line") {
  const auto one =
      prism::BuildTextModel(Bytes("a\n"), prism::TextEncoding::Utf8, prism::TextOptions());
  PRISM_CHECK_EQ(one.lines.size(), size_t(1));
  // A BLANK line in the middle is real content and must survive.
  const auto blank =
      prism::BuildTextModel(Bytes("a\n\n"), prism::TextEncoding::Utf8, prism::TextOptions());
  PRISM_CHECK_EQ(blank.lines.size(), size_t(2));
  PRISM_CHECK_EQ(blank.lines[1].text, std::string(""));
}

PRISM_TEST("line and byte budgets stop the model instead of growing it") {
  prism::TextOptions options;
  options.maxBytes = 0;  // no byte cap here
  options.maxLines = 3;
  options.maxLineChars = 0;

  std::string many;
  for (int i = 0; i < 9; ++i) many += "line\n";
  const auto model = prism::BuildTextModel(Bytes(many), prism::TextEncoding::Utf8, options);
  PRISM_CHECK_EQ(model.lines.size(), size_t(3));
  PRISM_CHECK(model.lineLimited);

  prism::TextOptions tiny;
  tiny.maxBytes = 5;
  const auto cut = prism::BuildTextModel(Bytes("0123456789"), prism::TextEncoding::Utf8, tiny);
  PRISM_CHECK(cut.byteLimited);
  PRISM_CHECK_EQ(cut.lines[0].text, std::string("01234"));
}

PRISM_TEST("UTF-16LE decodes to UTF-8 including a surrogate pair") {
  // BOM + 'h' 'i' '\n' + U+1F600 (D83D DE00), all little-endian.
  std::string wire("\xFF\xFE", 2);
  const char16_t units[] = {u'h', u'i', u'\n', char16_t(0xD83D), char16_t(0xDE00)};
  for (char16_t unit : units) {
    wire.push_back(static_cast<char>(unit & 0xFF));
    wire.push_back(static_cast<char>((unit >> 8) & 0xFF));
  }
  const auto model = prism::BuildTextModel(Bytes(wire), prism::TextEncoding::Utf16Le,
                                           prism::TextOptions());
  PRISM_CHECK(model.bomStripped);
  PRISM_CHECK_EQ(model.lines.size(), size_t(2));
  PRISM_CHECK_EQ(model.lines[0].text, std::string("hi"));
  PRISM_CHECK_EQ(model.lines[1].text, std::string("\xF0\x9F\x98\x80"));
}

PRISM_TEST("GBK refuses without a transcoder and uses one when injected") {
  const std::string gbk("\xD6\xD0\xCE\xC4", 4);  // "zhong wen" in GBK
  const auto missing = prism::BuildTextModel(Bytes(gbk), prism::TextEncoding::Gbk,
                                             prism::TextOptions());
  PRISM_CHECK(missing.transcoderMissing);
  PRISM_CHECK(missing.lines.empty());

  const auto decoded = prism::BuildTextModel(
      Bytes(gbk), prism::TextEncoding::Gbk, prism::TextOptions(),
      [](std::string_view raw) { return std::string("<") + std::to_string(raw.size()) + ">"; });
  PRISM_CHECK(!decoded.transcoderMissing);
  PRISM_CHECK_EQ(decoded.lines.size(), size_t(1));
  PRISM_CHECK_EQ(decoded.lines[0].text, std::string("<4>"));
}

PRISM_TEST("WAV header is the oracle the Windows suite compares against") {
  // 8000 Hz, 16-bit, mono, one second of PCM.
  std::string wav("RIFF", 4);
  const uint32_t dataBytes = 16000;
  const uint32_t riffSize = 36 + dataBytes;
  wav.push_back(static_cast<char>(riffSize & 0xFF));
  wav.push_back(static_cast<char>((riffSize >> 8) & 0xFF));
  wav.push_back(static_cast<char>((riffSize >> 16) & 0xFF));
  wav.push_back(static_cast<char>((riffSize >> 24) & 0xFF));
  wav += "WAVEfmt ";
  const auto put32 = [&wav](uint32_t value) {
    for (int i = 0; i < 4; ++i) wav.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  };
  const auto put16 = [&wav](uint16_t value) {
    wav.push_back(static_cast<char>(value & 0xFF));
    wav.push_back(static_cast<char>((value >> 8) & 0xFF));
  };
  put32(16);       // fmt chunk size
  put16(1);        // PCM
  put16(1);        // channels
  put32(8000);     // sample rate
  put32(16000);    // byte rate
  put16(2);        // block align
  put16(16);       // bits per sample
  wav += "data";
  put32(dataBytes);
  wav.append(dataBytes, '\0');

  const auto info = prism::ParseWavHeader(Bytes(wav));
  PRISM_CHECK(info.ok);
  PRISM_CHECK_EQ(info.sampleRate, uint32_t(8000));
  PRISM_CHECK_EQ(info.channels, uint16_t(1));
  PRISM_CHECK_EQ(info.bitsPerSample, uint16_t(16));
  PRISM_CHECK_EQ(info.bitrate, uint32_t(128000));
  PRISM_CHECK_EQ(info.duration100ns, uint64_t(10000000));  // exactly 1 second

  PRISM_CHECK(prism::PreviewPlanner::ClassifyName("x.wav") == prism::PreviewClass::Audio);
  PRISM_CHECK(!prism::ParseWavHeader(Bytes(std::string("RIFF\0\0\0\0AVI ", 12))).ok);
}

PRISM_TEST("planning is deterministic and side-effect free") {
  const auto item = Item("docs/manual.pdf", 1234);
  const auto a = prism::PreviewPlanner::Plan(item, Bytes("%PDF-1.7\n"));
  const auto b = prism::PreviewPlanner::Plan(item, Bytes("%PDF-1.7\n"));
  PRISM_CHECK(a.klass == b.klass && a.provider == b.provider &&
              a.byteBudget == b.byteBudget && a.reason == b.reason);
  PRISM_CHECK(a.Kind() == prism::PreviewKind::Pdf);  // back-compat bridge
}

PRISM_TEST("mutation injection is absent in the clean build (D3 self-check)") {
  // In a mutant build this case goes red on purpose; the mutant targets are
  // registered with WILL_FAIL, so CTest only stays green while the injection
  // is really compiled in (CONTRACTS D3).
  PRISM_CHECK(!kMutantKeepLongLines);
  PRISM_CHECK(!kMutantUtf8Always);
}

PRISM_TEST_MAIN()
