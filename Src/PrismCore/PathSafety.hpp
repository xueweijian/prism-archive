// PrismArchive -- PrismCore
// Archive path normalization + Zip Slip / traversal hardening (blueprint ch. 7.1-2).
// Purely lexical: no filesystem access, so it is unit-testable everywhere.
#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

inline std::string NormalizeArchivePath(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  bool prevSlash = false;
  for (char c : raw) {
    if (c == '\\') c = '/';
    if (c == '/') {
      if (prevSlash || out.empty()) continue;  // collapse '//' and drop leading '/'
      prevSlash = true;
    } else {
      prevSlash = false;
    }
    out.push_back(c);
  }
  while (!out.empty() && out.back() == '/') out.pop_back();
  return out;
}

inline std::vector<std::string> SplitPath(std::string_view normalized) {
  std::vector<std::string> parts;
  size_t pos = 0;
  for (;;) {
    const size_t slash = normalized.find('/', pos);
    if (slash == std::string_view::npos) {
      if (pos < normalized.size()) parts.emplace_back(normalized.substr(pos));
      break;
    }
    if (slash > pos) parts.emplace_back(normalized.substr(pos, slash - pos));
    pos = slash + 1;
  }
  return parts;
}

inline std::string JoinPath(std::string_view a, std::string_view b) {
  if (a.empty()) return std::string(b);
  if (b.empty()) return std::string(a);
  std::string out(a);
  if (out.back() != '/') out.push_back('/');
  out.append(b);
  return out;
}

// Leading path component ("" when the path has no separator).
inline std::string_view RootSegment(std::string_view normalized) {
  const size_t slash = normalized.find('/');
  return (slash == std::string_view::npos) ? normalized : normalized.substr(0, slash);
}

inline std::string_view FileNameOf(std::string_view normalized) {
  const size_t slash = normalized.rfind('/');
  return (slash == std::string_view::npos) ? normalized : normalized.substr(slash + 1);
}

// Mac/Windows Explorer droppings that must never be counted as real content.
inline bool IsNoiseEntry(std::string_view normalized) {
  const std::string_view name = FileNameOf(normalized);
  if (normalized == "__MACOSX" || normalized.rfind("__MACOSX/", 0) == 0) return true;
  if (name == ".DS_Store" || name == "Thumbs.db" || name == "desktop.ini") return true;
  if (name.rfind("._", 0) == 0 && name.size() > 2) return true;  // AppleDouble forks
  return false;
}

// Promotional junk that ships inside pirated/bundled archives (.url/.htm ads).
inline bool IsPromoNoise(std::string_view normalized) {
  const std::string_view name = FileNameOf(normalized);
  if (name.empty()) return false;
  std::string lower(name);
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t dot = lower.rfind('.');
  const std::string ext = (dot == std::string::npos) ? std::string() : lower.substr(dot + 1);
  // .exe is deliberately NOT in this list: in a real archive "setup.exe" is
  // usually the payload, and dropping it silently would destroy data.
  if (ext == "url" || ext == "htm" || ext == "html" || ext == "nfo") {
    // Only treat as promo when the name smells like an ad, not like real content.
    const bool adName = lower.find("download") != std::string::npos ||
                        lower.find("visit") != std::string::npos ||
                        lower.find("homepage") != std::string::npos ||
                        lower.find("buy") != std::string::npos ||
                        lower.find("read") != std::string::npos ||
                        lower.find("install") != std::string::npos ||
                        lower.find("setup") != std::string::npos;
    if (adName) return true;
  }
  // Deliberately NOT classified as promo: README / 说明 / changelog are real
  // content in most archives, and silently dropping them destroys data.
  return false;
}

// Zip Slip defence (blueprint ch. 7.1): drop '..'/'.'/drive letters/control
// characters and rebuild a strictly relative, '/'-separated path.
inline std::string SanitizeRelativePath(std::string_view raw) {
  const std::string rel = NormalizeArchivePath(raw);
  // Stack resolution: ".." pops the previous segment, and a ".." that would
  // escape above the archive root is dropped instead of honoured.
  std::vector<std::string> stack;
  for (const std::string& part : SplitPath(rel)) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!stack.empty()) stack.pop_back();
      continue;
    }
    if (part.find(':') != std::string::npos) continue;  // C: / \\?\ injection
    if (part.find('\0') != std::string::npos) continue;
    bool control = false;
    for (const char c : part) {
      if (static_cast<unsigned char>(c) < 0x20) {
        control = true;
        break;
      }
    }
    if (control) continue;
    stack.push_back(part);
  }
  std::string out;
  for (const std::string& part : stack) out = out.empty() ? part : JoinPath(out, part);
  return out;
}

// Second-stage guard: every written path must still resolve under baseDir.
inline bool IsContainedIn(std::string_view baseDir, std::string_view targetPath) {
  const std::string base = NormalizeArchivePath(baseDir);
  const std::string target = NormalizeArchivePath(targetPath);
  if (base.empty()) return false;
  if (target.size() < base.size()) return false;
  if (target.compare(0, base.size(), base) != 0) return false;
  if (target.size() == base.size()) return true;
  return target[base.size()] == '/';
}

// "Design_Assets_v4.tar.gz" -> "Design_Assets_v4" (also handles .tar.zst/.tar.bz2)
inline std::string ArchiveStem(std::string_view archivePath) {
  const std::string_view name = FileNameOf(NormalizeArchivePath(archivePath));
  std::string stem(name);
  for (const char* compound : {".tar.gz", ".tar.zst", ".tar.bz2", ".tar.xz", ".tar.lz4", ".tar"}) {
    const std::string suffix(compound);
    if (stem.size() > suffix.size()) {
      std::string tail = stem.substr(stem.size() - suffix.size());
      for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (tail == suffix) {
        stem.erase(stem.size() - suffix.size());
        break;
      }
    }
  }
  const size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0) stem.erase(dot);
  return stem;
}

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace prism
