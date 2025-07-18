// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <boost/asio/thread_pool.hpp>
#include <boost/thread.hpp>
#include <deque>
#include <list>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#include "ray/common/asio/asio_util.h"
#include "ray/common/id.h"
#include "ray/common/ray_object.h"
#include "ray/core_worker/actor_creator.h"
#include "ray/core_worker/context.h"
#include "ray/core_worker/store_provider/memory_store/memory_store.h"
#include "ray/core_worker/transport/actor_submit_queue.h"
#include "ray/core_worker/transport/dependency_resolver.h"
#include "ray/core_worker/transport/out_of_order_actor_submit_queue.h"
#include "ray/core_worker/transport/sequential_actor_submit_queue.h"
#include "ray/gcs/gcs_client/gcs_client.h"
#include "ray/rpc/worker/core_worker_client.h"

namespace ray {
namespace core {

// Interface for testing.
class ActorTaskSubmitterInterface {
 public:
  virtual void AddActorQueueIfNotExists(const ActorID &actor_id,
                                        int32_t max_pending_calls,
                                        bool execute_out_of_order,
                                        bool fail_if_actor_unreachable,
                                        bool owned) = 0;
  virtual void ConnectActor(const ActorID &actor_id,
                            const rpc::Address &address,
                            int64_t num_restarts) = 0;
  virtual void DisconnectActor(const ActorID &actor_id,
                               int64_t num_restarts,
                               bool dead,
                               const rpc::ActorDeathCause &death_cause,
                               bool is_restartable) = 0;

  virtual void CheckTimeoutTasks() = 0;

  /// Mark that the corresponding actor is preempted (e.g., spot preemption).
  /// If called, preempted = true will be set in the death cause upon actor death.
  virtual void SetPreempted(const ActorID &actor_id) = 0;

  virtual ~ActorTaskSubmitterInterface() {}
};

// This class is thread-safe.
class ActorTaskSubmitter : public ActorTaskSubmitterInterface {
 public:
  ActorTaskSubmitter(rpc::CoreWorkerClientPool &core_worker_client_pool,
                     CoreWorkerMemoryStore &store,
                     TaskManagerInterface &task_manager,
                     ActorCreatorInterface &actor_creator,
                     const TensorTransportGetter &tensor_transport_getter,
                     std::function<void(const ActorID &, int64_t)> warn_excess_queueing,
                     instrumented_io_context &io_service,
                     std::shared_ptr<ReferenceCounterInterface> reference_counter)
      : core_worker_client_pool_(core_worker_client_pool),
        actor_creator_(actor_creator),
        resolver_(store, task_manager, actor_creator, tensor_transport_getter),
        task_manager_(task_manager),
        warn_excess_queueing_(warn_excess_queueing),
        io_service_(io_service),
        reference_counter_(reference_counter) {
    next_queueing_warn_threshold_ =
        ::RayConfig::instance().actor_excess_queueing_warn_threshold();
  }

  void SetPreempted(const ActorID &actor_id) {
    absl::MutexLock lock(&mu_);
    if (auto iter = client_queues_.find(actor_id); iter != client_queues_.end()) {
      iter->second.preempted = true;
    }
  }

  /// Add an actor queue. This should be called whenever a reference to an
  /// actor is created in the language frontend.
  /// TODO(swang): Remove the actor queue once it is sure that this worker will
  /// not receive another reference to the same actor.
  ///
  /// \param[in] actor_id The actor for whom to add a queue.
  /// \param[in] max_pending_calls The max pending calls for the actor to be added.
  /// \param[in] execute_out_of_order Whether to execute tasks out of order.
  /// \param[in] fail_if_actor_unreachable Whether to fail newly submitted tasks
  /// \param[in] owned Whether the actor is owned by the current process.
  /// immediately when the actor is unreachable.
  void AddActorQueueIfNotExists(const ActorID &actor_id,
                                int32_t max_pending_calls,
                                bool execute_out_of_order,
                                bool fail_if_actor_unreachable,
                                bool owned);

  /// Submit a task to an actor for execution.
  ///
  /// \param[in] task_spec The task spec to submit.
  ///
  /// \return Status::Invalid if the task is not yet supported.
  Status SubmitTask(TaskSpecification task_spec);

  /// Submit an actor creation task to an actor via GCS.
  Status SubmitActorCreationTask(TaskSpecification task_spec);

  /// Create connection to actor and send all pending tasks.
  ///
  /// \param[in] actor_id Actor ID.
  /// \param[in] address The new address of the actor.
  /// \param[in] num_restarts How many times this actor has been restarted
  /// before. If we've already seen a later incarnation of the actor, we will
  /// ignore the command to connect.
  void ConnectActor(const ActorID &actor_id,
                    const rpc::Address &address,
                    int64_t num_restarts);

  /// Disconnect from a failed actor.
  ///
  /// \param[in] actor_id Actor ID.
  /// \param[in] num_restarts How many times this actor has been restarted
  /// before. If we've already seen a later incarnation of the actor, we will
  /// ignore the command to connect.
  /// \param[in] dead Whether the actor is dead. In this case, all
  /// pending tasks for the actor should be failed.
  /// \param[in] death_cause Context about why this actor is dead.
  /// \param[in] is_restartable Whether the dead actor is restartable.
  void DisconnectActor(const ActorID &actor_id,
                       int64_t num_restarts,
                       bool dead,
                       const rpc::ActorDeathCause &death_cause,
                       bool is_restartable);

  /// Set the timerstamp for the caller.
  void SetCallerCreationTimestamp(int64_t timestamp);

  /// Check timeout tasks that are waiting for Death info.
  void CheckTimeoutTasks();

  /// If the number of tasks in requests is greater than or equal to
  /// max_pending_calls.
  ///
  /// \param[in] actor_id Actor id.
  /// \return Whether the corresponding client queue is full or not.
  bool PendingTasksFull(const ActorID &actor_id) const;

  /// Get the number of pending tasks in the queue.
  ///
  /// \param[in] actor_id Actor id.
  /// \return The number of pending tasks in the queue.
  size_t NumPendingTasks(const ActorID &actor_id) const;

  /// Check whether the actor exists
  ///
  /// \param[in] actor_id Actor id.
  ///
  /// \return Return true if the actor exists.
  bool CheckActorExists(const ActorID &actor_id) const;

  /// Returns debug string for class.
  ///
  /// \param[in] actor_id The actor whose debug string to return.
  /// \return string.
  std::string DebugString(const ActorID &actor_id) const;

  /// Whether the specified actor is alive.
  ///
  /// \param[in] actor_id The actor ID.
  /// \return Whether this actor is alive.
  bool IsActorAlive(const ActorID &actor_id) const;

  /// Get the given actor id's address.
  /// It returns nullopt if the actor's address is not reported.
  std::optional<rpc::Address> GetActorAddress(const ActorID &actor_id) const;

  /// Get the local actor state. nullopt if the state is unknown.
  std::optional<rpc::ActorTableData::ActorState> GetLocalActorState(
      const ActorID &actor_id) const;

  /// Cancel an actor task of a given task spec.
  ///
  /// Asynchronous API.
  /// The API is thread-safe.
  ///
  /// The cancelation protocol requires the coordination between
  /// the caller and executor side.
  ///
  /// Once the task is canceled, tasks retry count becomes 0.
  ///
  /// The client side protocol is as follow;
  ///
  /// - Dependencies not resolved
  ///   - Cancel dep resolution and fail the object immediately.
  /// - Dependencies are resolved and tasks are queued.
  ///   - Unqueue the entry from the queue and fail the object immediately.
  /// - Tasks are sent to executor.
  ///   - We keep retrying cancel RPCs until the executor said it
  ///     succeeds (tasks were queued or executing) or the task is finished.
  /// - Tasks are finished
  ///   - Do nothing if cancel is requested here.
  ///
  /// The executor side protocol is as follow;
  ///
  /// - Tasks not received
  ///   - Fail the cancel RPC. The client will retry.
  /// - Tasks are queued
  ///   - Register the canceled tasks and fail when the task is
  ///     executed.
  /// - Tasks are executing
  ///   - if async task, trigger future.cancel. Otherwise, do nothing.
  ///     TODO(sang): We should ideally update runtime context so that
  ///     users can do cooperative cancelation.
  /// - Tasks are finished.
  ///   - We just fail the cancel RPC. We cannot distinguish this from
  ///     "Tasks not received" state because we don't track all finished
  ///     tasks. We rely on the client side stop retrying RPCs
  ///     when the task finishes.
  ///
  /// \param task_spec The task spec of a task that will be canceled.
  /// \param recursive If true, it will cancel all child tasks.
  /// \return True if cancel request is not needed or it will be
  /// requested. False otherwise. Note that tasks could be "not"
  /// canceled although the status is true because it is an
  /// asynchronous API.
  Status CancelTask(TaskSpecification task_spec, bool recursive);

  /// Retry the CancelTask in milliseconds.
  void RetryCancelTask(TaskSpecification task_spec, bool recursive, int64_t milliseconds);

  /// Queue the streaming generator up for resubmission.
  /// \return true if the task is still executing and the submitter agrees to resubmit
  /// when it finishes. false case is a TODO.
  bool QueueGeneratorForResubmit(const TaskSpecification &spec);

 private:
  struct PendingTaskWaitingForDeathInfo {
    int64_t deadline_ms;
    TaskSpecification task_spec;
    ray::Status status;
    rpc::RayErrorInfo timeout_error_info;
    bool actor_preempted = false;

    PendingTaskWaitingForDeathInfo(int64_t deadline_ms,
                                   TaskSpecification task_spec,
                                   ray::Status status,
                                   rpc::RayErrorInfo timeout_error_info)
        : deadline_ms(deadline_ms),
          task_spec(std::move(task_spec)),
          status(std::move(status)),
          timeout_error_info(std::move(timeout_error_info)) {}
  };
  /// A helper function to get task manager without holding mu_
  /// We should use this function when access
  /// - FailOrRetryPendingTask
  /// - FailPendingTask
  TaskManagerInterface &GetTaskManagerWithoutMu() {
    mu_.AssertNotHeld();
    return task_manager_;
  }

  struct ClientQueue {
    ClientQueue(ActorID actor_id,
                bool execute_out_of_order,
                int32_t max_pending_calls,
                bool fail_if_actor_unreachable,
                bool owned)
        : max_pending_calls(max_pending_calls),
          fail_if_actor_unreachable(fail_if_actor_unreachable),
          owned(owned) {
      if (execute_out_of_order) {
        actor_submit_queue = std::make_unique<OutofOrderActorSubmitQueue>(actor_id);
      } else {
        actor_submit_queue = std::make_unique<SequentialActorSubmitQueue>(actor_id);
      }
    }

    /// The current state of the actor. If this is ALIVE, then we should have
    /// an RPC client to the actor. If this is DEAD, then all tasks in the
    /// queue will be marked failed and all other ClientQueue state is ignored.
    rpc::ActorTableData::ActorState state = rpc::ActorTableData::DEPENDENCIES_UNREADY;
    /// The reason why this actor is dead.
    /// If the context is not set, it means the actor is not dead.
    rpc::ActorDeathCause death_cause;
    /// How many times this actor has been restarted before. Starts at -1 to
    /// indicate that the actor is not yet created. This is used to drop stale
    /// messages from the GCS.
    int64_t num_restarts = -1;
    /// How many times this actor has been lineage reconstructured.
    /// This is used to drop stale messages.
    int64_t num_restarts_due_to_lineage_reconstructions = 0;
    /// Whether this actor exits by spot preemption.
    bool preempted = false;
    /// The RPC client. We use shared_ptr to enable shared_from_this for
    /// pending client callbacks.
    std::shared_ptr<rpc::CoreWorkerClientInterface> rpc_client = nullptr;
    /// The intended worker ID of the actor.
    std::string worker_id = "";
    /// The actor is out of scope but the death info is not published
    /// to this worker yet.
    bool pending_out_of_scope_death = false;
    /// If the actor is dead, whether it can be restarted.
    bool is_restartable = false;

    /// The queue that orders actor requests.
    std::unique_ptr<IActorSubmitQueue> actor_submit_queue;

    /// Tasks that can't be sent because 1) the callee actor is dead. 2) network error.
    /// For 1) the task will wait for the DEAD state notification, then mark task as
    /// failed using the death_info in notification. For 2) we'll never receive a DEAD
    /// notification, in this case we'll wait for a fixed timeout value and then mark it
    /// as failed.
    ///
    /// Invariants: tasks are ordered by the field `deadline_ms`.
    ///
    /// If we got an actor dead notification, the error_info from that death cause is
    /// used.
    /// If a task timed out, it's possible that the Actor is not dead yet, so we use
    /// `timeout_error_info`. One special case is when the actor is preempted, where
    /// the actor may not be dead *just yet* but we want to treat it as dead. In this
    /// case we hard code an error info.
    std::deque<std::shared_ptr<PendingTaskWaitingForDeathInfo>> wait_for_death_info_tasks;

    /// Stores all callbacks of inflight tasks. An actor task is inflight
    /// if the PushTask RPC is sent but the reply is not received yet.
    absl::flat_hash_map<TaskAttempt, rpc::ClientCallback<rpc::PushTaskReply>>
        inflight_task_callbacks;

    /// The max number limit of task capacity used for back pressure.
    /// If the number of tasks in requests >= max_pending_calls, it can't continue to
    /// push task to ClientQueue.
    const int32_t max_pending_calls;

    /// The current task number in this client queue.
    int32_t cur_pending_calls = 0;

    /// Whether to fail newly submitted tasks immediately when the actor is unreachable.
    bool fail_if_actor_unreachable = true;

    /// Whether the current process is owner of the actor.
    bool owned;

    /// Returns debug string for class.
    ///
    /// \return string.
    std::string DebugString() const {
      std::ostringstream stream;
      stream << "max_pending_calls=" << max_pending_calls
             << " cur_pending_calls=" << cur_pending_calls;
      return stream.str();
    }
  };

  /// Fail the task with the timeout error, or the preempted error.
  void FailTaskWithError(const PendingTaskWaitingForDeathInfo &task);

  /// Push a task to a remote actor via the given client.
  /// Note, this function doesn't return any error status code. If an error occurs while
  /// sending the request, this task will be treated as failed.
  ///
  /// \param[in] queue The actor queue. Contains the RPC client state.
  /// \param[in] task_spec The task to send.
  /// \param[in] skip_queue Whether to skip the task queue. This will send the
  /// task for execution immediately.
  /// \return Void.
  void PushActorTask(ClientQueue &queue,
                     const TaskSpecification &task_spec,
                     bool skip_queue) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  void HandlePushTaskReply(const Status &status,
                           const rpc::PushTaskReply &reply,
                           const rpc::Address &addr,
                           const TaskSpecification &task_spec) ABSL_LOCKS_EXCLUDED(mu_);

  /// Send all pending tasks for an actor.
  ///
  /// If the actor is pending out-of-scope death notification, pending tasks will
  /// wait until the notification is received to decide whether we should
  /// fail pending tasks or restart the actor.
  /// \param[in] actor_id Actor ID.
  /// \return Void.
  void SendPendingTasks(const ActorID &actor_id) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  /// Disconnect the RPC client for an actor.
  void DisconnectRpcClient(ClientQueue &queue) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  /// Mark all in-flight tasks as failed if the actor was restarted. This will cause the
  /// tasks to be retried as usual.
  void FailInflightTasksOnRestart(
      const absl::flat_hash_map<TaskAttempt, rpc::ClientCallback<rpc::PushTaskReply>>
          &inflight_task_callbacks) ABSL_LOCKS_EXCLUDED(mu_);

  /// Restart the actor from DEAD by sending a RestartActorForLineageReconstruction rpc to
  /// GCS.
  void RestartActorForLineageReconstruction(const ActorID &actor_id)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  void NotifyGCSWhenActorOutOfScope(const ActorID &actor_id,
                                    uint64_t num_restarts_due_to_lineage_reconstructions);

  /// Pool for producing new core worker clients.
  rpc::CoreWorkerClientPool &core_worker_client_pool_;

  ActorCreatorInterface &actor_creator_;

  /// Mutex to protect the various maps below.
  mutable absl::Mutex mu_;

  absl::flat_hash_map<ActorID, ClientQueue> client_queues_ ABSL_GUARDED_BY(mu_);

  // Generators that are currently running and need to be resubmitted.
  absl::flat_hash_set<TaskID> generators_to_resubmit_ ABSL_GUARDED_BY(mu_);

  /// Resolve object dependencies.
  LocalDependencyResolver resolver_;

  /// Used to complete tasks.
  TaskManagerInterface &task_manager_;

  /// Used to warn of excessive queueing.
  std::function<void(const ActorID &, uint64_t num_queued)> warn_excess_queueing_;

  /// Warn the next time the number of queued task submissions to an actor
  /// exceeds this quantity. This threshold is doubled each time it is hit.
  uint64_t next_queueing_warn_threshold_;

  /// The event loop where the actor task events are handled.
  instrumented_io_context &io_service_;

  std::shared_ptr<ReferenceCounterInterface> reference_counter_;

  friend class CoreWorkerTest;
};

}  // namespace core
}  // namespace ray
