// PrismArchive -- the assembly layer (card C25, wave 2).
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
// WHAT IS *NOT* WIRED (deliberate, and stated in the PR):
//   * C9 (archive creation / compression) -- out of this card's scope (it merged
//     into main while this wiring was being built). See TODO(C9) below: the
//     verbs exist in shell_launch::ArgsForVerb, and the parser here REFUSES
//     them instead of guessing a command line.
//   * C14 (settings persistence) -- out of scope for the same reason. See
//     TODO(C14): the sidebar model takes an injected SidebarStore port and this
//     layer passes none, so nothing is written to disk and nothing pretends to
//     be remembered.
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
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/ExtractScheduler.hpp"
#include "../PrismCore/SecurityGuard.hpp"
#include "../PrismCore/SmartExtractor.hpp"
#include "../PrismWin/AmsiGuard.hpp"
#include "../PrismWin/MotwGuard.hpp"
#include "../PrismWin/PrismEngineBridge.hpp"
#include "../PrismWin/ReaderPool.hpp"
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
enum class LaunchMode : std::uint8_t {
  kGallery = 0,    // no verb: open the window (double-click path)
  kSmartExtract,   // --smart-extract
  kExtractHere,    // --extract-here
  kExtractSub,     // --extract-sub
  kTest,           // --test (verify integrity, write nothing)
};

inline const char* LaunchModeName(LaunchMode mode) {
  switch (mode) {
    case LaunchMode::kGallery: return "Gallery";
    case LaunchMode::kSmartExtract: return "SmartExtract";
    case LaunchMode::kExtractHere: return "ExtractHere";
    case LaunchMode::kExtractSub: return "ExtractSub";
    case LaunchMode::kTest: return "Test";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, LaunchMode mode) {
  return os << LaunchModeName(mode);
}

// True for the three modes that write files.
inline bool Extracts(LaunchMode mode) {
  return mode == LaunchMode::kSmartExtract || mode == LaunchMode::kExtractHere ||
         mode == LaunchMode::kExtractSub || mode == LaunchMode::kGallery;
}

struct LaunchRequest {
  LaunchMode mode = LaunchMode::kGallery;
  std::wstring archive;      // first non-flag argument
  std::wstring flag;         // the verb flag we matched ("" when none)
  // Set when the caller passed a flag we do NOT implement. The app must refuse
  // visibly rather than fall back to "open the gallery": a context-menu command
  // that silently does something else is worse than one that says it is not
  // ready (this is the C9/TODO surface, see the header comment).
  bool unknownFlag = false;
  std::wstring unknownText;
};

inline bool StartsWithFlag(const std::wstring& text) {
  return text.size() >= 2 && text[0] == L'-' && text[1] == L'-';
}

inline LaunchRequest ParseLaunch(const std::vector<std::wstring>& argv) {
  LaunchRequest request;
  for (const std::wstring& arg : argv) {
    if (StartsWithFlag(arg)) {
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
      } else {
        // TODO(C9): --compress-7z / --compress-zip / --smart-extract-each are
        // in C4's table but their implementation is card C9 (in flight). Refuse
        // loudly; never launch a guess.
        request.unknownFlag = true;
        request.unknownText = arg;
      }
      continue;
    }
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
// 6. Hooks for the waves that are still in flight.
//
// TODO(C9): archive creation (compress to 7z/zip) is card C9. This layer must
//   not grow a second writer before that card lands: the parser above already
//   refuses --compress-* instead of guessing, so the exe cannot half-implement
//   it. When C9 lands, its verb goes into ParseLaunch and its plan into a
//   sibling of BuildExtractionPlan.
// TODO(C14): settings persistence is card C14. prism::sidebar::Model takes an
//   injected SidebarStore port; this layer passes none, so favourites/recent are
//   session-only and nothing is written to disk or registry. When C14 lands, its
//   store is passed here (one line) and the window asks the model to Serialize.
// ---------------------------------------------------------------------------

}  // namespace prism::app
