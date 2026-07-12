/*
 * PostGraph
 * Copyright (C) 2026 by PostGraph
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/storage_xlog.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "utils/rel.h"
#include "catalog/storage.h"

#include "access/np_mutable.h"

static const TupleTableSlotOps *np_slot_callbacks(Relation rel);
static void np_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                    int options, BulkInsertState bistate);
static TM_Result np_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                            CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                                            bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                                            TU_UpdateIndexes *update_indexes);
static bool np_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot);
static void np_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);
static bool np_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
static bool np_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot);
static TableScanDesc np_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags);
static bool np_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static void np_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode);
static void np_scan_end(TableScanDesc sscan);
static IndexFetchTableData *np_index_fetch_begin(Relation rel);
static void np_index_fetch_reset(IndexFetchTableData *scan);
static void np_index_fetch_end(IndexFetchTableData *scan);
static bool np_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead);
static bool np_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream);
static bool np_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot);
static bool np_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated);
static bool np_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate);
static bool np_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot);
static Size np_parallelscan_estimate(Relation rel);
static Size np_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
static void np_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
static bool np_relation_needs_toast_table(Relation rel);
static Oid np_relation_toast_am(Relation rel);
static void np_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result);
static void np_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy);
static void np_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti);
static void np_relation_nontransactional_truncate(Relation rel);
static void np_relation_copy_data(Relation rel, const RelFileLocator *newrlocator);
static void np_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead);
static double np_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan);
static void np_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state);
static TM_Result np_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart);
static void np_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken);
static void np_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded);
static TM_Result np_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd);
static void np_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate);
static void np_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid);
static bool np_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static TransactionId np_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate);
static void np_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac);



/*
 * np_slot_callbacks - Provide the Executor with memory operations
 *
 * We bypass the private heapam function and directly return the 
 * globally exported Buffer Tuple operations struct.
 */
static const TupleTableSlotOps *
np_slot_callbacks(Relation rel)
{
    return &TTSOpsBufferHeapTuple;
}


static void np_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                       int options, BulkInsertState bistate) {
    bool shouldFree = true;
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
    slot->tts_tableOid = RelationGetRelid(relation);
    tuple->t_tableOid = slot->tts_tableOid;
    
    np_heap_insert(relation, tuple, cid, options, bistate);
    
    ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
    if (shouldFree)
        pfree(tuple);
}

static TM_Result
np_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                           CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                           bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                           TU_UpdateIndexes *update_indexes)
{
    bool shouldFree = true;
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
    slot->tts_tableOid = RelationGetRelid(relation);
    tuple->t_tableOid = slot->tts_tableOid;
    
    np_update_inplace(relation, otid, tuple, cid);
    
    ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
    *update_indexes = TU_None;
    if (shouldFree)
        pfree(tuple);
    return TM_Ok;
}

static bool
np_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
    Buffer buffer;
    Page page;
    ItemId lp;
    HeapTupleData tuple;

    buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));

    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        return false;
    }

    tuple.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    tuple.t_len = ItemIdGetLength(lp);
    tuple.t_tableOid = RelationGetRelid(rel);
    tuple.t_self = *tid;

    ExecStoreBufferHeapTuple(&tuple, slot, buffer);
    UnlockReleaseBuffer(buffer);
    return true;
}
static void
np_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
    Assert(ItemPointerIsValid(tid));
    return;
}

static bool
np_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
    HeapScanDesc hscan = (HeapScanDesc) scan;
    return ItemPointerIsValid(tid) && ItemPointerGetBlockNumber(tid) < hscan->rs_nblocks;
}

static bool
np_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
    return true;
}

static TableScanDesc
np_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Sequential scans disabled.")));
    return NULL;
}

static bool
np_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
    pg_unreachable();
}

static void
np_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    pg_unreachable();
}

static void
np_scan_end(TableScanDesc sscan)
{
}

static IndexFetchTableData *
np_index_fetch_begin(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return NULL;
}

static void
np_index_fetch_reset(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static void
np_index_fetch_end(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static bool
np_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
    pg_unreachable();
}

/* PG15+ Signatures */
static bool
np_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream)
{
    return false;
}

static bool
np_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot)
{
    return false;
}

static bool
np_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated)
{
    pg_unreachable();
}

static bool
np_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TABLESAMPLE disabled.")));
    return false;
}

static bool
np_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
    pg_unreachable();
}

static Size
np_parallelscan_estimate(Relation rel)
{
    return 0;
}

static Size
np_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
    return 0;
}

static void
np_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
}

static bool
np_relation_needs_toast_table(Relation rel)
{
    return false;
}

static Oid
np_relation_toast_am(Relation rel)
{
    return InvalidOid;
}

static void
np_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result)
{
    pg_unreachable();
}

static void
np_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy)
{
}

static void
np_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

	/*
	 * Initialize to the minimum XID that could put tuples in the table. We
	 * know that no xacts older than RecentXmin are still running, so that
	 * will do.
	 */
	*freezeXid = RecentXmin;

	/*
	 * Similarly, initialize the minimum Multixact to the first value that
	 * could possibly be stored in tuples in the table.  Running transactions
	 * could reuse values from their local cache, so we are careful to
	 * consider all currently running multis.
	 *
	 * XXX this could be refined further, but is it worth the hassle?
	 */
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	/*
	 * If required, set up an init fork for an unlogged table so that it can
	 * be correctly reinitialized on restart.
	 */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
	}

	smgrclose(srel);
}

static void
np_relation_nontransactional_truncate(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TRUNCATE disabled.")));
}

static void
np_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
    pg_unreachable();
}

static void
np_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: CLUSTER disabled.")));
}

static double
np_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return 0;
}

static void
np_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state)
{
    pg_unreachable();
}

static TM_Result
np_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: DELETE disabled.")));
    return TM_Ok;
}

static void
np_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: UPSERT disabled.")));
}

static void
np_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
    pg_unreachable();
}

static TM_Result
np_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: FOR UPDATE disabled.")));
    return TM_Ok;
}

static void
np_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: SQL multi-insert disabled.")));
}

static void
np_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TID Range Scans disabled.")));
}

static bool
np_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
    pg_unreachable();
}

static TransactionId
np_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    pg_unreachable();
}

static void
np_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
    *pages = 0;
    *tuples = 0;
    *allvisfrac = 1.0;
}

static const TableAmRoutine np_mutable_methods = {
  .type = T_TableAmRoutine,

  .slot_callbacks = np_slot_callbacks,
  .tuple_fetch_row_version = np_tuple_fetch_row_version,
  .tuple_insert = np_tableam_tuple_insert,
  .tuple_update = np_tableam_tuple_update,
  .tuple_tid_valid = np_heapam_tuple_tid_valid,
  .tuple_get_latest_tid = np_heap_get_latest_tid,
  .tuple_satisfies_snapshot = np_tableam_tuple_satisfies_snapshot,
  .relation_size = table_block_relation_size,
  .relation_estimate_size = np_estimate_rel_size,

  .tuple_delete = np_tableam_tuple_delete,
  .tuple_insert_speculative = np_tuple_insert_speculative,
  .tuple_complete_speculative = np_tuple_complete_speculative,
  .multi_insert = np_multi_insert,
  .tuple_lock = np_tableam_tuple_lock,

  .scan_begin = np_tableam_beginscan,
  .scan_end = np_scan_end,
  .scan_rescan = np_scan_rescan,
  .scan_getnextslot = np_scan_getnextslot,
  .scan_set_tidrange = np_scan_set_tidrange,
  .scan_getnextslot_tidrange = np_scan_getnextslot_tidrange,
  .scan_bitmap_next_tuple = np_scan_bitmap_next_tuple,
  .scan_sample_next_block = np_scan_sample_next_block,
  .scan_sample_next_tuple = np_scan_sample_next_tuple,

  .relation_vacuum = np_relation_vacuum,
  .scan_analyze_next_block = np_scan_analyze_next_block,
  .scan_analyze_next_tuple = np_scan_analyze_next_tuple,
  .index_build_range_scan = np_index_build_range_scan,
  .index_validate_scan = np_index_validate_scan,
  .index_fetch_begin = np_index_fetch_begin,
  .index_fetch_reset = np_index_fetch_reset,
  .index_fetch_end = np_index_fetch_end,
  .index_fetch_tuple = np_index_fetch_tuple,
  .index_delete_tuples = np_index_delete_tuples,

  .parallelscan_estimate = np_parallelscan_estimate,
  .parallelscan_initialize = np_parallelscan_initialize,
  .parallelscan_reinitialize = np_parallelscan_reinitialize,
  .relation_needs_toast_table = np_relation_needs_toast_table,
  .relation_toast_am = np_relation_toast_am,
  .relation_fetch_toast_slice = np_relation_fetch_toast_slice,
  .relation_set_new_filelocator = np_relation_set_new_filelocator,
  .relation_nontransactional_truncate = np_relation_nontransactional_truncate,
  .relation_copy_data = np_relation_copy_data,
  .relation_copy_for_cluster = np_relation_copy_for_cluster
};

PG_FUNCTION_INFO_V1(np_mutable_handler);
Datum
np_mutable_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&np_mutable_methods);
}