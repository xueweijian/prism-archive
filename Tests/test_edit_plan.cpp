// PrismArchive -- EditPlan suite (card C51).
//
// The DECISION layer of "edit an archive in place" is pure PrismCore, so every
// rule the card names is proven here in milliseconds, on every lane:
//   * a rename / move / delete / new folder reports WHICH entries it touches,
//     whether a solid block has to be re-encoded, and how many bytes that costs;
//   * the legality rules are part of the model, not the caller: duplicate names
//     (case-insensitively, because this is a Windows-first product), path
//     traversal, deleting a non-empty folder without recurse=true, a read-only
//     archive, a multi-volume set;
//   * a MALFORMED catalog entry (empty name, ".", "..", "a//b", a drive letter)
//     refuses the edit instead of being copied into a name the filesystem cannot
//     represent -- the boundary the Windows suite cannot fabricate with 7z.dll;
//   * a refused plan hands the writer NO item list, so "refused" can never be
//     mistaken for "write everything anyway".
//
// Exit policy (CONTRACTS D1): the success marker is printed only when every check
// passed, and the CTest entry requires that marker AND exit code 0.
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "../Src/PrismCore/ArchiveCatalog.hpp"
#include "../Src/PrismCore/EditPlan.hpp"
#include "mini_test.hpp"

namespace {

using prism::ArchiveItem;
using prism::BuildEditPlan;
using prism::EditArchiveFacts;
using prism::EditOp;
using prism::EditOpKind;
using prism::EditRefusal;

ArchiveItem Item(uint32_t index, const char* path, uint64_t size, bool isDir) {
  ArchiveItem item;
  item.index = index;
  item.path = path;
  item.size = size;
  item.isDir = isDir;
  return item;
}

// Five entries: two Chinese file names, one folder, one zero-byte file, plus a
// plain ASCII file. Same shape as the Windows fixture (which really creates it
// with 7z.dll), so a rule that holds here holds there.
std::vector<ArchiveItem> Catalog() {
  return {
      Item(0, "资料", 0, true),
      Item(1, "资料/说明.txt", 1024, false),
      Item(2, "资料/图片.bin", 4096, false),
      Item(3, "零字节.txt", 0, false),
      Item(4, "根文件.log", 777, false),
  };
}

EditArchiveFacts Facts() {
  EditArchiveFacts facts;
  facts.solid = true;
  return facts;
}

EditOp Op(EditOpKind kind, const char* path, const char* target = "", bool recurse = false) {
  EditOp op;
  op.kind = kind;
  op.path = path;
  op.target = target;
  op.recurse = recurse;
  return op;
}

const prism::EditPlanItem* Find(const prism::EditPlan& plan, const std::string& path) {
  for (const auto& item : plan.items) {
    if (item.path == path) return &item;
  }
  return nullptr;
}

bool HasAffected(const prism::EditOpImpact& impact, const std::string& path) {
  for (const std::string& entry : impact.affected) {
    if (entry == path) return true;
  }
  return false;
}

// The op-level refusal must also be the plan-level refusal, or a caller that
// only reads one of the two would report the wrong thing.
bool Refused(const prism::EditPlan& plan, EditRefusal expected) {
  if (plan.ok) return false;
  if (plan.refusal != expected) return false;
  if (plan.error.empty()) return false;
  for (const auto& impact : plan.ops) {
    if (impact.legal) return false;
    if (impact.refusal != expected) return false;
  }
  return plan.items.empty();  // a refused plan writes NOTHING
}

}  // namespace

PRISM_TEST("rename: new name, same bytes, the impact names the entry") {
  const auto plan = BuildEditPlan(Catalog(), {Op(EditOpKind::Rename, "根文件.log", "资料/重命名.log")},
                                  Facts());
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.refusal, EditRefusal::None);
  PRISM_CHECK_EQ(plan.items.size(), std::size_t{5});
  PRISM_CHECK_EQ(plan.removed.size(), std::size_t{0});
  const auto* moved = Find(plan, "资料/重命名.log");
  PRISM_CHECK(moved != nullptr);
  if (moved != nullptr) {
    PRISM_CHECK_EQ(moved->sourcePath, std::string("根文件.log"));
    PRISM_CHECK_EQ(moved->sourceIndex, uint32_t{4});
    PRISM_CHECK_EQ(moved->size, uint64_t{777});
    PRISM_CHECK(moved->renamed);
    PRISM_CHECK(!moved->added);
  }
  PRISM_CHECK(Find(plan, "根文件.log") == nullptr);
  // Informational, not decorative: the user sees WHICH entry moves, and the
  // estimate is "re-encode the solid block", not "0 bytes".
  PRISM_CHECK_EQ(plan.ops.size(), std::size_t{1});
  PRISM_CHECK(plan.ops[0].legal);
  PRISM_CHECK_EQ(plan.ops[0].affected.size(), std::size_t{1});
  PRISM_CHECK(HasAffected(plan.ops[0], "根文件.log"));
  PRISM_CHECK_EQ(plan.ops[0].bytesAffected, uint64_t{777});
  PRISM_CHECK(plan.ops[0].forcesSolidRewrite);
  PRISM_CHECK_EQ(plan.entriesAffected, uint64_t{1});
  PRISM_CHECK_EQ(plan.keptFiles, uint64_t{4});
  PRISM_CHECK_EQ(plan.keptBytes, uint64_t{1024 + 4096 + 0 + 777});
  PRISM_CHECK_EQ(plan.estimatedWriteBytes, plan.keptBytes);
  PRISM_CHECK(plan.solidRewrite);
  PRISM_CHECK(!plan.costNote.empty());
}

PRISM_TEST("rename: collides with an existing name, case-insensitively") {
  PRISM_CHECK(Refused(BuildEditPlan(Catalog(), {Op(EditOpKind::Rename, "根文件.log", "资料/说明.txt")},
                                    Facts()),
                       EditRefusal::DuplicateName));
  // Two different names differ; the same name in another case does not.
  const auto plan =
      BuildEditPlan(Catalog(), {Op(EditOpKind::Rename, "零字节.txt", "根文件.LOG")}, Facts());
  PRISM_CHECK(Refused(plan, EditRefusal::DuplicateName));
}

PRISM_TEST("rename: traversal and absolute paths are refused, never normalized away") {
  const char* targets[] = {"../逃出去.txt", "/绝对.txt", "C:/绝对.txt", "a/../b.txt", "a//b.txt",
                           "a/./b.txt", "资料/说明\\.txt", "a/b:stream"};
  for (const char* target : targets) {
    const auto plan = BuildEditPlan(Catalog(), {Op(EditOpKind::Rename, "根文件.log", target)}, Facts());
    PRISM_CHECK(!plan.ok);
    PRISM_CHECK(!plan.error.empty());
    PRISM_CHECK(plan.refusal == EditRefusal::UnsafePath ||
                plan.refusal == EditRefusal::MalformedEntryName ||
                // "资料/说明\.txt" normalizes to a path whose parent is the FILE
                // 资料/说明.txt-free folder "资料/说明": it must be refused for
                // the structural reason, never silently created.
                plan.refusal == EditRefusal::MissingEntry ||
                plan.refusal == EditRefusal::NotAFolder);
  }
  // An empty target is not a name either.
  PRISM_CHECK(Refused(BuildEditPlan(Catalog(), {Op(EditOpKind::Rename, "根文件.log", "")}, Facts()),
                       EditRefusal::MalformedEntryName));
  // Renaming a folder into itself would move the subtree under its own tail.
  PRISM_CHECK(Refused(BuildEditPlan(Catalog(), {Op(EditOpKind::Rename, "资料", "资料/子")}, Facts()),
                       EditRefusal::UnsafePath));
}

PRISM_TEST("delete: a file goes, a subtree needs recurse") {
  const auto filePlan = BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "资料/图片.bin")}, Facts());
  PRISM_CHECK(filePlan.ok);
  PRISM_CHECK_EQ(filePlan.items.size(), std::size_t{4});
  PRISM_CHECK(Find(filePlan, "资料/图片.bin") == nullptr);
  PRISM_CHECK_EQ(filePlan.removed.size(), std::size_t{1});
  PRISM_CHECK_EQ(filePlan.removed[0], std::string("资料/图片.bin"));
  PRISM_CHECK_EQ(filePlan.ops[0].bytesAffected, uint64_t{4096});
  PRISM_CHECK_EQ(filePlan.keptBytes, uint64_t{1024 + 0 + 777});

  // The folder still holds two entries: the default must refuse, and the reason
  // must tell the caller how to mean it.
  const auto refused = BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "资料")}, Facts());
  PRISM_CHECK(Refused(refused, EditRefusal::NonEmptyFolder));
  PRISM_CHECK(refused.error.find("recurse") != std::string::npos);

  const auto recursive = BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "资料", "", true)}, Facts());
  PRISM_CHECK(recursive.ok);
  PRISM_CHECK_EQ(recursive.items.size(), std::size_t{2});
  PRISM_CHECK_EQ(recursive.removed.size(), std::size_t{3});
  PRISM_CHECK_EQ(recursive.ops[0].affected.size(), std::size_t{3});
  PRISM_CHECK_EQ(recursive.keptBytes, uint64_t{0 + 777});

  // Deleting something that is not there is a refusal, not a no-op.
  PRISM_CHECK(Refused(BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "不存在.txt")}, Facts()),
                       EditRefusal::MissingEntry));
}

PRISM_TEST("new folder: missing ancestors are materialized and reported") {
  const auto plan =
      BuildEditPlan(Catalog(), {Op(EditOpKind::NewFolder, "新目录/子目录/最深")}, Facts());
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.items.size(), std::size_t{8});
  const auto* deepest = Find(plan, "新目录/子目录/最深");
  PRISM_CHECK(deepest != nullptr);
  if (deepest != nullptr) {
    PRISM_CHECK(deepest->isDir);
    PRISM_CHECK(deepest->added);
    PRISM_CHECK(deepest->sourcePath.empty());
    PRISM_CHECK_EQ(deepest->sourceIndex, uint32_t{0xFFFFFFFFu});
  }
  PRISM_CHECK(Find(plan, "新目录") != nullptr);
  PRISM_CHECK(Find(plan, "新目录/子目录") != nullptr);
  PRISM_CHECK_EQ(plan.ops[0].affected.size(), std::size_t{3});
  PRISM_CHECK_EQ(plan.ops[0].bytesAffected, uint64_t{0});
  // Nothing was removed and no payload is re-read for a folder entry.
  PRISM_CHECK_EQ(plan.removed.size(), std::size_t{0});
  PRISM_CHECK_EQ(plan.keptBytes, uint64_t{1024 + 4096 + 777});
  // Re-creating an existing folder is a duplicate, not a second entry.
  PRISM_CHECK(Refused(BuildEditPlan(Catalog(), {Op(EditOpKind::NewFolder, "资料")}, Facts()),
                       EditRefusal::DuplicateName));
  // A file cannot become a folder's parent.
  PRISM_CHECK(Refused(BuildEditPlan(Catalog(), {Op(EditOpKind::NewFolder, "根文件.log/子")}, Facts()),
                       EditRefusal::NotAFolder));
}

PRISM_TEST("move: into a folder, never into itself, never onto a file") {
  const auto plan = BuildEditPlan(Catalog(), {Op(EditOpKind::Move, "根文件.log", "资料")}, Facts());
  PRISM_CHECK(plan.ok);
  const auto* landed = Find(plan, "资料/根文件.log");
  PRISM_CHECK(landed != nullptr);
  if (landed != nullptr) PRISM_CHECK_EQ(landed->sourcePath, std::string("根文件.log"));
  PRISM_CHECK(Find(plan, "根文件.log") == nullptr);
  PRISM_CHECK_EQ(plan.items.size(), std::size_t{5});

  // Moving a folder moves its subtree and reports every entry it touches.
  auto catalog = Catalog();
  catalog.push_back(Item(5, "归档", 0, true));
  const auto folderPlan = BuildEditPlan(catalog, {Op(EditOpKind::Move, "资料", "归档")}, Facts());
  PRISM_CHECK(folderPlan.ok);
  PRISM_CHECK(Find(folderPlan, "归档/资料") != nullptr);
  PRISM_CHECK(Find(folderPlan, "归档/资料/说明.txt") != nullptr);
  PRISM_CHECK_EQ(folderPlan.ops[0].affected.size(), std::size_t{3});
  PRISM_CHECK(HasAffected(folderPlan.ops[0], "资料/图片.bin"));

  PRISM_CHECK(Refused(BuildEditPlan(catalog, {Op(EditOpKind::Move, "资料", "资料")}, Facts()),
                       EditRefusal::UnsafePath));
  PRISM_CHECK(Refused(BuildEditPlan(catalog, {Op(EditOpKind::Move, "资料", "资料/说明.txt")}, Facts()),
                       EditRefusal::NotAFolder));
  PRISM_CHECK(Refused(BuildEditPlan(catalog, {Op(EditOpKind::Move, "根文件.log", "根文件.log")},
                                    Facts()),
                       EditRefusal::NotAFolder));
  PRISM_CHECK(Refused(BuildEditPlan(catalog, {Op(EditOpKind::Move, "根文件.log", "缺目录")}, Facts()),
                       EditRefusal::MissingEntry));
  // Destination already holds that name.
  auto collide = catalog;
  collide.push_back(Item(6, "归档/根文件.log", 5, false));
  PRISM_CHECK(Refused(BuildEditPlan(collide, {Op(EditOpKind::Move, "根文件.log", "归档")}, Facts()),
                       EditRefusal::DuplicateName));
}

PRISM_TEST("read-only and multi-volume archives are refused with a reason") {
  EditArchiveFacts readOnly = Facts();
  readOnly.readOnly = true;
  const auto ro = BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "根文件.log")}, readOnly);
  PRISM_CHECK(Refused(ro, EditRefusal::ReadOnlyArchive));
  PRISM_CHECK(ro.error.find("read-only") != std::string::npos);

  EditArchiveFacts volumes = Facts();
  volumes.multiVolume = true;
  const auto mv = BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "根文件.log")}, volumes);
  PRISM_CHECK(Refused(mv, EditRefusal::MultiVolumeArchive));
  PRISM_CHECK(mv.error.find("multi-volume") != std::string::npos);

  // No operations is refused too: an empty request must not rewrite an archive.
  const auto none = BuildEditPlan(Catalog(), {}, Facts());
  PRISM_CHECK(Refused(none, EditRefusal::EmptyOperationList));
}

PRISM_TEST("solid and non-solid archives report different rewrite costs") {
  EditArchiveFacts plain = Facts();
  plain.solid = false;
  const auto plan = BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "资料/图片.bin")}, plain);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK(!plan.solidRewrite);
  PRISM_CHECK(!plan.ops[0].forcesSolidRewrite);
  PRISM_CHECK(plan.costNote.find("non-solid") != std::string::npos);
  const auto solid = BuildEditPlan(Catalog(), {Op(EditOpKind::Delete, "资料/图片.bin")}, Facts());
  PRISM_CHECK(solid.solidRewrite);
  PRISM_CHECK(solid.costNote.find("solid") != std::string::npos);
  PRISM_CHECK_EQ(solid.estimatedWriteBytes, solid.keptBytes);
}

PRISM_TEST("malformed catalog names refuse the edit instead of crashing") {
  // No 7z.dll can author these; a corrupt or hostile container can still report
  // them, and the model is the layer that must not hand them to the filesystem.
  const char* malformed[] = {"", ".", "a//b", "a/", "a\\b"};
  for (const char* name : malformed) {
    auto catalog = Catalog();
    catalog.push_back(Item(5, name, 16, false));
    const auto plan = BuildEditPlan(catalog, {Op(EditOpKind::Delete, "根文件.log")}, Facts());
    PRISM_CHECK(!plan.ok);
    PRISM_CHECK(!plan.error.empty());
    PRISM_CHECK(plan.refusal == EditRefusal::MalformedEntryName ||
                plan.refusal == EditRefusal::UnsafePath);
    PRISM_CHECK(plan.items.empty());
  }
  // ".." is a traversal, not just a bad name.
  auto traversal = Catalog();
  traversal.push_back(Item(5, "..", 16, false));
  PRISM_CHECK(Refused(BuildEditPlan(traversal, {Op(EditOpKind::Delete, "根文件.log")}, Facts()),
                       EditRefusal::UnsafePath));
  // A catalog item named "." must not crash the plan even when it is the ONLY
  // entry and the request would otherwise be a no-op.
  std::vector<ArchiveItem> single{Item(0, ".", 0, true)};
  const auto one = BuildEditPlan(single, {Op(EditOpKind::Delete, ".", "", true)}, Facts());
  PRISM_CHECK(!one.ok);
  PRISM_CHECK(!one.error.empty());
}

PRISM_TEST("a zero-byte entry survives every edit") {
  const EditOp ops[] = {Op(EditOpKind::Rename, "零字节.txt", "资料/零字节.txt"),
                        Op(EditOpKind::Move, "零字节.txt", "资料"),
                        Op(EditOpKind::NewFolder, "新目录")};
  for (const EditOp& op : ops) {
    const auto plan = BuildEditPlan(Catalog(), {op}, Facts());
    PRISM_CHECK(plan.ok);
    const std::size_t expectedCount =
        op.kind == EditOpKind::NewFolder ? std::size_t{6} : std::size_t{5};
    PRISM_CHECK_EQ(plan.items.size(), expectedCount);
    const std::string expected =
        op.kind == EditOpKind::NewFolder ? "零字节.txt" : "资料/零字节.txt";
    const auto* entry = Find(plan, expected);
    PRISM_CHECK(entry != nullptr);
    if (entry != nullptr) {
      PRISM_CHECK_EQ(entry->size, uint64_t{0});
      PRISM_CHECK(!entry->isDir);
    }
  }
}

PRISM_TEST("the plan is deterministic and sorted across two runs") {
  std::vector<EditOp> ops{Op(EditOpKind::NewFolder, "新目录"),
                          Op(EditOpKind::Rename, "根文件.log", "新目录/根文件.log"),
                          Op(EditOpKind::Delete, "资料/图片.bin")};
  const auto a = BuildEditPlan(Catalog(), ops, Facts());
  const auto b = BuildEditPlan(Catalog(), ops, Facts());
  PRISM_CHECK(a.ok);
  PRISM_CHECK_EQ(a.items.size(), std::size_t{5});
  PRISM_CHECK_EQ(a.items.size(), b.items.size());
  for (size_t i = 0; i < a.items.size() && i < b.items.size(); ++i) {
    PRISM_CHECK_EQ(a.items[i].path, b.items[i].path);
    PRISM_CHECK_EQ(a.items[i].isDir, b.items[i].isDir);
  }
  // Sorted by path: the writer receives the same list twice.
  for (size_t i = 1; i < a.items.size(); ++i) {
    PRISM_CHECK(a.items[i - 1].path < a.items[i].path);
  }
  PRISM_CHECK(Find(a, "新目录/根文件.log") != nullptr);
}

int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::printf("ALL EDIT-PLAN CHECKS PASSED\n");
  return 0;
}
