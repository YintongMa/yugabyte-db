// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/tablet/tablet_peer.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/quorum_util.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/docdb.h"

#include "yb/gutil/mathlimits.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/sysinfo.h"

#include "yb/rocksdb/db/memtable.h"

#include "yb/rpc/messenger.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet.pb.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_peer_mm_ops.h"

#include "yb/tablet/operations/alter_schema_operation.h"
#include "yb/tablet/operations/operation_driver.h"
#include "yb/tablet/operations/truncate_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/operations/update_txn_operation.h"

#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/stopwatch.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"

using std::shared_ptr;
using std::string;

namespace yb {
namespace tablet {

METRIC_DEFINE_histogram(tablet, op_prepare_queue_length, "Operation Prepare Queue Length",
                        MetricUnit::kTasks,
                        "Number of operations waiting to be prepared within this tablet. "
                        "High queue lengths indicate that the server is unable to process "
                        "operations as fast as they are being written to the WAL.",
                        10000, 2);

METRIC_DEFINE_histogram(tablet, op_prepare_queue_time, "Operation Prepare Queue Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent waiting in the prepare queue before being "
                        "processed. High queue times indicate that the server is unable to "
                        "process operations as fast as they are being written to the WAL.",
                        10000000, 2);

METRIC_DEFINE_histogram(tablet, op_prepare_run_time, "Operation Prepare Run Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent being prepared in the tablet. "
                        "High values may indicate that the server is under-provisioned or "
                        "that operations are experiencing high contention with one another for "
                        "locks.",
                        10000000, 2);

using consensus::Consensus;
using consensus::ConsensusBootstrapInfo;
using consensus::ConsensusMetadata;
using consensus::ConsensusOptions;
using consensus::ConsensusRound;
using consensus::StateChangeContext;
using consensus::StateChangeReason;
using consensus::OpId;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using consensus::RaftConsensus;
using consensus::ReplicateMsg;
using consensus::OpIdType;
using log::Log;
using log::LogAnchorRegistry;
using rpc::Messenger;
using strings::Substitute;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Tablet Peer
// ============================================================================
TabletPeer::TabletPeer(
    const scoped_refptr<TabletMetadata>& meta,
    const consensus::RaftPeerPB& local_peer_pb,
    const std::string& permanent_uuid,
    Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk)
  : meta_(meta),
    tablet_id_(meta->tablet_id()),
    local_peer_pb_(local_peer_pb),
    state_(TabletStatePB::NOT_STARTED),
    status_listener_(new TabletStatusListener(meta)),
    log_anchor_registry_(new LogAnchorRegistry()),
    mark_dirty_clbk_(std::move(mark_dirty_clbk)),
    permanent_uuid_(permanent_uuid) {}

TabletPeer::~TabletPeer() {
  std::lock_guard<simple_spinlock> lock(lock_);
  // We should either have called Shutdown(), or we should have never called
  // Init().
  LOG_IF_WITH_PREFIX(DFATAL, tablet_) << "TabletPeer not fully shut down.";
}

Status TabletPeer::InitTabletPeer(const shared_ptr<TabletClass> &tablet,
                                  const std::shared_future<client::YBClientPtr> &client_future,
                                  const scoped_refptr<server::Clock> &clock,
                                  const shared_ptr<Messenger> &messenger,
                                  rpc::ProxyCache* proxy_cache,
                                  const scoped_refptr<Log> &log,
                                  const scoped_refptr<MetricEntity> &metric_entity,
                                  ThreadPool* raft_pool,
                                  ThreadPool* tablet_prepare_pool) {

  DCHECK(tablet) << "A TabletPeer must be provided with a Tablet";
  DCHECK(log) << "A TabletPeer must be provided with a Log";

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    auto state = state_.load(std::memory_order_acquire);
    if (state != TabletStatePB::BOOTSTRAPPING) {
      return STATUS_FORMAT(
          IllegalState, "Invalid tablet state for init: $0", TabletStatePB_Name(state));
    }
    tablet_ = tablet;
    client_future_ = client_future;
    clock_ = clock;
    proxy_cache_ = proxy_cache;
    log_ = log;
    // "Publish" the log pointer so it can be retrieved using the log() accessor.
    log_atomic_ = log.get();
    service_thread_pool_ = &messenger->ThreadPool();

    tablet->SetMemTableFlushFilterFactory([log] {
      auto index = log->GetLatestEntryOpId().index;
      return [index] (const rocksdb::MemTable& memtable) -> Result<bool> {
        auto frontiers = memtable.Frontiers();
        if (frontiers) {
          const auto& largest = down_cast<const docdb::ConsensusFrontier&>(frontiers->Largest());
          // We can only flush this memtable if all operations written to it have also been written
          // to the log (maybe not synced, if durable_wal_write is disabled, but that's OK).
          return largest.op_id().index <= index;
        }
        // This is a degenerate case that should ideally never occur. An empty memtable got into the
        // list of immutable memtables. We say it is OK to flush it and move on.
        static const char* error_msg =
            "A memtable with no frontiers set found when deciding what memtables to "
            "flush! This should not happen.";
        LOG(ERROR) << error_msg << " Stack trace:\n" << GetStackTrace();
        return STATUS(IllegalState, error_msg);
      };
    });

    ConsensusOptions options;
    options.tablet_id = meta_->tablet_id();

    TRACE("Creating consensus instance");

    std::unique_ptr<ConsensusMetadata> cmeta;
    RETURN_NOT_OK(ConsensusMetadata::Load(meta_->fs_manager(), tablet_id_,
                                          meta_->fs_manager()->uuid(), &cmeta));

    consensus_ = RaftConsensus::Create(
        options,
        std::move(cmeta),
        local_peer_pb_,
        metric_entity,
        clock_,
        this,
        messenger,
        proxy_cache_,
        log_.get(),
        tablet_->mem_tracker(),
        mark_dirty_clbk_,
        tablet_->table_type(),
        std::bind(&Tablet::LostLeadership, tablet.get()),
        raft_pool);
    has_consensus_.store(true, std::memory_order_release);
    auto ht_lease_provider = [this](MicrosTime min_allowed, MonoTime deadline) {
      MicrosTime lease_micros {
          consensus_->MajorityReplicatedHtLeaseExpiration(min_allowed, deadline) };
      if (!lease_micros) {
        return HybridTime::kInvalid;
      }
      if (lease_micros >= kMaxHybridTimePhysicalMicros) {
        // This could happen when leader leases are disabled.
        return HybridTime::kMax;
      }
      return HybridTime(lease_micros, /* logical */ 0);
    };
    tablet_->SetHybridTimeLeaseProvider(ht_lease_provider);

    auto* mvcc_manager = tablet_->mvcc_manager();
    consensus_->SetPropagatedSafeTimeProvider([mvcc_manager, ht_lease_provider] {
      // Get the current majority-replicated HT leader lease without any waiting.
      auto ht_lease = ht_lease_provider(/* min_allowed */ 0, /* deadline */ MonoTime::kMax);
      if (!ht_lease) {
        return HybridTime::kInvalid;
      }
      return mvcc_manager->SafeTime(ht_lease);
    });

    prepare_thread_ = std::make_unique<Preparer>(consensus_.get(), tablet_prepare_pool);

    consensus_->SetMajorityReplicatedListener([mvcc_manager, ht_lease_provider] {
      auto ht_lease = ht_lease_provider(/* min_allowed */ 0, /* deadline */ MonoTime::kMax);
      if (ht_lease) {
        mvcc_manager->UpdatePropagatedSafeTimeOnLeader(ht_lease);
      }
    });
  }

  RETURN_NOT_OK(prepare_thread_->Start());

  if (tablet_->metrics() != nullptr) {
    TRACE("Starting instrumentation");
    operation_tracker_.StartInstrumentation(tablet_->GetMetricEntity());
  }
  operation_tracker_.StartMemoryTracking(tablet_->mem_tracker());

  if (tablet_->transaction_coordinator()) {
    tablet_->transaction_coordinator()->Start();
  }

  TRACE("TabletPeer::Init() finished");
  VLOG_WITH_PREFIX(2) << "Peer Initted";

  return Status::OK();
}

Status TabletPeer::Start(const ConsensusBootstrapInfo& bootstrap_info) {
  {
    std::lock_guard<simple_spinlock> l(state_change_lock_);
    TRACE("Starting consensus");

    VLOG_WITH_PREFIX(2) << "Peer starting";

    VLOG(2) << "RaftConfig before starting: " << consensus_->CommittedConfig().DebugString();

    RETURN_NOT_OK(consensus_->Start(bootstrap_info));
    RETURN_NOT_OK(UpdateState(TabletStatePB::BOOTSTRAPPING, TabletStatePB::RUNNING,
                              "Incorrect state to start TabletPeer, "));
  }
  // The context tracks that the current caller does not hold the lock for consensus state.
  // So mark dirty callback, e.g., consensus->ConsensusState() for master consensus callback of
  // SysCatalogStateChanged, can get the lock when needed.
  auto context =
      std::make_shared<StateChangeContext>(StateChangeReason::TABLET_PEER_STARTED, false);
  // Because we changed the tablet state, we need to re-report the tablet to the master.
  mark_dirty_clbk_.Run(context);

  return Status::OK();
}

const consensus::RaftConfigPB TabletPeer::RaftConfig() const {
  CHECK(consensus_) << "consensus is null";
  return consensus_->CommittedConfig();
}

bool TabletPeer::StartShutdown() {
  LOG_WITH_PREFIX(INFO) << "Initiating TabletPeer shutdown";

  if (tablet_) {
    tablet_->SetShutdownRequestedFlag();
  }

  {
    TabletStatePB state = state_.load(std::memory_order_acquire);
    for (;;) {
      if (state == TabletStatePB::QUIESCING || state == TabletStatePB::SHUTDOWN) {
        return false;
      }
      if (state_.compare_exchange_strong(
          state, TabletStatePB::QUIESCING, std::memory_order_acq_rel)) {
        LOG_WITH_PREFIX(INFO) << "Started shutdown from state: " << TabletStatePB_Name(state);
        break;
      }
    }
  }

  std::lock_guard<simple_spinlock> l(state_change_lock_);
  // Even though Tablet::Shutdown() also unregisters its ops, we have to do it here
  // to ensure that any currently running operation finishes before we proceed with
  // the rest of the shutdown sequence. In particular, a maintenance operation could
  // indirectly end up calling into the log, which we are about to shut down.
  UnregisterMaintenanceOps();

  if (consensus_) {
    consensus_->Shutdown();
  }

  return true;
}

void TabletPeer::CompleteShutdown() {
  // TODO: KUDU-183: Keep track of the pending tasks and send an "abort" message.
  LOG_SLOW_EXECUTION(WARNING, 1000,
      Substitute("TabletPeer: tablet $0: Waiting for Operations to complete", tablet_id())) {
    operation_tracker_.WaitForAllToFinish();
  }

  if (prepare_thread_) {
    prepare_thread_->Stop();
  }

  if (log_) {
    WARN_NOT_OK(log_->Close(), "Error closing the Log.");
  }

  if (VLOG_IS_ON(1)) {
    VLOG_WITH_PREFIX(1) << "Shut down!";
  }

  if (tablet_) {
    tablet_->Shutdown();
  }

  // Only mark the peer as SHUTDOWN when all other components have shut down.
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    // Release mem tracker resources.
    has_consensus_.store(false, std::memory_order_release);
    consensus_.reset();
    prepare_thread_.reset();
    tablet_.reset();
    auto state = state_.load(std::memory_order_acquire);
    LOG_IF_WITH_PREFIX(DFATAL, state != TabletStatePB::QUIESCING) <<
        "Bad state when completing shutdown: " << TabletStatePB_Name(state);
    state_.store(TabletStatePB::SHUTDOWN, std::memory_order_release);
  }
}

void TabletPeer::WaitUntilShutdown() {
  while (state_.load(std::memory_order_acquire) != TabletStatePB::SHUTDOWN) {
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
}

void TabletPeer::Shutdown() {
  if (StartShutdown()) {
    CompleteShutdown();
  } else {
    WaitUntilShutdown();
  }
}

Status TabletPeer::CheckRunning() const {
  if (state_.load(std::memory_order_acquire) != TabletStatePB::RUNNING) {
    return STATUS(IllegalState, Substitute("The tablet is not in a running state: $0",
                                           TabletStatePB_Name(state_)));
  }
  return Status::OK();
}

Status TabletPeer::CheckShutdownOrNotStarted() const {
  TabletStatePB value = state_.load(std::memory_order_acquire);
  if (value != TabletStatePB::SHUTDOWN && value != TabletStatePB::NOT_STARTED) {
    return STATUS(IllegalState, Substitute("The tablet is not in a shutdown state: $0",
                                           TabletStatePB_Name(value)));
  }
  return Status::OK();
}

Status TabletPeer::WaitUntilConsensusRunning(const MonoDelta& timeout) {
  MonoTime start(MonoTime::Now());

  int backoff_exp = 0;
  const int kMaxBackoffExp = 8;
  while (true) {
    TabletStatePB cached_state = state_.load(std::memory_order_acquire);
    if (cached_state == TabletStatePB::QUIESCING || cached_state == TabletStatePB::SHUTDOWN) {
      return STATUS(IllegalState,
          Substitute("The tablet is already shutting down or shutdown. State: $0",
                     TabletStatePB_Name(cached_state)));
    }
    if (cached_state == RUNNING && has_consensus_.load(std::memory_order_acquire) &&
        consensus_->IsRunning()) {
      break;
    }
    MonoTime now(MonoTime::Now());
    MonoDelta elapsed(now.GetDeltaSince(start));
    if (elapsed.MoreThan(timeout)) {
      return STATUS(TimedOut, Substitute("Consensus is not running after waiting for $0. State; $1",
                                         elapsed.ToString(), TabletStatePB_Name(cached_state)));
    }
    SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
    backoff_exp = std::min(backoff_exp + 1, kMaxBackoffExp);
  }
  return Status::OK();
}

void TabletPeer::WriteAsync(std::unique_ptr<WriteOperationState> state, MonoTime deadline) {
  auto status = CheckRunning();
  if (!status.ok()) {
    state->completion_callback()->CompleteWithStatus(status);
    return;
  }
  auto operation = std::make_unique<WriteOperation>(
      std::move(state), consensus::LEADER, deadline, this);
  tablet_->AcquireLocksAndPerformDocOperations(std::move(operation));
}

void TabletPeer::StartExecution(std::unique_ptr<Operation> operation) {
  auto driver = NewLeaderOperationDriver(&operation);
  if (driver.ok()) {
    (**driver).ExecuteAsync();
  } else {
    auto status = driver.status();
    operation->state()->completion_callback()->CompleteWithStatus(status);
  }
}

HybridTime TabletPeer::ReportReadRestart() {
  tablet_->metrics()->restart_read_requests->Increment();
  return tablet_->SafeTime(RequireLease::kTrue);
}

void TabletPeer::Submit(std::unique_ptr<Operation> operation) {
  auto status = CheckRunning();

  if (status.ok()) {
    auto driver = NewLeaderOperationDriver(&operation);
    if (driver.ok()) {
      (**driver).ExecuteAsync();
    } else {
      status = driver.status();
    }
  }
  if (!status.ok()) {
    operation->Finish(Operation::ABORTED);
    operation->state()->completion_callback()->CompleteWithStatus(status);
  }
}

void TabletPeer::SubmitUpdateTransaction(std::unique_ptr<UpdateTxnOperationState> state) {
  Submit(std::make_unique<tablet::UpdateTxnOperation>(std::move(state), consensus::LEADER));
}

HybridTime TabletPeer::Now() {
  return clock_->Now();
}

void TabletPeer::UpdateClock(HybridTime hybrid_time) {
  clock_->Update(hybrid_time);
}

std::unique_ptr<UpdateTxnOperationState> TabletPeer::CreateUpdateTransactionState(
    tserver::TransactionStatePB* request) {
  auto result = std::make_unique<UpdateTxnOperationState>(tablet());
  result->TakeRequest(request);
  return result;
}

void TabletPeer::GetTabletStatusPB(TabletStatusPB* status_pb_out) const {
  std::lock_guard<simple_spinlock> lock(lock_);
  DCHECK(status_pb_out != nullptr);
  DCHECK(status_listener_.get() != nullptr);
  status_pb_out->set_tablet_id(status_listener_->tablet_id());
  status_pb_out->set_table_name(status_listener_->table_name());
  status_pb_out->set_last_status(status_listener_->last_status());
  status_listener_->partition().ToPB(status_pb_out->mutable_partition());
  status_pb_out->set_state(state_);
  status_pb_out->set_tablet_data_state(meta_->tablet_data_state());
  status_pb_out->set_estimated_on_disk_size(OnDiskSize());
}

Status TabletPeer::RunLogGC() {
  if (!CheckRunning().ok()) {
    return Status::OK();
  }
  int64_t min_log_index = 0;
  int32_t num_gced = 0;
  RETURN_NOT_OK(GetEarliestNeededLogIndex(&min_log_index));
  return log_->GC(min_log_index, &num_gced);
}

string TabletPeer::HumanReadableState() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  TabletDataState data_state = meta_->tablet_data_state();
  // If failed, any number of things could have gone wrong.
  if (state() == TabletStatePB::FAILED) {
    return Substitute("$0 ($1): $2", TabletStatePB_Name(state_),
                      TabletDataState_Name(data_state),
                      error_.get()->ToString());
  // If it's remotely bootstrapping, or tombstoned, that is the important thing
  // to show.
  } else if (data_state != TABLET_DATA_READY) {
    return TabletDataState_Name(data_state);
  }
  // Otherwise, the tablet's data is in a "normal" state, so we just display
  // the runtime state (BOOTSTRAPPING, RUNNING, etc).
  return TabletStatePB_Name(state_.load(std::memory_order_acquire));
}

namespace {

consensus::OperationType MapOperationTypeToPB(OperationType operation_type) {
  switch (operation_type) {
    case OperationType::kWrite:
      return consensus::WRITE_OP;

    case OperationType::kAlterSchema:
      return consensus::ALTER_SCHEMA_OP;

    case OperationType::kUpdateTransaction:
      return consensus::UPDATE_TRANSACTION_OP;

    case OperationType::kSnapshot:
      return consensus::SNAPSHOT_OP;

    case OperationType::kTruncate:
      return consensus::TRUNCATE_OP;

    case OperationType::kEmpty:
      LOG(FATAL) << "OperationType::kEmpty cannot be converted to consensus::OperationType";
  }
  FATAL_INVALID_ENUM_VALUE(OperationType, operation_type);
}

} // namespace

void TabletPeer::GetInFlightOperations(Operation::TraceType trace_type,
                                       vector<consensus::OperationStatusPB>* out) const {
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    if (driver->state() == nullptr) {
      continue;
    }
    auto op_type = driver->operation_type();
    if (op_type == OperationType::kEmpty) {
      // This is a special-purpose in-memory-only operation for updating propagated safe time on
      // a follower.
      continue;
    }

    consensus::OperationStatusPB status_pb;
    status_pb.mutable_op_id()->CopyFrom(driver->GetOpId());
    status_pb.set_operation_type(MapOperationTypeToPB(op_type));
    status_pb.set_description(driver->ToString());
    int64_t running_for_micros =
        MonoTime::Now().GetDeltaSince(driver->start_time()).ToMicroseconds();
    status_pb.set_running_for_micros(running_for_micros);
    if (trace_type == Operation::TRACE_TXNS) {
      status_pb.set_trace_buffer(driver->trace()->DumpToString(true));
    }
    out->push_back(status_pb);
  }
}

Status TabletPeer::GetEarliestNeededLogIndex(int64_t* min_index) const {
  // First, we anchor on the last OpId in the Log to establish a lower bound
  // and avoid racing with the other checks. This limits the Log GC candidate
  // segments before we check the anchors.
  *min_index = log_->GetLatestEntryOpId().index;

  // If we never have written to the log, no need to proceed.
  if (*min_index == 0) {
    return Status::OK();
  }

  // Next, we interrogate the anchor registry.
  // Returns OK if minimum known, NotFound if no anchors are registered.
  {
    int64_t min_anchor_index;
    Status s = log_anchor_registry_->GetEarliestRegisteredLogIndex(&min_anchor_index);
    if (PREDICT_FALSE(!s.ok())) {
      DCHECK(s.IsNotFound()) << "Unexpected error calling LogAnchorRegistry: " << s.ToString();
    } else {
      *min_index = std::min(*min_index, min_anchor_index);
    }
  }

  // Next, interrogate the OperationTracker.
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    OpId tx_op_id = driver->GetOpId();
    // A operation which doesn't have an opid hasn't been submitted for replication yet and
    // thus has no need to anchor the log.
    if (tx_op_id.IsInitialized()) {
      *min_index = std::min(*min_index, tx_op_id.index());
    }
  }

  auto* transaction_coordinator = tablet()->transaction_coordinator();
  if (transaction_coordinator) {
    *min_index = std::min(*min_index, transaction_coordinator->PrepareGC());
  }

  int64_t last_committed_write_index = tablet_->last_committed_write_index();
  if (tablet_->table_type() != TableType::TRANSACTION_STATUS_TABLE_TYPE) {
    auto max_persistent_op_id = VERIFY_RESULT(tablet_->MaxPersistentOpId());
    int64_t max_persistent_index = max_persistent_op_id.regular.index;
    if (max_persistent_op_id.intents
        && max_persistent_op_id.intents < max_persistent_op_id.regular) {
      max_persistent_index = max_persistent_op_id.intents.index;
    }
    // Check whether we had writes after last persistent entry.
    // Note that last_committed_write_index could be zero if logs were cleaned before restart.
    // So correct check is 'less', and NOT 'not equals to'.
    if (max_persistent_index < last_committed_write_index) {
      *min_index = std::min(*min_index, max_persistent_index);
    }
  }

  // We keep at least one committed operation in the log so that we can always recover safe time
  // during bootstrap.
  OpId committed_op_id;
  const Status get_committed_op_id_status =
      consensus()->GetLastOpId(OpIdType::COMMITTED_OPID, &committed_op_id);
  if (!get_committed_op_id_status.IsNotFound()) {
    // NotFound is returned by local consensus. We should get rid of this logic once local
    // consensus is gone.
    RETURN_NOT_OK(get_committed_op_id_status);
    *min_index = std::min(*min_index, static_cast<int64_t>(committed_op_id.index()));
  }

  return Status::OK();
}

Status TabletPeer::GetMaxIndexesToSegmentSizeMap(MaxIdxToSegmentSizeMap* idx_size_map) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx;
  RETURN_NOT_OK(GetEarliestNeededLogIndex(&min_op_idx));
  log_->GetMaxIndexesToSegmentSizeMap(min_op_idx, idx_size_map);
  return Status::OK();
}

Status TabletPeer::GetGCableDataSize(int64_t* retention_size) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx;
  RETURN_NOT_OK(GetEarliestNeededLogIndex(&min_op_idx));
  log_->GetGCableDataSize(min_op_idx, retention_size);
  return Status::OK();
}

log::Log* TabletPeer::log() const {
  Log* log = log_atomic_.load(std::memory_order_acquire);
  LOG_IF_WITH_PREFIX(FATAL, !log) << "log() called before the log instance is initialized.";
  return log;
}

yb::OpId TabletPeer::GetLatestLogEntryOpId() const {
  Log* log = log_atomic_.load(std::memory_order_acquire);
  if (log) {
    return log->GetLatestEntryOpId();
  }
  return yb::OpId();
}

std::unique_ptr<Operation> TabletPeer::CreateOperation(consensus::ReplicateMsg* replicate_msg) {
  switch (replicate_msg->op_type()) {
    case consensus::WRITE_OP:
      DCHECK(replicate_msg->has_write_request()) << "WRITE_OP replica"
          " operation must receive a WriteRequestPB";
      return std::make_unique<WriteOperation>(
          std::make_unique<WriteOperationState>(tablet()), consensus::REPLICA, MonoTime::Max(),
          this);

    case consensus::ALTER_SCHEMA_OP:
      DCHECK(replicate_msg->has_alter_schema_request()) << "ALTER_SCHEMA_OP replica"
          " operation must receive an AlterSchemaRequestPB";
      return std::make_unique<AlterSchemaOperation>(
          std::make_unique<AlterSchemaOperationState>(tablet(), log()), consensus::REPLICA);

    case consensus::UPDATE_TRANSACTION_OP:
      DCHECK(replicate_msg->has_transaction_state()) << "UPDATE_TRANSACTION_OP replica"
          " operation must receive an TransactionStatePB";
      return std::make_unique<UpdateTxnOperation>(
          std::make_unique<UpdateTxnOperationState>(tablet()), consensus::REPLICA);

    case consensus::TRUNCATE_OP:
      DCHECK(replicate_msg->has_truncate_request()) << "TRUNCATE_OP replica"
          " operation must receive an TruncateRequestPB";
      return std::make_unique<TruncateOperation>(
          std::make_unique<TruncateOperationState>(tablet()), consensus::REPLICA);

    case consensus::SNAPSHOT_OP: FALLTHROUGH_INTENDED;
    case consensus::UNKNOWN_OP: FALLTHROUGH_INTENDED;
    case consensus::NO_OP: FALLTHROUGH_INTENDED;
    case consensus::CHANGE_CONFIG_OP:
      FATAL_INVALID_ENUM_VALUE(consensus::OperationType, replicate_msg->op_type());
  }
  FATAL_INVALID_ENUM_VALUE(consensus::OperationType, replicate_msg->op_type());
}

Status TabletPeer::StartReplicaOperation(
    const scoped_refptr<ConsensusRound>& round, HybridTime propagated_safe_time) {
  TabletStatePB value = state();
  if (value != TabletStatePB::RUNNING && value != TabletStatePB::BOOTSTRAPPING) {
    return STATUS(IllegalState, TabletStatePB_Name(value));
  }

  consensus::ReplicateMsg* replicate_msg = round->replicate_msg().get();
  DCHECK(replicate_msg->has_hybrid_time());
  auto operation = CreateOperation(replicate_msg);

  // TODO(todd) Look at wiring the stuff below on the driver
  OperationState* state = operation->state();
  // It's imperative that we set the round here on any type of operation, as this
  // allows us to keep the reference to the request in the round instead of copying it.
  state->set_consensus_round(round);
  HybridTime ht(replicate_msg->hybrid_time());
  state->set_hybrid_time(ht);
  clock_->Update(ht);

  // This sets the monotonic counter to at least replicate_msg.monotonic_counter() atomically.
  tablet_->UpdateMonotonicCounter(replicate_msg->monotonic_counter());

  OperationDriverPtr driver = VERIFY_RESULT(NewReplicaOperationDriver(&operation));

  // Unretained is required to avoid a refcount cycle.
  state->consensus_round()->SetConsensusReplicatedCallback(
      std::bind(&OperationDriver::ReplicationFinished, driver.get(), std::placeholders::_1));

  if (propagated_safe_time) {
    driver->SetPropagatedSafeTime(propagated_safe_time, tablet_->mvcc_manager());
  }
  driver->ExecuteAsync();
  return Status::OK();
}

void TabletPeer::SetPropagatedSafeTime(HybridTime ht) {
  auto driver = NewReplicaOperationDriver(nullptr);
  if (!driver.ok()) {
    LOG_WITH_PREFIX(ERROR) << "Failed to create operation driver to set propagated hybrid time";
    return;
  }
  (**driver).SetPropagatedSafeTime(ht, tablet_->mvcc_manager());
  (**driver).ExecuteAsync();
}

consensus::Consensus* TabletPeer::consensus() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return consensus_.get();
}

shared_ptr<consensus::Consensus> TabletPeer::shared_consensus() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return consensus_;
}

Result<OperationDriverPtr> TabletPeer::NewLeaderOperationDriver(
    std::unique_ptr<Operation>* operation) {
  return NewOperationDriver(operation, consensus::LEADER);
}

Result<OperationDriverPtr> TabletPeer::NewReplicaOperationDriver(
    std::unique_ptr<Operation>* operation) {
  return NewOperationDriver(operation, consensus::REPLICA);
}

Result<OperationDriverPtr> TabletPeer::NewOperationDriver(std::unique_ptr<Operation>* operation,
                                                          consensus::DriverType type) {
  auto operation_driver = CreateOperationDriver();
  RETURN_NOT_OK(operation_driver->Init(operation, type));
  return operation_driver;
}

void TabletPeer::RegisterMaintenanceOps(MaintenanceManager* maint_mgr) {
  // Taking state_change_lock_ ensures that we don't shut down concurrently with
  // this last start-up task.
  // Note that the state_change_lock_ is taken in Shutdown(),
  // prior to calling UnregisterMaintenanceOps().

  std::lock_guard<simple_spinlock> l(state_change_lock_);

  if (state() != TabletStatePB::RUNNING) {
    LOG_WITH_PREFIX(WARNING) << "Not registering maintenance operations: tablet not RUNNING";
    return;
  }

  DCHECK(maintenance_ops_.empty());

  gscoped_ptr<MaintenanceOp> log_gc(new LogGCOp(this));
  maint_mgr->RegisterOp(log_gc.get());
  maintenance_ops_.push_back(log_gc.release());
}

void TabletPeer::UnregisterMaintenanceOps() {
  DCHECK(state_change_lock_.is_locked());
  for (MaintenanceOp* op : maintenance_ops_) {
    op->Unregister();
  }
  STLDeleteElements(&maintenance_ops_);
}

uint64_t TabletPeer::OnDiskSize() const {
  uint64_t ret = 0;

  if (consensus_) {
    ret += consensus_->OnDiskSize();
  }

  if (tablet_) {
    ret += tablet_->GetTotalSSTFileSizes();
  }

  if (log_) {
    ret += log_->OnDiskSize();
  }

  return ret;
}

std::string TabletPeer::LogPrefix() const {
  return Substitute("T $0 P $1 [state=$2]: ",
      tablet_id_, permanent_uuid_, TabletStatePB_Name(state()));
}

scoped_refptr<OperationDriver> TabletPeer::CreateOperationDriver() {
  return scoped_refptr<OperationDriver>(new OperationDriver(
      &operation_tracker_,
      consensus_.get(),
      log_.get(),
      prepare_thread_.get(),
      &operation_order_verifier_,
      tablet_->table_type()));
}

consensus::Consensus::LeaderStatus TabletPeer::LeaderStatus() const {
  shared_ptr<consensus::Consensus> consensus;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    consensus = consensus_;
  }
  return consensus ? consensus->leader_status() : consensus::Consensus::LeaderStatus::NOT_LEADER;
}

HybridTime TabletPeer::HtLeaseExpiration() const {
  HybridTime result(consensus_->MajorityReplicatedHtLeaseExpiration(0, MonoTime::kMax), 0);
  return std::max(result, tablet_->mvcc_manager()->LastReplicatedHybridTime());
}

TableType TabletPeer::table_type() {
  // TODO: what if tablet is not set?
  return tablet()->table_type();
}

void TabletPeer::SetFailed(const Status& error) {
  DCHECK(error_.get(std::memory_order_acquire) == nullptr);
  error_ = MakeAtomicUniquePtr<Status>(error);
  auto state = state_.load(std::memory_order_acquire);
  while (state != TabletStatePB::FAILED && state != TabletStatePB::QUIESCING &&
         state != TabletStatePB::SHUTDOWN) {
    if (state_.compare_exchange_weak(state, TabletStatePB::FAILED, std::memory_order_acq_rel)) {
      LOG_WITH_PREFIX(INFO) << "Changed state from " << TabletStatePB_Name(state) << " to FAILED";
      break;
    }
  }
}

Status TabletPeer::UpdateState(TabletStatePB expected, TabletStatePB new_state,
                               const std::string& error_message) {
  TabletStatePB old = expected;
  if (!state_.compare_exchange_strong(old, new_state, std::memory_order_acq_rel)) {
    return STATUS_FORMAT(
        InvalidArgument, "$0 Expected state: $1, got: $2",
        error_message, TabletStatePB_Name(expected), TabletStatePB_Name(old));
  }

  LOG_WITH_PREFIX(INFO) << "Changed state from " << TabletStatePB_Name(old) << " to "
                        << TabletStatePB_Name(new_state);
  return Status::OK();
}

}  // namespace tablet
}  // namespace yb
