// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/sequence_manager/thread_controller_impl.h"

#include <algorithm>

#include "base/bind.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_pump.h"
#include "base/run_loop.h"
#include "base/task/sequence_manager/lazy_now.h"
#include "base/task/sequence_manager/sequence_manager_impl.h"
#include "base/task/sequence_manager/sequenced_task_source.h"
#include "base/trace_event/base_tracing.h"

namespace base {
namespace sequence_manager {
namespace internal {

using ShouldScheduleWork = WorkDeduplicator::ShouldScheduleWork;

ThreadControllerImpl::ThreadControllerImpl(
    SequenceManagerImpl* funneled_sequence_manager,
    scoped_refptr<SingleThreadTaskRunner> task_runner,
    const TickClock* time_source)
    : funneled_sequence_manager_(funneled_sequence_manager),
      task_runner_(task_runner),
      associated_thread_(AssociatedThreadId::CreateUnbound()),
      message_loop_task_runner_(funneled_sequence_manager
                                    ? funneled_sequence_manager->GetTaskRunner()
                                    : nullptr),
      time_source_(time_source),
      work_deduplicator_(associated_thread_) {
  if (task_runner_ || funneled_sequence_manager_)
    work_deduplicator_.BindToCurrentThread();
  immediate_do_work_closure_ =
      BindRepeating(&ThreadControllerImpl::DoWork, weak_factory_.GetWeakPtr(),
                    WorkType::kImmediate);
  delayed_do_work_closure_ =
      BindRepeating(&ThreadControllerImpl::DoWork, weak_factory_.GetWeakPtr(),
                    WorkType::kDelayed);

  // Unlike ThreadControllerWithMessagePumpImpl, ThreadControllerImpl isn't
  // explicitly Run(). Rather, DoWork() will be invoked at some point in the
  // future when the associated thread begins pumping messages.
  main_sequence_only().run_level_tracker.OnRunLoopStarted(
      RunLevelTracker::kIdle);
}

ThreadControllerImpl::~ThreadControllerImpl() {
  // Balances OnRunLoopStarted() in the constructor to satisfy the exit criteria
  // of ~RunLevelTracker().
  main_sequence_only().run_level_tracker.OnRunLoopEnded();
}

ThreadControllerImpl::MainSequenceOnly::MainSequenceOnly() = default;

ThreadControllerImpl::MainSequenceOnly::~MainSequenceOnly() = default;

std::unique_ptr<ThreadControllerImpl> ThreadControllerImpl::Create(
    SequenceManagerImpl* funneled_sequence_manager,
    const TickClock* time_source) {
  return WrapUnique(new ThreadControllerImpl(
      funneled_sequence_manager,
      funneled_sequence_manager ? funneled_sequence_manager->GetTaskRunner()
                                : nullptr,
      time_source));
}

void ThreadControllerImpl::SetSequencedTaskSource(
    SequencedTaskSource* sequence) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(associated_thread_->sequence_checker);
  DCHECK(sequence);
  DCHECK(!sequence_);
  sequence_ = sequence;
}

void ThreadControllerImpl::SetTimerSlack(TimerSlack timer_slack) {
  if (!funneled_sequence_manager_)
    return;
  funneled_sequence_manager_->SetTimerSlack(timer_slack);
}

void ThreadControllerImpl::ScheduleWork() {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("sequence_manager"),
               "ThreadControllerImpl::ScheduleWork::PostTask");

  if (work_deduplicator_.OnWorkRequested() ==
      ShouldScheduleWork::kScheduleImmediate) {
    task_runner_->PostTask(FROM_HERE, immediate_do_work_closure_);
  }
}

void ThreadControllerImpl::SetNextDelayedDoWork(LazyNow* lazy_now,
                                                TimeTicks run_time) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(associated_thread_->sequence_checker);
  DCHECK(sequence_);

  if (main_sequence_only().next_delayed_do_work == run_time)
    return;

  // Cancel DoWork if it was scheduled and we set an "infinite" delay now.
  if (run_time == TimeTicks::Max()) {
    cancelable_delayed_do_work_closure_.Cancel();
    main_sequence_only().next_delayed_do_work = TimeTicks::Max();
    return;
  }

  if (work_deduplicator_.OnDelayedWorkRequested() ==
      ShouldScheduleWork::kNotNeeded) {
    return;
  }

  base::TimeDelta delay = std::max(TimeDelta(), run_time - lazy_now->Now());
  TRACE_EVENT1(TRACE_DISABLED_BY_DEFAULT("sequence_manager"),
               "ThreadControllerImpl::SetNextDelayedDoWork::PostDelayedTask",
               "delay_ms", delay.InMillisecondsF());

  main_sequence_only().next_delayed_do_work = run_time;
  // Reset also causes cancellation of the previous DoWork task.
  cancelable_delayed_do_work_closure_.Reset(delayed_do_work_closure_);
  task_runner_->PostDelayedTask(
      FROM_HERE, cancelable_delayed_do_work_closure_.callback(), delay);
}

bool ThreadControllerImpl::RunsTasksInCurrentSequence() {
  return task_runner_->RunsTasksInCurrentSequence();
}

void ThreadControllerImpl::SetTickClock(const TickClock* clock) {
  time_source_ = clock;
}

void ThreadControllerImpl::SetDefaultTaskRunner(
    scoped_refptr<SingleThreadTaskRunner> task_runner) {
#if DCHECK_IS_ON()
  default_task_runner_set_ = true;
#endif
  if (!funneled_sequence_manager_)
    return;
  funneled_sequence_manager_->SetTaskRunner(task_runner);
}

scoped_refptr<SingleThreadTaskRunner>
ThreadControllerImpl::GetDefaultTaskRunner() {
  return funneled_sequence_manager_->GetTaskRunner();
}

void ThreadControllerImpl::RestoreDefaultTaskRunner() {
  if (!funneled_sequence_manager_)
    return;
  funneled_sequence_manager_->SetTaskRunner(message_loop_task_runner_);
}

void ThreadControllerImpl::BindToCurrentThread(
    std::unique_ptr<MessagePump> message_pump) {
  NOTREACHED();
}

void ThreadControllerImpl::WillQueueTask(PendingTask* pending_task,
                                         const char* task_queue_name) {
  task_annotator_.WillQueueTask("SequenceManager PostTask", pending_task,
                                task_queue_name);
}

void ThreadControllerImpl::DoWork(WorkType work_type) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("sequence_manager"),
               "ThreadControllerImpl::DoWork");

  DCHECK_CALLED_ON_VALID_SEQUENCE(associated_thread_->sequence_checker);
  DCHECK(sequence_);

  work_deduplicator_.OnWorkStarted();

  WeakPtr<ThreadControllerImpl> weak_ptr = weak_factory_.GetWeakPtr();
  // TODO(scheduler-dev): Consider moving to a time based work batch instead.
  for (int i = 0; i < main_sequence_only().work_batch_size_; i++) {
    absl::optional<SequencedTaskSource::SelectedTask> selected_task =
        sequence_->SelectNextTask();
    if (!selected_task)
      break;

    // [OnTaskStarted(), OnTaskEnded()] must outscope all other tracing calls
    // so that the "ThreadController active" trace event lives on top of all
    // "run task" events.
    DCHECK_GT(main_sequence_only().run_level_tracker.num_run_levels(), 0U);
    main_sequence_only().run_level_tracker.OnTaskStarted();
    {
      // Trace-parsing tools (DevTools, Lighthouse, etc) consume this event
      // to determine long tasks.
      // See https://crbug.com/681863 and https://crbug.com/874982
      TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "RunTask");

      // Note: all arguments after task are just passed to a TRACE_EVENT for
      // logging so lambda captures are safe as lambda is executed inline.
      task_annotator_.RunTask(
          "ThreadControllerImpl::RunTask", selected_task->task,
          [&selected_task](perfetto::EventContext& ctx) {
            if (selected_task->task_execution_trace_logger)
              selected_task->task_execution_trace_logger.Run(
                  ctx, selected_task->task);
          });
      if (!weak_ptr)
        return;

      // This processes microtasks, hence all scoped operations above must end
      // after it.
      sequence_->DidRunTask();
    }
    main_sequence_only().run_level_tracker.OnTaskEnded();

    // NOTE: https://crbug.com/828835.
    // When we're running inside a nested RunLoop it may quit anytime, so any
    // outstanding pending tasks must run in the outer RunLoop
    // (see SequenceManagerTestWithMessageLoop.QuitWhileNested test).
    // Unfortunately, it's MessageLoop who's receiving that signal and we can't
    // know it before we return from DoWork, hence, OnExitNestedRunLoop
    // will be called later. Since we must implement ThreadController and
    // SequenceManager in conformance with MessageLoop task runners, we need
    // to disable this batching optimization while nested.
    // Implementing MessagePump::Delegate ourselves will help to resolve this
    // issue.
    if (main_sequence_only().run_level_tracker.num_run_levels() > 1)
      break;
  }

  work_deduplicator_.WillCheckForMoreWork();

  LazyNow lazy_now(time_source_);
  sequence_->RemoveAllCanceledDelayedTasksFromFront(&lazy_now);
  TimeTicks next_task_time = sequence_->GetNextTaskTime(&lazy_now);
  // The OnSystemIdle callback allows the TimeDomains to advance virtual time
  // in which case we now have immediate work to do.
  if (next_task_time.is_null() || sequence_->OnSystemIdle()) {
    // The next task needs to run immediately, post a continuation if
    // another thread didn't get there first.
    if (work_deduplicator_.DidCheckForMoreWork(
            WorkDeduplicator::NextTask::kIsImmediate) ==
        ShouldScheduleWork::kScheduleImmediate) {
      task_runner_->PostTask(FROM_HERE, immediate_do_work_closure_);
    }
    return;
  }

  // It looks like we have a non-zero delay, however another thread may have
  // posted an immediate task while we computed the delay.
  if (work_deduplicator_.DidCheckForMoreWork(
          WorkDeduplicator::NextTask::kIsDelayed) ==
      ShouldScheduleWork::kScheduleImmediate) {
    task_runner_->PostTask(FROM_HERE, immediate_do_work_closure_);
    return;
  }

  // No more immediate work.
  main_sequence_only().run_level_tracker.OnIdle();

  // Any future work?
  if (next_task_time.is_max()) {
    main_sequence_only().next_delayed_do_work = TimeTicks::Max();
    cancelable_delayed_do_work_closure_.Cancel();
    return;
  }

  // Already requested next delay?
  if (next_task_time == main_sequence_only().next_delayed_do_work)
    return;

  // Schedule a callback after |delay_till_next_task| and cancel any previous
  // callback.
  main_sequence_only().next_delayed_do_work = next_task_time;
  cancelable_delayed_do_work_closure_.Reset(delayed_do_work_closure_);
  task_runner_->PostDelayedTask(FROM_HERE,
                                cancelable_delayed_do_work_closure_.callback(),
                                next_task_time - lazy_now.Now());
}

void ThreadControllerImpl::AddNestingObserver(
    RunLoop::NestingObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(associated_thread_->sequence_checker);
  nesting_observer_ = observer;
  RunLoop::AddNestingObserverOnCurrentThread(this);
}

void ThreadControllerImpl::RemoveNestingObserver(
    RunLoop::NestingObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(associated_thread_->sequence_checker);
  DCHECK_EQ(observer, nesting_observer_);
  nesting_observer_ = nullptr;
  RunLoop::RemoveNestingObserverOnCurrentThread(this);
}

const scoped_refptr<AssociatedThreadId>&
ThreadControllerImpl::GetAssociatedThread() const {
  return associated_thread_;
}

void ThreadControllerImpl::OnBeginNestedRunLoop() {
  main_sequence_only().run_level_tracker.OnRunLoopStarted(
      RunLevelTracker::kSelectingNextTask);

  // Just assume we have a pending task and post a DoWork to make sure we don't
  // grind to a halt while nested.
  work_deduplicator_.OnWorkRequested();  // Set the pending DoWork flag.
  task_runner_->PostTask(FROM_HERE, immediate_do_work_closure_);

  if (nesting_observer_)
    nesting_observer_->OnBeginNestedRunLoop();
}

void ThreadControllerImpl::OnExitNestedRunLoop() {
  if (nesting_observer_)
    nesting_observer_->OnExitNestedRunLoop();
  main_sequence_only().run_level_tracker.OnRunLoopEnded();
}

void ThreadControllerImpl::SetWorkBatchSize(int work_batch_size) {
  main_sequence_only().work_batch_size_ = work_batch_size;
}

void ThreadControllerImpl::SetTaskExecutionAllowed(bool allowed) {
  NOTREACHED();
}

bool ThreadControllerImpl::IsTaskExecutionAllowed() const {
  return true;
}

bool ThreadControllerImpl::ShouldQuitRunLoopWhenIdle() {
  // The MessageLoop does not expose the API needed to support this query.
  return false;
}

MessagePump* ThreadControllerImpl::GetBoundMessagePump() const {
  return nullptr;
}

#if defined(OS_IOS) || defined(OS_ANDROID)
void ThreadControllerImpl::AttachToMessagePump() {
  NOTREACHED();
}
#endif  // OS_IOS || OS_ANDROID

#if defined(OS_IOS)
void ThreadControllerImpl::DetachFromMessagePump() {
  NOTREACHED();
}
#endif  // OS_IOS

void ThreadControllerImpl::PrioritizeYieldingToNative(base::TimeTicks) {
  NOTREACHED();
}

}  // namespace internal
}  // namespace sequence_manager
}  // namespace base
