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
#include "access/relscan.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "access/generic_xlog.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "catalog/storage.h"
#include "nodes/execnodes.h"

#include "access/np_arraylist.h"

#define SizeOfNeoArrayListRecord offsetof(NeoArrayListRecord, adj_list_data)

typedef struct NpArrayScanDescData
{
    TableScanDescData rs_base;   
    BlockNumber       curr_block;
    OffsetNumber      curr_offset;
    BlockNumber       rs_nblocks;   
} NpArrayScanDescData;

typedef NpArrayScanDescData *NpArrayScanDesc;

/* Extern reference to your existing page writer */
extern void np_write_record_to_page(Relation rel, char *tuple_buf, Size size, ItemPointer out_tid);

/* ------------------------------------------------------------------------
 * 64-bit XID MVCC Logic
 * ------------------------------------------------------------------------ */

static inline FullTransactionId
FullXidRelativeTo(FullTransactionId rel, TransactionId xid)
{
    TransactionId rel_xid = XidFromFullTransactionId(rel);

    Assert(TransactionIdIsValid(xid));
    Assert(TransactionIdIsValid(rel_xid));
    AssertTransactionIdInAllowableRange(xid);

    return FullTransactionIdFromU64(U64FromFullTransactionId(rel)
                                    + (int32) (xid - rel_xid));
}

static inline bool
np_arraylist_record_satisfies_snapshot(NeoArrayListRecord *rec, Snapshot snapshot)
{
    FullTransactionId f_xmin = rec->xmin;
    FullTransactionId f_xmax = rec->xmax;
    
    TransactionId xmin_32 = XidFromFullTransactionId(f_xmin);
    TransactionId xmax_32 = XidFromFullTransactionId(f_xmax);

    FullTransactionId next_fxid = ReadNextFullTransactionId();
    FullTransactionId freeze_horizon = FullXidRelativeTo(next_fxid, snapshot->xmin);

    if (!FullTransactionIdIsValid(f_xmin)) {
        return false;
    }
    
    if (FullTransactionIdPrecedes(f_xmin, freeze_horizon)) {
        /* Tuple is committed */
    } else if (TransactionIdIsCurrentTransactionId(xmin_32)) {
        if (rec->cmin >= snapshot->curcid) {
            return false;
        }
    } else if (XidInMVCCSnapshot(xmin_32, snapshot)) {
        return false;
    } else if (!TransactionIdDidCommit(xmin_32)) {
        return false;
    }

    if (FullTransactionIdIsValid(f_xmax)) {
        if (FullTransactionIdPrecedes(f_xmax, freeze_horizon)) {
            return false; 
        } else if (TransactionIdIsCurrentTransactionId(xmax_32)) {
            if (rec->cmax >= snapshot->curcid) {
                return true; 
            } else {
                return false;
            }
        } else if (!XidInMVCCSnapshot(xmax_32, snapshot) && TransactionIdDidCommit(xmax_32)) {
            return false;
        }
    }

    return true;
}

static bool
nparraylist_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
    return true; 
}


/* ------------------------------------------------------------------------
 * Slot & Scan State 
 * ------------------------------------------------------------------------ */

static const TupleTableSlotOps *
nparraylist_slot_callbacks(Relation rel)
{
    return &TTSOpsVirtual;
}

static TableScanDesc
nparraylist_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys,
                              struct ScanKeyData *key, ParallelTableScanDesc pscan,
                              uint32 flags)
{
    NpArrayScanDesc npscan = (NpArrayScanDesc) palloc0(sizeof(NpArrayScanDescData));

    npscan->rs_base.rs_rd = relation;
    npscan->rs_base.rs_snapshot = snapshot;
    npscan->rs_base.rs_nkeys = nkeys;
    npscan->rs_base.rs_flags = flags;

    npscan->rs_nblocks   = RelationGetNumberOfBlocks(relation);
    npscan->curr_block   = 0;
    npscan->curr_offset  = FirstOffsetNumber;

    return (TableScanDesc) npscan;
}

static bool
nparraylist_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                             TupleTableSlot *slot)
{
    NpArrayScanDesc npscan = (NpArrayScanDesc) sscan;
    Relation rel = npscan->rs_base.rs_rd;
    Snapshot snapshot = npscan->rs_base.rs_snapshot;

    while (npscan->curr_block < npscan->rs_nblocks)
    {
        Buffer buffer = ReadBuffer(rel, npscan->curr_block);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);
        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        while (npscan->curr_offset <= maxoff)
        {
            ItemId lp = PageGetItemId(page, npscan->curr_offset);
            
            if (ItemIdIsNormal(lp))
            {
                Size item_len = ItemIdGetLength(lp);
                NeoArrayListRecord *rec = (NeoArrayListRecord *) PageGetItem(page, lp);

                if (np_arraylist_record_satisfies_snapshot(rec, snapshot))
                {
                    ExecClearTuple(slot);
                    memset(slot->tts_isnull, false, 5 * sizeof(bool));

                    slot->tts_values[0] = Int64GetDatum(rec->owner_id);
                    slot->tts_values[1] = ObjectIdGetDatum(rec->prev_tbl);
                    
                    ItemPointer copy_prev = (ItemPointer) MemoryContextAlloc(slot->tts_mcxt, sizeof(ItemPointerData));
                    *copy_prev = rec->prev_itemptr;
                    slot->tts_values[2] = PointerGetDatum(copy_prev);
                    slot->tts_isnull[2] = !ItemPointerIsValid(copy_prev);

                    /* 
                     * VARLENA RE-WRAP FIX:
                     * The payload on disk is pure raw binary. We must allocate space for the raw bytes 
                     * PLUS the mandatory 4-byte VARHDRSZ, apply the header, and then copy the data.
                     */
                    if (item_len < SizeOfNeoArrayListRecord) {
                        UnlockReleaseBuffer(buffer);
                        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("NeoPostGraph [SCAN FATAL]: Item length %zu is smaller than base record header %zu!", item_len, SizeOfNeoArrayListRecord)));
                    }

                    Size raw_payload_size = item_len - SizeOfNeoArrayListRecord;
                    Size total_varlena_size = VARHDRSZ + raw_payload_size;
                    bytea *adj_copy = (bytea *) MemoryContextAlloc(slot->tts_mcxt, total_varlena_size);
                    
                    /* Apply standard 4-byte header */
                    SET_VARSIZE(adj_copy, total_varlena_size);
                    
                    /* Copy the pure unadulterated bytes straight into the payload region */
                    memcpy(VARDATA(adj_copy), rec->adj_list_data, raw_payload_size);
                    
                    slot->tts_values[3] = PointerGetDatum(adj_copy);

                    ItemPointer copy_next = (ItemPointer) MemoryContextAlloc(slot->tts_mcxt, sizeof(ItemPointerData));
                    *copy_next = rec->next_itemptr;
                    slot->tts_values[4] = PointerGetDatum(copy_next);
                    slot->tts_isnull[4] = !ItemPointerIsValid(copy_next);

                    ExecStoreVirtualTuple(slot);
                    
                    ItemPointerSet(&slot->tts_tid, npscan->curr_block, npscan->curr_offset);
                    slot->tts_tableOid = RelationGetRelid(rel);

                    UnlockReleaseBuffer(buffer);
                    npscan->curr_offset++;
                    return true;
                }
            }
            npscan->curr_offset++;
        }

        UnlockReleaseBuffer(buffer);
        npscan->curr_block++;
        npscan->curr_offset = FirstOffsetNumber;
    }

    return false;
}

static void
nparraylist_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
                        bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    NpArrayScanDesc npscan = (NpArrayScanDesc) sscan;
    npscan->curr_block  = 0;
    npscan->curr_offset = FirstOffsetNumber;
}

static void
nparraylist_scan_end(TableScanDesc sscan)
{
    pfree(sscan);
}


/* ------------------------------------------------------------------------
 * DML
 * ------------------------------------------------------------------------ */

static void
nparraylist_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                 int options, struct BulkInsertStateData *bistate)
{
    bool isnull;
    
    int64 owner_id = DatumGetInt64(slot_getattr(slot, 1, &isnull));
    Oid prev_tbl = DatumGetObjectId(slot_getattr(slot, 2, &isnull));
    
    ItemPointerData prev_tid;
    Datum prev_tid_datum = slot_getattr(slot, 3, &isnull);
    if (!isnull) prev_tid = *((ItemPointer) DatumGetPointer(prev_tid_datum));
    else ItemPointerSetInvalid(&prev_tid);

    /* 
     * VARLENA STRIPPING FIX:
     * Use EXHDR to get the pure binary length, and VARDATA_ANY to get the pure pointer.
     * This safely ignores whatever internal header structure Postgres is currently using.
     */
    Datum adj_datum = slot_getattr(slot, 4, &isnull);
    bytea *adj_list_bytea = (bytea *) DatumGetPointer(adj_datum);
    Size raw_payload_size = VARSIZE_ANY_EXHDR(adj_list_bytea);
    
    ItemPointerData next_tid;
    Datum next_tid_datum = slot_getattr(slot, 5, &isnull);
    if (!isnull) next_tid = *((ItemPointer) DatumGetPointer(next_tid_datum));
    else ItemPointerSetInvalid(&next_tid);

    Size exact_tuple_size = SizeOfNeoArrayListRecord + raw_payload_size;
    char *tuple_buf = (char *) palloc0(exact_tuple_size);
    NeoArrayListRecord *rec = (NeoArrayListRecord *) tuple_buf;

    rec->xmin = GetTopFullTransactionId();
    rec->xmax = InvalidFullTransactionId;
    rec->cmin = cid;
    rec->cmax = InvalidCommandId;
    rec->flags = 0;
    
    rec->owner_id = owner_id;
    rec->prev_tbl = prev_tbl;
    rec->prev_itemptr = prev_tid;
    rec->next_itemptr = next_tid;

    /* Copy ONLY the pure payload bytes, leaving the header behind */
    memcpy(rec->adj_list_data, VARDATA_ANY(adj_list_bytea), raw_payload_size);

    np_write_record_to_page(relation, tuple_buf, exact_tuple_size, &(slot->tts_tid));

    slot->tts_tableOid = RelationGetRelid(relation);
    
    pfree(tuple_buf);
}

static TM_Result
nparraylist_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                 CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                                 bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                                 TU_UpdateIndexes *update_indexes)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: IN PLACE UPDATE disabled for array lists.")));
    return TM_Ok;
}

static TM_Result
nparraylist_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, 
                                 Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: DELETE disabled for array lists.")));
    return TM_Ok;
}


/* ------------------------------------------------------------------------
 * Index / Fetch Callbacks
 * ------------------------------------------------------------------------ */

static bool
nparraylist_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: Index fetches disabled for array lists.")));
    return false;
}

static void
nparraylist_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
    Assert(ItemPointerIsValid(tid));
}

static bool
nparraylist_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
    NpArrayScanDesc npscan = (NpArrayScanDesc) scan;
    return ItemPointerIsValid(tid) && ItemPointerGetBlockNumber(tid) < npscan->rs_nblocks;
}


/* ------------------------------------------------------------------------
 * Boilerplate Stubs
 * ------------------------------------------------------------------------ */

static IndexFetchTableData *nparraylist_index_fetch_begin(Relation rel) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: Indexes disabled."))); return NULL; }
static void nparraylist_index_fetch_reset(IndexFetchTableData *scan) { pg_unreachable(); }
static void nparraylist_index_fetch_end(IndexFetchTableData *scan) { pg_unreachable(); }
static bool nparraylist_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead) { pg_unreachable(); }

static bool nparraylist_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream) { return false; }
static bool nparraylist_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot) { return false; }
static bool nparraylist_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated) { pg_unreachable(); }
static bool nparraylist_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: TABLESAMPLE disabled."))); return false; }
static bool nparraylist_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot) { pg_unreachable(); }

static Size nparraylist_parallelscan_estimate(Relation rel) { return 0; }
static Size nparraylist_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan) { return 0; }
static void nparraylist_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan) { }

static bool nparraylist_relation_needs_toast_table(Relation rel) { return false; }
static Oid nparraylist_relation_toast_am(Relation rel) { return InvalidOid; }
static void nparraylist_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result) { pg_unreachable(); }
static void nparraylist_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy) { }

static void
nparraylist_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
    SMgrRelation srel;
    if (persistence != RELPERSISTENCE_PERMANENT)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: Only permanent relations are supported.")));

    *freezeXid = RecentXmin;
    *minmulti = GetOldestMultiXactId();
    srel = RelationCreateStorage(*newrlocator, persistence, true);
    smgrclose(srel);
}

static void nparraylist_relation_nontransactional_truncate(Relation rel) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: TRUNCATE disabled."))); }
static void nparraylist_relation_copy_data(Relation rel, const RelFileLocator *newrlocator) { pg_unreachable(); }
static void nparraylist_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: CLUSTER disabled."))); }
static double nparraylist_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: Indexes disabled."))); return 0; }
static void nparraylist_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state) { pg_unreachable(); }
static void nparraylist_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, struct BulkInsertStateData *bistate, uint32 specToken) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: UPSERT disabled."))); }
static void nparraylist_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded) { pg_unreachable(); }
static TM_Result nparraylist_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: FOR UPDATE disabled."))); return TM_Ok; }
static void nparraylist_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, struct BulkInsertStateData *bistate) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: SQL multi-insert disabled."))); }
static void nparraylist_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid) { ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("NeoPostGraph: TID Range Scans disabled."))); }
static bool nparraylist_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot) { pg_unreachable(); }
static TransactionId nparraylist_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate) { pg_unreachable(); }

static void
nparraylist_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
    *pages = 0;
    *tuples = 0;
    *allvisfrac = 1.0;
}

static const TableAmRoutine np_arraylist_methods = {
  .type = T_TableAmRoutine,

  .slot_callbacks = nparraylist_slot_callbacks,
  .tuple_fetch_row_version = nparraylist_tuple_fetch_row_version,
  .tuple_insert = nparraylist_tableam_tuple_insert,
  .tuple_update = nparraylist_tableam_tuple_update,
  .tuple_tid_valid = nparraylist_heapam_tuple_tid_valid,
  .tuple_get_latest_tid = nparraylist_heap_get_latest_tid,
  .tuple_satisfies_snapshot = nparraylist_tableam_tuple_satisfies_snapshot,
  .relation_size = table_block_relation_size,
  .relation_estimate_size = nparraylist_estimate_rel_size,

  .tuple_delete = nparraylist_tableam_tuple_delete,
  .tuple_insert_speculative = nparraylist_tuple_insert_speculative,
  .tuple_complete_speculative = nparraylist_tuple_complete_speculative,
  .multi_insert = nparraylist_multi_insert,
  .tuple_lock = nparraylist_tableam_tuple_lock,

  .scan_begin = nparraylist_tableam_beginscan,
  .scan_end = nparraylist_scan_end,
  .scan_rescan = nparraylist_scan_rescan,
  .scan_getnextslot = nparraylist_scan_getnextslot,
  .scan_set_tidrange = nparraylist_scan_set_tidrange,
  .scan_getnextslot_tidrange = nparraylist_scan_getnextslot_tidrange,
  .scan_bitmap_next_tuple = nparraylist_scan_bitmap_next_tuple,
  .scan_sample_next_block = nparraylist_scan_sample_next_block,
  .scan_sample_next_tuple = nparraylist_scan_sample_next_tuple,

  .relation_vacuum = nparraylist_relation_vacuum,
  .scan_analyze_next_block = nparraylist_scan_analyze_next_block,
  .scan_analyze_next_tuple = nparraylist_scan_analyze_next_tuple,
  .index_build_range_scan = nparraylist_index_build_range_scan,
  .index_validate_scan = nparraylist_index_validate_scan,
  .index_fetch_begin = nparraylist_index_fetch_begin,
  .index_fetch_reset = nparraylist_index_fetch_reset,
  .index_fetch_end = nparraylist_index_fetch_end,
  .index_fetch_tuple = nparraylist_index_fetch_tuple,
  .index_delete_tuples = nparraylist_index_delete_tuples,

  .parallelscan_estimate = nparraylist_parallelscan_estimate,
  .parallelscan_initialize = nparraylist_parallelscan_initialize,
  .parallelscan_reinitialize = nparraylist_parallelscan_reinitialize,
  .relation_needs_toast_table = nparraylist_relation_needs_toast_table,
  .relation_toast_am = nparraylist_relation_toast_am,
  .relation_fetch_toast_slice = nparraylist_relation_fetch_toast_slice,
  .relation_set_new_filelocator = nparraylist_relation_set_new_filelocator,
  .relation_nontransactional_truncate = nparraylist_relation_nontransactional_truncate,
  .relation_copy_data = nparraylist_relation_copy_data,
  .relation_copy_for_cluster = nparraylist_relation_copy_for_cluster
};

PG_FUNCTION_INFO_V1(nparraylist_handler);
Datum
nparraylist_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&np_arraylist_methods);
}