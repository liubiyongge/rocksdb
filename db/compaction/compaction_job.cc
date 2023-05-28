//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction/compaction_job.h"

#include <algorithm>
#include <cinttypes>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "db/blob/blob_counting_iterator.h"
#include "db/blob/blob_file_addition.h"
#include "db/blob/blob_file_builder.h"
#include "db/builder.h"
#include "db/compaction/clipping_iterator.h"
#include "db/compaction/compaction_state.h"
#include "db/db_impl/db_impl.h"
#include "db/dbformat.h"
#include "db/error_handler.h"
#include "db/event_helpers.h"
#include "db/history_trimming_iterator.h"
#include "db/log_writer.h"
#include "db/merge_helper.h"
#include "db/range_del_aggregator.h"
#include "db/version_set.h"
#include "file/filename.h"
#include "file/read_write_util.h"
#include "file/sst_file_manager_impl.h"
#include "file/writable_file_writer.h"
#include "logging/log_buffer.h"
#include "logging/logging.h"
#include "monitoring/iostats_context_imp.h"
#include "monitoring/thread_status_util.h"
#include "options/configurable_helper.h"
#include "options/options_helper.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "rocksdb/utilities/options_type.h"
#include "table/merging_iterator.h"
#include "table/table_builder.h"
#include "table/unique_id_impl.h"
#include "test_util/sync_point.h"
#include "util/stop_watch.h"

namespace ROCKSDB_NAMESPACE {

const char* GetCompactionReasonString(CompactionReason compaction_reason) {
  switch (compaction_reason) {
    case CompactionReason::kUnknown:
      return "Unknown";
    case CompactionReason::kLevelL0FilesNum:
      return "LevelL0FilesNum";
    case CompactionReason::kLevelMaxLevelSize:
      return "LevelMaxLevelSize";
    case CompactionReason::kUniversalSizeAmplification:
      return "UniversalSizeAmplification";
    case CompactionReason::kUniversalSizeRatio:
      return "UniversalSizeRatio";
    case CompactionReason::kUniversalSortedRunNum:
      return "UniversalSortedRunNum";
    case CompactionReason::kFIFOMaxSize:
      return "FIFOMaxSize";
    case CompactionReason::kFIFOReduceNumFiles:
      return "FIFOReduceNumFiles";
    case CompactionReason::kFIFOTtl:
      return "FIFOTtl";
    case CompactionReason::kManualCompaction:
      return "ManualCompaction";
    case CompactionReason::kFilesMarkedForCompaction:
      return "FilesMarkedForCompaction";
    case CompactionReason::kBottommostFiles:
      return "BottommostFiles";
    case CompactionReason::kTtl:
      return "Ttl";
    case CompactionReason::kFlush:
      return "Flush";
    case CompactionReason::kExternalSstIngestion:
      return "ExternalSstIngestion";
    case CompactionReason::kPeriodicCompaction:
      return "PeriodicCompaction";
    case CompactionReason::kChangeTemperature:
      return "ChangeTemperature";
    case CompactionReason::kForcedBlobGC:
      return "ForcedBlobGC";
    case CompactionReason::kNumOfReasons:
      // fall through
    default:
      assert(false);
      return "Invalid";
  }
}

CompactionJob::CompactionJob(
    int job_id, Compaction* compaction, const ImmutableDBOptions& db_options,
    const MutableDBOptions& mutable_db_options, const FileOptions& file_options,
    VersionSet* versions, const std::atomic<bool>* shutting_down,
    LogBuffer* log_buffer, FSDirectory* db_directory,
    FSDirectory* output_directory, FSDirectory* blob_output_directory,
    Statistics* stats, InstrumentedMutex* db_mutex,
    ErrorHandler* db_error_handler,
    std::vector<SequenceNumber> existing_snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    const SnapshotChecker* snapshot_checker, JobContext* job_context,
    std::shared_ptr<Cache> table_cache, EventLogger* event_logger,
    bool paranoid_file_checks, bool measure_io_stats, const std::string& dbname,
    CompactionJobStats* compaction_job_stats, Env::Priority thread_pri,
    const std::shared_ptr<IOTracer>& io_tracer,
    const std::atomic<bool>& manual_compaction_canceled,
    const std::string& db_id, const std::string& db_session_id,
    std::string full_history_ts_low, std::string trim_ts,
    BlobFileCompletionCallback* blob_callback)
    : compact_(new CompactionState(compaction)),
      compaction_stats_(compaction->compaction_reason(), 1),
      db_options_(db_options),
      mutable_db_options_copy_(mutable_db_options),
      log_buffer_(log_buffer),
      output_directory_(output_directory),
      stats_(stats),
      bottommost_level_(false),
      write_hint_(Env::WLTH_NOT_SET),
      compaction_job_stats_(compaction_job_stats),
      job_id_(job_id),
      dbname_(dbname),
      db_id_(db_id),
      db_session_id_(db_session_id),
      file_options_(file_options),
      env_(db_options.env),
      io_tracer_(io_tracer),
      fs_(db_options.fs, io_tracer),
      file_options_for_read_(
          fs_->OptimizeForCompactionTableRead(file_options, db_options_)),
      versions_(versions),
      shutting_down_(shutting_down),
      manual_compaction_canceled_(manual_compaction_canceled),
      db_directory_(db_directory),
      blob_output_directory_(blob_output_directory),
      db_mutex_(db_mutex),
      db_error_handler_(db_error_handler),
      existing_snapshots_(std::move(existing_snapshots)),
      earliest_write_conflict_snapshot_(earliest_write_conflict_snapshot),
      snapshot_checker_(snapshot_checker),
      job_context_(job_context),
      table_cache_(std::move(table_cache)),
      event_logger_(event_logger),
      paranoid_file_checks_(paranoid_file_checks),
      measure_io_stats_(measure_io_stats),
      thread_pri_(thread_pri),
      full_history_ts_low_(std::move(full_history_ts_low)),
      trim_ts_(std::move(trim_ts)),
      blob_callback_(blob_callback) {
  assert(compaction_job_stats_ != nullptr);
  assert(log_buffer_ != nullptr);
  const auto* cfd = compact_->compaction->column_family_data();
  ThreadStatusUtil::SetColumnFamily(cfd, cfd->ioptions()->env,
                                    db_options_.enable_thread_tracking);
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_COMPACTION);
  ReportStartedCompaction(compaction);
}

CompactionJob::~CompactionJob() {
  assert(compact_ == nullptr);
  ThreadStatusUtil::ResetThreadStatus();
}

void CompactionJob::ReportStartedCompaction(Compaction* compaction) {
  const auto* cfd = compact_->compaction->column_family_data();
  ThreadStatusUtil::SetColumnFamily(cfd, cfd->ioptions()->env,
                                    db_options_.enable_thread_tracking);

  ThreadStatusUtil::SetThreadOperationProperty(ThreadStatus::COMPACTION_JOB_ID,
                                               job_id_);

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_INPUT_OUTPUT_LEVEL,
      (static_cast<uint64_t>(compact_->compaction->start_level()) << 32) +
          compact_->compaction->output_level());

  // In the current design, a CompactionJob is always created
  // for non-trivial compaction.
  assert(compaction->IsTrivialMove() == false ||
         compaction->is_manual_compaction() == true);

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_PROP_FLAGS,
      compaction->is_manual_compaction() +
          (compaction->deletion_compaction() << 1));

  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_TOTAL_INPUT_BYTES,
      compaction->CalculateTotalInputSize());

  IOSTATS_RESET(bytes_written);
  IOSTATS_RESET(bytes_read);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, 0);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, 0);

  // Set the thread operation after operation properties
  // to ensure GetThreadList() can always show them all together.
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_COMPACTION);

  compaction_job_stats_->is_manual_compaction =
      compaction->is_manual_compaction();
  compaction_job_stats_->is_full_compaction = compaction->is_full_compaction();
}

void CompactionJob::Prepare() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PREPARE);

  // Generate file_levels_ for compaction before making Iterator
  auto* c = compact_->compaction;
  assert(c->column_family_data() != nullptr);
  assert(c->column_family_data()->current()->storage_info()->NumLevelFiles(
             compact_->compaction->level()) > 0);

  write_hint_ =
      c->column_family_data()->CalculateSSTWriteHint(c->output_level());
  bottommost_level_ = c->bottommost_level();

  if (c->ShouldFormSubcompactions()) {
    {
      StopWatch sw(db_options_.clock, stats_, SUBCOMPACTION_SETUP_TIME);
      GenSubcompactionBoundaries();
    }

    for (size_t i = 0; i <= boundaries_.size(); i++) {
      Slice* start = i == 0 ? nullptr : &boundaries_[i - 1];
      Slice* end = i == boundaries_.size() ? nullptr : &boundaries_[i];
      compact_->sub_compact_states.emplace_back(c, start, end,
                                                static_cast<uint32_t>(i));
    }
    RecordInHistogram(stats_, NUM_SUBCOMPACTIONS_SCHEDULED,
                      compact_->sub_compact_states.size());
  } else {
    constexpr Slice* start = nullptr;
    constexpr Slice* end = nullptr;
    compact_->sub_compact_states.emplace_back(c, start, end, /*sub_job_id*/ 0);
  }
}

struct RangeWithSize {
  Range range;
  uint64_t size;

  RangeWithSize(const Slice& a, const Slice& b, uint64_t s = 0)
      : range(a, b), size(s) {}
};

void CompactionJob::GenSubcompactionBoundaries() {
  auto* c = compact_->compaction;
  auto* cfd = c->column_family_data();
  const Comparator* cfd_comparator = cfd->user_comparator();
  std::vector<Slice> bounds;
  int start_lvl = c->start_level();
  int out_lvl = c->output_level();

  // Add the starting and/or ending key of certain input files as a potential
  // boundary
  for (size_t lvl_idx = 0; lvl_idx < c->num_input_levels(); lvl_idx++) {
    int lvl = c->level(lvl_idx);
    if (lvl >= start_lvl && lvl <= out_lvl) {
      const LevelFilesBrief* flevel = c->input_levels(lvl_idx);
      size_t num_files = flevel->num_files;

      if (num_files == 0) {
        continue;
      }

      if (lvl == 0) {
        // For level 0 add the starting and ending key of each file since the
        // files may have greatly differing key ranges (not range-partitioned)
        for (size_t i = 0; i < num_files; i++) {
          bounds.emplace_back(flevel->files[i].smallest_key);
          bounds.emplace_back(flevel->files[i].largest_key);
        }
      } else {
        // For all other levels add the smallest/largest key in the level to
        // encompass the range covered by that level
        bounds.emplace_back(flevel->files[0].smallest_key);
        bounds.emplace_back(flevel->files[num_files - 1].largest_key);
        if (lvl == out_lvl) {
          // For the last level include the starting keys of all files since
          // the last level is the largest and probably has the widest key
          // range. Since it's range partitioned, the ending key of one file
          // and the starting key of the next are very close (or identical).
          for (size_t i = 1; i < num_files; i++) {
            bounds.emplace_back(flevel->files[i].smallest_key);
          }
        }
      }
    }
  }

  std::sort(bounds.begin(), bounds.end(),
            [cfd_comparator](const Slice& a, const Slice& b) -> bool {
              return cfd_comparator->Compare(ExtractUserKey(a),
                                             ExtractUserKey(b)) < 0;
            });
  // Remove duplicated entries from bounds
  bounds.erase(
      std::unique(bounds.begin(), bounds.end(),
                  [cfd_comparator](const Slice& a, const Slice& b) -> bool {
                    return cfd_comparator->Compare(ExtractUserKey(a),
                                                   ExtractUserKey(b)) == 0;
                  }),
      bounds.end());

  // Combine consecutive pairs of boundaries into ranges with an approximate
  // size of data covered by keys in that range
  uint64_t sum = 0;
  std::vector<RangeWithSize> ranges;
  // Get input version from CompactionState since it's already referenced
  // earlier in SetInputVersioCompaction::SetInputVersion and will not change
  // when db_mutex_ is released below
  auto* v = compact_->compaction->input_version();
  for (auto it = bounds.begin();;) {
    const Slice a = *it;
    ++it;

    if (it == bounds.end()) {
      break;
    }

    const Slice b = *it;

    // ApproximateSize could potentially create table reader iterator to seek
    // to the index block and may incur I/O cost in the process. Unlock db
    // mutex to reduce contention
    db_mutex_->Unlock();
    uint64_t size = versions_->ApproximateSize(SizeApproximationOptions(), v, a,
                                               b, start_lvl, out_lvl + 1,
                                               TableReaderCaller::kCompaction);
    db_mutex_->Lock();
    ranges.emplace_back(a, b, size);
    sum += size;
  }

  // Group the ranges into subcompactions
  const double min_file_fill_percent = 4.0 / 5;
  int base_level = v->storage_info()->base_level();
  uint64_t max_output_files = static_cast<uint64_t>(std::ceil(
      sum / min_file_fill_percent /
      MaxFileSizeForLevel(
          *(c->mutable_cf_options()), out_lvl,
          c->immutable_options()->compaction_style, base_level,
          c->immutable_options()->level_compaction_dynamic_level_bytes)));
  uint64_t subcompactions =
      std::min({static_cast<uint64_t>(ranges.size()),
                static_cast<uint64_t>(c->max_subcompactions()),
                max_output_files});

  if (subcompactions > 1) {
    double mean = sum * 1.0 / subcompactions;
    // Greedily add ranges to the subcompaction until the sum of the ranges'
    // sizes becomes >= the expected mean size of a subcompaction
    sum = 0;
    for (size_t i = 0; i + 1 < ranges.size(); i++) {
      sum += ranges[i].size;
      if (subcompactions == 1) {
        // If there's only one left to schedule then it goes to the end so no
        // need to put an end boundary
        continue;
      }
      if (sum >= mean) {
        boundaries_.emplace_back(ExtractUserKey(ranges[i].range.limit));
        subcompactions--;
        sum = 0;
      }
    }
  }
}

Status CompactionJob::Run() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_RUN);
  TEST_SYNC_POINT("CompactionJob::Run():Start");
  log_buffer_->FlushBufferToLog();
  LogCompaction();

  const size_t num_threads = compact_->sub_compact_states.size();
  assert(num_threads > 0);
  const uint64_t start_micros = db_options_.clock->NowMicros();

  // Launch a thread for each of subcompactions 1...num_threads-1
  std::vector<port::Thread> thread_pool;
  thread_pool.reserve(num_threads - 1);
  for (size_t i = 1; i < compact_->sub_compact_states.size(); i++) {
    thread_pool.emplace_back(&CompactionJob::ProcessKeyValueCompaction, this,
                             &compact_->sub_compact_states[i]);
  }

  // Always schedule the first subcompaction (whether or not there are also
  // others) in the current thread to be efficient with resources
  ProcessKeyValueCompaction(&compact_->sub_compact_states[0]);

  // Wait for all other threads (if there are any) to finish execution
  for (auto& thread : thread_pool) {
    thread.join();
  }

  compaction_stats_.SetMicros(db_options_.clock->NowMicros() - start_micros);

  for (auto& state : compact_->sub_compact_states) {
    compaction_stats_.AddCpuMicros(state.compaction_job_stats.cpu_micros);
    state.RemoveLastEmptyOutput();
  }

  RecordTimeToHistogram(stats_, COMPACTION_TIME,
                        compaction_stats_.stats.micros);
  RecordTimeToHistogram(stats_, COMPACTION_CPU_TIME,
                        compaction_stats_.stats.cpu_micros);

  TEST_SYNC_POINT("CompactionJob::Run:BeforeVerify");

  // Check if any thread encountered an error during execution
  Status status;
  IOStatus io_s;
  bool wrote_new_blob_files = false;

  for (const auto& state : compact_->sub_compact_states) {
    if (!state.status.ok()) {
      status = state.status;
      io_s = state.io_status;
      break;
    }

    if (state.Current().HasBlobFileAdditions()) {
      wrote_new_blob_files = true;
    }
  }

  if (io_status_.ok()) {
    io_status_ = io_s;
  }
  if (status.ok()) {
    constexpr IODebugContext* dbg = nullptr;

    if (output_directory_) {
      io_s = output_directory_->FsyncWithDirOptions(
          IOOptions(), dbg,
          DirFsyncOptions(DirFsyncOptions::FsyncReason::kNewFileSynced));
    }

    if (io_s.ok() && wrote_new_blob_files && blob_output_directory_ &&
        blob_output_directory_ != output_directory_) {
      io_s = blob_output_directory_->FsyncWithDirOptions(
          IOOptions(), dbg,
          DirFsyncOptions(DirFsyncOptions::FsyncReason::kNewFileSynced));
    }
  }
  if (io_status_.ok()) {
    io_status_ = io_s;
  }
  if (status.ok()) {
    status = io_s;
  }
  if (status.ok()) {
    thread_pool.clear();
    std::vector<const CompactionOutputs::Output*> files_output;
    for (const auto& state : compact_->sub_compact_states) {
      for (const auto& output : state.GetOutputs()) {
        files_output.emplace_back(&output);
      }
    }
    ColumnFamilyData* cfd = compact_->compaction->column_family_data();
    auto& prefix_extractor =
        compact_->compaction->mutable_cf_options()->prefix_extractor;
    std::atomic<size_t> next_file_idx(0);
    auto verify_table = [&](Status& output_status) {
      while (true) {
        size_t file_idx = next_file_idx.fetch_add(1);
        if (file_idx >= files_output.size()) {
          break;
        }
        // Verify that the table is usable
        // We set for_compaction to false and don't OptimizeForCompactionTableRead
        // here because this is a special case after we finish the table building
        // No matter whether use_direct_io_for_flush_and_compaction is true,
        // we will regard this verification as user reads since the goal is
        // to cache it here for further user reads
        ReadOptions read_options;
        InternalIterator* iter = cfd->table_cache()->NewIterator(
            read_options, file_options_, cfd->internal_comparator(),
            files_output[file_idx]->meta, /*range_del_agg=*/nullptr,
            prefix_extractor,
            /*table_reader_ptr=*/nullptr,
            cfd->internal_stats()->GetFileReadHist(
                compact_->compaction->output_level()),
            TableReaderCaller::kCompactionRefill, /*arena=*/nullptr,
            /*skip_filters=*/false, compact_->compaction->output_level(),
            MaxFileSizeForL0MetaPin(
                *compact_->compaction->mutable_cf_options()),
            /*smallest_compaction_key=*/nullptr,
            /*largest_compaction_key=*/nullptr,
            /*allow_unprepared_value=*/false);
        auto s = iter->status();

        if (s.ok() && paranoid_file_checks_) {
          OutputValidator validator(cfd->internal_comparator(),
                                    /*_enable_order_check=*/true,
                                    /*_enable_hash=*/true);
          for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
            s = validator.Add(iter->key(), iter->value());
            if (!s.ok()) {
              break;
            }
          }
          if (s.ok()) {
            s = iter->status();
          }
          if (s.ok() &&
              !validator.CompareValidator(files_output[file_idx]->validator)) {
            s = Status::Corruption("Paranoid checksums do not match");
          }
        }

        delete iter;

        if (!s.ok()) {
          output_status = s;
          break;
        }
      }
    };
    for (size_t i = 1; i < compact_->sub_compact_states.size(); i++) {
      thread_pool.emplace_back(verify_table,
                               std::ref(compact_->sub_compact_states[i].status));
    }
    verify_table(compact_->sub_compact_states[0].status);
    for (auto& thread : thread_pool) {
      thread.join();
    }
    for (const auto& state : compact_->sub_compact_states) {
      if (!state.status.ok()) {
        status = state.status;
        break;
      }
    }
  }

  TablePropertiesCollection tp;
  for (const auto& state : compact_->sub_compact_states) {
    for (const auto& output : state.GetOutputs()) {
      auto fn =
          TableFileName(state.compaction->immutable_options()->cf_paths,
                        output.meta.fd.GetNumber(), output.meta.fd.GetPathId());
      tp[fn] = output.table_properties;
    }
  }
  compact_->compaction->SetOutputTableProperties(std::move(tp));

  // Finish up all book-keeping to unify the subcompaction results
  compact_->AggregateCompactionStats(compaction_stats_, *compaction_job_stats_);
  UpdateCompactionStats();

  RecordCompactionIOStats();
  LogFlush(db_options_.info_log);
  TEST_SYNC_POINT("CompactionJob::Run():End");

  compact_->status = status;
  return status;
}

Status CompactionJob::Install(const MutableCFOptions& mutable_cf_options) {
  assert(compact_);

  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_INSTALL);
  db_mutex_->AssertHeld();
  Status status = compact_->status;

  ColumnFamilyData* cfd = compact_->compaction->column_family_data();
  assert(cfd);

  int output_level = compact_->compaction->output_level();
  cfd->internal_stats()->AddCompactionStats(output_level, thread_pri_,
                                            compaction_stats_);

  if (status.ok()) {
    status = InstallCompactionResults(mutable_cf_options);
  }
  if (!versions_->io_status().ok()) {
    io_status_ = versions_->io_status();
  }

  VersionStorageInfo::LevelSummaryStorage tmp;
  auto vstorage = cfd->current()->storage_info();
  const auto& stats = compaction_stats_.stats;

  double read_write_amp = 0.0;
  double write_amp = 0.0;
  double bytes_read_per_sec = 0;
  double bytes_written_per_sec = 0;

  const uint64_t bytes_read_non_output_and_blob =
      stats.bytes_read_non_output_levels + stats.bytes_read_blob;
  const uint64_t bytes_read_all =
      stats.bytes_read_output_level + bytes_read_non_output_and_blob;
  const uint64_t bytes_written_all =
      stats.bytes_written + stats.bytes_written_blob;

  if (bytes_read_non_output_and_blob > 0) {
    read_write_amp = (bytes_written_all + bytes_read_all) /
                     static_cast<double>(bytes_read_non_output_and_blob);
    write_amp =
        bytes_written_all / static_cast<double>(bytes_read_non_output_and_blob);
  }
  if (stats.micros > 0) {
    bytes_read_per_sec = bytes_read_all / static_cast<double>(stats.micros);
    bytes_written_per_sec =
        bytes_written_all / static_cast<double>(stats.micros);
  }

  const std::string& column_family_name = cfd->GetName();

  constexpr double kMB = 1048576.0;

  ROCKS_LOG_BUFFER(
      log_buffer_,
      "[%s] compacted to: %s, MB/sec: %.1f rd, %.1f wr, level %d, "
      "files in(%d, %d) out(%d +%d blob) "
      "MB in(%.1f, %.1f +%.1f blob) out(%.1f +%.1f blob), "
      "read-write-amplify(%.1f) write-amplify(%.1f) %s, records in: %" PRIu64
      ", records dropped: %" PRIu64 " output_compression: %s\n",
      column_family_name.c_str(), vstorage->LevelSummary(&tmp),
      bytes_read_per_sec, bytes_written_per_sec,
      compact_->compaction->output_level(),
      stats.num_input_files_in_non_output_levels,
      stats.num_input_files_in_output_level, stats.num_output_files,
      stats.num_output_files_blob, stats.bytes_read_non_output_levels / kMB,
      stats.bytes_read_output_level / kMB, stats.bytes_read_blob / kMB,
      stats.bytes_written / kMB, stats.bytes_written_blob / kMB, read_write_amp,
      write_amp, status.ToString().c_str(), stats.num_input_records,
      stats.num_dropped_records,
      CompressionTypeToString(compact_->compaction->output_compression())
          .c_str());

  const auto& blob_files = vstorage->GetBlobFiles();
  if (!blob_files.empty()) {
    assert(blob_files.front());
    assert(blob_files.back());

    ROCKS_LOG_BUFFER(
        log_buffer_,
        "[%s] Blob file summary: head=%" PRIu64 ", tail=%" PRIu64 "\n",
        column_family_name.c_str(), blob_files.front()->GetBlobFileNumber(),
        blob_files.back()->GetBlobFileNumber());
  }

  if (compaction_stats_.has_penultimate_level_output) {
    ROCKS_LOG_BUFFER(
        log_buffer_,
        "[%s] has Penultimate Level output: %" PRIu64
        ", level %d, number of files: %" PRIu64 ", number of records: %" PRIu64,
        column_family_name.c_str(),
        compaction_stats_.penultimate_level_stats.bytes_written,
        compact_->compaction->GetPenultimateLevel(),
        compaction_stats_.penultimate_level_stats.num_output_files,
        compaction_stats_.penultimate_level_stats.num_output_records);
  }

  UpdateCompactionJobStats(stats);

  auto stream = event_logger_->LogToBuffer(log_buffer_, 8192);
  stream << "job" << job_id_ << "event"
         << "compaction_finished"
         << "compaction_time_micros" << stats.micros
         << "compaction_time_cpu_micros" << stats.cpu_micros << "output_level"
         << compact_->compaction->output_level() << "num_output_files"
         << stats.num_output_files << "total_output_size"
         << stats.bytes_written;

  if (stats.num_output_files_blob > 0) {
    stream << "num_blob_output_files" << stats.num_output_files_blob
           << "total_blob_output_size" << stats.bytes_written_blob;
  }

  stream << "num_input_records" << stats.num_input_records
         << "num_output_records" << stats.num_output_records
         << "num_subcompactions" << compact_->sub_compact_states.size()
         << "output_compression"
         << CompressionTypeToString(compact_->compaction->output_compression());

  stream << "num_single_delete_mismatches"
         << compaction_job_stats_->num_single_del_mismatch;
  stream << "num_single_delete_fallthrough"
         << compaction_job_stats_->num_single_del_fallthru;

  if (measure_io_stats_) {
    stream << "file_write_nanos" << compaction_job_stats_->file_write_nanos;
    stream << "file_range_sync_nanos"
           << compaction_job_stats_->file_range_sync_nanos;
    stream << "file_fsync_nanos" << compaction_job_stats_->file_fsync_nanos;
    stream << "file_prepare_write_nanos"
           << compaction_job_stats_->file_prepare_write_nanos;
  }

  stream << "lsm_state";
  stream.StartArray();
  for (int level = 0; level < vstorage->num_levels(); ++level) {
    stream << vstorage->NumLevelFiles(level);
  }
  stream.EndArray();

  if (!blob_files.empty()) {
    assert(blob_files.front());
    stream << "blob_file_head" << blob_files.front()->GetBlobFileNumber();

    assert(blob_files.back());
    stream << "blob_file_tail" << blob_files.back()->GetBlobFileNumber();
  }

  if (compaction_stats_.has_penultimate_level_output) {
    InternalStats::CompactionStats& pl_stats =
        compaction_stats_.penultimate_level_stats;
    stream << "penultimate_level_num_output_files" << pl_stats.num_output_files;
    stream << "penultimate_level_bytes_written" << pl_stats.bytes_written;
    stream << "penultimate_level_num_output_records"
           << pl_stats.num_output_records;
    stream << "penultimate_level_num_output_files_blob"
           << pl_stats.num_output_files_blob;
    stream << "penultimate_level_bytes_written_blob"
           << pl_stats.bytes_written_blob;
  }

  CleanupCompaction();
  return status;
}

void CompactionJob::NotifyOnSubcompactionBegin(
    SubcompactionState* sub_compact) {
#ifndef ROCKSDB_LITE
  Compaction* c = compact_->compaction;

  if (db_options_.listeners.empty()) {
    return;
  }
  if (shutting_down_->load(std::memory_order_acquire)) {
    return;
  }
  if (c->is_manual_compaction() &&
      manual_compaction_canceled_.load(std::memory_order_acquire)) {
    return;
  }

  sub_compact->notify_on_subcompaction_completion = true;

  SubcompactionJobInfo info{};
  sub_compact->BuildSubcompactionJobInfo(info);
  info.job_id = static_cast<int>(job_id_);
  info.thread_id = env_->GetThreadID();

  for (const auto& listener : db_options_.listeners) {
    listener->OnSubcompactionBegin(info);
  }
  info.status.PermitUncheckedError();

#else
  (void)sub_compact;
#endif  // ROCKSDB_LITE
}

void CompactionJob::NotifyOnSubcompactionCompleted(
    SubcompactionState* sub_compact) {
#ifndef ROCKSDB_LITE

  if (db_options_.listeners.empty()) {
    return;
  }
  if (shutting_down_->load(std::memory_order_acquire)) {
    return;
  }

  if (sub_compact->notify_on_subcompaction_completion == false) {
    return;
  }

  SubcompactionJobInfo info{};
  sub_compact->BuildSubcompactionJobInfo(info);
  info.job_id = static_cast<int>(job_id_);
  info.thread_id = env_->GetThreadID();

  for (const auto& listener : db_options_.listeners) {
    listener->OnSubcompactionCompleted(info);
  }
#else
  (void)sub_compact;
#endif  // ROCKSDB_LITE
}

void CompactionJob::ProcessKeyValueCompaction(SubcompactionState* sub_compact) {
  assert(sub_compact);
  assert(sub_compact->compaction);

#ifndef ROCKSDB_LITE
  if (db_options_.compaction_service) {
    CompactionServiceJobStatus comp_status =
        ProcessKeyValueCompactionWithCompactionService(sub_compact);
    if (comp_status == CompactionServiceJobStatus::kSuccess ||
        comp_status == CompactionServiceJobStatus::kFailure) {
      return;
    }
    // fallback to local compaction
    assert(comp_status == CompactionServiceJobStatus::kUseLocal);
  }
#endif  // !ROCKSDB_LITE

  uint64_t prev_cpu_micros = db_options_.clock->CPUMicros();

  ColumnFamilyData* cfd = sub_compact->compaction->column_family_data();

  // Create compaction filter and fail the compaction if
  // IgnoreSnapshots() = false because it is not supported anymore
  const CompactionFilter* compaction_filter =
      cfd->ioptions()->compaction_filter;
  std::unique_ptr<CompactionFilter> compaction_filter_from_factory = nullptr;
  if (compaction_filter == nullptr) {
    compaction_filter_from_factory =
        sub_compact->compaction->CreateCompactionFilter();
    compaction_filter = compaction_filter_from_factory.get();
  }
  if (compaction_filter != nullptr && !compaction_filter->IgnoreSnapshots()) {
    sub_compact->status = Status::NotSupported(
        "CompactionFilter::IgnoreSnapshots() = false is not supported "
        "anymore.");
    return;
  }

  NotifyOnSubcompactionBegin(sub_compact);

  auto range_del_agg = std::make_unique<CompactionRangeDelAggregator>(
      &cfd->internal_comparator(), existing_snapshots_);

  // TODO: since we already use C++17, should use
  // std::optional<const Slice> instead.
  const Slice* const start = sub_compact->start;
  const Slice* const end = sub_compact->end;

  ReadOptions read_options;
  read_options.verify_checksums = true;
  read_options.fill_cache = false;
  read_options.rate_limiter_priority = GetRateLimiterPriority();
  // Compaction iterators shouldn't be confined to a single prefix.
  // Compactions use Seek() for
  // (a) concurrent compactions,
  // (b) CompactionFilter::Decision::kRemoveAndSkipUntil.
  read_options.total_order_seek = true;

  // Note: if we're going to support subcompactions for user-defined timestamps,
  // the timestamp part will have to be stripped from the bounds here.
  assert((!start && !end) || cfd->user_comparator()->timestamp_size() == 0);
  read_options.iterate_lower_bound = start;
  read_options.iterate_upper_bound = end;

  // Although the v2 aggregator is what the level iterator(s) know about,
  // the AddTombstones calls will be propagated down to the v1 aggregator.
  std::unique_ptr<InternalIterator> raw_input(versions_->MakeInputIterator(
      read_options, sub_compact->compaction, range_del_agg.get(),
      file_options_for_read_,
      (start == nullptr) ? std::optional<const Slice>{}
                         : std::optional<const Slice>{*start},
      (end == nullptr) ? std::optional<const Slice>{}
                       : std::optional<const Slice>{*end}));
  InternalIterator* input = raw_input.get();

  IterKey start_ikey;
  IterKey end_ikey;
  Slice start_slice;
  Slice end_slice;

  if (start) {
    start_ikey.SetInternalKey(*start, kMaxSequenceNumber, kValueTypeForSeek);
    start_slice = start_ikey.GetInternalKey();
  }
  if (end) {
    end_ikey.SetInternalKey(*end, kMaxSequenceNumber, kValueTypeForSeek);
    end_slice = end_ikey.GetInternalKey();
  }

  std::unique_ptr<InternalIterator> clip;
  if (start || end) {
    clip = std::make_unique<ClippingIterator>(
        raw_input.get(), start ? &start_slice : nullptr,
        end ? &end_slice : nullptr, &cfd->internal_comparator());
    input = clip.get();
  }

  std::unique_ptr<InternalIterator> blob_counter;

  if (sub_compact->compaction->DoesInputReferenceBlobFiles()) {
    BlobGarbageMeter* meter = sub_compact->Current().CreateBlobGarbageMeter();
    blob_counter = std::make_unique<BlobCountingIterator>(input, meter);
    input = blob_counter.get();
  }

  std::unique_ptr<InternalIterator> trim_history_iter;
  if (cfd->user_comparator()->timestamp_size() > 0 && !trim_ts_.empty()) {
    trim_history_iter = std::make_unique<HistoryTrimmingIterator>(
        input, cfd->user_comparator(), trim_ts_);
    input = trim_history_iter.get();
  }

  input->SeekToFirst();

  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PROCESS_KV);

  // I/O measurement variables
  PerfLevel prev_perf_level = PerfLevel::kEnableTime;
  const uint64_t kRecordStatsEvery = 1000;
  uint64_t prev_write_nanos = 0;
  uint64_t prev_fsync_nanos = 0;
  uint64_t prev_range_sync_nanos = 0;
  uint64_t prev_prepare_write_nanos = 0;
  uint64_t prev_cpu_write_nanos = 0;
  uint64_t prev_cpu_read_nanos = 0;
  if (measure_io_stats_) {
    prev_perf_level = GetPerfLevel();
    SetPerfLevel(PerfLevel::kEnableTimeAndCPUTimeExceptForMutex);
    prev_write_nanos = IOSTATS(write_nanos);
    prev_fsync_nanos = IOSTATS(fsync_nanos);
    prev_range_sync_nanos = IOSTATS(range_sync_nanos);
    prev_prepare_write_nanos = IOSTATS(prepare_write_nanos);
    prev_cpu_write_nanos = IOSTATS(cpu_write_nanos);
    prev_cpu_read_nanos = IOSTATS(cpu_read_nanos);
  }

  MergeHelper merge(
      env_, cfd->user_comparator(), cfd->ioptions()->merge_operator.get(),
      compaction_filter, db_options_.info_log.get(),
      false /* internal key corruption is expected */,
      existing_snapshots_.empty() ? 0 : existing_snapshots_.back(),
      snapshot_checker_, compact_->compaction->level(), db_options_.stats);

  const MutableCFOptions* mutable_cf_options =
      sub_compact->compaction->mutable_cf_options();
  assert(mutable_cf_options);

  std::vector<std::string> blob_file_paths;

  // TODO: BlobDB to support output_to_penultimate_level compaction, which needs
  //  2 builders, so may need to move to `CompactionOutputs`
  std::unique_ptr<BlobFileBuilder> blob_file_builder(
      (mutable_cf_options->enable_blob_files &&
       sub_compact->compaction->output_level() >=
           mutable_cf_options->blob_file_starting_level)
          ? new BlobFileBuilder(
                versions_, fs_.get(),
                sub_compact->compaction->immutable_options(),
                mutable_cf_options, &file_options_, job_id_, cfd->GetID(),
                cfd->GetName(), Env::IOPriority::IO_LOW, write_hint_,
                io_tracer_, blob_callback_, BlobFileCreationReason::kCompaction,
                &blob_file_paths,
                sub_compact->Current().GetBlobFileAdditionsPtr())
          : nullptr);

  TEST_SYNC_POINT("CompactionJob::Run():Inprogress");
  TEST_SYNC_POINT_CALLBACK(
      "CompactionJob::Run():PausingManualCompaction:1",
      reinterpret_cast<void*>(
          const_cast<std::atomic<bool>*>(&manual_compaction_canceled_)));

  const std::string* const full_history_ts_low =
      full_history_ts_low_.empty() ? nullptr : &full_history_ts_low_;
  const SequenceNumber job_snapshot_seq =
      job_context_ ? job_context_->GetJobSnapshotSequence()
                   : kMaxSequenceNumber;

  auto c_iter = std::make_unique<CompactionIterator>(
      input, cfd->user_comparator(), &merge, versions_->LastSequence(),
      &existing_snapshots_, earliest_write_conflict_snapshot_, job_snapshot_seq,
      snapshot_checker_, env_, ShouldReportDetailedTime(env_, stats_),
      /*expect_valid_internal_key=*/true, range_del_agg.get(),
      blob_file_builder.get(), db_options_.allow_data_in_errors,
      db_options_.enforce_single_del_contracts, manual_compaction_canceled_,
      sub_compact->compaction, compaction_filter, shutting_down_,
      db_options_.info_log, full_history_ts_low);
  c_iter->SeekToFirst();

  // Assign range delete aggregator to the target output level, which makes sure
  // it only output to single level
  sub_compact->AssignRangeDelAggregator(std::move(range_del_agg));

  if (c_iter->Valid() && sub_compact->compaction->output_level() != 0) {
    sub_compact->FillFilesToCutForTtl();
    // ShouldStopBefore() maintains state based on keys processed so far. The
    // compaction loop always calls it on the "next" key, thus won't tell it the
    // first key. So we do that here.
    sub_compact->ShouldStopBefore(c_iter->key());
  }
  const auto& c_iter_stats = c_iter->iter_stats();

  // define the open and close functions for the compaction files, which will be
  // used open/close output files when needed.
  const CompactionFileOpenFunc open_file_func =
      [this, sub_compact](CompactionOutputs& outputs) {
        return this->OpenCompactionOutputFile(sub_compact, outputs);
      };
  const CompactionFileCloseFunc close_file_func =
      [this, sub_compact](CompactionOutputs& outputs, const Status& status,
                          const Slice& next_table_min_key) {
        return this->FinishCompactionOutputFile(status, sub_compact, outputs,
                                                next_table_min_key);
      };

  Status status;
  while (status.ok() && !cfd->IsDropped() && c_iter->Valid()) {
    // Invariant: c_iter.status() is guaranteed to be OK if c_iter->Valid()
    // returns true.

    assert(!end ||
           cfd->user_comparator()->Compare(c_iter->user_key(), *end) < 0);

    if (c_iter_stats.num_input_records % kRecordStatsEvery ==
        kRecordStatsEvery - 1) {
      RecordDroppedKeys(c_iter_stats, &sub_compact->compaction_job_stats);
      c_iter->ResetRecordCounts();
      RecordCompactionIOStats();
    }

    // Add current compaction_iterator key to target compaction output, if the
    // output file needs to be close or open, it will call the `open_file_func`
    // and `close_file_func`.
    // TODO: it would be better to have the compaction file open/close moved
    // into `CompactionOutputs` which has the output file information.
    status = sub_compact->AddToOutput(*c_iter, open_file_func, close_file_func);
    if (!status.ok()) {
      break;
    }

    TEST_SYNC_POINT_CALLBACK(
        "CompactionJob::Run():PausingManualCompaction:2",
        reinterpret_cast<void*>(
            const_cast<std::atomic<bool>*>(&manual_compaction_canceled_)));
    c_iter->Next();
    if (c_iter->status().IsManualCompactionPaused()) {
      break;
    }

    // TODO: Support earlier file cut for the penultimate level files. Maybe by
    //  moving `ShouldStopBefore()` to `CompactionOutputs` class. Currently
    //  the penultimate level output is only cut when it reaches the size limit.
    if (!sub_compact->Current().IsPendingClose() &&
        sub_compact->compaction->output_level() != 0 &&
        !sub_compact->compaction->SupportsPerKeyPlacement() &&
        sub_compact->ShouldStopBefore(c_iter->key())) {
      sub_compact->Current().SetPendingClose();
    }
  }

  sub_compact->compaction_job_stats.num_blobs_read =
      c_iter_stats.num_blobs_read;
  sub_compact->compaction_job_stats.total_blob_bytes_read =
      c_iter_stats.total_blob_bytes_read;
  sub_compact->compaction_job_stats.num_input_deletion_records =
      c_iter_stats.num_input_deletion_records;
  sub_compact->compaction_job_stats.num_corrupt_keys =
      c_iter_stats.num_input_corrupt_records;
  sub_compact->compaction_job_stats.num_single_del_fallthru =
      c_iter_stats.num_single_del_fallthru;
  sub_compact->compaction_job_stats.num_single_del_mismatch =
      c_iter_stats.num_single_del_mismatch;
  sub_compact->compaction_job_stats.total_input_raw_key_bytes +=
      c_iter_stats.total_input_raw_key_bytes;
  sub_compact->compaction_job_stats.total_input_raw_value_bytes +=
      c_iter_stats.total_input_raw_value_bytes;

  RecordTick(stats_, FILTER_OPERATION_TOTAL_TIME,
             c_iter_stats.total_filter_time);

  if (c_iter_stats.num_blobs_relocated > 0) {
    RecordTick(stats_, BLOB_DB_GC_NUM_KEYS_RELOCATED,
               c_iter_stats.num_blobs_relocated);
  }
  if (c_iter_stats.total_blob_bytes_relocated > 0) {
    RecordTick(stats_, BLOB_DB_GC_BYTES_RELOCATED,
               c_iter_stats.total_blob_bytes_relocated);
  }

  RecordDroppedKeys(c_iter_stats, &sub_compact->compaction_job_stats);
  RecordCompactionIOStats();

  if (status.ok() && cfd->IsDropped()) {
    status =
        Status::ColumnFamilyDropped("Column family dropped during compaction");
  }
  if ((status.ok() || status.IsColumnFamilyDropped()) &&
      shutting_down_->load(std::memory_order_relaxed)) {
    status = Status::ShutdownInProgress("Database shutdown");
  }
  if ((status.ok() || status.IsColumnFamilyDropped()) &&
      (manual_compaction_canceled_.load(std::memory_order_relaxed))) {
    status = Status::Incomplete(Status::SubCode::kManualCompactionPaused);
  }
  if (status.ok()) {
    status = input->status();
  }
  if (status.ok()) {
    status = c_iter->status();
  }

  // Call FinishCompactionOutputFile() even if status is not ok: it needs to
  // close the output files. Open file function is also passed, in case there's
  // only range-dels, no file was opened, to save the range-dels, it need to
  // create a new output file.
  status = sub_compact->CloseCompactionFiles(status, open_file_func,
                                             close_file_func);

  if (blob_file_builder) {
    if (status.ok()) {
      status = blob_file_builder->Finish();
    } else {
      blob_file_builder->Abandon(status);
    }
    blob_file_builder.reset();
    sub_compact->Current().UpdateBlobStats();
  }

  sub_compact->compaction_job_stats.cpu_micros =
      db_options_.clock->CPUMicros() - prev_cpu_micros;

  if (measure_io_stats_) {
    sub_compact->compaction_job_stats.file_write_nanos +=
        IOSTATS(write_nanos) - prev_write_nanos;
    sub_compact->compaction_job_stats.file_fsync_nanos +=
        IOSTATS(fsync_nanos) - prev_fsync_nanos;
    sub_compact->compaction_job_stats.file_range_sync_nanos +=
        IOSTATS(range_sync_nanos) - prev_range_sync_nanos;
    sub_compact->compaction_job_stats.file_prepare_write_nanos +=
        IOSTATS(prepare_write_nanos) - prev_prepare_write_nanos;
    sub_compact->compaction_job_stats.cpu_micros -=
        (IOSTATS(cpu_write_nanos) - prev_cpu_write_nanos +
         IOSTATS(cpu_read_nanos) - prev_cpu_read_nanos) /
        1000;
    if (prev_perf_level != PerfLevel::kEnableTimeAndCPUTimeExceptForMutex) {
      SetPerfLevel(prev_perf_level);
    }
  }
#ifdef ROCKSDB_ASSERT_STATUS_CHECKED
  if (!status.ok()) {
    if (c_iter) {
      c_iter->status().PermitUncheckedError();
    }
    if (input) {
      input->status().PermitUncheckedError();
    }
  }
#endif  // ROCKSDB_ASSERT_STATUS_CHECKED

  blob_counter.reset();
  clip.reset();
  raw_input.reset();
  sub_compact->status = status;
  NotifyOnSubcompactionCompleted(sub_compact);
}

uint64_t CompactionJob::GetCompactionId(SubcompactionState* sub_compact) const {
  return (uint64_t)job_id_ << 32 | sub_compact->sub_job_id;
}

void CompactionJob::RecordDroppedKeys(
    const CompactionIterationStats& c_iter_stats,
    CompactionJobStats* compaction_job_stats) {
  if (c_iter_stats.num_record_drop_user > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_USER,
               c_iter_stats.num_record_drop_user);
  }
  if (c_iter_stats.num_record_drop_hidden > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_NEWER_ENTRY,
               c_iter_stats.num_record_drop_hidden);
    if (compaction_job_stats) {
      compaction_job_stats->num_records_replaced +=
          c_iter_stats.num_record_drop_hidden;
    }
  }
  if (c_iter_stats.num_record_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_OBSOLETE,
               c_iter_stats.num_record_drop_obsolete);
    if (compaction_job_stats) {
      compaction_job_stats->num_expired_deletion_records +=
          c_iter_stats.num_record_drop_obsolete;
    }
  }
  if (c_iter_stats.num_record_drop_range_del > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_RANGE_DEL,
               c_iter_stats.num_record_drop_range_del);
  }
  if (c_iter_stats.num_range_del_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_RANGE_DEL_DROP_OBSOLETE,
               c_iter_stats.num_range_del_drop_obsolete);
  }
  if (c_iter_stats.num_optimized_del_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_OPTIMIZED_DEL_DROP_OBSOLETE,
               c_iter_stats.num_optimized_del_drop_obsolete);
  }
}

Status CompactionJob::FinishCompactionOutputFile(
    const Status& input_status, SubcompactionState* sub_compact,
    CompactionOutputs& outputs, const Slice& next_table_min_key) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_SYNC_FILE);
  assert(sub_compact != nullptr);
  assert(outputs.HasBuilder());

  FileMetaData* meta = outputs.GetMetaData();
  uint64_t output_number = meta->fd.GetNumber();
  assert(output_number != 0);

  ColumnFamilyData* cfd = sub_compact->compaction->column_family_data();
  std::string file_checksum = kUnknownFileChecksum;
  std::string file_checksum_func_name = kUnknownFileChecksumFuncName;

  // Check for iterator errors
  Status s = input_status;

  // Add range tombstones
  auto earliest_snapshot = kMaxSequenceNumber;
  if (existing_snapshots_.size() > 0) {
    earliest_snapshot = existing_snapshots_[0];
  }
  if (s.ok()) {
    CompactionIterationStats range_del_out_stats;
    // if the compaction supports per_key_placement, only output range dels to
    // the penultimate level.
    // Note: Use `bottommost_level_ = true` for both bottommost and
    // output_to_penultimate_level compaction here, as it's only used to decide
    // if range dels could be dropped.
    if (outputs.HasRangeDel()) {
      s = outputs.AddRangeDels(sub_compact->start, sub_compact->end,
                               range_del_out_stats, bottommost_level_,
                               cfd->internal_comparator(), earliest_snapshot,
                               next_table_min_key);
    }
    RecordDroppedKeys(range_del_out_stats, &sub_compact->compaction_job_stats);
    TEST_SYNC_POINT("CompactionJob::FinishCompactionOutputFile1");
  }

  const uint64_t current_entries = outputs.NumEntries();

  //Get Level
  const int file_output_level = write_hint_ - 3;
  //Level 0 and highest level not allowed
  if(file_output_level > 0 && file_output_level < compact_->compaction->GetVersionStorageInfo()->num_non_empty_levels() - 1){    
    //Get OverlapSize
    std::vector<FileMetaData*> overlapfiles;
    
    compact_->compaction->GetVersionStorageInfo()->GetOverlappingInputsRangeBinarySearch(file_output_level + 1, 
      &(meta->smallest), &(meta->largest), &overlapfiles);
    uint64_t overlapping_bytes = 0;
    for(auto ofile: overlapfiles){
      overlapping_bytes += ofile->fd.file_size;
    }
    if(overlapping_bytes != 0){
      //Get file Score
      if(outputs.builder_->FileSize() != 0){
        uint64_t file_score = overlapping_bytes * 1024U / outputs.builder_->FileSize() + 50000;
        //Get cmp index
        const int cmp_index  = compact_->compaction->GetVersionStorageInfo()->NextCompactionIndex(file_output_level);
        const std::vector<uint64_t>& file_scores = compact_->compaction->GetVersionStorageInfo()->ScoresByCompactionPri(file_output_level);
        auto files_pri = compact_->compaction->GetVersionStorageInfo()->FilesByCompactionPri(file_output_level);
        //Get file in cmp index
        int file_index = std::lower_bound(file_scores.begin(), file_scores.end(), file_score) - file_scores.begin();
        int priority_index = std::max(file_index - cmp_index, 0);
        //Get Level Score
        SPDLOG_INFO("filepriority {} {} {} {} {} {} {}", 
          meta->fd.GetNumber(), 
          compact_->compaction->GetVersionStorageInfo()->CompactionLevelScore(file_output_level),
          compact_->compaction->GetVersionStorageInfo()->CompactionLevelScore(file_output_level+1), 
          priority_index, 
          file_score, 
          outputs.builder_->FileSize(), 
          overlapfiles.size());

        SPDLOG_INFO("fileidlast {} lastscore {}", meta->fd.GetNumber(), file_scores.back());

        SPDLOG_INFO("fileidrange {}", meta->fd.GetNumber());  
        //解决overlap sst变化问题
        for(auto ofile: overlapfiles){
          SPDLOG_INFO("fileid {} overlapfile {}", meta->fd.GetNumber(), ofile->fd.GetNumber());
        }
        //outputs.file_writer_->writable_file()->SetWriteLifeTimeHint()
      }else{
        SPDLOG_INFO("error {} {}", meta->fd.GetNumber(), outputs.builder_->FileSize());
      }
    }else{
        SPDLOG_INFO("filepriority {} {} {} {} {} {} {}", 
        meta->fd.GetNumber(), 
        compact_->compaction->GetVersionStorageInfo()->CompactionLevelScore(file_output_level),
        compact_->compaction->GetVersionStorageInfo()->CompactionLevelScore(file_output_level+1), 
        0, 
        0, 
        outputs.builder_->FileSize(), 
        0);
    }

  }
  s = outputs.Finish(s);

  if (s.ok()) {
    // With accurate smallest and largest key, we can get a slightly more
    // accurate oldest ancester time.
    // This makes oldest ancester time in manifest more accurate than in
    // table properties. Not sure how to resolve it.
    if (meta->smallest.size() > 0 && meta->largest.size() > 0) {
      uint64_t refined_oldest_ancester_time;
      Slice new_smallest = meta->smallest.user_key();
      Slice new_largest = meta->largest.user_key();
      if (!new_largest.empty() && !new_smallest.empty()) {
        refined_oldest_ancester_time =
            sub_compact->compaction->MinInputFileOldestAncesterTime(
                &(meta->smallest), &(meta->largest));
        if (refined_oldest_ancester_time !=
            std::numeric_limits<uint64_t>::max()) {
          meta->oldest_ancester_time = refined_oldest_ancester_time;
        }
      }
    }
  }

  // Finish and check for file errors
  IOStatus io_s = outputs.WriterSyncClose(s, db_options_.clock, stats_,
                                          db_options_.use_fsync);

  if (s.ok() && io_s.ok()) {
    file_checksum = meta->file_checksum;
    file_checksum_func_name = meta->file_checksum_func_name;
  }

  if (s.ok()) {
    s = io_s;
  }
  if (sub_compact->io_status.ok()) {
    sub_compact->io_status = io_s;
    // Since this error is really a copy of the
    // "normal" status, it does not also need to be checked
    sub_compact->io_status.PermitUncheckedError();
  }

  TableProperties tp;
  if (s.ok()) {
    tp = outputs.GetTableProperties();
  }

  if (s.ok() && current_entries == 0 && tp.num_range_deletions == 0) {
    // If there is nothing to output, no necessary to generate a sst file.
    // This happens when the output level is bottom level, at the same time
    // the sub_compact output nothing.
    std::string fname =
        TableFileName(sub_compact->compaction->immutable_options()->cf_paths,
                      meta->fd.GetNumber(), meta->fd.GetPathId());

    // TODO(AR) it is not clear if there are any larger implications if
    // DeleteFile fails here
    Status ds = env_->DeleteFile(fname);
    if (!ds.ok()) {
      ROCKS_LOG_WARN(
          db_options_.info_log,
          "[%s] [JOB %d] Unable to remove SST file for table #%" PRIu64
          " at bottom level%s",
          cfd->GetName().c_str(), job_id_, output_number,
          meta->marked_for_compaction ? " (need compaction)" : "");
    }

    // Also need to remove the file from outputs, or it will be added to the
    // VersionEdit.
    outputs.RemoveLastOutput();
    meta = nullptr;
  }

  if (s.ok() && (current_entries > 0 || tp.num_range_deletions > 0)) {
    // Output to event logger and fire events.
    outputs.UpdateTableProperties();
    ROCKS_LOG_INFO(db_options_.info_log,
                   "[%s] [JOB %d] Generated table #%" PRIu64 ": %" PRIu64
                   " keys, %" PRIu64 " bytes%s, temperature: %s",
                   cfd->GetName().c_str(), job_id_, output_number,
                   current_entries, meta->fd.file_size,
                   meta->marked_for_compaction ? " (need compaction)" : "",
                   temperature_to_string[meta->temperature].c_str());
  }
  std::string fname;
  FileDescriptor output_fd;
  uint64_t oldest_blob_file_number = kInvalidBlobFileNumber;
  Status status_for_listener = s;
  if (meta != nullptr) {
    fname = GetTableFileName(meta->fd.GetNumber());
    output_fd = meta->fd;
    oldest_blob_file_number = meta->oldest_blob_file_number;
  } else {
    fname = "(nil)";
    if (s.ok()) {
      status_for_listener = Status::Aborted("Empty SST file not kept");
    }
  }
  EventHelpers::LogAndNotifyTableFileCreationFinished(
      event_logger_, cfd->ioptions()->listeners, dbname_, cfd->GetName(), fname,
      job_id_, output_fd, oldest_blob_file_number, tp,
      TableFileCreationReason::kCompaction, status_for_listener, file_checksum,
      file_checksum_func_name);

#ifndef ROCKSDB_LITE
  // Report new file to SstFileManagerImpl
  auto sfm =
      static_cast<SstFileManagerImpl*>(db_options_.sst_file_manager.get());
  if (sfm && meta != nullptr && meta->fd.GetPathId() == 0) {
    Status add_s = sfm->OnAddFile(fname);
    if (!add_s.ok() && s.ok()) {
      s = add_s;
    }
    if (sfm->IsMaxAllowedSpaceReached()) {
      // TODO(ajkr): should we return OK() if max space was reached by the final
      // compaction output file (similarly to how flush works when full)?
      s = Status::SpaceLimit("Max allowed space was reached");
      TEST_SYNC_POINT(
          "CompactionJob::FinishCompactionOutputFile:MaxAllowedSpaceReached");
      InstrumentedMutexLock l(db_mutex_);
      db_error_handler_->SetBGError(s, BackgroundErrorReason::kCompaction);
    }
  }
#endif

  outputs.ResetBuilder();
  return s;
}

Status CompactionJob::InstallCompactionResults(
    const MutableCFOptions& mutable_cf_options) {
  assert(compact_);

  db_mutex_->AssertHeld();

  auto* compaction = compact_->compaction;
  assert(compaction);

  {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    if (compaction_stats_.has_penultimate_level_output) {
      ROCKS_LOG_BUFFER(
          log_buffer_,
          "[%s] [JOB %d] Compacted %s => output_to_penultimate_level: %" PRIu64
          " bytes + last: %" PRIu64 " bytes. Total: %" PRIu64 " bytes",
          compaction->column_family_data()->GetName().c_str(), job_id_,
          compaction->InputLevelSummary(&inputs_summary),
          compaction_stats_.penultimate_level_stats.bytes_written,
          compaction_stats_.stats.bytes_written,
          compaction_stats_.TotalBytesWritten());
    } else {
      ROCKS_LOG_BUFFER(log_buffer_,
                       "[%s] [JOB %d] Compacted %s => %" PRIu64 " bytes",
                       compaction->column_family_data()->GetName().c_str(),
                       job_id_, compaction->InputLevelSummary(&inputs_summary),
                       compaction_stats_.TotalBytesWritten());
    }
  }

  VersionEdit* const edit = compaction->edit();
  assert(edit);

  // Add compaction inputs
  compaction->AddInputDeletions(edit);

  std::unordered_map<uint64_t, BlobGarbageMeter::BlobStats> blob_total_garbage;

  for (const auto& sub_compact : compact_->sub_compact_states) {
    sub_compact.AddOutputsEdit(edit);

    for (const auto& blob : sub_compact.Current().GetBlobFileAdditions()) {
      edit->AddBlobFile(blob);
    }

    if (sub_compact.Current().GetBlobGarbageMeter()) {
      const auto& flows = sub_compact.Current().GetBlobGarbageMeter()->flows();

      for (const auto& pair : flows) {
        const uint64_t blob_file_number = pair.first;
        const BlobGarbageMeter::BlobInOutFlow& flow = pair.second;

        assert(flow.IsValid());
        if (flow.HasGarbage()) {
          blob_total_garbage[blob_file_number].Add(flow.GetGarbageCount(),
                                                   flow.GetGarbageBytes());
        }
      }
    }
  }

  for (const auto& pair : blob_total_garbage) {
    const uint64_t blob_file_number = pair.first;
    const BlobGarbageMeter::BlobStats& stats = pair.second;

    edit->AddBlobFileGarbage(blob_file_number, stats.GetCount(),
                             stats.GetBytes());
  }

  if (compaction->compaction_reason() == CompactionReason::kLevelMaxLevelSize &&
      compaction->immutable_options()->compaction_pri == kRoundRobin) {
    int start_level = compaction->start_level();
    if (start_level > 0) {
      auto vstorage = compaction->input_version()->storage_info();
      edit->AddCompactCursor(start_level,
                             vstorage->GetNextCompactCursor(start_level));
    }
  }

  return versions_->LogAndApply(compaction->column_family_data(),
                                mutable_cf_options, edit, db_mutex_,
                                db_directory_);
}

void CompactionJob::RecordCompactionIOStats() {
  RecordTick(stats_, COMPACT_READ_BYTES, IOSTATS(bytes_read));
  RecordTick(stats_, COMPACT_WRITE_BYTES, IOSTATS(bytes_written));
  CompactionReason compaction_reason =
      compact_->compaction->compaction_reason();
  if (compaction_reason == CompactionReason::kFilesMarkedForCompaction) {
    RecordTick(stats_, COMPACT_READ_BYTES_MARKED, IOSTATS(bytes_read));
    RecordTick(stats_, COMPACT_WRITE_BYTES_MARKED, IOSTATS(bytes_written));
  } else if (compaction_reason == CompactionReason::kPeriodicCompaction) {
    RecordTick(stats_, COMPACT_READ_BYTES_PERIODIC, IOSTATS(bytes_read));
    RecordTick(stats_, COMPACT_WRITE_BYTES_PERIODIC, IOSTATS(bytes_written));
  } else if (compaction_reason == CompactionReason::kTtl) {
    RecordTick(stats_, COMPACT_READ_BYTES_TTL, IOSTATS(bytes_read));
    RecordTick(stats_, COMPACT_WRITE_BYTES_TTL, IOSTATS(bytes_written));
  }
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, IOSTATS(bytes_read));
  IOSTATS_RESET(bytes_read);
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}

Status CompactionJob::OpenCompactionOutputFile(SubcompactionState* sub_compact,
                                               CompactionOutputs& outputs) {
  assert(sub_compact != nullptr);

  // no need to lock because VersionSet::next_file_number_ is atomic
  uint64_t file_number = versions_->NewFileNumber();
  std::string fname = GetTableFileName(file_number);
  // Fire events.
  ColumnFamilyData* cfd = sub_compact->compaction->column_family_data();
#ifndef ROCKSDB_LITE
  EventHelpers::NotifyTableFileCreationStarted(
      cfd->ioptions()->listeners, dbname_, cfd->GetName(), fname, job_id_,
      TableFileCreationReason::kCompaction);
#endif  // !ROCKSDB_LITE
  // Make the output file
  std::unique_ptr<FSWritableFile> writable_file;
#ifndef NDEBUG
  bool syncpoint_arg = file_options_.use_direct_writes;
  TEST_SYNC_POINT_CALLBACK("CompactionJob::OpenCompactionOutputFile",
                           &syncpoint_arg);
#endif

  // Pass temperature of botommost files to FileSystem.
  FileOptions fo_copy = file_options_;
  Temperature temperature = sub_compact->compaction->output_temperature();
  if (temperature == Temperature::kUnknown && bottommost_level_ &&
      !sub_compact->IsCurrentPenultimateLevel()) {
    temperature =
        sub_compact->compaction->mutable_cf_options()->bottommost_temperature;
  }
  fo_copy.temperature = temperature;

  Status s;
  IOStatus io_s = NewWritableFile(fs_.get(), fname, &writable_file, fo_copy);
  s = io_s;
  if (sub_compact->io_status.ok()) {
    sub_compact->io_status = io_s;
    // Since this error is really a copy of the io_s that is checked below as s,
    // it does not also need to be checked.
    sub_compact->io_status.PermitUncheckedError();
  }
  if (!s.ok()) {
    ROCKS_LOG_ERROR(
        db_options_.info_log,
        "[%s] [JOB %d] OpenCompactionOutputFiles for table #%" PRIu64
        " fails at NewWritableFile with status %s",
        sub_compact->compaction->column_family_data()->GetName().c_str(),
        job_id_, file_number, s.ToString().c_str());
    LogFlush(db_options_.info_log);
    EventHelpers::LogAndNotifyTableFileCreationFinished(
        event_logger_, cfd->ioptions()->listeners, dbname_, cfd->GetName(),
        fname, job_id_, FileDescriptor(), kInvalidBlobFileNumber,
        TableProperties(), TableFileCreationReason::kCompaction, s,
        kUnknownFileChecksum, kUnknownFileChecksumFuncName);
    return s;
  }

  // Try to figure out the output file's oldest ancester time.
  int64_t temp_current_time = 0;
  auto get_time_status = db_options_.clock->GetCurrentTime(&temp_current_time);
  // Safe to proceed even if GetCurrentTime fails. So, log and proceed.
  if (!get_time_status.ok()) {
    ROCKS_LOG_WARN(db_options_.info_log,
                   "Failed to get current time. Status: %s",
                   get_time_status.ToString().c_str());
  }
  uint64_t current_time = static_cast<uint64_t>(temp_current_time);
  InternalKey tmp_start, tmp_end;
  if (sub_compact->start != nullptr) {
    tmp_start.SetMinPossibleForUserKey(*(sub_compact->start));
  }
  if (sub_compact->end != nullptr) {
    tmp_end.SetMinPossibleForUserKey(*(sub_compact->end));
  }
  uint64_t oldest_ancester_time =
      sub_compact->compaction->MinInputFileOldestAncesterTime(
          (sub_compact->start != nullptr) ? &tmp_start : nullptr,
          (sub_compact->end != nullptr) ? &tmp_end : nullptr);
  if (oldest_ancester_time == std::numeric_limits<uint64_t>::max()) {
    oldest_ancester_time = current_time;
  }

  // Initialize a SubcompactionState::Output and add it to sub_compact->outputs
  {
    FileMetaData meta;
    meta.fd = FileDescriptor(file_number,
                             sub_compact->compaction->output_path_id(), 0);
    meta.oldest_ancester_time = oldest_ancester_time;
    meta.file_creation_time = current_time;
    meta.temperature = temperature;
    assert(!db_id_.empty());
    assert(!db_session_id_.empty());
    s = GetSstInternalUniqueId(db_id_, db_session_id_, meta.fd.GetNumber(),
                               &meta.unique_id);
    if (!s.ok()) {
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "[%s] [JOB %d] file #%" PRIu64
                      " failed to generate unique id: %s.",
                      cfd->GetName().c_str(), job_id_, meta.fd.GetNumber(),
                      s.ToString().c_str());
      return s;
    }

    outputs.AddOutput(std::move(meta), cfd->internal_comparator(),
                      sub_compact->compaction->mutable_cf_options()
                          ->check_flush_compaction_key_order,
                      paranoid_file_checks_);
  }

  writable_file->SetIOPriority(GetRateLimiterPriority());
  writable_file->SetWriteLifeTimeHint(write_hint_);
  FileTypeSet tmp_set = db_options_.checksum_handoff_file_types;
  writable_file->SetPreallocationBlockSize(static_cast<size_t>(
      sub_compact->compaction->OutputFilePreallocationSize()));
  const auto& listeners =
      sub_compact->compaction->immutable_options()->listeners;
  outputs.AssignFileWriter(new WritableFileWriter(
      std::move(writable_file), fname, fo_copy, db_options_.clock, io_tracer_,
      db_options_.stats, listeners, db_options_.file_checksum_gen_factory.get(),
      tmp_set.Contains(FileType::kTableFile), false));

  TableBuilderOptions tboptions(
      *cfd->ioptions(), *(sub_compact->compaction->mutable_cf_options()),
      cfd->internal_comparator(), cfd->int_tbl_prop_collector_factories(),
      sub_compact->compaction->output_compression(),
      sub_compact->compaction->output_compression_opts(), cfd->GetID(),
      cfd->GetName(), sub_compact->compaction->output_level(),
      bottommost_level_, TableFileCreationReason::kCompaction,
      oldest_ancester_time, 0 /* oldest_key_time */, current_time, db_id_,
      db_session_id_, sub_compact->compaction->max_output_file_size(),
      file_number);

  outputs.NewBuilder(tboptions);

  LogFlush(db_options_.info_log);
  return s;
}

void CompactionJob::CleanupCompaction() {
  for (SubcompactionState& sub_compact : compact_->sub_compact_states) {
    sub_compact.Cleanup(table_cache_.get());
  }
  delete compact_;
  compact_ = nullptr;
}

#ifndef ROCKSDB_LITE
namespace {
void CopyPrefix(const Slice& src, size_t prefix_length, std::string* dst) {
  assert(prefix_length > 0);
  size_t length = src.size() > prefix_length ? prefix_length : src.size();
  dst->assign(src.data(), length);
}
}  // namespace

#endif  // !ROCKSDB_LITE

void CompactionJob::UpdateCompactionStats() {
  assert(compact_);

  Compaction* compaction = compact_->compaction;
  compaction_stats_.stats.num_input_files_in_non_output_levels = 0;
  compaction_stats_.stats.num_input_files_in_output_level = 0;
  for (int input_level = 0;
       input_level < static_cast<int>(compaction->num_input_levels());
       ++input_level) {
    if (compaction->level(input_level) != compaction->output_level()) {
      UpdateCompactionInputStatsHelper(
          &compaction_stats_.stats.num_input_files_in_non_output_levels,
          &compaction_stats_.stats.bytes_read_non_output_levels, input_level);
    } else {
      UpdateCompactionInputStatsHelper(
          &compaction_stats_.stats.num_input_files_in_output_level,
          &compaction_stats_.stats.bytes_read_output_level, input_level);
    }
  }

  assert(compaction_job_stats_);
  compaction_stats_.stats.bytes_read_blob =
      compaction_job_stats_->total_blob_bytes_read;

  compaction_stats_.stats.num_dropped_records =
      compaction_stats_.DroppedRecords();
}

void CompactionJob::UpdateCompactionInputStatsHelper(int* num_files,
                                                     uint64_t* bytes_read,
                                                     int input_level) {
  const Compaction* compaction = compact_->compaction;
  auto num_input_files = compaction->num_input_files(input_level);
  *num_files += static_cast<int>(num_input_files);

  for (size_t i = 0; i < num_input_files; ++i) {
    const auto* file_meta = compaction->input(input_level, i);
    *bytes_read += file_meta->fd.GetFileSize();
    compaction_stats_.stats.num_input_records +=
        static_cast<uint64_t>(file_meta->num_entries);
  }
}

void CompactionJob::UpdateCompactionJobStats(
    const InternalStats::CompactionStats& stats) const {
#ifndef ROCKSDB_LITE
  compaction_job_stats_->elapsed_micros = stats.micros;

  // input information
  compaction_job_stats_->total_input_bytes =
      stats.bytes_read_non_output_levels + stats.bytes_read_output_level;
  compaction_job_stats_->num_input_records = stats.num_input_records;
  compaction_job_stats_->num_input_files =
      stats.num_input_files_in_non_output_levels +
      stats.num_input_files_in_output_level;
  compaction_job_stats_->num_input_files_at_output_level =
      stats.num_input_files_in_output_level;

  // output information
  compaction_job_stats_->total_output_bytes = stats.bytes_written;
  compaction_job_stats_->total_output_bytes_blob = stats.bytes_written_blob;
  compaction_job_stats_->num_output_records = stats.num_output_records;
  compaction_job_stats_->num_output_files = stats.num_output_files;
  compaction_job_stats_->num_output_files_blob = stats.num_output_files_blob;

  if (stats.num_output_files > 0) {
    CopyPrefix(compact_->SmallestUserKey(),
               CompactionJobStats::kMaxPrefixLength,
               &compaction_job_stats_->smallest_output_key_prefix);
    CopyPrefix(compact_->LargestUserKey(), CompactionJobStats::kMaxPrefixLength,
               &compaction_job_stats_->largest_output_key_prefix);
  }
#else
  (void)stats;
#endif  // !ROCKSDB_LITE
}

void CompactionJob::LogCompaction() {
  Compaction* compaction = compact_->compaction;
  ColumnFamilyData* cfd = compaction->column_family_data();

  // Let's check if anything will get logged. Don't prepare all the info if
  // we're not logging
  if (db_options_.info_log_level <= InfoLogLevel::INFO_LEVEL) {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    ROCKS_LOG_INFO(
        db_options_.info_log, "[%s] [JOB %d] Compacting %s, score %.2f",
        cfd->GetName().c_str(), job_id_,
        compaction->InputLevelSummary(&inputs_summary), compaction->score());
    char scratch[2345];
    compaction->Summary(scratch, sizeof(scratch));
    ROCKS_LOG_INFO(db_options_.info_log, "[%s] Compaction start summary: %s\n",
                   cfd->GetName().c_str(), scratch);
    // build event logger report
    auto stream = event_logger_->Log();
    stream << "job" << job_id_ << "event"
           << "compaction_started"
           << "compaction_reason"
           << GetCompactionReasonString(compaction->compaction_reason());
    for (size_t i = 0; i < compaction->num_input_levels(); ++i) {
      stream << ("files_L" + std::to_string(compaction->level(i)));
      stream.StartArray();
      for (auto f : *compaction->inputs(i)) {
        stream << f->fd.GetNumber();
      }
      stream.EndArray();
    }
    stream << "score" << compaction->score() << "input_data_size"
           << compaction->CalculateTotalInputSize();
  }
}

std::string CompactionJob::GetTableFileName(uint64_t file_number) {
  return TableFileName(compact_->compaction->immutable_options()->cf_paths,
                       file_number, compact_->compaction->output_path_id());
}

Env::IOPriority CompactionJob::GetRateLimiterPriority() {
  if (versions_ && versions_->GetColumnFamilySet() &&
      versions_->GetColumnFamilySet()->write_controller()) {
    WriteController* write_controller =
        versions_->GetColumnFamilySet()->write_controller();
    if (write_controller->NeedsDelay() || write_controller->IsStopped()) {
      return Env::IO_USER;
    }
  }

  return Env::IO_LOW;
}

}  // namespace ROCKSDB_NAMESPACE
