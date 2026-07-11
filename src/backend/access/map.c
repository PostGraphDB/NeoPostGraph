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

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "commands/vacuum.h"
#include "access/multixact.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"

static const TupleTableSlotOps *neopg_slot_callbacks(Relation rel);
void neopg_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid);
void neopg_heap_insert(Relation relation, HeapTuple tup, CommandId cid, int options, BulkInsertState bistate);
static void neopg_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                    int options, BulkInsertState bistate);
static TM_Result neopg_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                            CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                                            bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                                            TU_UpdateIndexes *update_indexes);
static bool neopg_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot);
static void neopg_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);
static bool neopg_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
static bool neopg_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot);
static TableScanDesc neopg_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags);
static bool neopg_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static void neopg_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode);
static void neopg_scan_end(TableScanDesc sscan);
static IndexFetchTableData *neopg_index_fetch_begin(Relation rel);
static void neopg_index_fetch_reset(IndexFetchTableData *scan);
static void neopg_index_fetch_end(IndexFetchTableData *scan);
static bool neopg_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead);
static bool neopg_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream);
static bool neopg_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot);
static bool neopg_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated);
static bool neopg_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate);
static bool neopg_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot);
static Size neopg_parallelscan_estimate(Relation rel);
static Size neopg_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
static void neopg_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
static bool neopg_relation_needs_toast_table(Relation rel);
static Oid neopg_relation_toast_am(Relation rel);
static void neopg_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result);
static void neopg_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy);
static void neopg_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti);
static void neopg_relation_nontransactional_truncate(Relation rel);
static void neopg_relation_copy_data(Relation rel, const RelFileLocator *newrlocator);
static void neopg_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead);
static double neopg_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan);
static void neopg_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state);
static TM_Result neopg_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart);
static void neopg_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken);
static void neopg_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded);
static TM_Result neopg_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd);
static void neopg_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate);
static void neopg_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid);
static bool neopg_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static TransactionId neopg_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate);
static void neopg_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac);


void neopg_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid) {
    TM_Result result;
    TransactionId xid = GetCurrentTransactionId();
    ItemId lp;
    HeapTupleData oldtup;
    Page page;
    BlockNumber block;
    Buffer buffer;

    Assert(ItemPointerIsValid(otid));
    Assert(HeapTupleHeaderGetNatts(newtup->t_data) <= RelationGetNumberOfAttributes(relation));

    if (IsInParallelMode())
        ereport(ERROR, (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
             errmsg("cannot update tuples during a parallel operation")));

    block = ItemPointerGetBlockNumber(otid);
    buffer = ReadBuffer(relation, block);
    page = BufferGetPage(buffer);

    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(otid));

    if (!ItemIdIsNormal(lp))
    {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("NeoPostGraph: Positional mapping failed. Line pointer is not normal at TID (%u, %u)",
                ItemPointerGetBlockNumber(otid), ItemPointerGetOffsetNumber(otid))));
    }

    oldtup.t_tableOid = RelationGetRelid(relation);
    oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    oldtup.t_len = ItemIdGetLength(lp);
    oldtup.t_self = *otid;
    newtup->t_tableOid = RelationGetRelid(relation);

    result = HeapTupleSatisfiesUpdate(&oldtup, cid, buffer);

    if (result == TM_SelfModified) result = TM_Ok;

    if (result != TM_Ok)
    {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("NeoPostGraph: Unexpected visibility failure (%d) during in-place overwrite.", result)));
    }

    if (newtup->t_len != oldtup.t_len)
    {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: In-place update requires identical raw tuple sizes.")));
    }

    START_CRIT_SECTION();

    uint8 header_size = oldtup.t_data->t_hoff;
    Size payload_size = oldtup.t_len - header_size;

    memcpy((char *) oldtup.t_data + header_size, 
           (char *) newtup->t_data + newtup->t_data->t_hoff, 
           payload_size);

    oldtup.t_data->t_ctid = oldtup.t_self;

    MarkBufferDirty(buffer);

    if (RelationNeedsWAL(relation))
    {
        xl_heap_inplace xlrec;
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

        xlrec.offnum = ItemPointerGetOffsetNumber(&oldtup.t_self);
        XLogRegisterData((char *) &xlrec, sizeof(xl_heap_inplace));
        XLogRegisterBufData(0, (char *) newtup->t_data, newtup->t_len);

        recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE);
        PageSetLSN(page, recptr);
    }

    END_CRIT_SECTION();

    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    ReleaseBuffer(buffer);
}

void
neopg_heap_insert(Relation relation, HeapTuple tup, CommandId cid,
                  int options, BulkInsertState bistate)
{
    TransactionId xid = GetCurrentTransactionId();
    Buffer buffer;
    Page page;

    Assert(HeapTupleHeaderGetNatts(tup->t_data) <= RelationGetNumberOfAttributes(relation));

    tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
    tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
    tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
    HeapTupleHeaderSetXmin(tup->t_data, xid);
    HeapTupleHeaderSetCmin(tup->t_data, cid);
    HeapTupleHeaderSetXmax(tup->t_data, InvalidTransactionId);
    tup->t_tableOid = RelationGetRelid(relation);

    buffer = RelationGetBufferForTuple(relation, tup->t_len,
                       InvalidBuffer, options, bistate,
                       NULL, NULL, 0);

    page = BufferGetPage(buffer);

    START_CRIT_SECTION();

    RelationPutHeapTuple(relation, buffer, tup, false);
    MarkBufferDirty(buffer);

    if (RelationNeedsWAL(relation))
    {
        xl_heap_insert xlrec;
        xl_heap_header xlhdr;
        XLogRecPtr recptr;
        uint8 info = XLOG_HEAP_INSERT;
        int bufflags = 0;

        if (ItemPointerGetOffsetNumber(&(tup->t_self)) == FirstOffsetNumber &&
            PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
        {
            info |= XLOG_HEAP_INIT_PAGE;
            bufflags |= REGBUF_WILL_INIT;
        }

        xlrec.offnum = ItemPointerGetOffsetNumber(&tup->t_self);
        xlrec.flags = 0;
        
        XLogBeginInsert();
        XLogRegisterData((char *) &xlrec, SizeOfHeapInsert);

        xlhdr.t_infomask2 = tup->t_data->t_infomask2;
        xlhdr.t_infomask = tup->t_data->t_infomask;
        xlhdr.t_hoff = tup->t_data->t_hoff;

        XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);
        XLogRegisterBufData(0, (char *) &xlhdr, SizeOfHeapHeader);
        XLogRegisterBufData(0, (char *) tup->t_data + SizeofHeapTupleHeader,
                            tup->t_len - SizeofHeapTupleHeader);

        recptr = XLogInsert(RM_HEAP_ID, info);
        PageSetLSN(page, recptr);
    }

    END_CRIT_SECTION();
    UnlockReleaseBuffer(buffer);
}
/*
 * neopg_slot_callbacks - Provide the Executor with memory operations
 *
 * We bypass the private heapam function and directly return the 
 * globally exported Buffer Tuple operations struct.
 */
static const TupleTableSlotOps *
neopg_slot_callbacks(Relation rel)
{
    return &TTSOpsBufferHeapTuple;
}


static void neopg_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                       int options, BulkInsertState bistate) {
    bool shouldFree = true;
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
    slot->tts_tableOid = RelationGetRelid(relation);
    tuple->t_tableOid = slot->tts_tableOid;
    
    neopg_heap_insert(relation, tuple, cid, options, bistate);
    
    ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
    if (shouldFree)
        pfree(tuple);
}

static TM_Result
neopg_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                           CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                           bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                           TU_UpdateIndexes *update_indexes)
{
    bool shouldFree = true;
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
    slot->tts_tableOid = RelationGetRelid(relation);
    tuple->t_tableOid = slot->tts_tableOid;
    
    neopg_update_inplace(relation, otid, tuple, cid);
    
    ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
    *update_indexes = TU_None;
    if (shouldFree)
        pfree(tuple);
    return TM_Ok;
}

static bool
neopg_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
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
neopg_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
    Assert(ItemPointerIsValid(tid));
    return;
}

static bool
neopg_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
    HeapScanDesc hscan = (HeapScanDesc) scan;
    return ItemPointerIsValid(tid) && ItemPointerGetBlockNumber(tid) < hscan->rs_nblocks;
}

static bool
neopg_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
    return true;
}

static TableScanDesc
neopg_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Sequential scans disabled.")));
    return NULL;
}

static bool
neopg_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
    pg_unreachable();
}

static void
neopg_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    pg_unreachable();
}

static void
neopg_scan_end(TableScanDesc sscan)
{
}

static IndexFetchTableData *
neopg_index_fetch_begin(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return NULL;
}

static void
neopg_index_fetch_reset(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static void
neopg_index_fetch_end(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static bool
neopg_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
    pg_unreachable();
}

/* PG15+ Signatures */
static bool
neopg_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream)
{
    return false;
}

static bool
neopg_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot)
{
    return false;
}

static bool
neopg_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated)
{
    pg_unreachable();
}

static bool
neopg_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TABLESAMPLE disabled.")));
    return false;
}

static bool
neopg_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
    pg_unreachable();
}

static Size
neopg_parallelscan_estimate(Relation rel)
{
    return 0;
}

static Size
neopg_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
    return 0;
}

static void
neopg_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
}

static bool
neopg_relation_needs_toast_table(Relation rel)
{
    return false;
}

static Oid
neopg_relation_toast_am(Relation rel)
{
    return InvalidOid;
}

static void
neopg_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result)
{
    pg_unreachable();
}

static void
neopg_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy)
{
}

static void
neopg_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
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
neopg_relation_nontransactional_truncate(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TRUNCATE disabled.")));
}

static void
neopg_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
    pg_unreachable();
}

static void
neopg_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: CLUSTER disabled.")));
}

static double
neopg_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return 0;
}

static void
neopg_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state)
{
    pg_unreachable();
}

static TM_Result
neopg_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: DELETE disabled.")));
    return TM_Ok;
}

static void
neopg_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: UPSERT disabled.")));
}

static void
neopg_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
    pg_unreachable();
}

static TM_Result
neopg_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: FOR UPDATE disabled.")));
    return TM_Ok;
}

static void
neopg_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: SQL multi-insert disabled.")));
}

static void
neopg_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TID Range Scans disabled.")));
}

static bool
neopg_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
    pg_unreachable();
}

static TransactionId
neopg_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    pg_unreachable();
}

static void
neopg_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
    *pages = 0;
    *tuples = 0;
    *allvisfrac = 1.0;
}

static const TableAmRoutine neopg_methods = {
  .type = T_TableAmRoutine,

  .slot_callbacks = neopg_slot_callbacks,
  .tuple_fetch_row_version = neopg_tuple_fetch_row_version,
  .tuple_insert = neopg_tableam_tuple_insert,
  .tuple_update = neopg_tableam_tuple_update,
  .tuple_tid_valid = neopg_heapam_tuple_tid_valid,
  .tuple_get_latest_tid = neopg_heap_get_latest_tid,
  .tuple_satisfies_snapshot = neopg_tableam_tuple_satisfies_snapshot,
  .relation_size = table_block_relation_size,
  .relation_estimate_size = neopg_estimate_rel_size,

  .tuple_delete = neopg_tableam_tuple_delete,
  .tuple_insert_speculative = neopg_tuple_insert_speculative,
  .tuple_complete_speculative = neopg_tuple_complete_speculative,
  .multi_insert = neopg_multi_insert,
  .tuple_lock = neopg_tableam_tuple_lock,

  .scan_begin = neopg_tableam_beginscan,
  .scan_end = neopg_scan_end,
  .scan_rescan = neopg_scan_rescan,
  .scan_getnextslot = neopg_scan_getnextslot,
  .scan_set_tidrange = neopg_scan_set_tidrange,
  .scan_getnextslot_tidrange = neopg_scan_getnextslot_tidrange,
  .scan_bitmap_next_tuple = neopg_scan_bitmap_next_tuple,
  .scan_sample_next_block = neopg_scan_sample_next_block,
  .scan_sample_next_tuple = neopg_scan_sample_next_tuple,

  .relation_vacuum = neopg_relation_vacuum,
  .scan_analyze_next_block = neopg_scan_analyze_next_block,
  .scan_analyze_next_tuple = neopg_scan_analyze_next_tuple,
  .index_build_range_scan = neopg_index_build_range_scan,
  .index_validate_scan = neopg_index_validate_scan,
  .index_fetch_begin = neopg_index_fetch_begin,
  .index_fetch_reset = neopg_index_fetch_reset,
  .index_fetch_end = neopg_index_fetch_end,
  .index_fetch_tuple = neopg_index_fetch_tuple,
  .index_delete_tuples = neopg_index_delete_tuples,

  .parallelscan_estimate = neopg_parallelscan_estimate,
  .parallelscan_initialize = neopg_parallelscan_initialize,
  .parallelscan_reinitialize = neopg_parallelscan_reinitialize,
  .relation_needs_toast_table = neopg_relation_needs_toast_table,
  .relation_toast_am = neopg_relation_toast_am,
  .relation_fetch_toast_slice = neopg_relation_fetch_toast_slice,
  .relation_set_new_filelocator = neopg_relation_set_new_filelocator,
  .relation_nontransactional_truncate = neopg_relation_nontransactional_truncate,
  .relation_copy_data = neopg_relation_copy_data,
  .relation_copy_for_cluster = neopg_relation_copy_for_cluster
};

PG_FUNCTION_INFO_V1(neopg_tableam_hash_handler);
Datum
neopg_tableam_hash_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&neopg_methods);
}