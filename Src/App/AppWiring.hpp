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
// WHAT CARD C34 ADDED (wave 3 -- the last three joins):
//   * section 9: switching the OPEN archive after a re-pack. The compression
//     entry used to finish into a new file while the window kept showing the
//     old one; with C11's ReaderPool it is now safe to close the old pool and
//     open the new file (the order below is what makes a failed switch
//     non-destructive).
//   * section 10: the companion preview's HOST side. C30 shipped the channel
//     and the window; without a process that creates the section, every
//     right-click in Explorer took the NoReader fast path. CompanionHost is
//     that process half: hidden window + wake poll + "first 12 entries" scan +
//     ack + flyout.
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
#include <thread>
#include <utility>
#include <vector>

#include "../PrismCore/CompressionPlan.hpp"   // C9 decision layer (what to write)
#include "../PrismCore/DropPlan.hpp"           // C53: C52's drop model (what a drop means)
#include "../PrismCore/I18n.hpp"             // C19 resource tables + language choice
#include "../PrismCore/Log.hpp"              // C16 logger (the one warn a key miss gets)
#include "../PrismCore/ExtractScheduler.hpp"
#include "../PrismCore/ListSortModel.hpp"     // persisted sort key (C13 view state)
#include "../PrismCore/OpenFlow.hpp"          // C40: the measured E_ABORT password signal
#include "../PrismCore/SecurityGuard.hpp"
#include "../PrismCore/SessionStore.hpp"      // C46 session document (schema + crash guard)
#include "../PrismCore/SettingsStore.hpp"     // C14 document (schema + validation)
#include "../PrismCore/SidebarModel.hpp"      // C12 SidebarStore port
#include "../PrismCore/SmartExtractor.hpp"
#include "../PrismWin/AmsiGuard.hpp"
#include "../PrismWin/ArchiveWriter.hpp"      // C9 execution layer (the real writer)
#include "../PrismWin/DpapiVault.hpp"         // C40: the remember-me store (DPAPI)
#include "../PrismWin/MotwGuard.hpp"
#include "../PrismWin/PasswordDialog.hpp"     // C35: the password dialog (C40 wires it)
#include "../PrismWin/PrismEngineBridge.hpp"
#include "../PrismWin/ReaderPool.hpp"
#include "../PrismWin/CompanionPreviewBridge.hpp"  // C30: the shell <-> companion channel
#include "../PrismWin/CompanionWindow.hpp"            // C30: the focus-free flyout (C34 hosts it)
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
  // other verbs that take a path use archive, its head.
  std::vector<std::wstring> inputs;
  // C46: EVERY non-flag argument, for every mode. The gallery reads this list as
  // "the archives this launch opened" (a file-manager multi-selection or
  // "PrismArchive.exe a.7z b.7z"): the head is shown, the rest join the session
  // so the next launch can offer them. --compress-here keeps using inputs (its
  // arguments are sources to pack, not archives to open).
  std::vector<std::wstring> archives;
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
    request.archives.push_back(arg);  // C46: the gallery's session list
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
  // C40: a key that carries placeholders AND may be missing from a shipped table
  // (the tables on this branch predate several of them). Same fallback discipline
  // as Or(): a missing key shows the caller's REAL sentence, never the key name,
  // and Has() keeps the miss list clean.
  std::wstring Sub(std::string_view key,
                   const std::vector<prism::i18n::Substitution>& subs,
                   const std::wstring& fallback) const {
    if (translator == nullptr || !translator->Has(key)) return fallback;
    return prism::win::Utf8ToWide(translator->Translate(key, subs));
  }
};

// ---------------------------------------------------------------------------
// 7b. The sentences the open/refresh flow shows (C40).
//
// WHY THIS IS A FUNCTION AND NOT AN INLINE tr.Or(...) IN wWinMain: C38's audit
// found three fallbacks that had been DOUBLE-escaped ("\u5DF2\u5199..." in the
// source), so a table that lacked the key put a literal "\u5DF2\u5199..." on the
// user's status bar. A fallback that renders as escape text is worse than the key
// name it was hiding. Keeping them here makes the bytes assertable from a suite
// (the exe's own wWinMain is not reachable from a test), and the Chinese below is
// written as SINGLE-escaped universal character names -- real text at runtime,
// immune to the compiler's source-charset setting.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C40_MUTANT_ESCAPED_FALLBACK puts the
// refresh sentence back the way the audit found it; the suite's "a fallback
// contains no backslash" case MUST go red.
// ---------------------------------------------------------------------------
#if defined(PRISM_C40_MUTANT_ESCAPED_FALLBACK)
inline constexpr bool kFallbackIsEscaped = true;
#else
inline constexpr bool kFallbackIsEscaped = false;
#endif

// "Saved; the view refreshes when the running operation finishes."
inline std::wstring RefreshDeferredStatus(const Tr& tr) {
  const std::wstring fallback =
      kFallbackIsEscaped
          ? std::wstring(L"\\u5DF2\\u5199\\u5165\\uFF1B\\u5F53\\u524D"
                         L"\\u64CD\\u4F5C\\u7ED3\\u675F\\u540E\\u5237"
                         L"\\u65B0")
          : std::wstring(
                L"\u5DF2\u5199\u5165\uFF1B\u5F53\u524D\u64CD\u4F5C"
                L"\u7ED3\u675F\u540E\u5237\u65B0");
  return tr.Sub("status.refreshDeferred", {}, fallback);
}

// "Failed to open <name>" -- the table's error.open carries {name}. The fallback
// names the file too: a table-less checkout must not lose the one fact the user
// needs (which archive failed).
inline std::wstring OpenFailedStatus(const Tr& tr, const std::wstring& archiveName) {
  const std::wstring prefix =
      kFallbackIsEscaped
          ? std::wstring(L"\\u6253\\u5F00\\u5931\\u8D25")
          : std::wstring(L"\u6253\u5F00\u5931\u8D25");
  return tr.Sub("error.open", {{"name", prism::win::WideToUtf8(archiveName)}},
                prefix + L" " + archiveName);
}

// "(the engine reported no reason)". Split from the composition below because a
// WORKER must not touch the translator (its miss list is mutable): the UI thread
// resolves the sentence once and hands it to the worker as a value.
inline std::wstring EngineNoReasonText(const Tr& tr) {
  return tr.Sub("error.engine.noReason", {},
                std::wstring(L"\uFF08\u5F15\u64CE\u672A\u62A5\u544A"
                             L"\u539F\u56E0\uFF09"));
}

// The second line of a failed compression: the engine's reason, or `noReason`
// when it gave none. Pure string work, safe on any thread.
inline std::wstring CompressFailureDetail(const std::wstring& noReason,
                                          const std::string& engineError) {
  return engineError.empty() ? noReason : prism::win::Utf8ToWide(engineError);
}

// "3 items" for the status bar. status.items.count does not ship yet (C38's audit
// proposes it), so the fallback is the sentence the app already showed -- as real
// text, not "\x5171 ... \x9879".
inline std::wstring ItemsCountStatus(const Tr& tr, std::size_t count) {
  const std::wstring number = std::to_wstring(count);
  return tr.Sub("status.items.count", {{"n", std::to_string(count)}},
                kFallbackIsEscaped ? std::wstring(L"\\x5171 ") + number + L" \\x9879"
                                   : std::wstring(L"\u5171 ") + number + L" \u9879");
}

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

// ---------------------------------------------------------------------------
// 9. Switching the open archive (card C34).
//
// THE PROBLEM THIS SOLVES
//   C27's compression entry extracts the visible payload into %TEMP% and writes
//   a NEW archive. The refresh that followed re-read the archive the session
//   had opened -- not the file it had just produced -- because switching means
//   replacing every handler that holds the old file, and the sweep may still be
//   running. The user "compressed x.7z" and kept looking at the old bytes, with
//   the old handler pool still serving them.
//
// WHY IT WAS DEFERRED, AND WHY IT IS SAFE NOW
//   C11's ReaderPool made the handlers explicit (one lease per operation), so
//   the app can answer the only question that matters -- "is any lease still
//   live?" -- before it closes anything (Main.cpp asks it; this function is
//   only the ORDER).
//
// THE ORDER (each step prevents a failure)
//   1. OPEN the new file and read its catalogue. A broken new archive changes
//      nothing: the session keeps working on the old one.
//   2. LEASE a NEW pool against it. A pool that cannot open the new file is
//      reported while the old one is still alive and serving.
//   3. Only then CLOSE the old pool and publish the new state (path, catalog,
//      rows, pool). Closing first would leave the app with no handler at all if
//      (1) or (2) failed.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C34_MUTANT_NO_ARCHIVE_SWITCH restores
// the pre-C34 shape -- the refresh succeeds but the session keeps the archive it
// opened. The suite's "the current archive path and entry count are the new
// ones" assertion must go red.
// ---------------------------------------------------------------------------
struct ArchiveSession {
  prism::win::SevenZipLibrary* library = nullptr;
  std::shared_ptr<OperationHandles>* ops = nullptr;   // the lease behind the view
  std::vector<ArchiveItem>* catalog = nullptr;        // full catalogue (plans/search)
  std::vector<ArchiveItem>* items = nullptr;          // what the window navigates
  std::wstring* archivePath = nullptr;                // the file the session shows
  size_t handlerSlots = 2;
};

struct ArchiveSwitch {
  bool ok = false;            // the request was served (see switched)
  bool switched = false;      // the session really moved to the new file
  bool poolRebuilt = false;   // a new handler pool now owns the file
  std::string error;
  std::wstring archivePath;   // what the session shows AFTER the call
  size_t items = 0;
};

// Must run on the UI thread with NO live lease (see the order above). A caller
// that has a lease in flight either waits for it or defers the switch; this
// function does not guess.
inline ArchiveSwitch SwitchArchive(const ArchiveSession& session, const std::wstring& newPath) {
  ArchiveSwitch out;
  if (session.library == nullptr || session.ops == nullptr || session.catalog == nullptr ||
      session.items == nullptr || session.archivePath == nullptr) {
    out.error = "archive switch: incomplete session";
    return out;
  }
  if (newPath.empty()) {
    out.error = "archive switch: no target path";
    return out;
  }
  // (1) The new file, on its own handler, before anything is torn down.
  std::vector<ArchiveItem> next;
  {
    prism::win::ArchiveReader probe(*session.library);
    if (!probe.Open(newPath)) {
      out.error = "cannot open the new archive: " + probe.openTrace();
      return out;
    }
    next = probe.ReadCatalog();
  }
  if (next.empty()) {
    out.error = "the new archive has an empty catalogue";
    return out;
  }
  // (2) A pool that can serve it, still without touching the old one.
  OperationHandles::Config config;
  config.archivePath = newPath;
  config.handlerSlots = session.handlerSlots;
  std::string why;
  std::shared_ptr<OperationHandles> nextOps =
      OperationHandles::Create(*session.library, config, &why);
  if (!nextOps) {
    out.error = "handler pool for the new archive: " + why;
    return out;
  }
#if defined(PRISM_C34_MUTANT_NO_ARCHIVE_SWITCH)
  // MUTATION (D3): the refresh succeeds, the session does not move. The old
  // pool stays (it is still what the window is showing) and the freshly leased
  // one is closed so the mutant leaks no handler.
  nextOps->Close();
  out.ok = true;
  out.switched = false;
  out.poolRebuilt = false;
  out.archivePath = *session.archivePath;
  out.items = session.items->size();
  return out;
#else
  // (3) Publish. The old pool is closed FIRST so no new task can lease a
  // handler against a file the session no longer shows.
  if (*session.ops) (*session.ops)->Close();
  *session.ops = nextOps;
  *session.archivePath = newPath;
  *session.catalog = std::move(next);
  *session.items = *session.catalog;
  out.ok = true;
  out.switched = true;
  out.poolRebuilt = true;
  out.archivePath = *session.archivePath;
  out.items = session.items->size();
  return out;
#endif
}

// ---------------------------------------------------------------------------
// 10. The inline-preview companion's HOST half (card C34, C30's missing piece).
//
// C30 shipped the shared section, the wake/ack events and the focus-free flyout
// window, but nothing in PrismArchive ever CREATED the section -- so the shell's
// TryPublishPreview always answered NoReader and every right-click fell back to
// "launch the app". The channel is half a feature without a process that owns
// it, and that process is the running gallery.
//
// What this class is: the owner of the section/events (so CompanionPreviewPresent
// answers true while the app runs), the hidden flyout window, and the wake
// handling -- read the shell's packet, list the archive's FIRST N entries (12:
// one screenful, C30's kMaxPreviewEntries), acknowledge, place next to the click
// and show WITHOUT activation (C30's ExStylePolicy + SWP_NOACTIVATE are what make
// that true; nothing here may call SetForegroundWindow).
//
// Threading: everything runs on the UI thread (the window's idle timer calls
// Pump). The channel is two kernel objects, the scan is a header read, and the
// flyout is a window -- none of it needs a worker, and a worker would have to
// post everything back anyway.
//
// BOUNDS (why this cannot hang a right-click): the shell publishes
// fire-and-forget, so an abandoned companion stalls nobody; the reading side
// polls with a ZERO timeout (Pump is called from a timer) and only opens an
// archive the app does NOT have open when it is smaller than maxScanBytes -- a
// header scan of a 4 GiB archive is 78 s (docs/PERF.md), and a peek must never
// be that.
// ---------------------------------------------------------------------------
inline constexpr size_t kCompanionMaxEntries = prism::win::companion::kMaxPreviewEntries;
inline constexpr uint64_t kCompanionScanBudgetBytes = 256ull << 20;

class CompanionHost {
 public:
  struct Config {
    // The archive the window has open, and its catalogue. When the shell's
    // packet names THIS file the flyout needs no I/O at all (the common case:
    // the user right-clicks the archive they are looking at).
    const std::wstring* openArchive = nullptr;
    const std::vector<ArchiveItem>* catalog = nullptr;
    // The engine, for the foreign-archive case (a right-click on a file the
    // window is not showing). Without it, that case says "needs scan".
    prism::win::SevenZipLibrary* library = nullptr;
    // Test seam: replaces the real header read.
    std::function<bool(const std::wstring&, std::vector<ArchiveItem>*)> scanArchive;
    size_t maxScanBytes = static_cast<size_t>(kCompanionScanBudgetBytes);
  };

  CompanionHost() = default;
  ~CompanionHost() { Stop(); }
  CompanionHost(const CompanionHost&) = delete;
  CompanionHost& operator=(const CompanionHost&) = delete;

  static constexpr int kFlyoutWidth = 360;
  static constexpr int kFlyoutHeight = 300;

  // Creates the channel (the OWNER side: this is what makes the shell's presence
  // probe answer true) and the hidden flyout. False + *why is a diagnosable
  // degradation: the app runs, the shell takes its NoReader path.
  bool Start(HINSTANCE instance, const Config& config, std::string* why = nullptr) {
    Stop();
    config_ = config;
    if (!config_.scanArchive && config_.library != nullptr) {
      prism::win::SevenZipLibrary* library = config_.library;
      config_.scanArchive = [library](const std::wstring& path,
                                      std::vector<ArchiveItem>* out) {
        prism::win::ArchiveReader probe(*library);
        if (!probe.Open(path)) return false;
        *out = probe.ReadCatalog();
        return !out->empty();
      };
    }
    if (!channel_.CreateAsOwner()) {
      if (why != nullptr) {
        *why = "preview channel (section + wake/ack events) could not be created, win32 " +
               std::to_string(static_cast<unsigned long>(::GetLastError()));
      }
      return false;
    }
    if (!window_.RegisterClass(instance) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      if (why != nullptr) *why = "companion window class could not be registered";
      channel_.Close();
      return false;
    }
    // Created HIDDEN (WS_POPUP, never shown): a flyout visible before the user
    // asked for it is the bug this shape prevents.
    if (!window_.Create(instance, kFlyoutWidth, kFlyoutHeight)) {
      if (why != nullptr) *why = "companion window could not be created";
      channel_.Close();
      return false;
    }
    // C48: the flyout paints through a DirectComposition island now, and its
    // geometry is DPI-scaled. The host is the only place that knows this window
    // is per-monitor DPI aware, so the opt-in happens HERE -- Create() never
    // guesses (a 125% desktop must not silently move a test's 96-dpi rows).
    window_.SetDpiFromWindow();
    return true;
  }

  void Stop() {
    if (window_.Hwnd() != nullptr && ::IsWindow(window_.Hwnd())) ::DestroyWindow(window_.Hwnd());
    channel_.Close();
    scanned_.clear();
    shownPath_.clear();
    shownEntries_ = 0;
    wakes_ = handled_ = refused_ = oversized_ = 0;
  }

  // One non-blocking poll. True when a wake was consumed.
  bool HandleWake() {
    if (!channel_.IsOpen() || window_.Hwnd() == nullptr || !::IsWindow(window_.Hwnd())) {
      return false;
    }
    if (!channel_.WaitForWake(0)) return false;  // the timer's tick, never a wait
    ++wakes_;
    prism::win::companion::PreviewPacket incoming{};
    if (!channel_.Read(incoming)) {
      // Torn or foreign layout: keep whatever the flyout shows instead of
      // painting half a list (C30's coherency contract).
      ++refused_;
      return false;
    }
    const std::wstring path = prism::win::companion::ArchivePathOf(incoming);
    const std::vector<ArchiveItem>* rows = nullptr;
    if (config_.catalog != nullptr && config_.openArchive != nullptr &&
        *config_.openArchive == path) {
      rows = config_.catalog;
    } else if (ScanForeign(path)) {
      rows = &scanned_;
    }

    prism::win::companion::PreviewPacket outgoing{};
    prism::win::companion::ResetPacket(outgoing);
    prism::win::companion::SetArchivePath(outgoing, path);
    outgoing.cursorX = incoming.cursorX;
    outgoing.cursorY = incoming.cursorY;
    size_t listed = 0;
    if (rows != nullptr) {
      for (const ArchiveItem& item : *rows) {
        if (listed == kCompanionMaxEntries) break;
        if (!prism::win::companion::AddEntry(outgoing, prism::win::Utf8ToWide(item.path), item.size,
                                            item.isDir)) {
          break;
        }
        ++listed;
      }
    } else {
      // Nothing to show is SAID, not faked: the flyout keeps its header and the
      // shell's own flag tells it why the list is empty.
      outgoing.flags |= prism::win::companion::kFlagNeedsCatalogScan;
    }
    window_.ApplyPreview(outgoing);
    channel_.Acknowledge();  // the ack is the writer's (bounded) proof of receipt
    ++handled_;
    shownPath_ = path;
    shownEntries_ = listed;
    ShowAtCursor(incoming.cursorX, incoming.cursorY);
    return true;
  }

  // The idle tick: drain what the shell published since the last one.
  void Pump(int maxWakes = 8) {
    for (int i = 0; i < maxWakes; ++i) {
      if (!HandleWake()) break;
    }
  }

  // ---- read-back (the suite asserts against these, not against internals) --
  prism::win::companion::CompanionWindow& Window() { return window_; }
  const prism::win::companion::CompanionWindow& Window() const { return window_; }
  bool ChannelOpen() const { return channel_.IsOpen(); }
  bool ChannelOwner() const { return channel_.IsOwner(); }
  bool Visible() const { return window_.IsVisible(); }
  const std::wstring& ShownPath() const { return shownPath_; }
  size_t ShownEntries() const { return shownEntries_; }
  size_t Wakes() const { return wakes_; }
  size_t Handled() const { return handled_; }
  size_t Refused() const { return refused_; }
  size_t Oversized() const { return oversized_; }

 private:
  // An archive the window is not showing: header read, bounded by size.
  bool ScanForeign(const std::wstring& path) {
    if (!config_.scanArchive) return false;
    if (config_.maxScanBytes > 0) {
      WIN32_FILE_ATTRIBUTE_DATA data{};
      if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        ULARGE_INTEGER bytes{};
        bytes.LowPart = data.nFileSizeLow;
        bytes.HighPart = data.nFileSizeHigh;
        if (bytes.QuadPart > config_.maxScanBytes) {
          ++oversized_;
          return false;
        }
      }
    }
    scanned_.clear();
    if (!config_.scanArchive(path, &scanned_)) return false;
    return !scanned_.empty();
  }

  // Off the click, inside the monitor's work area, WITHOUT activation: C30's
  // flyout owns that policy (SWP_NOACTIVATE / SW_SHOWNOACTIVATE); this only
  // supplies the cursor and the work area.
  void ShowAtCursor(int cursorX, int cursorY) {
    const POINT cursor{cursorX, cursorY};
    RECT work{};
    bool haveWork = false;
    if (const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)) {
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      if (::GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
        haveWork = true;
      }
    }
    if (!haveWork) {
      work.left = 0;
      work.top = 0;
      work.right = ::GetSystemMetrics(SM_CXSCREEN);
      work.bottom = ::GetSystemMetrics(SM_CYSCREEN);
    }
    window_.PlaceForCursor(cursor, work);
  }

  Config config_;
  prism::win::companion::PreviewChannel channel_;
  prism::win::companion::CompanionWindow window_;
  std::vector<ArchiveItem> scanned_;  // the foreign archive's rows (reused)
  std::wstring shownPath_;
  size_t shownEntries_ = 0;
  size_t wakes_ = 0;
  size_t handled_ = 0;
  size_t refused_ = 0;
  size_t oversized_ = 0;
};

// ---------------------------------------------------------------------------
// 11. The password gate (C40): C35's dialog meets the app's open flow.
//
// WHAT WAS MISSING: C5 built the ring, the open-flow state machine and the DPAPI
// vault; C35 built the dialog. Nothing joined them, so an encrypted archive
// produced "Failed to open" and no way to type a password.
//
// THE THREADING SHAPE IS THE CARD'S REAL CONTENT (CONTRACTS B9):
//   * Submit runs on the UI thread (the dialog's own WM_COMMAND);
//   * the PROBE runs on ONE worker (7z.dll Open can take seconds on a large
//     header), and that worker's only way back is PostMessage;
//   * FinishProbe runs on the UI thread, because it touches controls;
//   * the vault write happens in the dialog's opened handler, i.e. after the UI
//     thread has seen the hit -- never on the worker.
// PasswordProbeRunner and PasswordResultWindow are the two halves of that hop,
// and the suite drives BOTH (plus the dialog and the vault) end to end.
// ---------------------------------------------------------------------------
struct PasswordProbeResult {
  bool found = false;
  bool cancelled = false;
  std::string password;     // filled from the attempt that opened the archive
  std::string diagnostics;  // engine trace, redacted by the dialog's model
};

// One worker, one POST. Start() is UI-thread-only; Probe runs off it; Post is
// called ON the worker and must not touch UI state (the app passes PostMessageW,
// the suite passes PasswordResultWindow::Post).
class PasswordProbeRunner {
 public:
  using Probe = std::function<bool(const std::string& password, std::string* diagnostics)>;
  using Post = std::function<void(const PasswordProbeResult& result)>;

  PasswordProbeRunner() = default;
  ~PasswordProbeRunner() { Join(); }
  PasswordProbeRunner(const PasswordProbeRunner&) = delete;
  PasswordProbeRunner& operator=(const PasswordProbeRunner&) = delete;

  // Returns false when a probe is already in flight: the dialog disables its OK
  // button in that state, so this is the belt to that braces.
  bool Start(const std::string& password, Probe probe, Post post) {
    if (busy_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, password, probe = std::move(probe),
                           post = std::move(post)]() {
      // A mutable local, not the capture: the attempt is scrubbed the moment the
      // engine has answered, so the worker's copy does not outlive the probe.
      std::string attempt = password;
      PasswordProbeResult result;
      std::string diagnostics;
      result.found = probe(attempt, &diagnostics);
      result.diagnostics = std::move(diagnostics);
      if (result.found) result.password = attempt;
      prism::pwd::PasswordRing::SecureZero(attempt);
      busy_.store(false);
      post(result);  // POST only: the UI thread owns every window call
    });
    return true;
  }

  bool Busy() const { return busy_.load(); }
  void Join() {
    if (worker_.joinable()) worker_.join();
  }

 private:
  std::thread worker_;
  std::atomic<bool> busy_{false};
};

// A message-only window that turns "the worker posted a result" into "the UI
// thread runs this handler". The payload is owned by the message: the receiver
// frees it, and a failed PostMessage frees it immediately (nothing leaks when the
// window is already gone).
class PasswordResultWindow {
 public:
  static constexpr UINT kResultMessage = WM_APP + 0x40;
  using Deliver = std::function<void(const PasswordProbeResult&)>;

  PasswordResultWindow() = default;
  ~PasswordResultWindow() { Destroy(); }
  PasswordResultWindow(const PasswordResultWindow&) = delete;
  PasswordResultWindow& operator=(const PasswordResultWindow&) = delete;

  bool Create(HINSTANCE instance, Deliver deliver) {
    deliver_ = std::move(deliver);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PasswordResultWindow::ProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = L"PrismPasswordProbeSink";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, instance, this);
    return hwnd_ != nullptr;
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  HWND Hwnd() const { return hwnd_; }

  // WORKER THREAD. Never touches deliver_; it only posts.
  bool Post(const PasswordProbeResult& result) {
    if (hwnd_ == nullptr) return false;
    auto* payload = new PasswordProbeResult(result);
    if (PostMessageW(hwnd_, kResultMessage, 0, reinterpret_cast<LPARAM>(payload)) == FALSE) {
      delete payload;
      return false;
    }
    return true;
  }

  // Diagnostics for the suite: how many results arrived and on WHICH thread.
  int delivered() const { return delivered_; }
  DWORD deliveredThread() const { return deliveredThread_; }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PasswordResultWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<PasswordResultWindow*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      // Children (there are none) and the first WM_ message need the handle
      // stored before CreateWindowExW returns.
      if (self != nullptr) self->hwnd_ = hwnd;
    }
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->Handle(hwnd, message, wparam, lparam);
  }

  LRESULT Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kResultMessage) {
      std::unique_ptr<PasswordProbeResult> payload(
          reinterpret_cast<PasswordProbeResult*>(lparam));
      ++delivered_;
      deliveredThread_ = GetCurrentThreadId();
      if (payload && deliver_) deliver_(*payload);
      return 0;
    }
    if (message == WM_DESTROY) {
      hwnd_ = nullptr;
      return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  HWND hwnd_ = nullptr;
  Deliver deliver_;
  int delivered_ = 0;
  DWORD deliveredThread_ = 0;
};

// Where the remember-me vault lives: <LOCALAPPDATA>\PrismArchive\passwords.vault.
inline std::wstring DefaultVaultPath() {
  const std::wstring root = prism::win::settingsfile::PathsUnder(
                                prism::win::settingsfile::LocalAppDataDirectory())
                                .root;
  return root.empty() ? std::wstring() : root + L"\\passwords.vault";
}

// "Remember" persists the ONE password the probe PROVED, through DPAPI. The path
// is a parameter so the suite can use a temp store (a test must never touch the
// developer's real vault), and a failed write is a false, never a silent success.
inline bool RememberPassword(const std::wstring& storePath, const std::string& password) {
  if (storePath.empty() || password.empty()) return false;
  prism::win::DpapiVault vault(storePath);
  return vault.Add(password);
}

// Does this failed Open mean "we were never given a password"? The measured
// signal (docs/OPENFLOW.md): a header-encrypted archive asks our open callback
// for a password, and with none supplied the callback answers E_ABORT -- the
// trace carries that HRESULT. A WRONG password on the same archive is
// indistinguishable from "not this format", which is why this predicate only ever
// answers "we have not tried one yet".
inline bool OpenNeedsPassword(const std::string& openTrace) {
  char needle[32] = {};
  std::snprintf(needle, sizeof(needle), "hr=0x%08lX",
                static_cast<unsigned long>(prism::openflow::kOpenHrAbort));
  return openTrace.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 12. Session (C46): the last archives, their view, and the crash guard.
//
// C14/C27's settings document answers "how do I like it"; it cannot answer "what
// was I doing". This section is the FILE half of the session card -- the model,
// the schema and the clean-shutdown policy live in PrismCore/SessionStore.hpp so
// they are testable on Linux. Everything Win32 stays on this side:
//
//   * ONE location, next to the settings file: %LOCALAPPDATA%\PrismArchive\
//     session.json (C14 already owns that folder's creation path);
//   * the SAME atomic publish C14 uses (temp file + ReplaceFileW through
//     SaveFileAtomic) -- a half-written session is indistinguishable from a
//     crash, and would be treated as one;
//   * a test seam: LOCALAPPDATA is an input, so a suite can point the whole
//     stack at a temp folder without touching the developer's real session.
//
// The dirty mark is written by the CALLER's order: the exe arms it
// (MarkCleanShutdown(false) + SaveSession) as soon as it owns a session, and
// clears it at the end of a normal exit. A killed process therefore leaves
// "cleanShutdown": false behind, and the next start restores nothing.
// ---------------------------------------------------------------------------
inline constexpr wchar_t kSessionFileName[] = L"session.json";

inline std::wstring SessionFileUnder(const std::wstring& localAppData) {
  const prism::win::settingsfile::Paths root = prism::win::settingsfile::PathsUnder(localAppData);
  if (!root.valid()) return {};
  return root.root + L"\\" + kSessionFileName;
}

struct SessionBootstrap {
  prism::session::SessionStore store;
  prism::session::SessionLoadReport report;
  std::wstring file;
  bool loaded = false;   // bytes existed and were handed to the model
  std::string detail;    // why loading failed (a warn line, never a dialog)
};

inline SessionBootstrap LoadSessionUnder(const std::wstring& localAppData) {
  SessionBootstrap bootstrap;
  bootstrap.file = SessionFileUnder(localAppData);
  if (bootstrap.file.empty()) {
    bootstrap.detail = "%LOCALAPPDATA% could not be resolved";
    return bootstrap;
  }
  std::string bytes;
  DWORD readError = 0;
  if (!prism::win::settingsfile::ReadAllBytes(bootstrap.file, bytes, &readError)) {
    // Not found is NORMAL: the first run of a fresh profile has no session yet.
    if (readError != ERROR_FILE_NOT_FOUND && readError != ERROR_PATH_NOT_FOUND) {
      bootstrap.detail = "session read failed (win32 error " + std::to_string(readError) + ")";
    }
    return bootstrap;
  }
  bootstrap.report = bootstrap.store.Load(bytes);
  bootstrap.loaded = true;
  return bootstrap;
}

inline SessionBootstrap LoadSession() {
  return LoadSessionUnder(prism::win::settingsfile::LocalAppDataDirectory());
}

// Atomically publishes the document. Returns false and fills detail on failure:
// a session that silently stops being remembered is a bug, not a preference.
inline bool SaveSession(SessionBootstrap* bootstrap, std::string* detail = nullptr) {
  if (bootstrap == nullptr) {
    if (detail != nullptr) *detail = "no session document";
    return false;
  }
  if (bootstrap->file.empty()) {
    if (detail != nullptr) *detail = "%LOCALAPPDATA% could not be resolved";
    return false;
  }
  std::string bytes = bootstrap->store.ToJson();
  bytes.push_back('\n');
  prism::win::settingsfile::SaveResult saved;
  if (!prism::win::settingsfile::SaveFileAtomic(bootstrap->file, bytes, &saved)) {
    if (detail != nullptr) {
      *detail = "session save failed (win32 error " + std::to_string(saved.error) + ")";
    }
    return false;
  }
  return true;
}

// The restore pass as a decision, with the engine injected: an entry is
// restorable only when it still OPENS. Missing and damaged archives are counted
// and skipped -- eight message boxes at startup is worse than eight quiet skips
// (the card's contract, and the reason this is one predicate and not a loop of
// error handlers).
struct SessionRestorePlan {
  std::vector<prism::session::SessionEntry> entries;  // in session order
  std::size_t tried = 0;
  std::size_t skipped = 0;
};

inline SessionRestorePlan PlanSessionRestore(const prism::session::SessionStore& store,
                                             const std::function<bool(const std::wstring&)>& canOpen) {
  SessionRestorePlan plan;
  for (const prism::session::SessionEntry& entry : store.RestorableEntries()) {
    ++plan.tried;
    if (canOpen && canOpen(prism::win::Utf8ToWide(entry.path))) {
      plan.entries.push_back(entry);
    } else {
      ++plan.skipped;
    }
  }
  return plan;
}

// Applies a restored entry's VIEW to a gallery window: list/grid mode first, then
// the folder inside the archive, walked one segment at a time through the same
// ActivateRow a double-click uses (a window that owns navigation stays the only
// thing that moves the cursor -- no new public navigation API is invented here
// while GALLERY V2 is being written).
//
// A segment that is no longer there ends the walk: the user lands at the deepest
// folder that still exists, which is what "the archive changed since yesterday"
// should do. Returns true when the whole saved folder was reached.
template <class WindowLike>
bool ApplySessionView(WindowLike& window, const prism::session::SessionEntry& entry) {
  window.SetListMode(entry.listMode);
  bool complete = true;
  for (const std::string& segment : prism::session::FolderSegments(entry.folder)) {
    bool stepped = false;
    const auto& items = window.Items();
    for (std::size_t row = 0; row < items.size(); ++row) {
      const std::string& path = items[row].path;
      if (!items[row].isDir) continue;
      const std::size_t slash = path.find_last_of('/');
      const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
      if (leaf != segment) continue;
      stepped = window.ActivateRow(static_cast<int>(row));
      break;
    }
    if (!stepped) {
      complete = false;
      break;
    }
  }
  return complete;
}

// ---------------------------------------------------------------------------
// C53: THE APP'S HALF OF A DROP ONTO THE WINDOW (C52's missing seam)
//
// C52 delivered the window's half: Explorer's HDROP becomes a tree, the tree
// becomes a prism::drop::DropPlan, and the window forwards that plan to whoever
// registered SetDropHandler. Nothing executed it, so a user saw the hint and
// then nothing. This section is the EXECUTOR, and it lives in AppWiring so the
// suite drives the production lambda instead of re-implementing it.
//
// WHAT EACH ACTION MAY DO TODAY
//   * OpenArchive  -- reuse the archive-changed flow: post the path, the app
//                     re-opens the file and rebuilds the view (the same door the
//                     compress card's re-pack uses, so there is one switch path).
//   * Refuse       -- post the model's own reason. A refusal that says nothing
//                     is indistinguishable from a broken drop target (C5/C52).
//   * AddToArchive -- REFUSED EXPLICITLY. ArchiveWriter has no incremental
//                     "add these files to this archive" operation: it writes a
//                     NEW archive from a source root. Pretending success here
//                     would be a half-write the user could not see, so the app
//                     says "append is not supported yet" and writes NOTHING.
//   * ExtractHere / TestArchive -- need the extract/verify flow callable with an
//                     EXPLICIT file list instead of the whole catalog; until that
//                     seam exists they report the plan's own headline on the
//                     status line (visible feedback, never a half-run write).
//
// The negative control (D3/D27) compiles the AddToArchive branch into a claim
// of success (PRISM_C53_MUTANT_FAKE_ADD): the suite's "the bytes did not move", the suite's "the bytes did not move"
// conjunction goes red, and the first failure prints the CAUGHT marker.
// ---------------------------------------------------------------------------
namespace dropin {

enum class DropEffect : std::uint8_t {
  OpenArchive = 0,  // post the path to the archive-changed flow
  Refuse,           // post the model's reason text
  UnsupportedAdd,   // AddToArchive: explicitly not implemented, nothing written
  ReportStatus,     // ExtractHere/TestArchive: status only, nothing executed
};

inline const char* DropEffectName(DropEffect effect) {
  switch (effect) {
    case DropEffect::OpenArchive: return "OpenArchive";
    case DropEffect::Refuse: return "Refuse";
    case DropEffect::UnsupportedAdd: return "UnsupportedAdd";
    case DropEffect::ReportStatus: return "ReportStatus";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, DropEffect effect) {
  return os << DropEffectName(effect);
}

struct DropDecision {
  DropEffect effect = DropEffect::ReportStatus;
  std::wstring archivePath;  // OpenArchive only
  std::wstring status;       // everything except OpenArchive
};

// The one decision the drop handler makes. Pure on purpose: the suite asserts
// the TEXT the user would read (D2), not a boolean that could be satisfied by
// any message at all.
inline DropDecision DecideDrop(const prism::drop::DropPlan& plan) {
  using prism::drop::DropAction;
  DropDecision decision;
  decision.status = prism::win::Utf8ToWide(plan.headline);
  if (!plan.detail.empty()) {
    decision.status += L"  ";
    decision.status += prism::win::Utf8ToWide(plan.detail);
  }
  if (plan.primary == DropAction::OpenArchive && !plan.members.empty()) {
    decision.effect = DropEffect::OpenArchive;
    decision.archivePath = prism::win::Utf8ToWide(plan.members.front());
    decision.status.clear();  // the switch reports its own outcome
    return decision;
  }
  switch (plan.primary) {
    case DropAction::OpenArchive:
      // The model said "open" but handed over no member: nothing to open. Say
      // so instead of posting an empty path into the switch (D18).
      decision.effect = DropEffect::Refuse;
      decision.status = L"\u6CA1\u6709\u53EF\u6253\u5F00\u7684\u5F52\u6863";
      return decision;
    case DropAction::Refuse:
      decision.effect = DropEffect::Refuse;
      return decision;  // headline + detail already name the reason
    case DropAction::AddToArchive:
#if defined(PRISM_C53_MUTANT_FAKE_ADD)
      // MUTANT ACTIVE (D28: the banner sits inside the guard, not at the end of
      // the file): a drop that the model read as "add" claims the files were
      // written although no writer call exists anywhere on this path.
      decision.effect = DropEffect::ReportStatus;
      decision.status = L"\u5DF2\u6DFB\u52A0 " + std::to_wstring(plan.fileCount) + L" \u9879";
      return decision;
#else
      // No half-write, no fake success: the writer has no incremental add, so
      // the app states that in the user's words and leaves the archive alone.
      decision.effect = DropEffect::UnsupportedAdd;
      decision.status = L"\u6682\u4E0D\u652F\u6301\u8FFD\u52A0 (AddToArchive)\uFF1A\u62D6\u5165\u7684 " +
                        std::to_wstring(plan.fileCount) +
                        L" \u4E2A\u6587\u4EF6\u6CA1\u6709\u88AB\u5199\u5165"
                        L"\uFF08ArchiveWriter \u5C1A\u65E0\u589E\u91CF\u6DFB\u52A0\u80FD\u529B\uFF09";
      return decision;
#endif
    case DropAction::ExtractHere:
    case DropAction::TestArchive:
      decision.effect = DropEffect::ReportStatus;
      decision.status += L"\uFF08\u5C1A\u672A\u63A5\u5165\uFF1A\u672C\u6B21\u6CA1\u6709\u6267\u884C\u4EFB\u4F55\u5199\u5165\uFF09";
      return decision;
  }
  return decision;
}

// The PRODUCTION handler. Main.cpp calls this once; the Windows suite calls the
// same function, so the tested lambda is the shipped lambda.
template <class WindowLike>
void InstallDropHandler(WindowLike& window) {
  window.SetDropHandler([&window](const prism::drop::DropPlan& plan) {
    const DropDecision decision = DecideDrop(plan);
    if (decision.effect == DropEffect::OpenArchive) {
      window.PostArchiveChanged(decision.archivePath);
      return;
    }
    if (!decision.status.empty()) window.PostStatus(decision.status);
  });
}

}  // namespace dropin

}  // namespace prism::app
