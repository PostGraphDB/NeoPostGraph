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

#include "access/np_linked_list.h"

typedef struct NpScanDescData
{
    TableScanDescData rs_base;   
    BlockNumber       curr_block;
    OffsetNumber      curr_offset;
    BlockNumber       rs_nblocks;   
    
    /* Zero-allocation safety buffers for pass-by-reference types */
    ItemPointerData   curr_next_ip;
    ItemPointerData   curr_prev_ip;
} NpScanDescData;

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



typedef NpScanDescData *NpScanDesc;

static const TupleTableSlotOps *np_linked_list_slot_callbacks(Relation rel);
static void np_linked_list_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                    int options, BulkInsertState bistate);
static TM_Result np_linked_list_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                            CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                                            bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                                            TU_UpdateIndexes *update_indexes);
static bool np_linked_list_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot);
static void np_linked_list_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid);
static bool np_linked_list_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid);
static bool np_linked_list_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot);
static TableScanDesc np_linked_list_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags);
static bool np_linked_list_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static void np_linked_list_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode);
static void np_linked_list_scan_end(TableScanDesc sscan);
static IndexFetchTableData *np_linked_list_index_fetch_begin(Relation rel);
static void np_linked_list_index_fetch_reset(IndexFetchTableData *scan);
static void np_linked_list_index_fetch_end(IndexFetchTableData *scan);
static bool np_linked_list_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead);
static bool np_linked_list_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream);
static bool np_linked_list_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot);
static bool np_linked_list_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated);
static bool np_linked_list_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate);
static bool np_linked_list_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot);
static Size np_linked_list_parallelscan_estimate(Relation rel);
static Size np_linked_list_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
static void np_linked_list_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
static bool np_linked_list_relation_needs_toast_table(Relation rel);
static Oid np_linked_list_relation_toast_am(Relation rel);
static void np_linked_list_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result);
static void np_linked_list_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy);
static void np_linked_list_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti);
static void np_linked_list_relation_nontransactional_truncate(Relation rel);
static void np_linked_list_relation_copy_data(Relation rel, const RelFileLocator *newrlocator);
static void np_linked_list_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead);
static double np_linked_list_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan);
static void np_linked_list_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state);
static TM_Result np_linked_list_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart);
static void np_linked_list_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken);
static void np_linked_list_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded);
static TM_Result np_linked_list_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd);
static void np_linked_list_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate);
static void np_linked_list_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid);
static bool np_linked_list_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot);
static TransactionId np_linked_list_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate);
static void np_linked_list_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac);

/*
 * np_linked_list_slot_callbacks - Provide the Executor with memory operations
 *
 * We bypass the private heapam function and directly return the 
 * globally exported Buffer Tuple operations struct.
 */
static const TupleTableSlotOps *
np_linked_list_slot_callbacks(Relation rel)
{
    return &TTSOpsBufferHeapTuple;
}
static void 
np_write_record_to_page(Relation rel, char *data, Size data_size, ItemPointer out_tid)
{
    Buffer buffer;
    Page page;
    OffsetNumber offnum;
    BlockNumber blockNum;

    // For simplicity, we just grab the last block of the relation. 
    // In a production graph engine, you would use a Free Space Map (FSM) here.
    blockNum = RelationGetNumberOfBlocks(rel);
    
    if (blockNum == 0) {
        // Table is completely empty, extend it by 1 block
        buffer = ReadBuffer(rel, P_NEW);
        blockNum = BufferGetBlockNumber(buffer);
    } else {
        // Read the last block
        buffer = ReadBuffer(rel, blockNum - 1);
        blockNum = BufferGetBlockNumber(buffer);
    }

    // Lock the buffer exclusively so no one else writes to it at the same time
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(buffer);

    // If the page is brand new, initialize its header
    if (PageIsNew(page)) {
        PageInit(page, BufferGetPageSize(buffer), 0);
    }

    // Attempt to add the item to the page
    offnum = PageAddItem(page, (Item) data, data_size, InvalidOffsetNumber, false, false);

    if (offnum == InvalidOffsetNumber) {
        // The page is full! We need to extend the file and try again.
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

    // Record the physical address
    ItemPointerSet(out_tid, blockNum, offnum);

    // Mark the buffer dirty so Postgres knows it needs to be written to disk
    MarkBufferDirty(buffer);
    
    // (Note: WAL logging code would go exactly here before unlocking)

    UnlockReleaseBuffer(buffer);
}
static void 
np_linked_list_tableam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
                                    int options, BulkInsertState bistate) 
{
    NeoLinkedListRecord rec;
    ItemPointerData new_tid;

    /* 1. Force the virtual slot to unpack its Datums */
    slot_getallattrs(slot);

    /* 2. Populate the Micro-MVCC Header */
    rec.xmin = GetTopFullTransactionId();
    rec.cmin = cid;
    rec.xmax = InvalidFullTransactionId;
    rec.cmax = InvalidCommandId;

    /* 3. Populate the Data Payload (10 columns) */
    rec.id = DatumGetInt64(slot->tts_values[0]);
    rec.edge_lid = DatumGetInt32(slot->tts_values[1]);
    rec.dir = DatumGetChar(slot->tts_values[2]);
    
    rec.owner_id = DatumGetInt64(slot->tts_values[3]);    /* NEW: owner_id */
    
    rec.other_id = DatumGetInt64(slot->tts_values[4]);
    rec.other_lid = DatumGetInt32(slot->tts_values[5]);
    
    rec.next_tbl = DatumGetObjectId(slot->tts_values[6]);
    ItemPointer n_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[7]);
    rec.next_itemptr = *n_ptr;
    
    rec.prev_tbl = DatumGetObjectId(slot->tts_values[8]);
    ItemPointer p_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[9]);
    rec.prev_itemptr = *p_ptr;

    /* 4. Write the raw C struct directly to a Postgres 8KB buffer */
    /* (Using the np_write_record_to_page function we defined earlier) */
    np_write_record_to_page(relation, (char *) &rec, sizeof(NeoLinkedListRecord), &new_tid);

    /* 5. Update the slot with the newly assigned physical address */
    slot->tts_tableOid = RelationGetRelid(relation);
    ItemPointerCopy(&new_tid, &slot->tts_tid);
}

/*
 * Helper function to physically overwrite a record in a Postgres disk buffer.
 */
static void
np_overwrite_record_in_page(Relation rel, ItemPointer tid, NeoLinkedListRecord *new_data)
{
    Buffer buffer;
    Page page;
    ItemId lp;
    NeoLinkedListRecord *disk_rec;

    /* Read the specific block that holds the old tuple */
    buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
    
    /* Lock it exclusively so no one reads it while we are writing */
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(buffer);
    
    /* Find the exact line pointer for our record */
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));

    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: attempted to in-place update an invalid or dead tuple");
    }

    /* Cast the raw page memory directly to our C struct */
    disk_rec = (NeoLinkedListRecord *) PageGetItem(page, lp);

    /* 
     * CRITICAL: Because this is an IN-PLACE update, we must preserve 
     * the original Micro-MVCC header. If we overwrite xmin with a new one,
     * older transactions will suddenly think this row is invisible!
     */
    new_data->xmin = disk_rec->xmin;
    new_data->cmin = disk_rec->cmin;
    new_data->xmax = disk_rec->xmax;
    new_data->cmax = disk_rec->cmax;

    /* Overwrite the 45-byte disk record with our newly formed stack record */
    memcpy(disk_rec, new_data, sizeof(NeoLinkedListRecord));

    /* Mark the buffer dirty so the background writer flushes it to disk */
    MarkBufferDirty(buffer);
    
    /* (WAL logging for the update would go here) */

    UnlockReleaseBuffer(buffer);
}

TM_Result
np_linked_list_tableam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                    CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                                    bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
                                    TU_UpdateIndexes *update_indexes)
{
    NeoLinkedListRecord rec;

    /* 1. Force the virtual slot to unpack its Datums */
    slot_getallattrs(slot);

    /* 2. Populate our raw C struct payload from the 10 columns */
    rec.id = DatumGetInt64(slot->tts_values[0]);
    rec.edge_lid = DatumGetInt32(slot->tts_values[1]);
    rec.dir = DatumGetChar(slot->tts_values[2]);
    rec.owner_id = DatumGetInt64(slot->tts_values[3]);
    rec.other_id = DatumGetInt64(slot->tts_values[4]);
    rec.other_lid = DatumGetInt32(slot->tts_values[5]);
    
    rec.next_tbl = DatumGetObjectId(slot->tts_values[6]);
    ItemPointer n_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[7]);
    rec.next_itemptr = *n_ptr;
    
    rec.prev_tbl = DatumGetObjectId(slot->tts_values[8]);
    ItemPointer p_ptr = (ItemPointer) DatumGetPointer(slot->tts_values[9]);
    rec.prev_itemptr = *p_ptr;

    /* 3. Perform the in-place physical overwrite on the disk page */
    np_overwrite_record_in_page(relation, otid, &rec);

    /* 4. Update the slot to reflect the physical address (which didn't change) */
    slot->tts_tableOid = RelationGetRelid(relation);
    ItemPointerCopy(otid, &slot->tts_tid);
    
    /* We don't use standard indexes, so tell the executor not to bother */
    *update_indexes = TU_None; 

    return TM_Ok;
}

static bool
np_linked_list_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
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
np_linked_list_heap_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
    Assert(ItemPointerIsValid(tid));
    return;
}

static bool
np_linked_list_heapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
    HeapScanDesc hscan = (HeapScanDesc) scan;
    return ItemPointerIsValid(tid) && ItemPointerGetBlockNumber(tid) < hscan->rs_nblocks;
}

static inline bool
np_record_satisfies_snapshot(NeoLinkedListRecord *rec, Snapshot snapshot)
{
    FullTransactionId f_xmin = rec->xmin;
    FullTransactionId f_xmax = rec->xmax;
    
    TransactionId xmin_32 = XidFromFullTransactionId(f_xmin);
    TransactionId xmax_32 = XidFromFullTransactionId(f_xmax);

    FullTransactionId next_fxid = ReadNextFullTransactionId();
    FullTransactionId freeze_horizon = FullXidRelativeTo(next_fxid, snapshot->xmin);

    /* RULE 1: Creation Visibility */
    if (!FullTransactionIdIsValid(f_xmin)) return false;
    
    if (FullTransactionIdPrecedes(f_xmin, freeze_horizon)) {
        /* Logical freeze - committed */
    } else if (TransactionIdIsCurrentTransactionId(xmin_32)) {
        if (rec->cmin >= snapshot->curcid) return false;
    } else if (XidInMVCCSnapshot(xmin_32, snapshot)) {
        return false;
    } else if (!TransactionIdDidCommit(xmin_32)) {
        return false;
    }

    /* RULE 2: Deletion Visibility */
    if (FullTransactionIdIsValid(f_xmax)) {
        if (FullTransactionIdPrecedes(f_xmax, freeze_horizon)) {
            return false; /* Dead */
        } else if (TransactionIdIsCurrentTransactionId(xmax_32)) {
            if (rec->cmax >= snapshot->curcid) return true; 
            else return false;
        } else if (!XidInMVCCSnapshot(xmax_32, snapshot) && TransactionIdDidCommit(xmax_32)) {
            return false;
        }
    }

    return true;
}

static bool
np_linked_list_tableam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
    NeoTupleTableSlot *nslot = (NeoTupleTableSlot *) slot;
    NeoLinkedListRecord *rec = (NeoLinkedListRecord *) nslot->tuple->t_data;

    return np_record_satisfies_snapshot(rec, snapshot);
}

static TableScanDesc
np_linked_list_tableam_beginscan(Relation relation, Snapshot snapshot, int nkeys,
                     struct ScanKeyData *key, ParallelTableScanDesc pscan,
                     uint32 flags)
{
    NpScanDesc npscan = (NpScanDesc) palloc0(sizeof(NpScanDescData));

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
np_linked_list_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
                                TupleTableSlot *slot)
{
    NpScanDesc npscan = (NpScanDesc) sscan;
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
                NeoLinkedListRecord *rec = (NeoLinkedListRecord *) PageGetItem(page, lp);

                if (np_record_satisfies_snapshot(rec, snapshot))
                {
                    ExecClearTuple(slot);
                    memset(slot->tts_isnull, false, 10 * sizeof(bool));

                    slot->tts_values[0] = Int64GetDatum(rec->id);
                    slot->tts_values[1] = Int32GetDatum(rec->edge_lid);
                    slot->tts_values[2] = CharGetDatum(rec->dir);
                    slot->tts_values[3] = Int64GetDatum(rec->owner_id);
                    slot->tts_values[4] = Int64GetDatum(rec->other_id);
                    slot->tts_values[5] = Int32GetDatum(rec->other_lid);
                    slot->tts_values[6] = ObjectIdGetDatum(rec->next_tbl);
                    npscan->curr_next_ip = rec->next_itemptr;
                    slot->tts_values[7] = PointerGetDatum(&npscan->curr_next_ip);
                    slot->tts_values[8] = ObjectIdGetDatum(rec->prev_tbl);
                    npscan->curr_prev_ip = rec->prev_itemptr;
                    slot->tts_values[9] = PointerGetDatum(&npscan->curr_prev_ip);

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
np_linked_list_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
               bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    NpScanDesc npscan = (NpScanDesc) sscan;
    npscan->curr_block  = 0;
    npscan->curr_offset = FirstOffsetNumber;
}

static void
np_linked_list_scan_end(TableScanDesc sscan)
{
    pfree(sscan);
}


static IndexFetchTableData *
np_linked_list_index_fetch_begin(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return NULL;
}

static void
np_linked_list_index_fetch_reset(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static void
np_linked_list_index_fetch_end(IndexFetchTableData *scan)
{
    pg_unreachable();
}

static bool
np_linked_list_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
    pg_unreachable();
}

/* PG15+ Signatures */
static bool
np_linked_list_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream)
{
    return false;
}

static bool
np_linked_list_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin, double *liverows, double *deadrows, TupleTableSlot *slot)
{
    return false;
}

static bool
np_linked_list_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *call_again, uint64 *losers, uint64 *mutated)
{
    pg_unreachable();
}

static bool
np_linked_list_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TABLESAMPLE disabled.")));
    return false;
}

static bool
np_linked_list_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
    pg_unreachable();
}

static Size
np_linked_list_parallelscan_estimate(Relation rel)
{
    return 0;
}

static Size
np_linked_list_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
    return 0;
}

static void
np_linked_list_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
}

static bool
np_linked_list_relation_needs_toast_table(Relation rel)
{
    return false;
}

static Oid
np_linked_list_relation_toast_am(Relation rel)
{
    return InvalidOid;
}

static void
np_linked_list_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 curchunk, int32 chunksize, int32 requestsize, struct varlena *result)
{
    pg_unreachable();
}

static void
np_linked_list_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy)
{
}

static void
np_linked_list_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

    if (persistence != RELPERSISTENCE_PERMANENT)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("NeoPostGraph: Only permanent relations are supported for linked list tables")));

	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	smgrclose(srel);
}

static void
np_linked_list_relation_nontransactional_truncate(Relation rel)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TRUNCATE disabled.")));
}

static void
np_linked_list_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
    pg_unreachable();
}

static void
np_linked_list_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: CLUSTER disabled.")));
}

static double
np_linked_list_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: Indexes disabled.")));
    return 0;
}

static void
np_linked_list_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state)
{
    pg_unreachable();
}

static TM_Result
np_linked_list_tableam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: DELETE disabled.")));
    return TM_Ok;
}

static void
np_linked_list_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: UPSERT disabled.")));
}

static void
np_linked_list_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
    pg_unreachable();
}

static TM_Result
np_linked_list_tableam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: FOR UPDATE disabled.")));
    return TM_Ok;
}

static void
np_linked_list_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples, CommandId cid, int options, BulkInsertState bistate)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: SQL multi-insert disabled.")));
}

static void
np_linked_list_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: TID Range Scans disabled.")));
}

static bool
np_linked_list_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
    pg_unreachable();
}

static TransactionId
np_linked_list_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    pg_unreachable();
}

static void
np_linked_list_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples, double *allvisfrac)
{
    *pages = 0;
    *tuples = 0;
    *allvisfrac = 1.0;
}

static const TableAmRoutine np_linked_list_mutable_methods = {
  .type = T_TableAmRoutine,

  .slot_callbacks = np_linked_list_slot_callbacks,
  .tuple_fetch_row_version = np_linked_list_tuple_fetch_row_version,
  .tuple_insert = np_linked_list_tableam_tuple_insert,
  .tuple_update = np_linked_list_tableam_tuple_update,
  .tuple_tid_valid = np_linked_list_heapam_tuple_tid_valid,
  .tuple_get_latest_tid = np_linked_list_heap_get_latest_tid,
  .tuple_satisfies_snapshot = np_linked_list_tableam_tuple_satisfies_snapshot,
  .relation_size = table_block_relation_size,
  .relation_estimate_size = np_linked_list_estimate_rel_size,

  .tuple_delete = np_linked_list_tableam_tuple_delete,
  .tuple_insert_speculative = np_linked_list_tuple_insert_speculative,
  .tuple_complete_speculative = np_linked_list_tuple_complete_speculative,
  .multi_insert = np_linked_list_multi_insert,
  .tuple_lock = np_linked_list_tableam_tuple_lock,

  .scan_begin = np_linked_list_tableam_beginscan,
  .scan_end = np_linked_list_scan_end,
  .scan_rescan = np_linked_list_scan_rescan,
  .scan_getnextslot = np_linked_list_scan_getnextslot,
  .scan_set_tidrange = np_linked_list_scan_set_tidrange,
  .scan_getnextslot_tidrange = np_linked_list_scan_getnextslot_tidrange,
  .scan_bitmap_next_tuple = np_linked_list_scan_bitmap_next_tuple,
  .scan_sample_next_block = np_linked_list_scan_sample_next_block,
  .scan_sample_next_tuple = np_linked_list_scan_sample_next_tuple,

  .relation_vacuum = np_linked_list_relation_vacuum,
  .scan_analyze_next_block = np_linked_list_scan_analyze_next_block,
  .scan_analyze_next_tuple = np_linked_list_scan_analyze_next_tuple,
  .index_build_range_scan = np_linked_list_index_build_range_scan,
  .index_validate_scan = np_linked_list_index_validate_scan,
  .index_fetch_begin = np_linked_list_index_fetch_begin,
  .index_fetch_reset = np_linked_list_index_fetch_reset,
  .index_fetch_end = np_linked_list_index_fetch_end,
  .index_fetch_tuple = np_linked_list_index_fetch_tuple,
  .index_delete_tuples = np_linked_list_index_delete_tuples,

  .parallelscan_estimate = np_linked_list_parallelscan_estimate,
  .parallelscan_initialize = np_linked_list_parallelscan_initialize,
  .parallelscan_reinitialize = np_linked_list_parallelscan_reinitialize,
  .relation_needs_toast_table = np_linked_list_relation_needs_toast_table,
  .relation_toast_am = np_linked_list_relation_toast_am,
  .relation_fetch_toast_slice = np_linked_list_relation_fetch_toast_slice,
  .relation_set_new_filelocator = np_linked_list_relation_set_new_filelocator,
  .relation_nontransactional_truncate = np_linked_list_relation_nontransactional_truncate,
  .relation_copy_data = np_linked_list_relation_copy_data,
  .relation_copy_for_cluster = np_linked_list_relation_copy_for_cluster
};

PG_FUNCTION_INFO_V1(np_linked_list_mutable_handler);
Datum
np_linked_list_mutable_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&np_linked_list_mutable_methods);
}