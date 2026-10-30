// PrismArchive -- PrismCore
// Copy-on-Write in-archive virtual directory tree (blueprint ch. 4.5).
//
// Lets the user create folders, rename and move entries directly inside an
// archive. All mutations stay in memory until the UI commits one transaction,
// which is translated into a single IOutArchive::UpdateItems() call where every
// untouched entry reports newData = 0 / newProps = 1 -- i.e. only the trailing
// central directory is rewritten, never the payload (a 10 GB archive rename
// costs milliseconds instead of a full rewrite).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"

namespace prism {

// What the update callback has to do for one entry.
struct UpdateItem {
  int64_t index = -1;     // archive index; -1 = brand new node (empty folder)
  std::string newPath;    // UTF-8, '/'-separated
  bool newData = false;   // payload must be recompressed
  bool newProps = true;   // metadata (path) changed
  bool removed = false;   // entry deleted from the archive
};

class VirtualArchiveTree {
 public:
  struct Node {
    std::string name;
    bool isDir = false;
    int64_t index = -1;  // -1 when this node was created in this session
    std::map<std::string, std::unique_ptr<Node>> children;
    Node* parent = nullptr;
    // Set on this node and every descendant by Rename/Move so the update plan
    // can emit metadata-only entries for the whole subtree.
    bool touched = false;
  };

  static VirtualArchiveTree FromCatalog(const std::vector<ArchiveItem>& items) {
    VirtualArchiveTree tree;
    for (const auto& item : items) {
      const std::string p = NormalizeArchivePath(item.path);
      if (p.empty()) continue;
      tree.Insert(p, item.isDir, item.index);
    }
    return tree;
  }

  bool dirty() const { return dirty_; }

  Node* Find(std::string_view path) {
    const std::string norm = NormalizeArchivePath(path);
    Node* cur = &root_;
    if (norm.empty()) return cur;
    size_t pos = 0;
    for (;;) {
      const size_t slash = norm.find('/', pos);
      const std::string part = (slash == std::string::npos)
                                   ? norm.substr(pos)
                                   : norm.substr(pos, slash - pos);
      auto it = cur->children.find(part);
      if (it == cur->children.end()) return nullptr;
      cur = it->second.get();
      if (slash == std::string::npos) break;
      pos = slash + 1;
    }
    return cur;
  }

  // Creates every missing intermediate folder as well.
  bool Mkdir(std::string_view path) {
    const std::string norm = NormalizeArchivePath(path);
    if (norm.empty()) return false;
    Node* cur = &root_;
    size_t pos = 0;
    for (;;) {
      const size_t slash = norm.find('/', pos);
      const std::string part = (slash == std::string::npos)
                                   ? norm.substr(pos)
                                   : norm.substr(pos, slash - pos);
      if (part.empty()) return false;
      auto it = cur->children.find(part);
      if (it == cur->children.end()) {
        auto node = std::make_unique<Node>();
        node->name = part;
        node->isDir = true;
        node->parent = cur;
        Node* inserted = node.get();
        cur->children.emplace(part, std::move(node));
        cur = inserted;
        dirty_ = true;
      } else {
        cur = it->second.get();
      }
      if (slash == std::string::npos) break;
      pos = slash + 1;
    }
    cur->isDir = true;
    return true;
  }

  bool Rename(std::string_view from, std::string_view to) {
    Node* src = Find(from);
    const std::string target = NormalizeArchivePath(to);
    if (!src || target.empty() || src == &root_) return false;
    if (Find(target)) return false;  // refuse silent overwrite
    if (src->parent == nullptr) return false;

    const std::string newName = std::string(FileNameOf(target));
    auto handler = src->parent->children.extract(src->name);
    if (handler.empty()) return false;
    src->name = newName;
    handler.key() = newName;  // std::map::extract keeps the old key otherwise
    src->parent->children.insert(std::move(handler));
    MarkSubtree(src);
    dirty_ = true;
    return true;
  }

  bool Move(std::string_view from, std::string_view toDir) {
    Node* src = Find(from);
    Node* dstDir = Find(toDir);
    if (!src || !dstDir || src == &root_ || src->parent == nullptr) return false;
    if (!dstDir->isDir) return false;
    if (src->parent == dstDir) return false;
    if (dstDir->children.count(src->name)) return false;

    auto handler = src->parent->children.extract(src->name);
    if (handler.empty()) return false;
    Node* moved = handler.mapped().get();
    moved->parent = dstDir;
    dstDir->children.insert(std::move(handler));
    MarkSubtree(moved);
    dirty_ = true;
    return true;
  }

  bool Erase(std::string_view path) {
    Node* node = Find(path);
    if (!node || node == &root_ || node->parent == nullptr) return false;
    CollectIndices(node, &erasedIndices_);
    node->parent->children.erase(node->name);
    dirty_ = true;
    return true;
  }

  // Metadata-only transaction plan fed to IOutArchive::UpdateItems().
  std::vector<UpdateItem> MakeUpdatePlan() const {
    std::vector<UpdateItem> plan;
    for (const uint32_t idx : erasedIndices_) {
      UpdateItem item;
      item.index = idx;
      item.removed = true;
      item.newProps = false;
      plan.push_back(item);
    }
    Walk(&root_, "", plan);
    return plan;
  }

  const std::vector<uint32_t>& erasedIndices() const { return erasedIndices_; }

 private:
  void Insert(const std::string& normalizedPath, bool isDir, int64_t index) {
    Node* cur = &root_;
    size_t pos = 0;
    for (;;) {
      const size_t slash = normalizedPath.find('/', pos);
      const std::string part = (slash == std::string::npos)
                                   ? normalizedPath.substr(pos)
                                   : normalizedPath.substr(pos, slash - pos);
      auto it = cur->children.find(part);
      if (it == cur->children.end()) {
        auto node = std::make_unique<Node>();
        node->name = part;
        node->parent = cur;
        Node* inserted = node.get();
        cur->children.emplace(part, std::move(node));
        cur = inserted;
      } else {
        cur = it->second.get();
      }
      if (slash == std::string::npos) break;
      pos = slash + 1;
    }
    cur->isDir = isDir;
    cur->index = index;
  }

  void MarkSubtree(Node* node) {
    node->touched = true;
    for (auto& kv : node->children) MarkSubtree(kv.second.get());
  }

  void CollectIndices(Node* node, std::vector<uint32_t>* out) {
    if (node->index >= 0) out->push_back(static_cast<uint32_t>(node->index));
    for (auto& kv : node->children) CollectIndices(kv.second.get(), out);
  }

  void Walk(const Node* node, const std::string& prefix, std::vector<UpdateItem>& plan) const {
    for (const auto& kv : node->children) {
      const Node* child = kv.second.get();
      const std::string path = prefix.empty() ? child->name : JoinPath(prefix, child->name);
      if (child->index >= 0 && child->touched) {
        UpdateItem item;
        item.index = child->index;
        item.newPath = path;
        item.newData = false;
        item.newProps = true;
        plan.push_back(item);
      } else if (child->index < 0) {
        UpdateItem item;
        item.index = -1;
        item.newPath = path;
        item.newData = false;
        item.newProps = true;
        plan.push_back(item);
      }
      Walk(child, path, plan);
    }
  }

  Node root_;
  bool dirty_ = false;
  std::vector<uint32_t> erasedIndices_;
};

}  // namespace prism
