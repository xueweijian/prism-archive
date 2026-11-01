// PrismArchive -- i18n KEY AUDIT (card C38).
//
// WHY THIS FILE EXISTS
// C19 proved the tables agree with EACH OTHER and with RequiredKeys(). C27 then
// wired the menu/status/error text through Tr::Or(), and reported three keys
// (menu.file / action.exit / progress.compressed) that no table defined --
// because Or() takes a code default and therefore records NO MISS. The result
// was exactly the failure a translation gate exists to prevent: a hardcoded
// English menu bar in a Chinese build, invisible to every test we had.
//
// This suite closes the other direction of the same gate: it reads the SOURCE,
// extracts every i18n key literal passed to a translator call site, and asserts
// that BOTH shipped tables define it. RequiredKeys() can only check the keys a
// human remembered to list; this checks the keys the code actually asks for.
//
// WHAT IS ASSERTED (and what is only REPORTED)
//   * the scan is real: >= kMinSourceFiles files were read and >= kMinUsedKeys
//     distinct keys were found. A path typo or a renamed helper turns this suite
//     RED instead of vacuously green (D1: a suite that cannot SKIP must not be
//     able to pass while reading nothing);
//   * every used key exists in en-US AND in zh-CN (reported separately, so the
//     failure names the table a translator has to touch);
//   * the two tables define the SAME key set (C19's assertion, re-checked here
//     from the parsed catalogs so a hand-edited table cannot slip through);
//   * every literal handed to these helpers is SPELLED like a key (lowercase
//     dotted ASCII). A sentence passed to Tr() is a defect too: it would look
//     up a garbage key and render the sentence as a "key" -- ugly in the same
//     way, and impossible to translate;
//   * REPORT ONLY: table keys that no call site uses. Not a failure -- the
//     right-click menu keys are resolved through ContextMenuModel's verbs, and
//     a key can legitimately land before the UI code that consumes it. The list
//     is printed every run so it stays visible.
//
// CONTRACTS: D1 (stdout marker is part of the verdict; the asset and source
// directories are COMPILE-TIME inputs, so "read nothing" cannot pass), D2 (the
// assertions are about tables and source bytes, not about a file existing), D3
// + D26 (the negative control is its own build: a key NO table defines is
// injected into the audit, and the marker is a CONJUNCTION -- it prints only if
// the clean pass is green AND the injected key is named as missing in both
// tables. No WILL_FAIL + regex pair anywhere: that shape is always red).
//
// The scan is a HEURISTIC over three call shapes (tr("k"), tr.Or/Count("k"),
// Translate/TranslateCount("k")). It proves "every key-shaped literal reaching
// these helpers exists", not "every string in the app is a key"; the hardcoded
// strings that never reach a helper are listed in docs/I18N-AUDIT.md, which is
// the inventory C34 wires against.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "I18n.hpp"
#include "mini_test.hpp"

#if !defined(PRISM_C38_LANG_DIR)
#error "PRISM_C38_LANG_DIR must name Assets/Lang (see Tests/suites/c38-i18n2.cmake)"
#endif
#if !defined(PRISM_C38_SOURCE_DIR)
#error "PRISM_C38_SOURCE_DIR must name the repository root (see Tests/suites/c38-i18n2.cmake)"
#endif

namespace {

using prism::i18n::Catalog;

// A shrink guard, not a census: Src/ ships 66 files and ~16 distinct keys today.
// If either number drops below this, the SCAN broke -- update the constant in the
// same commit that legitimately removes files/keys.
constexpr std::size_t kMinSourceFiles = 20;
constexpr std::size_t kMinUsedKeys = 12;

// NEGATIVE CONTROL (CONTRACTS D3). This key exists in no table, in no source
// file: it is injected into the used-key set of the mutant build only, so the
// audit MUST name it and go red.
#if defined(PRISM_C38_MUTANT_FAKE_KEY)
constexpr bool kMutantBuild = true;
constexpr std::string_view kBogusKey = "menu.file.typo.control";
#else
constexpr bool kMutantBuild = false;
constexpr std::string_view kBogusKey = "";
#endif

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::string();
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// Removes // and /* */ comments while PRESERVING newlines, so a line number
// stays valid and a key quoted in prose (this project documents keys in
// comments constantly) cannot masquerade as a call site.
std::string StripComments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool inLine = false;
  bool inBlock = false;
  bool inString = false;
  bool inChar = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (inLine) {
      if (c == '\n') {
        inLine = false;
        out.push_back('\n');
      }
      continue;
    }
    if (inBlock) {
      if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
        inBlock = false;
        ++i;
      } else if (c == '\n') {
        out.push_back('\n');  // keep the line count honest
      }
      continue;
    }
    if (inString || inChar) {
      out.push_back(c);
      if (c == '\\' && i + 1 < text.size()) {
        out.push_back(text[++i]);
        continue;
      }
      if (inString && c == '"') inString = false;
      if (inChar && c == '\'') inChar = false;
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      inLine = true;
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      inBlock = true;
      ++i;
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '\'') {
      inChar = true;
    }
    out.push_back(c);
  }
  return out;
}

// A resource key: lowercase-ish dotted ASCII (status.notEncrypted keeps a camel
// hump, menu.compress.to.7z keeps a digit). No spaces, no "..", no leading or
// trailing dot.
bool IsKeyShaped(std::string_view key) {
  if (key.size() < 3) return false;
  if (key.front() == '.' || key.back() == '.') return false;
  if (key.find('.') == std::string_view::npos) return false;
  bool previousWasDot = false;
  for (const char c : key) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!allowed) return false;
    if (c == '.') {
      if (previousWasDot) return false;
      previousWasDot = true;
    } else {
      previousWasDot = false;
    }
  }
  return true;
}

std::string RelativeToRoot(const std::filesystem::path& path) {
  const std::filesystem::path root(PRISM_C38_SOURCE_DIR);
  std::error_code ec;
  const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
  return ec ? path.filename().string() : relative.generic_string();
}

std::vector<std::filesystem::path> SourceFiles() {
  std::vector<std::filesystem::path> files;
  const std::filesystem::path root = std::filesystem::path(PRISM_C38_SOURCE_DIR) / "Src";
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return files;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    std::error_code fileEc;
    if (!it->is_regular_file(fileEc)) continue;
    const std::string extension = it->path().extension().string();
    if (extension != ".hpp" && extension != ".cpp") continue;
    files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

struct CallSite {
  std::string key;
  std::string where;  // "Src/App/Main.cpp:541"
};

std::size_t LineOf(std::string_view text, std::size_t offset) {
  std::size_t line = 1;
  for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') ++line;
  }
  return line;
}

struct Scan {
  std::size_t files = 0;
  std::size_t literals = 0;
  std::vector<CallSite> sites;             // one per literal occurrence
  std::vector<std::string> notKeyShaped;   // "Src/...:line  literal"
};

Scan ScanSource() {
  // The call shapes this codebase uses (plus Tr::Sub, the placeholder-carrying
  // shape the C34 wiring patch introduces). Keeping them explicit is the point:
  // a new helper has to be added HERE, which is where the reviewer looks --
  // otherwise its keys would silently escape the audit.
  static const std::regex patterns[] = {
      std::regex(R"re(\btr\s*\.\s*(?:Or|Count|Sub)\s*\(\s*"([^"]*)")re"),
      std::regex(R"re(\btr\s*\(\s*"([^"]*)")re"),
      std::regex(R"re(\b(?:Translate|TranslateCount)\s*\(\s*"([^"]*)")re"),
  };
  Scan scan;
  for (const std::filesystem::path& path : SourceFiles()) {
    ++scan.files;
    const std::string raw = ReadWholeFile(path);
    if (raw.empty()) continue;
    const std::string text = StripComments(raw);
    std::vector<std::pair<std::size_t, std::string>> hits;
    std::vector<std::pair<std::size_t, std::string>> bad;
    for (const std::regex& pattern : patterns) {
      for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        const std::string literal = (*it)[1].str();
        ++scan.literals;
        if (IsKeyShaped(literal)) {
          hits.emplace_back(static_cast<std::size_t>((*it).position(1)), literal);
        } else {
          bad.emplace_back(static_cast<std::size_t>((*it).position(1)), literal);
        }
      }
    }
    std::sort(hits.begin(), hits.end());
    for (const std::pair<std::size_t, std::string>& hit : hits) {
      scan.sites.push_back(CallSite{hit.second, RelativeToRoot(path) + ":" +
                                        std::to_string(LineOf(text, hit.first))});
    }
    for (const std::pair<std::size_t, std::string>& literal : bad) {
      scan.notKeyShaped.push_back(RelativeToRoot(path) + ":" +
                                  std::to_string(LineOf(text, literal.first)) + "  \"" +
                                  literal.second + "\"");
    }
  }
  return scan;
}

struct AuditResult {
  std::size_t files = 0;
  std::size_t literals = 0;
  std::size_t injected = 0;
  std::vector<std::string> used;        // sorted, unique
  std::vector<std::string> missingEn;   // "key  (first asked for at path:line)"
  std::vector<std::string> missingZh;
  std::vector<std::string> notKeyShaped;
  std::vector<std::string> onlyInEn;
  std::vector<std::string> onlyInZh;
  std::vector<std::string> unused;      // REPORT ONLY

  bool clean() const {
    return files >= kMinSourceFiles && used.size() >= kMinUsedKeys && missingEn.empty() &&
           missingZh.empty() && notKeyShaped.empty() && onlyInEn.empty() && onlyInZh.empty();
  }
};

std::string LangPath(const char* file) {
  return std::string(PRISM_C38_LANG_DIR) + "/" + file;
}

bool Contains(const std::vector<std::string>& haystack, std::string_view needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

// One audit run. "injectBogusKey" is what the mutant build turns on; the normal
// build always passes false (there is no runtime switch, so a shipping binary
// cannot be talked into auditing a fake key).
AuditResult RunAudit(bool injectBogusKey) {
  AuditResult result;
  Catalog en;
  Catalog zh;
  std::string error;
  const bool haveEn = en.LoadFromFile(LangPath("en-US.json"), &error);
  if (!haveEn) result.missingEn.push_back("the en-US table could not be loaded: " + error);
  error.clear();
  const bool haveZh = zh.LoadFromFile(LangPath("zh-CN.json"), &error);
  if (!haveZh) result.missingZh.push_back("the zh-CN table could not be loaded: " + error);

  const Scan scan = ScanSource();
  result.files = scan.files;
  result.literals = scan.literals;
  result.notKeyShaped = scan.notKeyShaped;

  for (const CallSite& site : scan.sites) {
    if (!Contains(result.used, site.key)) result.used.push_back(site.key);
  }
  if (injectBogusKey) {
    // The injected key is a well-formed resource key that no table defines, so
    // it exercises exactly the assertion a real missing key would trip.
    result.used.push_back(std::string(kBogusKey));
    result.injected = 1;
  }
  std::sort(result.used.begin(), result.used.end());
  // First call site per key, for the failure line ("first asked for at ...").
  for (std::size_t i = 0; i < result.used.size(); ++i) {
    std::string where = "unknown";
    for (const CallSite& site : scan.sites) {
      if (site.key == result.used[i]) {
        where = site.where;
        break;
      }
    }
    if (result.used[i] == kBogusKey) {
      where = "injected by the C38 mutant build (there is no such call site)";
    }
    if (haveEn && en.Find(result.used[i]) == nullptr) {
      result.missingEn.push_back(result.used[i] + "  (first asked for at " + where + ")");
    }
    if (haveZh && zh.Find(result.used[i]) == nullptr) {
      result.missingZh.push_back(result.used[i] + "  (first asked for at " + where + ")");
    }
  }

  // Same key set in both tables, checked from the PARSED catalogs (C19 asserts
  // it too; this is the copy that runs next to the source audit).
  for (const prism::i18n::StringEntry& entry : en.entries()) {
    if (zh.Find(entry.key) == nullptr) result.onlyInEn.push_back(entry.key);
  }
  for (const prism::i18n::StringEntry& entry : zh.entries()) {
    if (en.Find(entry.key) == nullptr) result.onlyInZh.push_back(entry.key);
  }
  // REPORT ONLY: table keys no call site asks for.
  for (const prism::i18n::StringEntry& entry : en.entries()) {
    if (!Contains(result.used, entry.key)) result.unused.push_back(entry.key);
  }
  std::sort(result.onlyInEn.begin(), result.onlyInEn.end());
  std::sort(result.onlyInZh.begin(), result.onlyInZh.end());
  std::sort(result.unused.begin(), result.unused.end());
  return result;
}

const AuditResult& CachedAudit() {
  static const AuditResult result = RunAudit(false);
  return result;
}

void ReportList(const char* what, const std::vector<std::string>& items, const char* file,
                int line) {
  if (items.empty()) return;
  std::cout << "  " << what << " (" << items.size() << "):" << std::endl;
  for (const std::string& item : items) std::cout << "    - " << item << std::endl;
  prism::test::ReportFailure(file, line,
                             std::string(what) + " -- " + std::to_string(items.size()) +
                                 " entr(y/ies), listed above");
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. The scan itself. If this goes red, every assertion below is meaningless.
// ---------------------------------------------------------------------------
PRISM_TEST("scan: the source tree was really read, and the C38 keys were found") {
  const AuditResult& audit = CachedAudit();
  std::cout << "  scanned " << audit.files << " files, " << audit.literals
            << " key literals, " << audit.used.size() << " distinct keys" << std::endl;
  PRISM_CHECK(audit.files >= kMinSourceFiles);
  PRISM_CHECK(audit.used.size() >= kMinUsedKeys);
  // The three keys C27 shipped as code defaults -- the subject of this card.
  // If the scan loses them, the audit has stopped covering what it was built for.
  PRISM_CHECK(Contains(audit.used, "menu.file"));
  PRISM_CHECK(Contains(audit.used, "action.exit"));
  PRISM_CHECK(Contains(audit.used, "progress.compressed"));
}

// ---------------------------------------------------------------------------
// 2. The audit proper: used keys versus each shipped table.
// ---------------------------------------------------------------------------
PRISM_TEST("audit: every key literal in the source exists in Assets/Lang/en-US.json") {
  const AuditResult& audit = CachedAudit();
  ReportList("keys asked for by the source but absent from en-US", audit.missingEn, __FILE__,
             __LINE__);
  PRISM_CHECK(audit.missingEn.empty());
}

PRISM_TEST("audit: every key literal in the source exists in Assets/Lang/zh-CN.json") {
  const AuditResult& audit = CachedAudit();
  ReportList("keys asked for by the source but absent from zh-CN", audit.missingZh, __FILE__,
             __LINE__);
  PRISM_CHECK(audit.missingZh.empty());
}

PRISM_TEST("audit: the two tables define the same key set") {
  const AuditResult& audit = CachedAudit();
  ReportList("keys only in en-US", audit.onlyInEn, __FILE__, __LINE__);
  ReportList("keys only in zh-CN", audit.onlyInZh, __FILE__, __LINE__);
  PRISM_CHECK(audit.onlyInEn.empty());
  PRISM_CHECK(audit.onlyInZh.empty());
}

PRISM_TEST("audit: every literal handed to a translator is spelled like a key") {
  const AuditResult& audit = CachedAudit();
  ReportList("literals that are not key-shaped (a sentence cannot be translated)", 
             audit.notKeyShaped, __FILE__, __LINE__);
  PRISM_CHECK(audit.notKeyShaped.empty());
}

// ---------------------------------------------------------------------------
// 3. Report-only direction: table keys no call site uses. Not a failure --
//    the right-click menu keys resolve through ContextMenuModel's verbs -- but
//    printed every run so dead weight cannot hide.
// ---------------------------------------------------------------------------
PRISM_TEST("audit: report-only -- table keys with no call site") {
  const AuditResult& audit = CachedAudit();
  Catalog en;
  std::string error;
  PRISM_CHECK(en.LoadFromFile(LangPath("en-US.json"), &error));
  if (en.size() == 0) return;

  std::cout << "  REPORT-ONLY: " << audit.unused.size() << " of " << en.size()
            << " table keys have no tr.*/Translate() call site:" << std::endl;
  for (const std::string& key : audit.unused) std::cout << "    - " << key << std::endl;

  // The partition must add up: every table key is either asked for by a call
  // site or on this report. If it is in neither, the audit lost a key.
  const std::size_t usedInTable = audit.used.size() - audit.missingEn.size();
  PRISM_CHECK_EQ(usedInTable + audit.unused.size(), en.size());
}

int main(int argc, char** argv) {
  if constexpr (!kMutantBuild) {
    const int failures = prism::test::RunAll(argc, argv);
    // D1: the marker IS the verdict. It prints only on a clean run, so a crash,
    // a silent skip or an early return cannot leave a regex-matching log line
    // behind while the assertions were never evaluated.
    if (failures == 0) std::cout << "ALL I18N AUDIT CHECKS PASSED" << std::endl;
    return failures;
  } else {
    // NEGATIVE CONTROL (CONTRACTS D3 + D26): the marker is a CONJUNCTION.
    // It prints only when (a) the injection actually landed (injected == 1),
    // (b) the CLEAN pass is green -- so an always-red audit cannot look caught,
    // and (c) the bogus key is named as missing in BOTH tables. A WILL_FAIL +
    // PASS_REGULAR_EXPRESSION pair would be permanently red on this CTest
    // version (D26); a plain WILL_FAIL would go green on a crash (a crash also
    // exits non-zero). This shape has neither hole.
    const AuditResult clean = RunAudit(false);
    const AuditResult mutated = RunAudit(true);
    std::cout << "C38 MUTANT ACTIVE: fake-key (injected=" << mutated.injected
              << ", cleanMissing=" << (clean.missingEn.size() + clean.missingZh.size()) << ")"
              << std::endl;
    const bool cleanGreen = clean.clean();
    bool named = false;
    for (const std::string& entry : mutated.missingEn) {
      if (entry.rfind(std::string(kBogusKey), 0) == 0) named = true;
    }
    bool namedZh = false;
    for (const std::string& entry : mutated.missingZh) {
      if (entry.rfind(std::string(kBogusKey), 0) == 0) namedZh = true;
    }
    if (cleanGreen && named && namedZh && mutated.injected == 1) {
      std::cout << "C38 MUTANT CAUGHT: " << kBogusKey << " is in no shipped table" << std::endl;
      return 0;
    }
    std::cout << "C38 MUTANT NOT CAUGHT: cleanGreen=" << cleanGreen << " namedEn=" << named
              << " namedZh=" << namedZh << " injected=" << mutated.injected << std::endl;
    return 1;
  }
}
