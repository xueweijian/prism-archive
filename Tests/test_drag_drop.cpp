// TDD: the drag/drop policy must select a transport explicitly and never
// pretend a path, free-space result, password, or stream producer is safe.
// Negative control: CTest builds this same suite with deliberate mutants and
// expects that binary to fail; if a mutant survives, the CTest gate goes red.
#include "../Src/PrismCore/DragDrop.hpp"
#include "mini_test.hpp"

#include <limits>
#include <string>
#include <vector>

using prism::dnd::Channel;
using prism::dnd::DenyReason;
using prism::dnd::DragTargetDescription;
using prism::dnd::StreamError;

namespace {
DragTargetDescription FileManager(std::string path = "D:/Videos") {
  DragTargetDescription target;
  target.isFileManager = true;
  target.physicalPath = std::move(path);
  target.archiveTotalBytes = 1024;
  target.freeBytes = 64ull * 1024 * 1024;
  return target;
}
}

PRISM_TEST("trusted physical path with verified capacity selects direct zero-temp channel") {
  const auto d = prism::dnd::DecideDragChannel(FileManager());
  PRISM_CHECK_EQ(d.channel, Channel::DirectToPath);
  PRISM_CHECK_EQ(d.reason, DenyReason::None);
  PRISM_CHECK(d.zeroTempFiles);
  PRISM_CHECK(d.targetPath.trusted);
  PRISM_CHECK_EQ(d.targetPath.value, std::string("D:/Videos"));
}

PRISM_TEST("non-file-manager IStream target gets lazy memory stream") {
  DragTargetDescription target;
  target.acceptsIStream = true;
  target.archiveTotalBytes = 10;
  const auto d = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK_EQ(d.channel, Channel::MemoryStream);
  PRISM_CHECK_EQ(d.reason, DenyReason::None);
  PRISM_CHECK(d.zeroTempFiles);
}

PRISM_TEST("unresolved encrypted password explicitly refuses zero-copy") {
  auto target = FileManager();
  target.encrypted = true;
  const auto d = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK_EQ(d.channel, Channel::TempCopyFallback);
  PRISM_CHECK_EQ(d.reason, DenyReason::PasswordNotResolved);
  PRISM_CHECK(d.HasExplicitReason());
  PRISM_CHECK(d.explanation.find("password") != std::string::npos);
}

PRISM_TEST("known password allows encrypted direct path") {
  auto target = FileManager();
  target.encrypted = true;
  target.passwordResolved = true;
  PRISM_CHECK_EQ(prism::dnd::DecideDragChannel(target).channel, Channel::DirectToPath);
}

PRISM_TEST("not enough destination capacity has a loud named refusal") {
  auto target = FileManager();
  target.archiveTotalBytes = 32ull * 1024 * 1024;
  target.freeBytes = 32ull * 1024 * 1024;
  const auto d = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK_EQ(d.channel, Channel::TempCopyFallback);
  PRISM_CHECK_EQ(d.reason, DenyReason::DiskSpaceInsufficient);
  PRISM_CHECK(d.explanation.find("free space") != std::string::npos);
}

PRISM_TEST("unknown disk capacity is not treated as infinite") {
  auto target = FileManager();
  target.freeBytes.reset();
  const auto d = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK_EQ(d.channel, Channel::TempCopyFallback);
  PRISM_CHECK_EQ(d.reason, DenyReason::DiskSpaceUnknown);
}

PRISM_TEST("UNC path is rejected even when it looks like a directory") {
  const auto d = prism::dnd::DecideDragChannel(FileManager("\\\\server\\share"));
  PRISM_CHECK_EQ(d.channel, Channel::TempCopyFallback);
  PRISM_CHECK_EQ(d.reason, DenyReason::PathUnc);
  PRISM_CHECK(!d.targetPath.trusted);
}

PRISM_TEST("relative and empty physical paths are rejected") {
  const auto relative = prism::dnd::DropTargetPath::Parse("folder/subfolder");
  const auto empty = prism::dnd::DropTargetPath::Parse("");
  PRISM_CHECK(!relative.trusted);
  PRISM_CHECK_EQ(relative.rejection, DenyReason::PathRelative);
  PRISM_CHECK(!empty.trusted);
  PRISM_CHECK_EQ(empty.rejection, DenyReason::PathEmpty);
}

PRISM_TEST("ordinary Windows backslashes normalize to PrismCore forward slashes") {
  const auto p = prism::dnd::DropTargetPath::Parse("D:\\Media\\Videos\\");
  PRISM_CHECK(p.trusted);
  PRISM_CHECK_EQ(p.value, std::string("D:/Media/Videos"));
}

PRISM_TEST("extended prefix with mixed separators is rejected, not half-normalized") {
  const auto p = prism::dnd::DropTargetPath::Parse("\\\\?\\C:/Videos");
  PRISM_CHECK(!p.trusted);
  PRISM_CHECK_EQ(p.rejection, DenyReason::PathExtendedMixedSep);
}

PRISM_TEST("dot traversal and reserved path characters are refused") {
  const auto traversal = prism::dnd::DropTargetPath::Parse("D:/safe/../outside");
  const auto reserved = prism::dnd::DropTargetPath::Parse("D:/bad?name");
  PRISM_CHECK(!traversal.trusted);
  PRISM_CHECK_EQ(traversal.rejection, DenyReason::PathTraversal);
  PRISM_CHECK(!reserved.trusted);
  PRISM_CHECK_EQ(reserved.rejection, DenyReason::PathReservedChars);
}

PRISM_TEST("solid archive reports whole-block decode cost and threshold verdict") {
  auto target = FileManager();
  target.solidBlock = true;
  target.solidBlockBytes = 2ull * 1024 * 1024 * 1024;
  target.archiveTotalBytes = 512;
  const auto d = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK_EQ(d.channel, Channel::DirectToPath);
  PRISM_CHECK(d.solidCost.applies);
  PRISM_CHECK(!d.solidCost.worthDirect);
  PRISM_CHECK(d.explanation.find("entire solid block") != std::string::npos);
  PRISM_CHECK(d.explanation.find("threshold") != std::string::npos);
}

PRISM_TEST("small solid block is called affordable and non-solid has no cost warning") {
  auto target = FileManager();
  target.solidBlock = true;
  target.solidBlockBytes = 1024;
  auto small = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK(small.solidCost.worthDirect);
  PRISM_CHECK(small.explanation.find("entire solid block") != std::string::npos);
  target.solidBlock = false;
  auto plain = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK(!plain.solidCost.applies);
  PRISM_CHECK(plain.explanation.find("entire solid block") == std::string::npos);
}

PRISM_TEST("descriptor generation preserves order and CollisionResolver naming") {
  const std::vector<prism::dnd::VirtualFileDescriptor> input{
      {"one/report.txt", 1, 11, false}, {"two/REPORT.TXT", 2, 22, false},
      {"three/report.txt", 3, 33, false}, {"docs", 0, 44, true}, {"docs", 0, 55, true}};
  const auto group = prism::dnd::BuildGroupDescriptor(input);
  PRISM_CHECK_EQ(group.error, DenyReason::None);
  PRISM_CHECK_EQ(group.files.size(), size_t(5));
  PRISM_CHECK_EQ(group.files[0].name, std::string("report.txt"));
  PRISM_CHECK_EQ(group.files[1].name, std::string("REPORT (2).TXT"));
  PRISM_CHECK_EQ(group.files[2].name, std::string("report (3).txt"));
  PRISM_CHECK_EQ(group.files[3].name, std::string("docs"));
  PRISM_CHECK_EQ(group.files[4].name, std::string("docs"));
  PRISM_CHECK_EQ(group.files[1].size, uint64_t(2));
  PRISM_CHECK_EQ(group.files[1].modifiedTime, int64_t(22));
}

PRISM_TEST("descriptor list rejects malicious empty basename and excessive count") {
  const auto bad = prism::dnd::BuildGroupDescriptor(
      std::vector<prism::dnd::VirtualFileDescriptor>{{"../../", 0, 0, false}});
  PRISM_CHECK_EQ(bad.error, DenyReason::PathTraversal);
  prism::dnd::DescriptorOptions options;
  options.maxFiles = 1;
  const auto tooMany = prism::dnd::BuildGroupDescriptor(
      std::vector<prism::dnd::VirtualFileDescriptor>{{"a", 1, 0, false}, {"b", 1, 0, false}}, options);
  PRISM_CHECK_EQ(tooMany.error, DenyReason::NoDeliverableFormat);
}

PRISM_TEST("descriptor filename limit counts UTF-16 units without splitting CJK") {
  prism::dnd::DescriptorOptions options;
  options.maxNameUtf16Units = 3;
  const auto group = prism::dnd::BuildGroupDescriptor(
      std::vector<prism::dnd::VirtualFileDescriptor>{{"中文文档.txt", 1, 0, false}}, options);
  PRISM_CHECK_EQ(group.error, DenyReason::None);
  PRISM_CHECK(prism::dnd::Utf16CodeUnits(group.files[0].name) <= 3);
  PRISM_CHECK_EQ(group.files[0].name, std::string("中文文"));
}

PRISM_TEST("stream pump reads ordered chunks and returns exact EOF") {
  prism::dnd::StreamPumpPlan plan(5, 16, [](uint64_t offset, size_t count) -> std::optional<std::string> {
    const std::string bytes = "hello";
    return bytes.substr(static_cast<size_t>(offset), count);
  });
  auto first = plan.Read(2);
  auto second = plan.Read(8);
  PRISM_CHECK_EQ(first.bytesRead, size_t(2));
  PRISM_CHECK(!first.eof);
  PRISM_CHECK_EQ(second.bytesRead, size_t(3));
  PRISM_CHECK(second.eof);
  PRISM_CHECK_EQ(plan.position(), uint64_t(5));
  const auto eof = plan.Read(1);
  PRISM_CHECK(eof.eof);
  PRISM_CHECK_EQ(eof.error, StreamError::Eof);
}

PRISM_TEST("stream seek permits random access and refuses invalid offsets") {
  prism::dnd::StreamPumpPlan plan(4, 4, [](uint64_t offset, size_t count) -> std::optional<std::string> {
    return std::string("abcd").substr(static_cast<size_t>(offset), count);
  });
  PRISM_CHECK(plan.Seek(2));
  PRISM_CHECK_EQ(plan.Read(1).bytesRead, size_t(1));
  PRISM_CHECK_EQ(plan.position(), uint64_t(3));
  PRISM_CHECK(!plan.Seek(5));
  PRISM_CHECK_EQ(plan.error(), StreamError::SeekOutOfRange);
}

PRISM_TEST("stream size limit, producer underrun and unavailable seek fail loudly") {
  prism::dnd::StreamPumpPlan tooLarge(100, 99, {});
  PRISM_CHECK_EQ(tooLarge.error(), StreamError::SizeLimitExceeded);
  prism::dnd::StreamPumpPlan shortRead(3, 3, [](uint64_t, size_t) -> std::optional<std::string> {
    return std::string("x");
  });
  PRISM_CHECK_EQ(shortRead.Read(3).error, StreamError::ProducerUnderrun);
  prism::dnd::StreamPumpPlan sequential(3, 3, [](uint64_t, size_t) -> std::optional<std::string> {
    return std::string("abc");
  });
  PRISM_CHECK(!sequential.Seek(1, false));
  PRISM_CHECK_EQ(sequential.error(), StreamError::SeekUnsupported);
}

PRISM_TEST("empty selection and unsupported target formats have explicit reasons") {
  auto empty = FileManager();
  empty.nonEmptySelection = false;
  PRISM_CHECK_EQ(prism::dnd::DecideDragChannel(empty).reason, DenyReason::EmptySelection);
  DragTargetDescription unsupported;
  unsupported.acceptsIStream = false;
  unsupported.acceptsFileDrop = false;
  const auto d = prism::dnd::DecideDragChannel(unsupported);
  PRISM_CHECK_EQ(d.channel, Channel::TempCopyFallback);
  PRISM_CHECK_EQ(d.reason, DenyReason::NoDeliverableFormat);
}

PRISM_TEST("empty directories remain mergeable while conflicting files rename") {
  const auto group = prism::dnd::BuildGroupDescriptor(
      std::vector<prism::dnd::VirtualFileDescriptor>{{"dir", 0, 0, true},
                                                     {"DIR", 0, 0, true},
                                                     {"dir/file.bin", 4, 1, false},
                                                     {"other/file.bin", 5, 2, false}});
  PRISM_CHECK_EQ(group.files.size(), size_t(4));
  PRISM_CHECK_EQ(group.files[0].name, std::string("dir"));
  PRISM_CHECK_EQ(group.files[1].name, std::string("DIR"));
  PRISM_CHECK_EQ(group.files[2].name, std::string("file.bin"));
  PRISM_CHECK_EQ(group.files[3].name, std::string("file (2).bin"));
}

PRISM_TEST("virtual descriptor names are sanitised before collision planning") {
  const auto group = prism::dnd::BuildGroupDescriptor(
      std::vector<prism::dnd::VirtualFileDescriptor>{{"docs/CON.txt", 1, 0, false},
                                                     {"logs/a.txt:b", 2, 0, false},
                                                     {"tmp/x.", 3, 0, false},
                                                     {"tmp/x", 4, 0, false}});
  PRISM_CHECK_EQ(group.error, DenyReason::None);
  PRISM_CHECK_EQ(group.files[0].name, std::string("CON_.txt"));
  PRISM_CHECK_EQ(group.files[1].name, std::string("a.txt_b"));
  // "x." sanitises to "x", so the verbatim "x" after it must rename instead
  // of aliasing the same destination file -- the planned-set has to observe
  // post-filesystem names, otherwise two entries silently overwrite.
  PRISM_CHECK_EQ(group.files[2].name, std::string("x"));
  PRISM_CHECK_EQ(group.files[3].name, std::string("x (2)"));
}

PRISM_TEST("no target combination yields a silent TempCopyFallback downgrade") {
  // Exhaustive sweep over the target-feature space: every fallback must name
  // its reason and carry an explanation (7-Zip's silent %TEMP% downgrade is
  // the exact failure this module exists to kill).
  static_assert(sizeof(int) >= 4);
  for (int mask = 0; mask < 128; ++mask) {
    DragTargetDescription target;
    target.isFileManager = (mask & 1) != 0;
    target.encrypted = (mask & 2) != 0;
    target.passwordResolved = (mask & 4) != 0;
    target.acceptsIStream = (mask & 8) != 0;
    target.acceptsFileDrop = (mask & 16) != 0;
    target.nonEmptySelection = (mask & 32) != 0;
    target.physicalPath = (mask & 64) != 0 ? "D:/Videos" : "";
    if (target.isFileManager) target.freeBytes = 64ull * 1024 * 1024;
    const auto d = prism::dnd::DecideDragChannel(target);
    PRISM_CHECK(d.HasExplicitReason());
    if (d.channel == Channel::TempCopyFallback) {
      PRISM_CHECK(d.reason != DenyReason::None);
      PRISM_CHECK(!d.explanation.empty());
    }
  }
}

PRISM_TEST_MAIN()
