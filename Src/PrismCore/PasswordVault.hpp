// PrismArchive -- PrismCore
// Smart Password Ring: candidate sniffing + vault interface (blueprint ch. 4.5,
// ch. 8.2).
//
// The *policy* lives here (platform-free, unit-tested); the *storage* is
// platform-specific: Windows uses DPAPI (Src/PrismWin/DpapiVault.hpp), other
// platforms get the deliberately-not-secure FileVault used by the test suite.
#pragma once

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "PathSafety.hpp"

namespace prism {

// Ordered, de-duplicated candidate passwords for one archive.
inline std::vector<std::string> ExtractPasswordCandidates(
    const std::string& archivePath, const std::string& archiveComment = {},
    const std::vector<std::string>& siblingTexts = {}) {
  // NOTE: kept as a byte-oriented regex on purpose -- source strings are UTF-8
  // and the keywords include CJK bytes, which std::regex must match literally.
  // Alternatives are ordered longest-first: ECMAScript alternation is
  // leftmost-first, so "pass" would otherwise swallow "password:" and capture
  // the keyword tail ("word:") as if it were the secret.
  static const std::regex kPattern(
      "(?:(?:解压码|提取码|密码|口令|暗号)|(?:password|passwd|pass|pwd|pw|code))"
      "[:：=\\s_\\-\\[\\]{}()（）【】]*"
      // '.' is excluded on purpose: otherwise "素材包_密码123456.7z" captures
      // the file extension as part of the password.
      "([A-Za-z0-9!@#$%^&*_+\\-=\\[\\]{};:,?~/`｜|]{3,32})",
      std::regex_constants::icase);

  std::vector<std::string> out;
  auto scan = [&out](const std::string& text) {
    if (text.empty()) return;
    std::sregex_iterator it(text.begin(), text.end(), kPattern);
    const std::sregex_iterator end;
    for (; it != end; ++it) {
      if (it->size() > 1) out.push_back((*it)[1].str());
    }
  };

  const std::string fileName = std::string(FileNameOf(NormalizeArchivePath(archivePath)));
  scan(fileName);
  scan(archiveComment);
  for (const std::string& text : siblingTexts) scan(text);

  // Very common "the password is just the archive name" case.
  const std::string stem = ArchiveStem(archivePath);
  if (!stem.empty()) out.push_back(stem);

  std::vector<std::string> deduped;
  for (std::string& c : out) {
    // Trim trailing punctuation that the greedy regex swallows -- including the
    // UTF-8 bytes of the CJK full stop (E3 80 82), which is not a single char.
    auto trimTail = [&c]() {
      while (!c.empty()) {
        const char last = c.back();
        if (last == ')' || last == ']' || last == '}') {
          c.pop_back();
        } else if (c.size() >= 3 && c.compare(c.size() - 3, 3, "\xe3\x80\x82") == 0) {
          c.erase(c.size() - 3);
        } else {
          break;
        }
      }
    };
    trimTail();
    if (c.size() < 3) continue;
    if (std::find(deduped.begin(), deduped.end(), c) == deduped.end()) {
      deduped.push_back(std::move(c));
    }
  }
  return deduped;
}

class IPasswordVault {
 public:
  virtual ~IPasswordVault() = default;
  virtual std::vector<std::string> List() const = 0;
  virtual bool Add(const std::string& password) = 0;
  virtual bool Remove(const std::string& password) = 0;
  virtual const char* Backend() const = 0;
};

// In-memory vault: used by tests and by the "do not persist" privacy mode.
class MemoryVault final : public IPasswordVault {
 public:
  std::vector<std::string> List() const override { return entries_; }
  bool Add(const std::string& password) override {
    if (password.empty()) return false;
    if (std::find(entries_.begin(), entries_.end(), password) != entries_.end()) return false;
    entries_.push_back(password);
    return true;
  }
  bool Remove(const std::string& password) override {
    const auto it = std::find(entries_.begin(), entries_.end(), password);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
  }
  const char* Backend() const override { return "memory"; }

 private:
  std::vector<std::string> entries_;
};

// SECURITY: plaintext fallback for non-Windows platforms only. It exists so the
// resolver policy can be tested everywhere; the shipping Windows build uses
// DpapiVault (CryptProtectData, per-user key). Never ship this on by default.
class FileVault final : public IPasswordVault {
 public:
  explicit FileVault(std::string path) : path_(std::move(path)) { Load(); }

  std::vector<std::string> List() const override { return entries_; }

  bool Add(const std::string& password) override {
    if (password.empty()) return false;
    if (std::find(entries_.begin(), entries_.end(), password) != entries_.end()) return false;
    entries_.push_back(password);
    return Save();
  }

  bool Remove(const std::string& password) override {
    const auto it = std::find(entries_.begin(), entries_.end(), password);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return Save();
  }

  const char* Backend() const override { return "file(NOT ENCRYPTED - testing only)"; }

 private:
  void Load() {
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) entries_.push_back(line);
    }
  }
  bool Save() {
    std::ofstream out(path_, std::ios::trunc);
    if (!out) return false;
    for (const std::string& e : entries_) out << e << '\n';
    return true;
  }

  std::string path_;
  std::vector<std::string> entries_;
};

// Builds the ordered collision list handed to the concurrent password prober:
// context-derived guesses first (they are right far more often), then the vault.
inline std::vector<std::string> BuildCandidateList(const IPasswordVault& vault,
                                                   const std::string& archivePath,
                                                   const std::string& archiveComment = {},
                                                   const std::vector<std::string>& siblings = {},
                                                   size_t maxCandidates = 64) {
  std::vector<std::string> out =
      ExtractPasswordCandidates(archivePath, archiveComment, siblings);
  for (const std::string& p : vault.List()) {
    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  }
  if (out.size() > maxCandidates) out.resize(maxCandidates);
  return out;
}

}  // namespace prism
