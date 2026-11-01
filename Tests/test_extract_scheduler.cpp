// PrismArchive -- PrismCore test
// ExtractScheduler: the pure decision core that keeps "one handler per
// concurrent operation" true, gives user extraction priority over background
// sweeping, and makes backpressure and cancellation explicit answers.
//
// Negative controls (CONTRACTS D3, compiled by Tests/suites/c11-scheduler.cmake
// with WILL_FAIL): the same file builds against
//   PRISM_C11_MUTANT_IGNORE_PRIORITY   -> priority test goes red
//   PRISM_C11_MUTANT_UNBOUNDED_BACKLOG -> backpressure test goes red
//   PRISM_C11_MUTANT_SHARE_SLOT        -> slot-invariance test goes red
// A mutation macro that stopped matching would make the mutant PASS and the
// WILL_FAIL registration go red, so the injection is verified, not assumed.
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "mini_test.hpp"

#include "ExtractScheduler.hpp"

namespace sched = prism::core::sched;

namespace {

sched::TaskSpec Spec(sched::Kind kind, sched::Priority priority, const std::string& label,
                     const std::string& key = std::string()) {
  sched::TaskSpec spec;
  spec.kind = kind;
  spec.priority = priority;
  spec.label = label;
  spec.key = key;
  return spec;
}

sched::TaskSpec Background(const std::string& label, const std::string& key = std::string()) {
  return Spec(sched::Kind::kSweep, sched::Priority::kBackground, label, key);
}

sched::TaskSpec User(const std::string& label, const std::string& key = std::string()) {
  return Spec(sched::Kind::kExtract, sched::Priority::kUser, label, key);
}

}  // namespace

// A config that promises nothing cannot be scheduled: 0 handler slots means
// "no concurrent operation", 0 backlog means "no queue". Both are programmer
// errors, so they throw instead of being silently clamped (a clamped capacity
// would be a promise the object no longer keeps -- CONTRACTS D17).
PRISM_TEST("extract_scheduler.rejects_impossible_config") {
  bool slotsThrew = false;
  try {
    sched::Config config;
    config.handlerSlots = 0;
    sched::ExtractScheduler scheduler(config);
  } catch (const std::invalid_argument&) {
    slotsThrew = true;
  }
  PRISM_CHECK(slotsThrew);

  bool backlogThrew = false;
  try {
    sched::Config config;
    config.backlog = 0;
    sched::ExtractScheduler scheduler(config);
  } catch (const std::invalid_argument&) {
    backlogThrew = true;
  }
  PRISM_CHECK(backlogThrew);
}

// Submit -> Dispatch -> Complete, with handler slots handed out exactly once
// each and returned on completion.
PRISM_TEST("extract_scheduler.dispatch_leases_one_slot_per_task") {
  sched::Config config;
  config.handlerSlots = 2;
  config.backlog = 4;
  sched::ExtractScheduler scheduler(config);

  uint64_t sweepId = 0;
  uint64_t extractId = 0;
  PRISM_CHECK(scheduler.Submit(Background("sweep", "sweep:a.7z"), &sweepId) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(User("extract", "extract:a.7z:3"), &extractId) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK_EQ(scheduler.queuedCount(), size_t(2));
  PRISM_CHECK_EQ(scheduler.runningCount(), size_t(0));

  const auto first = scheduler.NextDispatch();
  PRISM_CHECK(first.has_value());
  // Priority is the whole point: the user task runs first even though the
  // sweep was submitted first.
  if (!PRISM_CHECK(first.has_value() && first->id == extractId)) return;
  PRISM_CHECK_EQ(first->slot, size_t(0));
  PRISM_CHECK_EQ(scheduler.freeSlots(), size_t(1));

  const auto second = scheduler.NextDispatch();
  if (!PRISM_CHECK(second.has_value() && second->id == sweepId)) return;
  PRISM_CHECK_EQ(second->slot, size_t(1));  // distinct handler, not a shared one
  PRISM_CHECK(scheduler.slotInvariantHolds());
  PRISM_CHECK_EQ(scheduler.runningCount(), size_t(2));
  PRISM_CHECK_EQ(scheduler.freeSlots(), size_t(0));

  // No third slot: the pool has exactly two handlers, so a third concurrent
  // dispatch must be refused rather than silently sharing one.
  PRISM_CHECK(!scheduler.NextDispatch().has_value());

  PRISM_CHECK(scheduler.Complete(extractId, true));
  PRISM_CHECK_EQ(scheduler.freeSlots(), size_t(1));
  const auto info = scheduler.TaskInfo(extractId);
  if (!PRISM_CHECK(info.has_value())) return;
  PRISM_CHECK(info->state == sched::TaskState::kDone);
  PRISM_CHECK(!info->slot.has_value());  // the slot was returned
  PRISM_CHECK(!scheduler.Complete(extractId, true));  // double complete is refused

  PRISM_CHECK(scheduler.Complete(sweepId, true));
  PRISM_CHECK_EQ(scheduler.stats().completedOk, uint64_t(2));
  PRISM_CHECK_EQ(scheduler.stats().dispatches, uint64_t(2));
  PRISM_CHECK_EQ(scheduler.stats().peakRunning, size_t(2));
  PRISM_CHECK(scheduler.slotInvariantHolds());
}

// Strict priority: a queued user extraction jumps the queue ahead of an older
// queued background sweep. MUTANT IGNORE_PRIORITY dispatches FIFO here, so the
// assertion below goes red -- that is the priority guard, not a restatement.
PRISM_TEST("extract_scheduler.user_extraction_beats_queued_background") {
  sched::Config config;
  config.handlerSlots = 2;
  config.backlog = 8;
  sched::ExtractScheduler scheduler(config);

  uint64_t bg1 = 0, bg2 = 0, bg3 = 0, user = 0;
  PRISM_CHECK(scheduler.Submit(Background("sweep-1", "sweep:1"), &bg1) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(Background("sweep-2", "sweep:2"), &bg2) ==
              sched::SubmitStatus::kQueued);
  const auto d1 = scheduler.NextDispatch();
  const auto d2 = scheduler.NextDispatch();
  if (!PRISM_CHECK(d1.has_value() && d2.has_value())) return;
  PRISM_CHECK_EQ(scheduler.freeSlots(), size_t(0));

  // Both slots are busy; these two wait. The background one is OLDER.
  PRISM_CHECK(scheduler.Submit(Background("sweep-3", "sweep:3"), &bg3) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(User("double-clicked-extract", "extract:x:7"), &user) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK_EQ(scheduler.queuedCount(sched::Priority::kUser), size_t(1));
  PRISM_CHECK_EQ(scheduler.queuedCount(sched::Priority::kBackground), size_t(1));

  // A slot frees up. The user task must win, even though it arrived later.
  PRISM_CHECK(scheduler.Complete(bg1, true));
  const auto next = scheduler.NextDispatch();
  if (!PRISM_CHECK(next.has_value())) return;
  PRISM_CHECK_EQ(next->id, user);
  PRISM_CHECK(next->priority == sched::Priority::kUser);

  // And the background task still runs afterwards, so priority is ordering,
  // not starvation of a single queued item.
  PRISM_CHECK(scheduler.Complete(d2->id, true));
  const auto after = scheduler.NextDispatch();
  if (!PRISM_CHECK(after.has_value())) return;
  PRISM_CHECK_EQ(after->id, bg3);
  (void)bg2;
}

// Backpressure: the backlog holds EXACTLY Config::backlog tasks. The next
// submission is answered kBusy and the queue does not grow. MUTANT
// UNBOUNDED_BACKLOG accepts it, so this assertion goes red.
PRISM_TEST("extract_scheduler.backlog_is_a_hard_bound") {
  sched::Config config;
  config.handlerSlots = 1;
  config.backlog = 2;
  sched::ExtractScheduler scheduler(config);

  uint64_t running = 0;
  PRISM_CHECK(scheduler.Submit(Background("running", "sweep:r"), &running) ==
              sched::SubmitStatus::kQueued);
  const auto dispatched = scheduler.NextDispatch();
  if (!PRISM_CHECK(dispatched.has_value())) return;
  PRISM_CHECK_EQ(scheduler.queuedCount(), size_t(0));

  PRISM_CHECK(scheduler.Submit(Background("waiting-1", "sweep:w1")) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(Background("waiting-2", "sweep:w2")) ==
              sched::SubmitStatus::kQueued);
  // Exactly at the promise: 2 waiting, 1 running, 1 slot.
  PRISM_CHECK_EQ(scheduler.queuedCount(), config.backlog);
  PRISM_CHECK_EQ(scheduler.freeSlots(), size_t(0));

  PRISM_CHECK(scheduler.Submit(Background("rejected", "sweep:r2")) ==
              sched::SubmitStatus::kBusy);
  PRISM_CHECK_EQ(scheduler.queuedCount(), config.backlog);  // did NOT grow
  PRISM_CHECK_EQ(scheduler.stats().rejectedBusy, uint64_t(1));
  PRISM_CHECK_EQ(scheduler.taskCount(), size_t(3));  // rejected task not recorded

  // Once a waiting task is cancelled, capacity returns.
  uint64_t waiting1 = 0;
  PRISM_CHECK(scheduler.Submit(Background("waiting-1-again", "sweep:w1b"), &waiting1) ==
              sched::SubmitStatus::kBusy);  // still full
  PRISM_CHECK(scheduler.Complete(running, true));
  const auto next = scheduler.NextDispatch();
  if (!PRISM_CHECK(next.has_value())) return;
  PRISM_CHECK(scheduler.Submit(Background("now-fits", "sweep:w3")) ==
              sched::SubmitStatus::kQueued);
}

// Duplicate keys: double-clicking the same extraction twice must not start two
// engine operations on one archive.
PRISM_TEST("extract_scheduler.deduplicates_live_keys") {
  sched::Config config;
  config.handlerSlots = 1;
  config.backlog = 4;
  sched::ExtractScheduler scheduler(config);

  uint64_t first = 0;
  PRISM_CHECK(scheduler.Submit(User("extract-a", "extract:a.7z:1"), &first) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(User("extract-a-again", "extract:a.7z:1")) ==
              sched::SubmitStatus::kDuplicate);
  PRISM_CHECK_EQ(scheduler.stats().rejectedDuplicate, uint64_t(1));
  PRISM_CHECK_EQ(scheduler.queuedCount(), size_t(1));

  // Same content, different key: legitimately a second task.
  PRISM_CHECK(scheduler.Submit(User("extract-b", "extract:a.7z:2")) ==
              sched::SubmitStatus::kQueued);
  // Empty key never dedupes (callers that do not care about identity).
  PRISM_CHECK(scheduler.Submit(User("anon-1")) == sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(User("anon-2")) == sched::SubmitStatus::kQueued);

  const auto d = scheduler.NextDispatch();
  if (!PRISM_CHECK(d.has_value())) return;
  PRISM_CHECK(scheduler.Complete(d->id, true));
  // The key is free again once the task finishes.
  PRISM_CHECK(scheduler.Submit(User("extract-a-third", "extract:a.7z:1")) ==
              sched::SubmitStatus::kQueued);
  (void)first;
}

// Cancellation has two shapes and they must not be confused: a queued task is
// dropped synchronously, a running one can only be ASKED (7z stops from its
// own callbacks), and its failure is then recorded as cancelled, not failed.
PRISM_TEST("extract_scheduler.cancellation_queued_vs_running") {
  sched::Config config;
  config.handlerSlots = 1;
  config.backlog = 4;
  sched::ExtractScheduler scheduler(config);

  uint64_t running = 0;
  uint64_t queued = 0;
  PRISM_CHECK(scheduler.Submit(Background("sweep", "sweep:s"), &running) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(Background("waiting", "sweep:w"), &queued) ==
              sched::SubmitStatus::kQueued);
  const auto d = scheduler.NextDispatch();
  if (!PRISM_CHECK(d.has_value() && d->id == running)) return;

  PRISM_CHECK(scheduler.Cancel(queued) == sched::CancelOutcome::kCancelledQueued);
  PRISM_CHECK_EQ(scheduler.queuedCount(), size_t(0));
  const auto queuedInfo = scheduler.TaskInfo(queued);
  if (!PRISM_CHECK(queuedInfo.has_value())) return;
  PRISM_CHECK(queuedInfo->state == sched::TaskState::kCancelled);
  PRISM_CHECK(!queuedInfo->slot.has_value());  // never held a handler

  PRISM_CHECK(scheduler.Cancel(running) == sched::CancelOutcome::kCancellationRequested);
  PRISM_CHECK(scheduler.cancelRequested(running));
  // Asking twice is idempotent (one request, one counter).
  PRISM_CHECK(scheduler.Cancel(running) == sched::CancelOutcome::kCancellationRequested);
  PRISM_CHECK_EQ(scheduler.stats().cancellationRequests, uint64_t(1));
  PRISM_CHECK_EQ(scheduler.runningCount(), size_t(1));  // still running

  // A cooperative abort reports failure; because cancellation was requested,
  // it is counted as cancelled rather than as an engine failure.
  PRISM_CHECK(scheduler.Complete(running, false));
  PRISM_CHECK_EQ(scheduler.stats().cancelledRunning, uint64_t(1));
  PRISM_CHECK_EQ(scheduler.stats().failed, uint64_t(0));
  const auto done = scheduler.TaskInfo(running);
  if (!PRISM_CHECK(done.has_value())) return;
  PRISM_CHECK(done->state == sched::TaskState::kCancelled);

  PRISM_CHECK(scheduler.Cancel(running) == sched::CancelOutcome::kAlreadyFinished);
  PRISM_CHECK(scheduler.Cancel(9999) == sched::CancelOutcome::kNotFound);
}

// Preemption: a user extraction that finds every slot held by background work
// asks the caller to abort the oldest background task. Without this, a large
// solid sweep can hold both handlers for seconds while the user waits.
PRISM_TEST("extract_scheduler.preempts_background_for_user") {
  sched::Config config;
  config.handlerSlots = 1;
  config.backlog = 4;
  sched::ExtractScheduler scheduler(config);

  uint64_t sweep = 0;
  uint64_t extract = 0;
  PRISM_CHECK(scheduler.Submit(Background("sweep", "sweep:a"), &sweep) ==
              sched::SubmitStatus::kQueued);
  const auto d = scheduler.NextDispatch();
  if (!PRISM_CHECK(d.has_value())) return;
  PRISM_CHECK(scheduler.TakePreemptions().empty());  // nothing to preempt yet

  PRISM_CHECK(scheduler.Submit(User("extract", "extract:a:4"), &extract) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK_EQ(scheduler.stats().preemptions, uint64_t(1));
  auto requests = scheduler.TakePreemptions();
  if (!PRISM_CHECK_EQ(requests.size(), size_t(1))) return;
  PRISM_CHECK_EQ(requests[0].id, sweep);
  PRISM_CHECK_EQ(requests[0].forTask, extract);
  PRISM_CHECK(!requests[0].reason.empty());
  // One request per victim: a second user submission must not duplicate it.
  PRISM_CHECK(scheduler.Submit(User("extract-2", "extract:a:5")) ==
              sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.TakePreemptions().empty());

  // The caller honours the request: abort, complete as cancelled, then the
  // user's task gets the freed slot.
  PRISM_CHECK(scheduler.Cancel(sweep) == sched::CancelOutcome::kCancellationRequested);
  PRISM_CHECK(scheduler.Complete(sweep, false));
  const auto next = scheduler.NextDispatch();
  if (!PRISM_CHECK(next.has_value())) return;
  PRISM_CHECK_EQ(next->id, extract);

  // With a free slot there is nothing to preempt (no false requests).
  sched::Config quiet;
  quiet.handlerSlots = 2;
  quiet.backlog = 4;
  sched::ExtractScheduler scheduler2(quiet);
  uint64_t quietSweep = 0;
  PRISM_CHECK(scheduler2.Submit(Background("sweep"), &quietSweep) == sched::SubmitStatus::kQueued);
  (void)scheduler2.NextDispatch();
  PRISM_CHECK(scheduler2.Submit(User("extract"), nullptr) == sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler2.TakePreemptions().empty());

  // ...and the knob can turn the policy off entirely.
  sched::Config noPreempt;
  noPreempt.handlerSlots = 1;
  noPreempt.backlog = 4;
  noPreempt.preemptBackgroundForUser = false;
  sched::ExtractScheduler scheduler3(noPreempt);
  PRISM_CHECK(scheduler3.Submit(Background("sweep"), nullptr) == sched::SubmitStatus::kQueued);
  (void)scheduler3.NextDispatch();
  PRISM_CHECK(scheduler3.Submit(User("extract"), nullptr) == sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler3.TakePreemptions().empty());
  PRISM_CHECK_EQ(scheduler3.stats().preemptions, uint64_t(0));
}

// The contract itself: two running tasks may never share a handler slot.
// MUTANT SHARE_SLOT hands slot 0 to everyone, so this goes red.
PRISM_TEST("extract_scheduler.never_shares_a_handler_slot") {
  sched::Config config;
  config.handlerSlots = 3;
  config.backlog = 4;
  sched::ExtractScheduler scheduler(config);

  uint64_t ids[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    PRISM_CHECK(scheduler.Submit(Background("sweep-" + std::to_string(i)), &ids[i]) ==
                sched::SubmitStatus::kQueued);
  }
  bool seen[3] = {false, false, false};
  for (int i = 0; i < 3; ++i) {
    const auto d = scheduler.NextDispatch();
    if (!PRISM_CHECK(d.has_value())) return;
    PRISM_CHECK(d->slot < 3);
    if (d->slot >= 3) return;
    PRISM_CHECK(!seen[d->slot]);  // no sharing
    seen[d->slot] = true;
  }
  PRISM_CHECK(scheduler.slotInvariantHolds());
  PRISM_CHECK_EQ(scheduler.runningSlots().size(), size_t(3));
  PRISM_CHECK(!scheduler.NextDispatch().has_value());

  // Releasing one slot makes exactly that slot available again.
  PRISM_CHECK(scheduler.Complete(ids[1], true));
  const auto recycled = scheduler.NextDispatch();
  PRISM_CHECK(!recycled.has_value());  // nothing queued
  PRISM_CHECK_EQ(scheduler.freeSlots(), size_t(1));
  PRISM_CHECK(scheduler.slotInvariantHolds());
}

// Close(): the application is shutting down. New work is refused loudly, queued
// work is cancelled, and work already running is still allowed to report in
// (its handler must not be yanked out from under the engine).
PRISM_TEST("extract_scheduler.close_cancels_waiters_but_not_runners") {
  sched::Config config;
  config.handlerSlots = 1;
  config.backlog = 8;
  sched::ExtractScheduler scheduler(config);

  uint64_t running = 0;
  PRISM_CHECK(scheduler.Submit(Background("running"), &running) == sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(Background("waiting-1")) == sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.Submit(Background("waiting-2")) == sched::SubmitStatus::kQueued);
  const auto d = scheduler.NextDispatch();
  if (!PRISM_CHECK(d.has_value())) return;

  scheduler.Close();
  PRISM_CHECK(scheduler.closed());
  PRISM_CHECK_EQ(scheduler.queuedCount(), size_t(0));
  PRISM_CHECK_EQ(scheduler.stats().cancelledQueued, uint64_t(2));
  PRISM_CHECK(scheduler.Submit(Background("late")) == sched::SubmitStatus::kClosed);
  PRISM_CHECK_EQ(scheduler.stats().rejectedClosed, uint64_t(1));

  PRISM_CHECK(scheduler.Complete(running, true));
  PRISM_CHECK_EQ(scheduler.stats().completedOk, uint64_t(1));
  scheduler.Close();  // idempotent
  PRISM_CHECK_EQ(scheduler.stats().cancelledQueued, uint64_t(2));
}

// CancelQueued() reports how many waiters were dropped and leaves the running
// task (and its slot) alone -- the shape used when the archive view closes.
PRISM_TEST("extract_scheduler.cancel_queued_is_measured") {
  sched::Config config;
  config.handlerSlots = 1;
  config.backlog = 8;
  sched::ExtractScheduler scheduler(config);

  uint64_t running = 0;
  PRISM_CHECK(scheduler.Submit(User("running"), &running) == sched::SubmitStatus::kQueued);
  PRISM_CHECK(scheduler.NextDispatch().has_value());
  for (int i = 0; i < 3; ++i) {
    PRISM_CHECK(scheduler.Submit(Background("waiting-" + std::to_string(i))) ==
                sched::SubmitStatus::kQueued);
  }
  PRISM_CHECK_EQ(scheduler.CancelQueued(), size_t(3));
  PRISM_CHECK_EQ(scheduler.queuedCount(), size_t(0));
  PRISM_CHECK_EQ(scheduler.runningCount(), size_t(1));
  PRISM_CHECK(scheduler.Complete(running, true));
  PRISM_CHECK_EQ(scheduler.freeSlots(), size_t(1));
}

int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL EXTRACT-SCHEDULER CHECKS PASSED" << std::endl;
  return 0;
}
