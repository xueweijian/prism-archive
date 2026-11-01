// PrismArchive -- the assembly layer (cards C25 wave 2 + C27 wave 3).
//
// WHY THIS FILE EXISTS
//   Fourteen modules landed on main with their own tests, but nothing in the
//   EXE used them: Main.cpp still shared one ArchiveReader between the sweep
//   and the extract, never stamped :Zone.Identifier, never asked AMSI, kept a
//   hand-written key chain in WndProc, and ignored every context-menu flag the
//   shell DLLs pass (SPARSE-PackageRegistrar's ArgsForVerb table).
//
//   This header is the wiring, and it is deliberately boring: one small
//   function or class per module, no policy of its own. Every decision stays in
//   PrismCore (Linux-tested); every system call stays in PrismWin. What lives
//   here is the ORDER and the OWNERSHIP -- which is where assembly bugs live,
//   which is why the Windows suite (Tests/win/test_app_wiring.cpp) drives THIS
//   file against a real HWND and a real 7z.dll instead of re-implementing it.
//
// WHAT CARD C27 ADDED (wave 3 -- this is what C25 left open):
//   * C9 (archive creation): section 6. ParseLaunch now understands
//     --compress-here, PlanCompression turns paths into a plan, RunCompression
//     drives ArchiveWriter::CreateArchive with a progress callback.
//   * C14 (settings): sections 7 and 8. LoadSettings/SaveSettings join the
//     document to its file, SettingsSidebarStore is the SidebarStore port the
//     C12 model asks for, and WindowState persists size/view/sort/columns.
//   * C19 (language): section 7. LoadLanguageFor loads the tables, resolves
//     settings.language.tag (or the system locale), and Tr is the two call
//     shapes the app uses -- a missing key returns itself and is logged once.
//   * C12 draw commands into pixels: GalleryRenderer::DrawSidebar (PrismWin).
//
// WHAT IS STILL *NOT* WIRED (deliberate, and stated in the PR):
//   * The shell's --compress-7z / --compress-zip verbs. C25's suite PINS the
//     refusal of those two flags; flipping them here would turn another card's
//     green test red, so --compress-here carries the same path instead.
//   * Re-packing an OPEN archive into a new one is a two-step job (extract to
//     %TEMP%, then create) and the window keeps showing the archive it opened;
//     the refresh it performs re-reads that archive. Switching the open archive
//     needs the handler pool rebuilt against the new file -- follow-up work.
//
// THREADING (CONTRACTS B9 + HANDOVER 5)
//   * A worker never touches window state; it only reaches the window through
//     Post* helpers (see Main.cpp).
//   * OperationHandles IS internally synchronized (a mutex around the
//     scheduler). ExtractScheduler itself is deliberately unsynchronized and
//     documents "the owner serializes every call" -- this class is that owner,
//     for every caller thread, so a worker may lease its own handler.
//   * ReaderPool leases may outlive the pool object; every lease is released by
//     the task that acquired it, and the app joins every worker before the
//     window (a stack object) leaves scope.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/CompressionPlan.hpp"   // C9 decision layer (what to write)
#include "../PrismCore/I18n.hpp"             // C19 resource tables + language choice
#include "../PrismCore/Log.hpp"              // C16 logger (the one warn a key miss gets)
#include "../PrismCore/ExtractScheduler.hpp"
#include "../PrismCore/ListSortModel.hpp"     // persisted sort key (C13 view state)
#include "../PrismCore/SecurityGuard.hpp"
#include "../PrismCore/SettingsStore.hpp"     // C14 document (schema + validation)
#include "../PrismCore/SidebarModel.hpp"      // C12 SidebarStore port
#include "../PrismCore/SmartExtractor.hpp"
#include "../PrismWin/AmsiGuard.hpp"
#include "../PrismWin/ArchiveWriter.hpp"      // C9 execution layer (the real writer)
#include "../PrismWin/MotwGuard.hpp"
#include "../PrismWin/PrismEngineBridge.hpp"
#include "../PrismWin/ReaderPool.hpp"
#include "../PrismWin/SettingsFile.hpp"       // C14 file layer (%LOCALAPPDATA%, atomic)
#include "../PrismWin/SparsePackageRegistrar.hpp"  // shell_launch flag constants (C4)

namespace prism::app {

// ---------------------------------------------------------------------------
// 1. Command line: the four context-menu verbs (C4 contract).
//
// shell_launch::ArgsForVerb renders "verb -> flag + quoted paths"; the exe side
// of that contract is this parser. The flag constants are INCLUDED from the
// registrar header, not re-spelled, so the two halves cannot drift: a test
// feeds a verb through ArgsForVerb and asserts the exe parses it.
// ---------------------------------------------------------------------------
// The automation verb for archive CREATION (this card's contract). It is NOT
// one of C4's shell flags on purpose -- see section 6 for why the shell's
// --compress-7z / --compress-zip stay refused for now.
inline constexpr wchar_t kCompressHere[] = L"--compress-here";

enum class LaunchMode : std::uint8_t {
  kGallery = 0,    // no verb: open the window (double-click path)
  kSmartExtract,   // --smart-extract
  kExtractHere,    // --extract-here
  kExtractSub,     // --extract-sub
  kTest,           // --test (verify integrity, write nothing)
  kCompressHere,   // --compress-here <files...> (C9 create, automation verb)
};

inline const char* LaunchModeName(LaunchMode mode) {
  switch (mode) {
    case LaunchMode::kGallery: return "Gallery";
    case LaunchMode::kSmartExtract: return "SmartExtract";
    case LaunchMode::kExtractHere: return "ExtractHere";
    case LaunchMode::kExtractSub: return "ExtractSub";
    case LaunchMode::kTest: return "Test";
    case LaunchMode::kCompressHere: return "CompressHere";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, LaunchMode mode) {
  return os << LaunchModeName(mode);
}

// True for the modes that write files by extracting.
inline bool Extracts(LaunchMode mode) {
  return mode == LaunchMode::kSmartExtract || mode == LaunchMode::kExtractHere ||
         mode == LaunchMode::kExtractSub || mode == LaunchMode::kGallery;
}

// True for the mode that CREATES an archive (C9).
inline bool Compresses(LaunchMode mode) { return mode == LaunchMode::kCompressHere; }

struct LaunchRequest {
  LaunchMode mode = LaunchMode::kGallery;
  std::wstring archive;      // first non-flag argument
  // Every non-flag argument. For --compress-here that is the input list; the
  // other verbs only ever take one path, and archive is its head.
  std::vector<std::wstring> inputs;
  std::wstring flag;         // the verb flag we matched ("" when none)
  // Set when the caller passed a flag we do NOT implement. The app must refuse
  // visibly rather than fall back to "open the gallery": a context-menu command
  // that silently does something else is worse than one that says it is not
  // ready (the C9 surface -- the shell's --compress-7z/zip are still here).
  bool unknownFlag = false;
  std::wstring unknownText;
};

inline bool StartsWithFlag(const std::wstring& text) {
  return text.size() >= 2 && text[0] == L'-' && text[1] == L'-';
}

// Two passes on purpose: the verb is decided FIRST and the paths are then
// collected FOR that verb. A single pass loses every path that appears before
// the flag, which is exactly the ordering a hand-typed command line uses.
inline LaunchRequest ParseLaunch(const std::vector<std::wstring>& argv) {
  LaunchRequest request;
  for (const std::wstring& arg : argv) {
    if (!StartsWithFlag(arg)) continue;
    if (arg == prism::win::shell_launch::kSmartExtract) {
      request.mode = LaunchMode::kSmartExtract;
      request.flag = arg;
    } else if (arg == prism::win::shell_launch::kExtractHere) {
      request.mode = LaunchMode::kExtractHere;
      request.flag = arg;
    } else if (arg == prism::win::shell_launch::kExtractSub) {
      request.mode = LaunchMode::kExtractSub;
      request.flag = arg;
    } else if (arg == prism::win::shell_launch::kTest) {
      request.mode = LaunchMode::kTest;
      request.flag = arg;
    } else if (arg == kCompressHere) {
      request.mode = LaunchMode::kCompressHere;
      request.flag = arg;
    } else {
      // --compress-7z / --compress-zip / --smart-extract-each are in C4's
      // table but the shell-verb wiring is still open (C25's suite PINS the
      // refusal; see section 6). Refuse loudly; never launch a guess.
      request.unknownFlag = true;
      request.unknownText = arg;
    }
  }
  for (const std::wstring& arg : argv) {
    if (StartsWithFlag(arg)) continue;
    if (request.mode == LaunchMode::kCompressHere) request.inputs.push_back(arg);
    if (request.archive.empty()) request.archive = arg;
  }
  return request;
}

// ---------------------------------------------------------------------------
// 2. Plan per mode: the four flags must reach four DIFFERENT plans.
//
// The plan is a PrismCore SmartPlan, so the decision about where bytes land is
// still tested on Linux; this function only picks which planner call the mode
// means. Modes are deliberately non-equivalent:
//   * smart      -- SmartExtractionPlanner: strips a redundant root, drops promo
//                   junk, wraps scattered roots (blueprint ch.2);
//   * extract-here -- 7-Zip's "Extract Here": the archive's own tree, verbatim,
//                   into the archive's folder (no strip, no filtering);
//   * extract-sub  -- 7-Zip's "Extract to <name>\": the whole tree under a
//                   folder named after the archive (collision-suffixed);
//   * test         -- verifyOnly: decode every entry, write nothing.
// ---------------------------------------------------------------------------
struct ExtractionPlan {
  LaunchMode mode = LaunchMode::kSmartExtract;
  prism::SmartPlan plan;
  bool verifyOnly = false;
  std::wstring label;  // HUD/status headline (wide: the window paints wide)
};

namespace detail {

inline std::string ParentDirOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline void CountPayload(const std::vector<ArchiveItem>& catalog, prism::SmartPlan* plan) {
  for (const ArchiveItem& item : catalog) {
    if (item.isDir) continue;
    ++plan->fileCount;
    plan->totalBytes += item.size;
  }
}

}  // namespace detail

inline ExtractionPlan BuildExtractionPlan(LaunchMode mode, const std::string& archivePathUtf8,
                                          const std::vector<ArchiveItem>& catalog,
                                          prism::SmartExtractionPlanner::ExistsFn exists) {
  ExtractionPlan out;
  out.mode = mode;
  const std::string parent = detail::ParentDirOf(archivePathUtf8);
  const std::string stem = prism::ArchiveStem(archivePathUtf8);

  switch (mode) {
    case LaunchMode::kTest: {
      out.verifyOnly = true;
      out.plan.strategy = prism::SmartStrategy::EmptyArchive;
      out.plan.reason = "verify only: decode every entry, write nothing";
      out.label = L"\u6821\u9A8C\u5B8C\u6574\u6027";  // 校验完整性
      return out;
    }
    case LaunchMode::kExtractHere: {
      out.plan.strategy = prism::SmartStrategy::FlatDirectly;
      out.plan.baseTargetDir = parent;
      out.plan.reason = "extract here: the archive's own tree into " + parent;
      out.label = L"\u89E3\u538B\u5230\u5F53\u524D\u6587\u4EF6\u5939";  // 解压到当前文件夹
      detail::CountPayload(catalog, &out.plan);
      return out;
    }
    case LaunchMode::kExtractSub: {
      out.plan.strategy = prism::SmartStrategy::FlatDirectly;
      out.plan.baseTargetDir = prism::SmartExtractionPlanner::ResolveCollisionFreeDir(
          parent, stem.empty() ? std::string("extracted") : stem, exists);
      out.plan.reason = "extract to subfolder: whole tree under a folder named after the archive";
      out.label = L"\u89E3\u538B\u5230\u5B50\u6587\u4EF6\u5939";  // 解压到子文件夹
      detail::CountPayload(catalog, &out.plan);
      return out;
    }
    case LaunchMode::kGallery:
    case LaunchMode::kSmartExtract:
    default:
      // The gallery's double-click and the shell's "smart extract" verb run the
      // SAME plan: one code path, so a fix here is a fix in both.
      out.plan = prism::SmartExtractionPlanner::Build(archivePathUtf8, parent, catalog, exists);
      out.label = L"\u667A\u80FD\u89E3\u538B";  // 智能解压
      return out;
  }
}

// ---------------------------------------------------------------------------
// 3. Handlers per concurrent operation (C11: ReaderPool + ExtractScheduler).
//
// One task == one scheduler dispatch == one pool lease == one IInArchive
// handler and one input stream. Before this class the sweep and the extract
// shared one ArchiveReader (docs/PROGRESS known debt): a race inside 7z.dll and
// one shared file pointer.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C25_MUTANT_SHARED_READER restores the
// old shared-reader shape -- both tasks get the same reader and slot 0. The
// suite compiles once clean and once with this switch, and the mutant build is
// registered WILL_FAIL, so CTest is green only while the bug is caught.
// ---------------------------------------------------------------------------
class OperationHandles {
 public:
  using Kind = prism::core::sched::Kind;
  using Priority = prism::core::sched::Priority;

  struct Config {
    std::wstring archivePath;
    std::wstring password;
    // == ReaderPool capacity == ExtractScheduler::Config::handlerSlots.
    size_t handlerSlots = 2;
    size_t backlog = 4;
  };

  // A running operation. Move-only (it owns a pool lease).
  struct Task {
    Task() = default;
    Task(Task&&) = default;
    Task& operator=(Task&&) = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    uint64_t id = 0;                                  // scheduler task id
    size_t slot = 0;                                  // handler slot
    Kind kind = prism::core::sched::Kind::kExtract;
    prism::win::ArchiveReader* reader = nullptr;      // borrowed; owned by lease
    prism::win::ReaderPool::Lease lease;              // empty under the mutant
    bool sharedHandler = false;                       // diagnostics
  };

  static std::shared_ptr<OperationHandles> Create(prism::win::SevenZipLibrary& library,
                                                  const Config& config,
                                                  std::string* error = nullptr) {
    auto handles = std::shared_ptr<OperationHandles>(new OperationHandles());
    handles->config_ = config;
    prism::core::sched::Config schedulerConfig;
    schedulerConfig.handlerSlots = config.handlerSlots;
    schedulerConfig.backlog = config.backlog;
    schedulerConfig.preemptBackgroundForUser = true;
    if (schedulerConfig.handlerSlots == 0) {
      if (error) *error = "OperationHandles: handlerSlots must be >= 1";
      return nullptr;
    }
    handles->scheduler_ = std::make_unique<prism::core::sched::ExtractScheduler>(
        schedulerConfig);

#if defined(PRISM_C25_MUTANT_SHARED_READER)
    // MUTATION: the pre-C11 shape. ONE reader for every concurrent operation.
    handles->sharedReader_ = std::make_shared<prism::win::ArchiveReader>(library);
    if (!handles->sharedReader_->Open(config.archivePath, config.password)) {
      if (error) {
        *error = "OperationHandles: shared reader could not open the archive: " +
                 handles->sharedReader_->openTrace();
      }
      return nullptr;
    }
#else
    prism::win::ReaderPool::Config poolConfig;
    poolConfig.archivePath = config.archivePath;
    poolConfig.password = config.password;
    poolConfig.capacity = config.handlerSlots;
    handles->pool_ = prism::win::ReaderPool::Create(library, poolConfig, error);
    if (!handles->pool_) return nullptr;
#endif
    return handles;
  }

  // Lease a handler for one operation. Non-blocking: a caller that finds no free
  // slot gets nullopt plus a reason, which the UI turns into backpressure -- it
  // never blocks the UI thread.
  std::optional<Task> Begin(Kind kind, Priority priority, const std::string& key,
                            const std::string& label, std::string* why = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = 0;
    const prism::core::sched::SubmitStatus status =
        scheduler_->Submit({kind, priority, key, label}, &id);
    if (status != prism::core::sched::SubmitStatus::kQueued) {
      if (why) {
        *why = std::string("submit refused: ") +
               prism::core::sched::SubmitStatusName(status);
      }
      return std::nullopt;
    }
    const std::optional<prism::core::sched::Dispatch> dispatch = scheduler_->NextDispatch();
    if (!dispatch.has_value()) {
      scheduler_->Cancel(id);
      if (why) *why = "no free handler slot";
      return std::nullopt;
    }

    Task task;
    task.id = id;
    task.kind = kind;

#if defined(PRISM_C25_MUTANT_SHARED_READER)
    task.slot = 0;
    task.reader = sharedReader_.get();
    task.sharedHandler = true;
    (void)dispatch;  // the mutant ignores the scheduler's slot on purpose
#else
    std::optional<prism::win::ReaderPool::Lease> lease = pool_->Acquire();
    if (!lease.has_value()) {
      scheduler_->Complete(id, false);
      if (why) *why = "pool has no free handler";
      return std::nullopt;
    }
    // The two indexes are the SAME index by contract (ReaderPool.hpp: "Slot
    // index == ExtractScheduler::Dispatch::slot"). Both hand out their lowest
    // free slot, so a mismatch means they disagree about what is busy -- that is
    // a contract breach, and running anyway is exactly how two operations end up
    // on one handler. Refuse instead.
    if (lease->slot() != dispatch->slot) {
      lease->Release();
      scheduler_->Complete(id, false);
      if (why) {
        *why = "handler slot mismatch: scheduler " + std::to_string(dispatch->slot) +
               " vs pool " + std::to_string(lease->slot());
      }
      return std::nullopt;
    }
    task.slot = dispatch->slot;
    task.reader = lease->get();
    task.lease = std::move(*lease);
#endif
    return task;
  }

  // Report the outcome and give the slot back. Safe from any thread (the mutex
  // is the "owner" the scheduler's contract asks for).
  void Finish(Task& task, bool succeeded) {
    if (task.id != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      scheduler_->Complete(task.id, succeeded);
    }
    task.lease.Release();
    task.reader = nullptr;
    task.id = 0;
  }

  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_->Close();
#if !defined(PRISM_C25_MUTANT_SHARED_READER)
    if (pool_) pool_->Close();
#endif
  }

  size_t capacity() const { return config_.handlerSlots; }

  prism::win::ReaderPool::Stats poolStats() const {
#if defined(PRISM_C25_MUTANT_SHARED_READER)
    prism::win::ReaderPool::Stats stats;
    stats.capacity = 1;
    stats.handlers = 1;
    stats.sharedHandlerInjected = true;
    return stats;
#else
    return pool_->stats();
#endif
  }

  prism::core::sched::Stats schedulerStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_->stats();
  }

  // Diagnostics: how many DISTINCT handlers the app is currently running on.
  // A shared-reader regression cannot hide behind this number.
  size_t distinctHandlersInUse() const {
#if defined(PRISM_C25_MUTANT_SHARED_READER)
    return sharedReader_ ? 1 : 0;
#else
    return pool_->inUse();
#endif
  }

 private:
  OperationHandles() = default;

  mutable std::mutex mutex_;  // serializes every scheduler call (its contract)
  Config config_;
  std::unique_ptr<prism::core::sched::ExtractScheduler> scheduler_;
#if defined(PRISM_C25_MUTANT_SHARED_READER)
  std::shared_ptr<prism::win::ArchiveReader> sharedReader_;
#else
  std::shared_ptr<prism::win::ReaderPool> pool_;
#endif
};

// ---------------------------------------------------------------------------
// 4. Extraction, then the C5 security pass (MoTW inheritance + AMSI pre-scan).
//
// The ordering is the contract: bytes first (the user asked for files), then the
// mark, then the scan. Both post-steps are REPORTED, never silent -- including
// "we could not scan", which is the whole point of AmsiVerdict::Unavailable.
// ---------------------------------------------------------------------------
struct SecuritySummary {
  prism::win::motw::SourceState motwSource = prism::win::motw::SourceState::Absent;
  size_t filesStamped = 0;
  size_t stampFailed = 0;
  size_t amsiScanned = 0;
  size_t amsiBlocked = 0;
  size_t amsiCouldNotScan = 0;  // Unavailable: reported, never treated as Allow
  prism::SecurityAction worstAction = prism::SecurityAction::Allow;
  std::string motwDetail;
  std::string amsiDetail;
};

inline prism::SecurityAction Worse(prism::SecurityAction a, prism::SecurityAction b) {
  const auto rank = [](prism::SecurityAction action) {
    switch (action) {
      case prism::SecurityAction::Allow: return 0;
      case prism::SecurityAction::Warn: return 1;
      case prism::SecurityAction::Block: return 2;
    }
    return 1;
  };
  return rank(a) >= rank(b) ? a : b;
}

// scanExecutables == false keeps the default run cheap (the CI suite has no
// malware to detect and no reason to call into antivirus on every extraction).
// amsiMode lets a caller model "AMSI is not installed" (Unavailable) without
// needing a machine where amsi.dll is missing.
inline SecuritySummary ApplySecurityPass(const std::wstring& archivePathWide,
                                         const std::vector<std::string>& writtenPathsUtf8,
                                         bool scanExecutables,
                                         prism::win::amsi::InitMode amsiMode =
                                             prism::win::amsi::InitMode::LoadAmsi) {
  SecuritySummary summary;
  const prism::win::motw::SourceMark source(archivePathWide);
  summary.motwSource = source.state();
  const prism::MotwPlan motwPlan = prism::PlanMotwInheritance(source.policySource());
  summary.worstAction = Worse(summary.worstAction, motwPlan.action);
  if (!motwPlan.stamp && source.hasMoTW()) {
    summary.motwDetail = motwPlan.reason;  // only reachable through a mutation
  } else if (summary.motwSource == prism::win::motw::SourceState::Unreadable) {
    summary.motwDetail = source.detail();
  }

#if !defined(PRISM_C25_MUTANT_NO_MOTW)
  if (motwPlan.stamp) {
    for (const std::string& path : writtenPathsUtf8) {
      const prism::win::motw::StampReport report =
          source.StampDetailed(prism::win::Utf8ToWide(path));
      if (report.result == prism::win::motw::StampResult::Stamped) {
        ++summary.filesStamped;
      } else {
        ++summary.stampFailed;
        if (summary.motwDetail.empty()) summary.motwDetail = report.detail;
        summary.worstAction = Worse(summary.worstAction, prism::MotwStampFailureAction());
      }
    }
  }
#else
  // MUTATION (CONTRACTS D3): stamping is skipped, so a copied file is clean and
  // the suite's :Zone.Identifier assertion must go red.
  (void)archivePathWide;
#endif

  if (!scanExecutables) return summary;

  prism::win::amsi::Scanner scanner(amsiMode);
  if (!scanner.available()) {
    summary.amsiDetail = scanner.unavailableReason();
  }
  for (const std::string& path : writtenPathsUtf8) {
    if (!prism::IsAmsiTarget(path)) continue;
    if (!scanner.available()) {
      // Unavailable is a THIRD state, not "allow". Count it, warn, and say why.
      ++summary.amsiCouldNotScan;
      summary.worstAction =
          Worse(summary.worstAction, prism::DecideAmsiAction(prism::AmsiVerdict::Unavailable));
      continue;
    }
    const prism::win::amsi::ScanResult result = scanner.ScanFile(prism::win::Utf8ToWide(path));
    const prism::SecurityAction action = prism::DecideAmsiAction(result.verdict);
    switch (result.verdict) {
      case prism::AmsiVerdict::Allow:
        ++summary.amsiScanned;
        break;
      case prism::AmsiVerdict::Block:
        ++summary.amsiBlocked;
        summary.amsiDetail = "blocked " + path + (result.detail.empty() ? "" : ": " + result.detail);
        DeleteFileW(prism::win::motw::detail::ToExtendedPath(prism::win::Utf8ToWide(path)).c_str());
        break;
      case prism::AmsiVerdict::Unavailable:
        ++summary.amsiCouldNotScan;
        if (summary.amsiDetail.empty()) summary.amsiDetail = result.detail;
        break;
    }
    summary.worstAction = Worse(summary.worstAction, action);
  }
  return summary;
}

struct ExtractionOutcome {
  bool ok = false;
  uint64_t filesWritten = 0;
  uint64_t bytesWritten = 0;
  std::string error;          // guard/fuse/backpressure reason (UTF-8, diagnostic)
  SecuritySummary security;
  std::wstring status;        // ready for PostStatus/SetStatus
};

namespace detail {

inline std::wstring Wide(const char* utf8) { return prism::win::Utf8ToWide(utf8); }

inline std::wstring BuildStatusText(const ExtractionOutcome& outcome, bool ok,
                                    const std::string& targetDir) {
  std::wstring text = ok ? std::wstring(L"\x5DF2\x89E3\x538B")   // 已解压
                         : std::wstring(L"\x89E3\x538B\x5931\x8D25");  // 解压失败
  text += L" " + std::to_wstring(outcome.filesWritten) + L" \u4E2A\u6587\u4EF6";
  if (ok && !targetDir.empty()) text += L" \x2192 " + prism::win::Utf8ToWide(targetDir);
  if (outcome.security.filesStamped > 0) {
    text += L" | Zone.Identifier " + std::to_wstring(outcome.security.filesStamped);
  }
  if (outcome.security.stampFailed > 0) {
    text += L" | \u76D6\u7AE0\u5931\u8D25 " + std::to_wstring(outcome.security.stampFailed);
  }
  if (outcome.security.amsiBlocked > 0) {
    text += L" | AMSI \u62E6\u622A " + std::to_wstring(outcome.security.amsiBlocked);
  }
  if (outcome.security.amsiCouldNotScan > 0) {
    // Never silent: the user must know a payload was not scanned.
    text += L" | \u672A\u626B\u63CF " + std::to_wstring(outcome.security.amsiCouldNotScan);
  }
  return text;
}

}  // namespace detail

// One extraction, on whichever handler the caller's task owns. Returns the
// outcome; the caller posts it (a worker must not touch window state).
inline ExtractionOutcome RunExtraction(OperationHandles& handles,
                                       const std::vector<ArchiveItem>& catalog,
                                       const ExtractionPlan& plan,
                                       const std::wstring& archivePathWide,
                                       bool scanExecutables,
                                       prism::win::amsi::InitMode amsiMode =
                                           prism::win::amsi::InitMode::LoadAmsi) {
  ExtractionOutcome outcome;
  std::string why;
  std::optional<OperationHandles::Task> task =
      handles.Begin(OperationHandles::Kind::kExtract, OperationHandles::Priority::kUser,
                    "extract:" + prism::win::WideToUtf8(archivePathWide),
                    LaunchModeName(plan.mode), &why);
  if (!task.has_value()) {
    outcome.error = why;
    outcome.status = L"\x89E3\x538B\x88AB\x62D2\x7EDD";  // 解压被拒绝
    if (!why.empty()) outcome.status += L" (" + prism::win::Utf8ToWide(why) + L")";
    return outcome;
  }

  prism::win::SmartExtractCallback::Result result;
  const bool ok = task->reader->ExtractWithPlan(catalog, plan.plan, plan.plan.baseTargetDir,
                                                nullptr, &result);
  handles.Finish(*task, ok);

  outcome.ok = ok;
  outcome.filesWritten = result.filesWritten;
  outcome.bytesWritten = result.bytesWritten;
  if (!ok) {
    if (result.guardTripped) outcome.error = result.guardReason;
    else if (result.fuseTripped) outcome.error = result.fuseReason;
    else outcome.error = "engine reported failure";
  }
  if (ok) {
    outcome.security =
        ApplySecurityPass(archivePathWide, result.writtenPaths, scanExecutables, amsiMode);
  }
  outcome.status = detail::BuildStatusText(outcome, ok, plan.plan.baseTargetDir);
  return outcome;
}

// ---------------------------------------------------------------------------
// 5. Integrity verification (--test): decode every entry, write nothing.
//
// It reuses the engine's own per-item result: a bad CRC arrives as a non-zero
// SetOperationResult, so "verified" means the bytes actually decoded -- not
// "the header parsed".
// ---------------------------------------------------------------------------
struct VerifyOutcome {
  bool ok = false;
  uint64_t items = 0;
  uint64_t bytes = 0;
  int32_t failedIndex = -1;
  std::string error;
};

namespace detail {

// Discards every byte but still lets the engine produce them (a CRC error is
// only reported after the data stage, so a "skip everything" callback would
// verify nothing).
class NullSink final : public prism::win::abi::ISequentialOutStream {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == prism::win::abi::IID_ISequentialOutStream) {
      *out = static_cast<prism::win::abi::ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }
  STDMETHODIMP Write(const void*, prism::win::abi::UInt32 size,
                     prism::win::abi::UInt32* processed) override {
    total_ += size;
    if (processed) *processed = size;
    return S_OK;
  }
  uint64_t total() const { return total_; }

 private:
  uint64_t total_ = 0;
  LONG refs_ = 1;
};

class VerifyCallback final : public prism::win::abi::IArchiveExtractCallback {
 public:
  VerifyCallback() : sink_(new NullSink()) { sink_->AddRef(); }
  ~VerifyCallback() {
    if (sink_) sink_->Release();
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == prism::win::abi::IID_IArchiveExtractCallback) {
      *out = static_cast<prism::win::abi::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP SetTotal(prism::win::abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const prism::win::abi::UInt64*) override { return S_OK; }
  STDMETHODIMP PrepareOperation(prism::win::abi::Int32) override { return S_OK; }
  STDMETHODIMP GetStream(prism::win::abi::UInt32 index,
                         prism::win::abi::ISequentialOutStream** outStream,
                         prism::win::abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != prism::win::abi::kExtract) return S_OK;
    pending_ = index;
    sink_->AddRef();
    *outStream = sink_;
    return S_OK;
  }
  STDMETHODIMP SetOperationResult(prism::win::abi::Int32 operationResult) override {
    if (operationResult != 0 && failedIndex_ < 0) {
      failedIndex_ = static_cast<int32_t>(pending_);
      error_ = "entry " + std::to_string(pending_) + " failed with operation result " +
               std::to_string(operationResult);
      return E_ABORT;  // one bad entry is the answer; do not grind the rest
    }
    ++items_;
    pending_ = 0xFFFFFFFFu;
    return S_OK;
  }

  uint64_t items() const { return items_; }
  uint64_t bytes() const { return sink_ ? sink_->total() : 0; }
  int32_t failedIndex() const { return failedIndex_; }
  const std::string& error() const { return error_; }

 private:
  NullSink* sink_ = nullptr;
  prism::win::abi::UInt32 pending_ = 0xFFFFFFFFu;
  uint64_t items_ = 0;
  int32_t failedIndex_ = -1;
  std::string error_;
  LONG refs_ = 1;
};

}  // namespace detail

inline VerifyOutcome VerifyIntegrity(OperationHandles& handles,
                                     const std::vector<ArchiveItem>& catalog,
                                     const std::wstring& archivePathWide) {
  VerifyOutcome outcome;
  std::string why;
  std::optional<OperationHandles::Task> task =
      handles.Begin(OperationHandles::Kind::kExtract, OperationHandles::Priority::kUser,
                    "verify:" + prism::win::WideToUtf8(archivePathWide), "integrity", &why);
  if (!task.has_value()) {
    outcome.error = why;
    return outcome;
  }
  std::vector<uint32_t> indices;
  indices.reserve(catalog.size());
  for (const ArchiveItem& item : catalog) {
    if (!item.isDir) indices.push_back(item.index);
  }
  auto* callback = new detail::VerifyCallback();
  const bool callOk = task->reader->ExtractBatch(indices, callback);
  outcome.items = callback->items();
  outcome.bytes = callback->bytes();
  outcome.failedIndex = callback->failedIndex();
  outcome.error = callback->error();
  callback->Release();
  handles.Finish(*task, callOk && outcome.failedIndex < 0);
  outcome.ok = callOk && outcome.failedIndex < 0;
  return outcome;
}

// ---------------------------------------------------------------------------
// 6. Compression (C9 decision + execution, C18 dialog) -- the CREATE half.
//
// Until this section existed the app could only read archives: the parser above
// refused every --compress-* verb because C9 had not landed. It has, so the
// verb, the plan and the run live here -- still as wiring, never as a second
// policy: CompressionPlan decides WHAT to write, ArchiveWriter does the writing,
// and this section only turns a list of paths into a plan and a plan into a job.
//
// WHY --compress-here AND NOT --compress-7z
//   C4's table renders --compress-7z / --compress-zip for the shell verbs, but
//   card C25's suite PINS the refusal of those two flags (Tests/win/
//   test_app_wiring.cpp (3): ParseLaunch({--compress-7z}) must report
//   unknownFlag). Wiring them here would turn another card's green test red, so
//   the shell verbs stay refused until the card that owns that assertion updates
//   it. The automation verb --compress-here <files...> (this card's contract)
//   carries the same code path end to end, and the window's Compress action is
//   the real user entry. Both land in RunCompression().
// ---------------------------------------------------------------------------

inline std::wstring ParentDirectory(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

inline std::wstring LeafName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

inline std::wstring StemName(const std::wstring& path) {
  const std::wstring leaf = LeafName(path);
  const size_t dot = leaf.find_last_of(L'.');
  return dot == std::wstring::npos || dot == 0 ? leaf : leaf.substr(0, dot);
}

// PrismCore's entry-path contract: UTF-8, '/'-separated, RELATIVE to the source
// root (CONTRACTS B4). The writer converts back to '\' when it talks to 7-Zip.
inline std::string EntryPathOf(const std::wstring& wide) {
  std::string text = prism::win::WideToUtf8(wide);
  for (char& c : text) {
    if (c == '\\') c = '/';
  }
  return text;
}

// Enumerate one input (file, or folder subtree) into plan items. Directories are
// INCLUDED so an empty folder survives the round trip: ArchiveWriter announces
// them with kpidIsDir, which the mkdir matrix (B1) proved is the only shape the
// engine keeps.
inline bool EnumerateInput(const std::wstring& root, const std::wstring& input,
                           std::vector<prism::PlanItem>* items,
                           std::vector<std::string>* problems) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path absolute(input);
  const fs::path base(root);
  const auto relativeOf = [&](const fs::path& node, std::string* out) {
    const fs::path rel = fs::relative(node, base, ec);
    if (ec) {
      if (problems != nullptr) {
        problems->push_back("cannot relativize " + prism::win::WideToUtf8(node.wstring()));
      }
      return false;
    }
    *out = EntryPathOf(rel.wstring());
    return true;
  };
  const bool isDir = fs::is_directory(absolute, ec);
  if (ec) {
    if (problems != nullptr) problems->push_back("cannot stat " + prism::win::WideToUtf8(input));
    return false;
  }
  if (!isDir) {
    prism::PlanItem item;
    if (!relativeOf(absolute, &item.path)) return false;
    item.isDir = false;
    item.size = fs::file_size(absolute, ec);
    if (ec) {
      if (problems != nullptr) problems->push_back("cannot size " + prism::win::WideToUtf8(input));
      return false;
    }
    items->push_back(std::move(item));
    return true;
  }
  prism::PlanItem folder;
  if (!relativeOf(absolute, &folder.path)) return false;
  folder.isDir = true;
  items->push_back(std::move(folder));
  fs::recursive_directory_iterator it(absolute, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      if (problems != nullptr) problems->push_back("enumeration stopped: " + ec.message());
      return false;
    }
    const fs::directory_entry& entry = *it;
    const bool entryIsDir = entry.is_directory(ec);
    if (ec) continue;  // a broken link inside the tree is skipped, not fatal
    prism::PlanItem item;
    if (!relativeOf(entry.path(), &item.path)) continue;
    item.isDir = entryIsDir;
    if (!entryIsDir) {
      item.size = entry.file_size(ec);
      if (ec) continue;
    }
    items->push_back(std::move(item));
  }
  return true;
}

// The plan for "compress these paths". sourceRoot is the folder every input's
// relative path is computed against; mixing folders in ONE archive would need
// per-item roots, which ArchiveWriter does not model, so that case is refused
// with a sentence instead of an archive whose layout nobody can predict.
struct CompressionTarget {
  bool ok = false;
  std::string error;
  std::wstring archivePath;
  std::wstring sourceRoot;
  prism::CompressionPlan plan;
  std::vector<prism::PlanItem> items;
};

inline CompressionTarget PlanCompression(const std::vector<std::wstring>& inputs,
                                         prism::ArchiveFormat format,
                                         const std::wstring& targetPath = std::wstring(),
                                         prism::CompressionOptions options = {}) {
  CompressionTarget out;
  if (inputs.empty()) {
    out.error = "nothing to compress: no input paths";
    return out;
  }
  out.sourceRoot = ParentDirectory(inputs.front());
  if (out.sourceRoot.empty()) {
    out.error = "the first input has no parent folder";
    return out;
  }
  std::vector<std::string> problems;
  for (const std::wstring& input : inputs) {
    if (ParentDirectory(input) != out.sourceRoot) {
      out.error = "all inputs must live in one folder (the archive needs a single source root)";
      return out;
    }
    if (!EnumerateInput(out.sourceRoot, input, &out.items, &problems)) {
      out.error = problems.empty() ? "cannot enumerate the input paths" : problems.front();
      return out;
    }
  }
  if (out.items.empty()) {
    out.error = "the input paths produced no entries";
    return out;
  }
  const std::string name =
      prism::DefaultArchiveFileName(EntryPathOf(StemName(inputs.front())), format);
  out.archivePath =
      targetPath.empty() ? out.sourceRoot + L"\\" + prism::win::Utf8ToWide(name) : targetPath;
  options.format = format;
  out.plan = prism::MakeCompressionPlan(out.items, options);
  out.ok = out.plan.ok;
  if (!out.plan.ok) out.error = out.plan.error;
  return out;
}

// One compression run. Synchronous BY DESIGN: the caller runs it on a worker and
// posts the result (CONTRACTS B9 -- a worker never touches window state), which
// is exactly how Main.cpp drives it.
struct CompressionRequest {
  std::wstring archivePath;
  std::wstring sourceRoot;
  prism::CompressionPlan plan;
  std::wstring password;  // empty = unencrypted
};

struct CompressionOutcome {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  std::string trace;
  std::wstring archivePath;
  uint64_t inputBytes = 0;
  uint64_t bytesWritten = 0;
  uint64_t itemsWritten = 0;
  uint64_t verifiedItems = 0;
  int64_t elapsedMs = 0;
};

// done/total are INPUT bytes -- the only honest progress axis for a create: the
// output size is not known until the last block is written (CompressionPlan's
// cost model says so). Returning false cancels the run.
using CompressionProgress = std::function<bool(uint64_t doneBytes, uint64_t totalBytes)>;

inline CompressionOutcome RunCompression(const CompressionRequest& request,
                                         const CompressionProgress& progress = {}) {
  CompressionOutcome outcome;
  outcome.archivePath = request.archivePath;
  outcome.inputBytes = request.plan.inputBytes;
  if (!request.plan.ok) {
    outcome.error = "plan is not usable (" + request.plan.error + ")";
    return outcome;
  }
  prism::win::cw::WriteRequest write;
  write.archivePath = request.archivePath;
  write.sourceRoot = request.sourceRoot;
  write.plan = request.plan;
  write.password = request.password;
  bool cancelled = false;
  if (progress) {
    const uint64_t total = request.plan.inputBytes;
    write.progress = [&progress, &cancelled, total](uint64_t done) {
      if (progress(done, total)) return true;
      cancelled = true;
      return false;
    };
  }
  const prism::win::cw::WriteResult result = prism::win::cw::CreateArchive(write);
  outcome.ok = result.ok;
  outcome.cancelled = cancelled;
  outcome.error = result.error;
  outcome.trace = result.trace;
  if (!result.archivePath.empty()) outcome.archivePath = result.archivePath;
  outcome.bytesWritten = result.bytesWritten;
  outcome.itemsWritten = result.itemsWritten;
  outcome.verifiedItems = result.verifiedItems;
  outcome.elapsedMs = result.elapsedMs;
  return outcome;
}

// What the compress dialog shows as its input: the entries the window currently
// shows. The BYTES are not on disk, so the caller stages them (a re-pack) before
// CreateArchive runs; the counts here are what the estimate line uses.
inline std::vector<prism::PlanItem> PlanItemsFromCatalog(const std::vector<ArchiveItem>& catalog) {
  std::vector<prism::PlanItem> items;
  items.reserve(catalog.size());
  for (const ArchiveItem& item : catalog) {
    prism::PlanItem plan;
    plan.path = item.path;
    plan.isDir = item.isDir;
    plan.size = item.isDir ? 0 : item.size;
    items.push_back(std::move(plan));
  }
  return items;
}

// ---------------------------------------------------------------------------
// 7. Language (C19): the tables on disk, the choice, and the two call shapes.
//
// The app asks the translator for a KEY; it never switches on a language tag. A
// key that no table defines comes back as the key itself (C19's contract: ugly
// but debuggable) and is logged ONCE as a warn -- that is how "we shipped an
// untranslated string" becomes visible from a user's machine.
// ---------------------------------------------------------------------------
struct LanguageRuntime {
  prism::i18n::Translator translator;
  prism::i18n::LanguageChoice choice;
  std::wstring tag;
  int catalogs = 0;
  std::string detail;
};

// NEGATIVE CONTROL (CONTRACTS D3): load ONLY the chosen language, so the
// fallback chain resolves nothing. The suite's "a key that lives only in the
// en-US table still resolves under zh-CN" case MUST go red.
#if defined(PRISM_C27_MUTANT_NO_I18N_FALLBACK)
inline constexpr bool kLoadFallbackTable = false;
#else
inline constexpr bool kLoadFallbackTable = true;
#endif

// Loads every table under <directory>\<tag>.json, then resolves the language:
// the store value (default "auto") against the system locale. A missing table is
// REPORTED, never fatal: the UI degrades to key text instead of not starting.
inline LanguageRuntime LoadLanguageFor(const std::wstring& directory,
                                       const prism::settings::SettingsStore& store,
                                       std::string systemLocale,
                                       prism::log::Logger* logger = nullptr) {
  LanguageRuntime runtime;
  prism::i18n::SetSystemLocaleOverride(systemLocale);
  runtime.choice = prism::i18n::ChooseLanguageFromSettings(store, systemLocale);
  if (directory.empty()) {
    runtime.detail = "no language directory could be resolved";
  } else {
    for (const std::string& tag : prism::i18n::SupportedTags()) {
      // The fallback table is en-US. Without it a zh-CN table that is missing a
      // key has nowhere to fall back to -- which is what the mutant builds.
      if (!kLoadFallbackTable && !prism::i18n::SameTag(tag, runtime.choice.tag)) continue;
      std::string why;
      const std::wstring file = directory + L"\\" + prism::win::Utf8ToWide(tag) + L".json";
      if (runtime.translator.LoadCatalogFile(prism::win::WideToUtf8(file), &why)) {
        ++runtime.catalogs;
      } else if (runtime.detail.empty()) {
        runtime.detail = why;
      }
    }
  }
  runtime.translator.SetLanguage(runtime.choice.tag);
  runtime.tag = prism::win::Utf8ToWide(runtime.choice.tag);
  if (logger != nullptr) {
    logger->Emit(prism::log::Level::kInfo, "i18n", "language resolved",
                 {{"tag", runtime.choice.tag},
                  {"source", runtime.choice.source},
                  {"system", systemLocale.empty() ? std::string("(unset)") : systemLocale},
                  {"catalogs", std::to_string(runtime.catalogs)}});
    if (!runtime.detail.empty()) {
      logger->Emit(prism::log::Level::kWarn, "i18n", "language table unavailable",
                   {{"detail", runtime.detail}});
    }
  }
  return runtime;
}

// The translator plus the two call shapes the app uses. Or() is for keys the
// shipped tables do not carry yet: they get a code default instead of a MISS in
// the log, because a menu bar reading "menu.file" is worse than a fallback.
struct Tr {
  const prism::i18n::Translator* translator = nullptr;

  std::wstring operator()(std::string_view key) const {
    if (translator == nullptr) return prism::win::Utf8ToWide(std::string(key));
    return prism::win::Utf8ToWide(translator->Translate(key));
  }
  std::wstring Or(std::string_view key, const std::wstring& fallback) const {
    if (translator == nullptr || !translator->Has(key)) return fallback;
    return prism::win::Utf8ToWide(translator->Translate(key));
  }
  std::wstring Count(std::string_view key, int64_t n) const {
    if (translator == nullptr) return prism::win::Utf8ToWide(std::string(key));
    return prism::win::Utf8ToWide(translator->TranslateCount(key, n));
  }
};

// ---------------------------------------------------------------------------
// 8. Settings (C14): the document, its file, and the port that rides on it.
//
// The store is the ONLY in-memory truth for persisted state (schema, defaults,
// migration, validation all live there); SettingsFile is the only code that
// touches the disk. This section is the join: load once at startup, save once at
// exit, and hand the sidebar a port so neither the window nor the model needs to
// know a file path exists.
// ---------------------------------------------------------------------------
struct SettingsBootstrap {
  prism::settings::SettingsStore store;
  prism::win::settingsfile::Paths paths;
  prism::win::settingsfile::LoadReport report;
  bool loaded = false;  // the document was read, or defaults were written
  bool tooNew = false;  // written by a newer build: readable but NOT writable
};

// Test seam: LOCALAPPDATA is an INPUT here, so a suite can point the whole stack
// at a temp folder without touching the developer's real settings.
inline SettingsBootstrap LoadSettingsUnder(const std::wstring& localAppData) {
  SettingsBootstrap bootstrap;
  bootstrap.paths = prism::win::settingsfile::PathsUnder(localAppData);
  bootstrap.report = prism::win::settingsfile::LoadStore(bootstrap.store, bootstrap.paths);
  bootstrap.loaded = bootstrap.report.outcome != prism::win::settingsfile::LoadOutcome::Failed;
  bootstrap.tooNew = !bootstrap.store.writable();
  return bootstrap;
}

inline SettingsBootstrap LoadSettings() {
  return LoadSettingsUnder(prism::win::settingsfile::LocalAppDataDirectory());
}

// Returns false when the document could not be written; detail says why (a
// silent failure to remember the user's settings is the bug this avoids).
// NEGATIVE CONTROL (CONTRACTS D3): the document is "saved" only in memory, so a
// restarted process reads defaults. The suite's cross-process read-back MUST go
// red.
#if defined(PRISM_C27_MUTANT_SETTINGS_MEMORY_ONLY)
inline constexpr bool kPersistToDisk = false;
#else
inline constexpr bool kPersistToDisk = true;
#endif

inline bool SaveSettings(SettingsBootstrap* bootstrap, std::string* detail = nullptr) {
  if (!kPersistToDisk) return true;  // mutant: nothing reaches the disk
  if (bootstrap == nullptr) {
    if (detail != nullptr) *detail = "no settings session";
    return false;
  }
  if (!bootstrap->store.writable()) {
    if (detail != nullptr) {
      *detail = "the settings document was written by a newer build; it is left untouched";
    }
    return false;
  }
  prism::win::settingsfile::SaveResult saved;
  const bool ok =
      prism::win::settingsfile::SaveStore(bootstrap->store, bootstrap->paths, &saved);
  if (!ok && detail != nullptr) {
    *detail = "save failed (win32 error " + std::to_string(saved.error) + ")";
  }
  return ok;
}

// C12's SidebarStore port over the settings document. Favourites / recent /
// collapsed folders are ONE string blob (the model owns that format); it lands in
// the document under "ui.<key with dots to underscores>", so a key the sidebar
// model invents later cannot collide with a schema path.
class SettingsSidebarStore final : public prism::sidebar::SidebarStore {
 public:
  explicit SettingsSidebarStore(prism::settings::SettingsStore& store) : store_(&store) {}

  std::string Load(std::string_view key) const override {
    ++reads_;
    lastKey_ = std::string(key);
    return store_->GetString(PathFor(key), std::string());
  }

  void Store(std::string_view key, std::string_view value) override {
    ++writes_;
    lastKey_ = std::string(key);
    lastBytes_ = value.size();
    store_->SetString(PathFor(key), value);
  }

  static std::string PathFor(std::string_view key) {
    std::string path = "ui.";
    for (const char c : key) path.push_back(c == '.' ? '_' : c);
    return path;
  }

  int reads() const { return reads_; }
  int writes() const { return writes_; }
  const std::string& lastKey() const { return lastKey_; }
  size_t lastBytes() const { return lastBytes_; }

 private:
  prism::settings::SettingsStore* store_;
  mutable int reads_ = 0;
  mutable int writes_ = 0;
  mutable std::string lastKey_;
  mutable size_t lastBytes_ = 0;
};

// ---------------------------------------------------------------------------
// 8. The main window's persisted view state (size, view mode, sort, columns).
//
// The schema owns window.width/window.height; the rest rides along as additional
// keys in the same document -- SettingsStore preserves keys it cannot name (that
// is its unknown-key contract), and putting them under the "window." object keeps
// a future schema version able to adopt them without a migration.
// ---------------------------------------------------------------------------
struct WindowState {
  int width = 0;  // 0 = "no opinion" (the window keeps its own default)
  int height = 0;
  bool listMode = false;
  prism::list::MultiSortSpec sort;
  bool hasSort = false;
  prism::list::ColumnLayout columns;
  bool hasColumns = false;
};

inline std::string SortKeyNameOf(prism::list::SortKey key) {
  switch (key) {
    case prism::list::SortKey::kName: return "name";
    case prism::list::SortKey::kOriginalSize: return "size";
    case prism::list::SortKey::kPackedSize: return "packed";
    case prism::list::SortKey::kRatio: return "ratio";
    case prism::list::SortKey::kMtime: return "mtime";
    case prism::list::SortKey::kType: return "type";
  }
  return "name";
}

inline bool SortKeyFromName(std::string_view name, prism::list::SortKey* out) {
  for (const prism::list::SortKey key :
       {prism::list::SortKey::kName, prism::list::SortKey::kOriginalSize,
        prism::list::SortKey::kPackedSize, prism::list::SortKey::kRatio,
        prism::list::SortKey::kMtime, prism::list::SortKey::kType}) {
    if (SortKeyNameOf(key) == name) {
      if (out != nullptr) *out = key;
      return true;
    }
  }
  return false;
}

inline WindowState LoadWindowState(const prism::settings::SettingsStore& store) {
  WindowState state;
  state.width = store.GetInt("window.width", 0);
  state.height = store.GetInt("window.height", 0);
  state.listMode = store.GetBool("window.listMode", false);
  const std::string sortName = store.GetString("window.sort.key", "");
  prism::list::SortKey key = prism::list::SortKey::kName;
  if (!sortName.empty() && SortKeyFromName(sortName, &key)) {
    prism::list::SortSpec spec;
    spec.key = key;
    spec.direction = store.GetBool("window.sort.descending", false)
                         ? prism::list::SortDirection::kDescending
                         : prism::list::SortDirection::kAscending;
    state.sort.keys.push_back(spec);
    state.sort.dirsFirst = true;
    state.hasSort = true;
  }
  const int sizeW = store.GetInt("window.columns.size", 0);
  const int packedW = store.GetInt("window.columns.packed", 0);
  const int ratioW = store.GetInt("window.columns.ratio", 0);
  if (sizeW > 0 || packedW > 0 || ratioW > 0) {
    // A column narrower than 40 px cannot show a number; the store cannot know
    // that, so the clamp lives with the geometry it protects.
    if (sizeW >= 40) state.columns.sizeW = static_cast<uint32_t>(sizeW);
    if (packedW >= 40) state.columns.packedW = static_cast<uint32_t>(packedW);
    if (ratioW >= 40) state.columns.ratioW = static_cast<uint32_t>(ratioW);
    state.hasColumns = true;
  }
  return state;
}

inline void SaveWindowState(prism::settings::SettingsStore& store, const WindowState& state) {
  if (state.width > 0) store.SetInt("window.width", state.width);
  if (state.height > 0) store.SetInt("window.height", state.height);
  store.SetBool("window.listMode", state.listMode);
  if (!state.sort.keys.empty()) {
    store.SetString("window.sort.key", SortKeyNameOf(state.sort.keys.front().key));
    store.SetBool("window.sort.descending",
                  state.sort.keys.front().direction == prism::list::SortDirection::kDescending);
  }
  if (state.hasColumns) {
    store.SetInt("window.columns.size", static_cast<int>(state.columns.sizeW));
    store.SetInt("window.columns.packed", static_cast<int>(state.columns.packedW));
    store.SetInt("window.columns.ratio", static_cast<int>(state.columns.ratioW));
  }
}

}  // namespace prism::app
