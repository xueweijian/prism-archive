// PrismArchive -- PrismCore
// ExtractScheduler: the decision core behind "one 7-Zip handler per concurrent
// operation" (blueprint ch. 4.3 + docs/PROGRESS known debt, 2026-11-01).
//
// THE DEBT THIS MODULE EXISTS FOR
//   sweep (background thumbnail scan) and extract (the thing the user is
//   waiting for) used to share ONE ArchiveReader. A 7-Zip handler is not
//   thread-safe, and the reader owns a single input stream, so "shared reader"
//   is both a data race inside 7z.dll and a shared file pointer. 7-Zip's own
//   rule is one handler per concurrent operation. This file answers the
//   scheduling half of that rule, entirely as pure C++ (Linux-tested):
//
//     * a task queue with a HARD backlog (backpressure is an answer, not a
//       silent growth),
//     * one handler slot per running task -- Dispatch() hands out a slot index
//       that maps 1:1 onto a ReaderPool lease,
//     * strict priority: user-initiated extraction runs before background
//       thumbnail sweeping, and a user task may ask the caller to abort a
//       background task that is merely holding a slot (cooperative
//       preemption; 7z can only be stopped from its callbacks),
//     * cancellation that distinguishes queued (drop now) from running
//       (request; the caller aborts at its next callback),
//     * a contract predicate for the concurrency rule itself:
//       slotInvariantHolds().
//
// THREADING CONTRACT
//   ExtractScheduler is deliberately NOT internally synchronized: the owner
//   (the UI thread) serializes every call. Workers never touch it -- they are
//   handed a Dispatch plus an abort flag and they report back with Complete()
//   through the owner. That keeps the decision core deterministic and unit
//   testable; the lock lives in ReaderPool (PrismWin), where the actual
//   handler leases are contended.
//
// MUTATION SEAMS (CONTRACTS D3 -- the suite compiles the same test against
// each of these and requires it to go red):
//   PRISM_C11_MUTANT_IGNORE_PRIORITY  -- dispatch FIFO, ignoring priority
//   PRISM_C11_MUTANT_UNBOUNDED_BACKLOG-- never answer kBusy (queue grows)
//   PRISM_C11_MUTANT_SHARE_SLOT       -- every running task gets slot 0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(PRISM_C11_MUTANT_IGNORE_PRIORITY) || defined(PRISM_C11_MUTANT_UNBOUNDED_BACKLOG) || \
    defined(PRISM_C11_MUTANT_SHARE_SLOT)
#define PRISM_C11_SCHEDULER_MUTANT 1
#endif

namespace prism::core::sched {

// User-initiated extraction wins over background thumbnail sweeping. This is
// the whole reason the queue has priorities: a large solid sweep must never
// make a double-clicked extraction wait behind a screenful of thumbnails.
enum class Priority : int { kBackground = 0, kUser = 1 };

// What the task will do with its handler. Diagnostics + policy input; the
// scheduler does not call the engine itself.
enum class Kind : int { kSweep = 0, kExtract = 1 };

enum class TaskState : int { kQueued = 0, kRunning = 1, kDone = 2, kFailed = 3, kCancelled = 4 };

enum class SubmitStatus : int {
  kQueued,     // accepted into the backlog; the caller pumps with Dispatch()
  kBusy,       // rejected: the backlog is full (backpressure -- retry later)
  kDuplicate,  // rejected: an equal non-empty key is still queued/running
  kClosed,     // rejected: Close() has been called
};

enum class CancelOutcome : int {
  kNotFound = 0,        // never existed (or already forgotten)
  kCancelledQueued = 1, // dropped before it ever got a slot
  kCancellationRequested = 2,  // running: caller must abort at its next callback
  kAlreadyFinished = 3,
};

inline const char* PriorityName(Priority p) { return p == Priority::kUser ? "user" : "background"; }
inline const char* KindName(Kind k) { return k == Kind::kExtract ? "extract" : "sweep"; }
inline const char* TaskStateName(TaskState s) {
  switch (s) {
    case TaskState::kQueued: return "queued";
    case TaskState::kRunning: return "running";
    case TaskState::kDone: return "done";
    case TaskState::kFailed: return "failed";
    case TaskState::kCancelled: return "cancelled";
  }
  return "?";
}
inline const char* SubmitStatusName(SubmitStatus s) {
  switch (s) {
    case SubmitStatus::kQueued: return "queued";
    case SubmitStatus::kBusy: return "busy";
    case SubmitStatus::kDuplicate: return "duplicate";
    case SubmitStatus::kClosed: return "closed";
  }
  return "?";
}
inline const char* CancelOutcomeName(CancelOutcome c) {
  switch (c) {
    case CancelOutcome::kNotFound: return "not-found";
    case CancelOutcome::kCancelledQueued: return "cancelled-queued";
    case CancelOutcome::kCancellationRequested: return "cancellation-requested";
    case CancelOutcome::kAlreadyFinished: return "already-finished";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, Priority p) { return os << PriorityName(p); }
inline std::ostream& operator<<(std::ostream& os, Kind k) { return os << KindName(k); }
inline std::ostream& operator<<(std::ostream& os, TaskState s) { return os << TaskStateName(s); }
inline std::ostream& operator<<(std::ostream& os, SubmitStatus s) {
  return os << SubmitStatusName(s);
}
inline std::ostream& operator<<(std::ostream& os, CancelOutcome c) {
  return os << CancelOutcomeName(c);
}

struct Config {
  // Handler slots == ReaderPool capacity. One slot is one IInArchive handler
  // and one input stream; the pool is built with exactly this number.
  size_t handlerSlots = 2;
  // Maximum tasks WAITING for a slot. A full backlog answers Submit with kBusy
  // instead of growing without bound (D17: the queue holds exactly backlog).
  size_t backlog = 8;
  // A user task that finds every slot held by background work asks the caller
  // to abort the oldest background task (cooperative; see Preemption).
  bool preemptBackgroundForUser = true;
};

// A unit of work. key is a caller-chosen identity (for example
// "sweep:<archive>" or "extract:<archive>:<entry>"); non-empty keys dedupe, so
// double-clicking the same entry twice cannot start two extractions.
struct TaskSpec {
  Kind kind = Kind::kExtract;
  Priority priority = Priority::kUser;
  std::string key;
  std::string label;
};

struct Task {
  uint64_t id = 0;       // 1-based, monotonic; also the records_ index + 1
  uint64_t sequence = 0;
  Kind kind = Kind::kExtract;
  Priority priority = Priority::kUser;
  std::string key;
  std::string label;
  TaskState state = TaskState::kQueued;
  std::optional<size_t> slot;  // handler slot while running
  bool cancelRequested = false;
  bool preemptionRequested = false;
};

// What the caller must run, and which handler it must lease. The slot index is
// the ReaderPool slot; a test that sees two running dispatches with the same
// slot has caught the shared-handler bug.
struct Dispatch {
  uint64_t id = 0;
  size_t slot = 0;
  Kind kind = Kind::kExtract;
  Priority priority = Priority::kUser;
  std::string label;
};

// "Abort this running background task now; a user extraction wants its slot."
struct Preemption {
  uint64_t id = 0;        // the RUNNING task to abort
  uint64_t forTask = 0;   // the user task waiting for the slot
  std::string reason;
};

struct Stats {
  uint64_t submitted = 0;
  uint64_t rejectedBusy = 0;
  uint64_t rejectedDuplicate = 0;
  uint64_t rejectedClosed = 0;
  uint64_t dispatches = 0;
  uint64_t completedOk = 0;
  uint64_t failed = 0;
  uint64_t cancelledQueued = 0;
  uint64_t cancellationRequests = 0;  // running tasks asked to abort
  uint64_t cancelledRunning = 0;
  uint64_t preemptions = 0;
  size_t peakRunning = 0;
};

class ExtractScheduler {
 public:
  explicit ExtractScheduler(Config config = Config{}) : config_(config) {
    if (config_.handlerSlots == 0) {
      throw std::invalid_argument("ExtractScheduler: handlerSlots must be >= 1");
    }
    if (config_.backlog == 0) {
      throw std::invalid_argument("ExtractScheduler: backlog must be >= 1");
    }
  }

  ExtractScheduler(const ExtractScheduler&) = delete;
  ExtractScheduler& operator=(const ExtractScheduler&) = delete;

  // ---- submission ---------------------------------------------------------
  SubmitStatus Submit(const TaskSpec& spec, uint64_t* idOut = nullptr) {
    ++stats_.submitted;
    if (closed_) {
      ++stats_.rejectedClosed;
      return SubmitStatus::kClosed;
    }
    if (!spec.key.empty()) {
      const auto it = activeKeys_.find(spec.key);
      if (it != activeKeys_.end()) {
        ++stats_.rejectedDuplicate;
        return SubmitStatus::kDuplicate;
      }
    }
#if !defined(PRISM_C11_MUTANT_UNBOUNDED_BACKLOG)
    if (queuedCount() >= config_.backlog) {
      ++stats_.rejectedBusy;
      return SubmitStatus::kBusy;
    }
#endif
    Task task;
    task.id = static_cast<uint64_t>(records_.size()) + 1;
    task.sequence = nextSequence_++;
    task.kind = spec.kind;
    task.priority = spec.priority;
    task.key = spec.key;
    task.label = spec.label;
    task.state = TaskState::kQueued;
    records_.push_back(std::move(task));
    const uint64_t id = records_.back().id;
    if (!records_.back().key.empty()) activeKeys_[records_.back().key] = id;
    if (records_.back().priority == Priority::kUser) {
      userQueue_.push_back(id);
    } else {
      backgroundQueue_.push_back(id);
    }
    if (idOut) *idOut = id;
    MaybeRequestPreemption(id);
    return SubmitStatus::kQueued;
  }

  // ---- dispatch -----------------------------------------------------------
  // Hands out the next runnable task and a FREE handler slot, or nullopt when
  // no slot is free / nothing is queued. The caller leases pool slot
  // Dispatch::slot, runs the work, then calls Complete().
  std::optional<Dispatch> NextDispatch() {
    if (freeSlots() == 0 || queuedCount() == 0) return std::nullopt;
    const uint64_t id = PopNextQueued();
    Task& task = records_[static_cast<size_t>(id - 1)];
    task.state = TaskState::kRunning;
    const size_t slot = PickSlot();
    task.slot = slot;
    runningSlots_.push_back(slot);
    ++stats_.dispatches;
    stats_.peakRunning = (std::max)(stats_.peakRunning, runningCount());
    Dispatch d;
    d.id = id;
    d.slot = slot;
    d.kind = task.kind;
    d.priority = task.priority;
    d.label = task.label;
    return d;
  }

  bool hasRunnable() const { return freeSlots() != 0 && queuedCount() != 0; }

  // ---- completion ---------------------------------------------------------
  // succeeded == false is a real outcome (E_ABORT from a callback, a corrupt
  // archive, ...). A failed task whose cancellation was requested is recorded
  // as cancelled, so stats can separate user cancels from engine failures.
  bool Complete(uint64_t id, bool succeeded = true) {
    Task* task = Find(id);
    if (!task || task->state != TaskState::kRunning) return false;
    if (task->slot) ReleaseSlot(*task->slot);
    const bool wasCancelled = task->cancelRequested;
    task->slot.reset();
    task->state =
        succeeded ? TaskState::kDone : (wasCancelled ? TaskState::kCancelled : TaskState::kFailed);
    if (succeeded) {
      ++stats_.completedOk;
    } else if (wasCancelled) {
      ++stats_.cancelledRunning;
    } else {
      ++stats_.failed;
    }
    ForgetKey(*task);
    return true;
  }

  // ---- cancellation -------------------------------------------------------
  CancelOutcome Cancel(uint64_t id) {
    Task* task = Find(id);
    if (!task) return CancelOutcome::kNotFound;
    switch (task->state) {
      case TaskState::kQueued:
        RemoveFromQueue(id);
        task->state = TaskState::kCancelled;
        ForgetKey(*task);
        ++stats_.cancelledQueued;
        return CancelOutcome::kCancelledQueued;
      case TaskState::kRunning:
        if (!task->cancelRequested) {
          task->cancelRequested = true;
          ++stats_.cancellationRequests;
        }
        return CancelOutcome::kCancellationRequested;
      case TaskState::kDone:
      case TaskState::kFailed:
      case TaskState::kCancelled:
        return CancelOutcome::kAlreadyFinished;
    }
    return CancelOutcome::kNotFound;
  }

  // Drops every QUEUED task (used when the archive view is torn down). Running
  // tasks are not touched: they need the cooperative path above.
  size_t CancelQueued() {
    size_t cancelled = 0;
    std::vector<uint64_t> ids;
    ids.reserve(queuedCount());
    for (const uint64_t id : userQueue_) ids.push_back(id);
    for (const uint64_t id : backgroundQueue_) ids.push_back(id);
    for (const uint64_t id : ids) {
      if (Cancel(id) == CancelOutcome::kCancelledQueued) ++cancelled;
    }
    return cancelled;
  }

  // No new work (application shutdown / archive closed). Queued work is
  // cancelled; running work still has to be Completed by its owner.
  void Close() {
    if (closed_) return;
    closed_ = true;
    CancelQueued();
  }

  // ---- preemption ---------------------------------------------------------
  std::vector<Preemption> TakePreemptions() {
    std::vector<Preemption> out;
    out.swap(preemptions_);
    return out;
  }

  // ---- introspection (tests + diagnostics) --------------------------------
  bool cancelRequested(uint64_t id) const {
    const Task* t = FindConst(id);
    return t != nullptr && t->cancelRequested;
  }

  std::optional<Task> TaskInfo(uint64_t id) const {
    const Task* t = FindConst(id);
    if (!t) return std::nullopt;
    return *t;
  }

  // The contract as a predicate: no two running tasks may hold the same slot,
  // and every slot must be inside [0, handlerSlots). A shared-handler mutant
  // makes this false on the second dispatch -- the pure-logic negative control
  // for the Windows ReaderPool mutation.
  bool slotInvariantHolds() const {
    std::vector<bool> seen(config_.handlerSlots, false);
    size_t running = 0;
    for (const Task& task : records_) {
      if (task.state != TaskState::kRunning) continue;
      ++running;
      if (!task.slot || *task.slot >= config_.handlerSlots) return false;
      if (seen[*task.slot]) return false;
      seen[*task.slot] = true;
    }
    return running == runningSlots_.size();
  }

  size_t freeSlots() const { return config_.handlerSlots - runningSlots_.size(); }
  size_t runningCount() const { return runningSlots_.size(); }
  size_t queuedCount() const { return userQueue_.size() + backgroundQueue_.size(); }
  size_t queuedCount(Priority priority) const {
    return priority == Priority::kUser ? userQueue_.size() : backgroundQueue_.size();
  }
  bool closed() const { return closed_; }
  const Config& config() const { return config_; }
  const Stats& stats() const { return stats_; }
  size_t taskCount() const { return records_.size(); }
  // Handler slots currently leased, in dispatch order (diagnostics).
  const std::vector<size_t>& runningSlots() const { return runningSlots_; }

 private:
  Task* Find(uint64_t id) {
    if (id == 0 || id > records_.size()) return nullptr;
    return &records_[static_cast<size_t>(id - 1)];
  }
  const Task* FindConst(uint64_t id) const {
    if (id == 0 || id > records_.size()) return nullptr;
    return &records_[static_cast<size_t>(id - 1)];
  }

  uint64_t PopNextQueued() {
#if defined(PRISM_C11_MUTANT_IGNORE_PRIORITY)
    // Mutation: FIFO across priorities -- exactly what a "just one queue" model
    // does, and what lets a background sweep delay the user's extraction.
    uint64_t id = 0;
    if (!backgroundQueue_.empty() && !userQueue_.empty()) {
      id = (std::min)(backgroundQueue_.front(), userQueue_.front());
      if (id == backgroundQueue_.front()) {
        backgroundQueue_.pop_front();
      } else {
        userQueue_.pop_front();
      }
    } else if (!userQueue_.empty()) {
      id = userQueue_.front();
      userQueue_.pop_front();
    } else {
      id = backgroundQueue_.front();
      backgroundQueue_.pop_front();
    }
    return id;
#else
    if (!userQueue_.empty()) {
      const uint64_t id = userQueue_.front();
      userQueue_.pop_front();
      return id;
    }
    const uint64_t id = backgroundQueue_.front();
    backgroundQueue_.pop_front();
    return id;
#endif
  }

  void RemoveFromQueue(uint64_t id) {
    RemoveFrom(userQueue_, id);
    RemoveFrom(backgroundQueue_, id);
  }
  static void RemoveFrom(std::deque<uint64_t>& queue, uint64_t id) {
    for (auto it = queue.begin(); it != queue.end(); ++it) {
      if (*it == id) {
        queue.erase(it);
        return;
      }
    }
  }

  size_t PickSlot() {
#if defined(PRISM_C11_MUTANT_SHARE_SLOT)
    // Mutation: the old "one shared reader" path -- every concurrent task is
    // handed the same handler. Dispatch() still succeeds, which is precisely
    // why the contract needs its own predicate (slotInvariantHolds).
    return 0;
#else
    for (size_t slot = 0; slot < config_.handlerSlots; ++slot) {
      if ((std::find)(runningSlots_.begin(), runningSlots_.end(), slot) == runningSlots_.end()) {
        return slot;
      }
    }
    return 0;  // unreachable while freeSlots() > 0
#endif
  }

  void ReleaseSlot(size_t slot) {
    for (auto it = runningSlots_.begin(); it != runningSlots_.end(); ++it) {
      if (*it == slot) {
        runningSlots_.erase(it);
        return;
      }
    }
  }

  void ForgetKey(const Task& task) {
    if (task.key.empty()) return;
    const auto it = activeKeys_.find(task.key);
    if (it != activeKeys_.end() && it->second == task.id) activeKeys_.erase(it);
  }

  void MaybeRequestPreemption(uint64_t forTaskId) {
    if (!config_.preemptBackgroundForUser) return;
    const Task* forTask = FindConst(forTaskId);
    if (!forTask || forTask->priority != Priority::kUser) return;
    if (freeSlots() != 0) return;  // a slot is available, no need to preempt
    Task* victim = nullptr;
    for (Task& task : records_) {
      if (task.state != TaskState::kRunning) continue;
      if (task.priority != Priority::kBackground) continue;
      if (task.cancelRequested || task.preemptionRequested) continue;
      if (!victim || task.sequence < victim->sequence) victim = &task;
    }
    if (!victim) return;
    victim->preemptionRequested = true;
    Preemption request;
    request.id = victim->id;
    request.forTask = forTaskId;
    request.reason = "user extraction preempts background " + std::string(KindName(victim->kind));
    preemptions_.push_back(std::move(request));
    ++stats_.preemptions;
  }

  Config config_;
  bool closed_ = false;
  uint64_t nextSequence_ = 1;
  std::deque<Task> records_;  // id - 1 is the index; addresses stay stable
  std::deque<uint64_t> userQueue_;
  std::deque<uint64_t> backgroundQueue_;
  std::unordered_map<std::string, uint64_t> activeKeys_;
  std::vector<size_t> runningSlots_;
  std::vector<Preemption> preemptions_;
  Stats stats_;
};

}  // namespace prism::core::sched
