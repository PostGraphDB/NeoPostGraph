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
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/storage_xlog.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "utils/rel.h"
#include "catalog/storage.h"
#include "access/transam.h"
#include "utils/snapmgr.h"

#include "access/np_phys_map.h"


/* The state kept across each row we return */
typedef struct NpPhysMapScanDescData
{
    TableScanDescData rs_base;   
    BlockNumber       curr_block;
    OffsetNumber      curr_offset;
    BlockNumber       rs_nblocks;   
    ItemPointerData   curr_v_ip;
    ItemPointerData   curr_e_ip;
} NpPhysMapScanDescData;
typedef NpPhysMapScanDescData *NpPhysMapScanDesc;



static const TupleTableSlotOps *np_physmap_slot_callbacks(Relation rel);
static void np_physmap_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                    int options, BulkInsertState bistate);
static TM_Result np_physmap_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                            CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                                            bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                                            TU_UpdateIndexes *update_indexes);
static bool np_physmap_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot);
static void np_physmap_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);
static bool np_physmap_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
static bool np_physmap_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot);
static TableScanDesc np_physmap_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags);
static bool np_physmap_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static void np_physmap_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode);
static void np_physmap_scan_end(TableScanDesc sscan);
static IndexFetchTableData *np_physmap_index_fetch_begin(Relation rel);
static void np_physmap_index_fetch_reset(IndexFetchTableData *scan);
static void np_physmap_index_fetch_end(IndexFetchTableData *scan);
static bool np_physmap_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead);
static bool np_physmap_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream);
static bool np_physmap_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot);
static bool np_physmap_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated);
static bool np_physmap_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate);
static bool np_physmap_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot);
static Size np_physmap_parallelscan_estimate(Relation rel);
static Size np_physmap_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
static void np_physmap_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
static bool np_physmap_relation_needs_toast_table(Relation rel);
static Oid np_physmap_relation_toast_am(Relation rel);
static void np_physmap_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result);
static void np_physmap_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy);
static void np_physmap_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti);
static void np_physmap_relation_nontransactional_truncate(Relation rel);
static void np_physmap_relation_copy_data(Relation rel, const RelFileLocator *newrlocator);
static void np_physmap_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead);
static double np_physmap_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan);
static void np_physmap_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state);
static TM_Result np_physmap_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart);
static void np_physmap_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken);
static void np_physmap_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded);
static TM_Result np_physmap_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd);
static void np_physmap_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate);
static void np_physmap_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid);
static bool np_physmap_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static TransactionId np_physmap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate);
static void np_physmap_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac);

/*
 * np_physmap_slot_callbacks - Provide the Executor with memory operations
 *
 * We bypass the private heapam function and directly return the 
 * globally exported Buffer Tuple operations struct.
 */
static const TupleTableSlotOps *
np_physmap_slot_callbacks(Relation rel)
{
    return &TTSOpsBufferHeapTuple;
}

/*
 * Convert a 32 bit transaction id into 64 bit transaction id, by assuming it
 * is within MaxTransactionId / 2 of XidFromFullTransactionId(rel).
 *
 * Be very careful about when to use this function. It can only safely be used
 * when there is a guarantee that xid is within MaxTransactionId / 2 xids of
 * rel. That e.g. can be guaranteed if the caller assures a snapshot is
 * held by the backend and xid is from a table (where vacuum/freezing ensures
 * the xid has to be within that range), or if xid is from the procarray and
 * prevents xid wraparound that way.
 */
static inline FullTransactionId
FullXidRelativeTo(FullTransactionId rel, TransactionId xid)
{
	TransactionId rel_xid = XidFromFullTransactionId(rel);

	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdIsValid(rel_xid));

	/* not guaranteed to find issues, but likely to catch mistakes */
	AssertTransactionIdInAllowableRange(xid);

	return FullTransactionIdFromU64(U64FromFullTransactionId(rel)
									+ (int32) (xid - rel_xid));
}

/* Visibility check for the Phys Map */
static bool
np_physmap_satisfies_snapshot(NeoPhysMapRecord *rec, Snapshot snapshot)
{

    return true;
}
#include "access/generic_xlog.h"
#include "storage/lmgr.h"
#include "storage/bufpage.h"
#include "access/generic_xlog.h"

void
np_set_edge_physmap_record(Relation rel, ItemPointer tid, NeoEdgePhysMapRecord *new_data)
{
    BlockNumber target_block = ItemPointerGetBlockNumber(tid);
    OffsetNumber target_offset = ItemPointerGetOffsetNumber(tid);
    BlockNumber nblocks;

    /* 1. Safely Extend the File if the Block Doesn't Exist Yet */
    LockRelationForExtension(rel, ExclusiveLock);
    nblocks = RelationGetNumberOfBlocks(rel);
    while (nblocks <= target_block)
    {
        Buffer extend_buf = ReadBuffer(rel, P_NEW);
        LockBuffer(extend_buf, BUFFER_LOCK_EXCLUSIVE);
        
        Page page = BufferGetPage(extend_buf);
        PageInit(page, BLCKSZ, 0);
        
        MarkBufferDirty(extend_buf);
        UnlockReleaseBuffer(extend_buf);
        nblocks++;
    }
    UnlockRelationForExtension(rel, ExclusiveLock);

    /* 2. Lock the Target Block */
    Buffer buffer = ReadBuffer(rel, target_block);
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    
    GenericXLogState *state = GenericXLogStart(rel);
    Page page = GenericXLogRegisterBuffer(state, buffer, 0);
    
    OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

    /* 3. Pad Missing Offsets if Inserting Out-of-Order (e.g., ID 5 before ID 1) */
    if (target_offset > maxoff)
    {
        NeoEdgePhysMapRecord empty_pad;
        memset(&empty_pad, 0, sizeof(NeoEdgePhysMapRecord));
        ItemPointerSetInvalid(&empty_pad.e_itemptr);

        /* Pad line pointers with full-size empty structs to preserve $O(1) space math */
        while (maxoff < target_offset - 1)
        {
            PageAddItemExtended(page, (Item) &empty_pad, sizeof(NeoEdgePhysMapRecord), 
                                InvalidOffsetNumber, 0);
            maxoff++;
        }
        
        /* Add the actual target record at target_offset */
        PageAddItemExtended(page, (Item) new_data, sizeof(NeoEdgePhysMapRecord), 
                            InvalidOffsetNumber, 0);
    }
    else
    {
        /* 4. Overwrite Existing Slot In-Place */
        ItemId lp = PageGetItemId(page, target_offset);
        if (ItemIdIsNormal(lp))
        {
            NeoEdgePhysMapRecord *disk_rec = (NeoEdgePhysMapRecord *) PageGetItem(page, lp);
            memcpy(disk_rec, new_data, sizeof(NeoEdgePhysMapRecord));
        }
        else
        {
            /* Reclaim a dead line pointer */
            PageAddItemExtended(page, (Item) new_data, sizeof(NeoEdgePhysMapRecord), 
                                target_offset, PAI_OVERWRITE);
        }
    }

    GenericXLogFinish(state);
    UnlockReleaseBuffer(buffer);
}

void
np_overwrite_physmap_in_page(Relation rel, ItemPointer tid, NeoPhysMapRecord *new_data)
{
    Buffer buffer;
    Page page;
    ItemId lp;
    NeoPhysMapRecord *disk_rec;
    GenericXLogState *state;

    buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    
    /* Start the WAL record and register the buffer */
    state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(state, buffer, 0);
    
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));

    if (!ItemIdIsNormal(lp)) {
        GenericXLogAbort(state); /* Cancel the WAL record */
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: attempted to in-place update an invalid phys_map tuple");
    }

    disk_rec = (NeoPhysMapRecord *) PageGetItem(page, lp);


    /* Overwrite the data directly into the WAL-registered page buffer */
    memcpy(disk_rec, new_data, sizeof(NeoPhysMapRecord));

    /* Commit the WAL record (this automatically marks the buffer dirty) */
    GenericXLogFinish(state);
    
    UnlockReleaseBuffer(buffer);
}

/*
 * get_phys_map_vpointer
 * Safely reads the current vertex property TID from the phys_map block.
 */
ItemPointerData
get_phys_map_vpointer(Relation pmap_rel, ItemPointer pmap_tid)
{
    Buffer buffer = ReadBuffer(pmap_rel, ItemPointerGetBlockNumber(pmap_tid));
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buffer);
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(pmap_tid));
    
    NeoPhysMapRecord *rec = (NeoPhysMapRecord *) PageGetItem(page, lp);
    ItemPointerData v_tid = rec->v_itemptr;
    
    UnlockReleaseBuffer(buffer);
    return v_tid;
}

static void 
np_write_record_to_page(Relation rel, char *data, Size data_size, ItemPointer out_tid)
{
    BlockNumber blockNum = RelationGetNumberOfBlocks(rel);

    Buffer buffer;
    if (blockNum == 0) {
        buffer = ReadBuffer(rel, P_NEW);
        blockNum = BufferGetBlockNumber(buffer);
    } else {
        buffer = ReadBuffer(rel, blockNum - 1);
        blockNum = BufferGetBlockNumber(buffer);
    }

    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);

    if (PageIsNew(page)) {
        PageInit(page, BufferGetPageSize(buffer), 0);
    }

    OffsetNumber offnum = PageAddItem(page, (Item) data, data_size, InvalidOffsetNumber, false, false);

    if (offnum == InvalidOffsetNumber) {
        UnlockReleaseBuffer(buffer);
        
        buffer = ReadBuffer(rel, P_NEW);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(buffer);
        PageInit(page, BufferGetPageSize(buffer), 0);
        
        offnum = PageAddItem(page, (Item) data, data_size, InvalidOffsetNumber, false, false);
        blockNum = BufferGetBlockNumber(buffer);
        
        if (offnum == InvalidOffsetNumber)
            elog(ERROR, "NeoPostGraph: Failed to add tuple to new page");
    }

    ItemPointerSet(out_tid, blockNum, offnum);

    MarkBufferDirty(buffer);
    
    // (TODO: WAL logging)

    UnlockReleaseBuffer(buffer);
}

static void 
np_physmap_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                int options, BulkInsertState bistate) 
{
    // materialize slot
    slot_getallattrs(slot);

    // convert item pointers
    ItemPointer v_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[0]);
    ItemPointer e_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[2]);

    // set values in record
    NeoPhysMapRecord rec = {
        .v_itemptr = *v_ptr,
        .e_tbl_id = DatumGetObjectId(slot->tts_values[1]),
        .e_itemptr = *e_ptr
    };

    // write
    ItemPointerData new_tid;
    np_write_record_to_page(relation, (char *) &rec, sizeof(NeoPhysMapRecord), &new_tid);

    // send TID back to calling method
    slot->tts_tableOid = RelationGetRelid(relation);
    ItemPointerCopy(&new_tid, &slot->tts_tid);
}

static TM_Result
np_physmap_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                                bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                                TU_UpdateIndexes *update_indexes)
{
    // materialize slot
    slot_getallattrs(slot);

    // convert item pointers
    ItemPointer v_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[0]);
    ItemPointer e_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[2]);

    // set values in record (MVCC headers are preserved by the writer)
    NeoPhysMapRecord rec = {
        .v_itemptr = *v_ptr,
        .e_tbl_id = DatumGetObjectId(slot->tts_values[1]),
        .e_itemptr = *e_ptr
    };

    // write in-place
    np_overwrite_physmap_in_page(relation, otid, &rec);

    // send TID back to calling method
    slot->tts_tableOid = RelationGetRelid(relation);
    ItemPointerCopy(otid, &slot->tts_tid);
    
    // skip standard index updates
    *update_indexes = TU_None; 

    return TM_Ok;
}

static bool
np_physmap_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
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
np_physmap_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
    Assert(ItemPointerIsValid(tid));
    return;
}

static bool
np_physmap_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
    HeapScanDesc hscan = (HeapScanDesc) scan;
    return ItemPointerIsValid(tid) && ItemPointerGetBlockNumber(tid) < hscan->rs_nblocks;
}

static bool
np_physmap_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
    BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
    
    NeoPhysMapRecord *rec = (NeoPhysMapRecord *) bslot->base.tuple->t_data;
    return np_physmap_satisfies_snapshot(rec, snapshot);
}

static TableScanDesc
np_physmap_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys,
                             struct ScanKeyData *key, ParallelTableScanDesc pscan,
                             uint32 flags)
{
    NpPhysMapScanDesc npscan = (NpPhysMapScanDesc) palloc(sizeof(NpPhysMapScanDescData));

    npscan->rs_base.rs_rd = relation;
    npscan->rs_base.rs_snapshot = snapshot;
    npscan->rs_base.rs_nkeys = nkeys;
    npscan->rs_base.rs_flags = flags;

    npscan->rs_nblocks = RelationGetNumberOfBlocks(relation);
    npscan->curr_block = 0;
    npscan->curr_offset = FirstOffsetNumber;

    return (TableScanDesc) npscan;
}

static bool
np_physmap_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                            TupleTableSlot *slot)
{
    NpPhysMapScanDesc npscan = (NpPhysMapScanDesc) sscan;
    Relation rel = npscan->rs_base.rs_rd;
    Snapshot snapshot = npscan->rs_base.rs_snapshot;

    while (npscan->curr_block < npscan->rs_nblocks) {
        Buffer buffer = ReadBuffer(rel, npscan->curr_block);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);
        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        while (npscan->curr_offset <= maxoff) {
            ItemId lp = PageGetItemId(page, npscan->curr_offset);
            
            if (ItemIdIsNormal(lp)) {
                NeoPhysMapRecord *rec = (NeoPhysMapRecord *) PageGetItem(page, lp);

                if (np_physmap_satisfies_snapshot(rec, snapshot)) {
                    ExecClearTuple(slot);
                    memset(slot->tts_isnull, false, 3 * sizeof(bool));

                    npscan->curr_v_ip = rec->v_itemptr;
                    slot->tts_values[0] = PointerGetDatum(&npscan->curr_v_ip);
                    slot->tts_values[1] = ObjectIdGetDatum(rec->e_tbl_id);
                    npscan->curr_e_ip = rec->e_itemptr;
                    slot->tts_values[2] = PointerGetDatum(&npscan->curr_e_ip);

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
np_physmap_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
                       bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    NpPhysMapScanDesc npscan = (NpPhysMapScanDesc) sscan;
    npscan->curr_block  = 0;
    npscan->curr_offset = FirstOffsetNumber;
}

static void
np_physmap_scan_end(TableScanDesc sscan)
{
    pfree(sscan);
}

static IndexFetchTableData *
np_physmap_index_fetch_begin(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return NULL;
}

static void
np_physmap_index_fetch_reset(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static void
np_physmap_index_fetch_end(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static bool
np_physmap_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
    pg_unreachable();
}

/* PG15+ Signatures */
static bool
np_physmap_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream)
{
    return false;
}

static bool
np_physmap_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot)
{
    return false;
}

static bool
np_physmap_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated)
{
    pg_unreachable();
}

static bool
np_physmap_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TABLESAMPLE disabled.")));
    return false;
}

static bool
np_physmap_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
    pg_unreachable();
}

static Size
np_physmap_parallelscan_estimate(Relation rel)
{
    return 0;
}

static Size
np_physmap_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
    return 0;
}

static void
np_physmap_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
}

static bool
np_physmap_relation_needs_toast_table(Relation rel)
{
    return false;
}

static Oid
np_physmap_relation_toast_am(Relation rel)
{
    return InvalidOid;
}

static void
np_physmap_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result)
{
    pg_unreachable();
}

static void
np_physmap_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy)
{
}

static void
np_physmap_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

    if (persistence != RELPERSISTENCE_PERMANENT)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("NeoPostGraph: Only permanent relations are supported for physical mapping tables")));

	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	smgrclose(srel);
}

static void
np_physmap_relation_nontransactional_truncate(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TRUNCATE disabled.")));
}

static void
np_physmap_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
    pg_unreachable();
}

static void
np_physmap_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: CLUSTER disabled.")));
}

static double
np_physmap_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return 0;
}

static void
np_physmap_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state)
{
    pg_unreachable();
}

static TM_Result
np_physmap_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: DELETE disabled.")));
    return TM_Ok;
}

static void
np_physmap_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: UPSERT disabled.")));
}

static void
np_physmap_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
    pg_unreachable();
}

static TM_Result
np_physmap_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: FOR UPDATE disabled.")));
    return TM_Ok;
}

static void
np_physmap_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: SQL multi-insert disabled.")));
}

static void
np_physmap_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TID Range Scans disabled.")));
}

static bool
np_physmap_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
    pg_unreachable();
}

static TransactionId
np_physmap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    pg_unreachable();
}

static void
np_physmap_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
    *pages = 0;
    *tuples = 0;
    *allvisfrac = 1.0;
}

static const TableAmRoutine np_physmap_mutable_methods = {
  .type = T_TableAmRoutine,

  .slot_callbacks = np_physmap_slot_callbacks,
  .tuple_fetch_row_version = np_physmap_tuple_fetch_row_version,
  .tuple_insert = np_physmap_tableam_tuple_insert,
  .tuple_update = np_physmap_tableam_tuple_update,
  .tuple_tid_valid = np_physmap_heapam_tuple_tid_valid,
  .tuple_get_latest_tid = np_physmap_heap_get_latest_tid,
  .tuple_satisfies_snapshot = np_physmap_tableam_tuple_satisfies_snapshot,
  .relation_size = table_block_relation_size,
  .relation_estimate_size = np_physmap_estimate_rel_size,

  .tuple_delete = np_physmap_tableam_tuple_delete,
  .tuple_insert_speculative = np_physmap_tuple_insert_speculative,
  .tuple_complete_speculative = np_physmap_tuple_complete_speculative,
  .multi_insert = np_physmap_multi_insert,
  .tuple_lock = np_physmap_tableam_tuple_lock,

  .scan_begin = np_physmap_tableam_beginscan,
  .scan_end = np_physmap_scan_end,
  .scan_rescan = np_physmap_scan_rescan,
  .scan_getnextslot = np_physmap_scan_getnextslot,
  .scan_set_tidrange = np_physmap_scan_set_tidrange,
  .scan_getnextslot_tidrange = np_physmap_scan_getnextslot_tidrange,
  .scan_bitmap_next_tuple = np_physmap_scan_bitmap_next_tuple,
  .scan_sample_next_block = np_physmap_scan_sample_next_block,
  .scan_sample_next_tuple = np_physmap_scan_sample_next_tuple,

  .relation_vacuum = np_physmap_relation_vacuum,
  .scan_analyze_next_block = np_physmap_scan_analyze_next_block,
  .scan_analyze_next_tuple = np_physmap_scan_analyze_next_tuple,
  .index_build_range_scan = np_physmap_index_build_range_scan,
  .index_validate_scan = np_physmap_index_validate_scan,
  .index_fetch_begin = np_physmap_index_fetch_begin,
  .index_fetch_reset = np_physmap_index_fetch_reset,
  .index_fetch_end = np_physmap_index_fetch_end,
  .index_fetch_tuple = np_physmap_index_fetch_tuple,
  .index_delete_tuples = np_physmap_index_delete_tuples,

  .parallelscan_estimate = np_physmap_parallelscan_estimate,
  .parallelscan_initialize = np_physmap_parallelscan_initialize,
  .parallelscan_reinitialize = np_physmap_parallelscan_reinitialize,
  .relation_needs_toast_table = np_physmap_relation_needs_toast_table,
  .relation_toast_am = np_physmap_relation_toast_am,
  .relation_fetch_toast_slice = np_physmap_relation_fetch_toast_slice,
  .relation_set_new_filelocator = np_physmap_relation_set_new_filelocator,
  .relation_nontransactional_truncate = np_physmap_relation_nontransactional_truncate,
  .relation_copy_data = np_physmap_relation_copy_data,
  .relation_copy_for_cluster = np_physmap_relation_copy_for_cluster
};
 
PG_FUNCTION_INFO_V1(np_physmap_mutable_handler);
Datum
np_physmap_mutable_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&np_physmap_mutable_methods);
}