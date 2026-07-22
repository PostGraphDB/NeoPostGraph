#include "postgres.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "commands/defrem.h"
#include "nodes/execnodes.h"

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

#include "access/np_entity_store.h"

struct ReadStream;

static const TupleTableSlotOps *
np_entity_store_slot_callbacks(Relation rel)
{
    return &TTSOpsVirtual;
}

static bool
np_entity_store_relation_needs_toast_table(Relation rel)
{
    return false;
}

static Oid
np_entity_store_relation_toast_am(Relation rel)
{
    return InvalidOid;
}

static void
np_entity_store_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 attrsize,
                                           int32 sliceoffset, int32 slicelength,
                                           struct varlena *result)
{
    ereport(ERROR, (errmsg("np_entity_store: toast slice fetch not implemented")));
}

static uint64
np_entity_store_relation_size(Relation rel, ForkNumber forkNumber)
{
    if (!smgrexists(RelationGetSmgr(rel), forkNumber))
        return 0;
    return smgrnblocks(RelationGetSmgr(rel), forkNumber) * BLCKSZ;
}

static void
np_entity_store_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages,
                                  double *tuples, double *allvisfrac)
{
    *pages = RelationGetNumberOfBlocks(rel);
    *tuples = (*pages) * 100;
    *allvisfrac = 1.0;
}

static TableScanDesc
np_entity_store_scan_begin(Relation rel, Snapshot snapshot, int nkeys, struct ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
    NPEntityScanDesc scan = (NPEntityScanDesc) palloc0(sizeof(NPEntityScanDescData));

    scan->rs_base.rs_rd = rel;
    scan->rs_base.rs_snapshot = snapshot;
    scan->rs_base.rs_nkeys = nkeys;
    scan->rs_base.rs_flags = flags;
    
    scan->rs_nblocks = RelationGetNumberOfBlocks(rel);
    scan->rs_cblock = 0;
    scan->rs_coffset = FirstOffsetNumber;
    scan->rs_cbuf = InvalidBuffer;

    return &scan->rs_base;
}

static void
np_entity_store_scan_end(TableScanDesc sscan)
{
    NPEntityScanDesc scan = (NPEntityScanDesc) sscan;

    if (BufferIsValid(scan->rs_cbuf))
        ReleaseBuffer(scan->rs_cbuf);

    pfree(scan);
}

static void
np_entity_store_scan_rescan(TableScanDesc sscan, struct ScanKeyData *key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    NPEntityScanDesc scan = (NPEntityScanDesc) sscan;

    if (BufferIsValid(scan->rs_cbuf))
    {
        ReleaseBuffer(scan->rs_cbuf);
        scan->rs_cbuf = InvalidBuffer;
    }

    scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);
    scan->rs_cblock = 0;
    scan->rs_coffset = FirstOffsetNumber;
}

static bool
np_entity_store_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
    NPEntityScanDesc scan = (NPEntityScanDesc) sscan;

    for (;;)
    {
        if (!BufferIsValid(scan->rs_cbuf))
        {
            if (scan->rs_cblock >= scan->rs_nblocks)
                return false; /* End of scan */

            scan->rs_cbuf = ReadBuffer(sscan->rs_rd, scan->rs_cblock);
            scan->rs_coffset = FirstOffsetNumber;
        }

        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(scan->rs_cbuf);
        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        while (scan->rs_coffset <= maxoff)
        {
            ItemId lp = PageGetItemId(page, scan->rs_coffset);
            OffsetNumber current_offset = scan->rs_coffset;
            scan->rs_coffset++;

            if (ItemIdIsNormal(lp))
            {
                NPEntityTupleHeader hdr = (NPEntityTupleHeader) PageGetItem(page, lp);

                ExecClearTuple(slot);

                slot->tts_values[0] = Int64GetDatum(hdr->id);
                slot->tts_isnull[0] = false;

                slot->tts_values[1] = PointerGetDatum(hdr->serialized_entity);
                slot->tts_isnull[1] = false;

                ExecStoreVirtualTuple(slot);
                ItemPointerSet(&slot->tts_tid, scan->rs_cblock, current_offset);

                LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
                return true;
            }
        }

        UnlockReleaseBuffer(scan->rs_cbuf);
        scan->rs_cbuf = InvalidBuffer;
        scan->rs_cblock++;
    }
}

static bool np_entity_store_scan_bitmap_next_tuple(TableScanDesc sscan, TupleTableSlot *slot, bool *skip, uint64 *c, uint64 *d) { return false; }

static bool np_entity_store_scan_sample_next_block(TableScanDesc sscan, struct SampleScanState *scanstate) { return false; }
static bool np_entity_store_scan_sample_next_tuple(TableScanDesc sscan, struct SampleScanState *scanstate, TupleTableSlot *slot) { return false; }
static IndexFetchTableData *np_entity_store_index_fetch_begin(Relation rel) {
    ereport(ERROR, (errmsg("np_entity_store: index_fetch_begin not implemented"))); return NULL;
}
static void np_entity_store_index_fetch_reset(IndexFetchTableData *scan) {}
static void np_entity_store_index_fetch_end(IndexFetchTableData *scan) {}
static bool np_entity_store_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *call_again, bool *all_dead) { return false; }
static void np_entity_store_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate) {
    ereport(ERROR, (errmsg("np_entity_store: tuple_insert not implemented")));
}
static void np_entity_store_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, int options, BulkInsertState bistate, uint32 specToken) {
    ereport(ERROR, (errmsg("np_entity_store: tuple_insert_speculative not implemented")));
}
static void np_entity_store_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded) {}
static void np_entity_store_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, int options, BulkInsertState bistate) {
    ereport(ERROR, (errmsg("np_entity_store: multi_insert not implemented")));
}
static TM_Result np_entity_store_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, bool changingPart) {
    ereport(ERROR, (errmsg("np_entity_store: tuple_delete not implemented"))); return TM_Invisible;
}
static TM_Result np_entity_store_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes) {
    ereport(ERROR, (errmsg("np_entity_store: tuple_update not implemented"))); return TM_Invisible;
}
static TM_Result np_entity_store_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd) {
    ereport(ERROR, (errmsg("np_entity_store: tuple_lock not implemented"))); return TM_Invisible;
}

static bool np_entity_store_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot) { return false; }
static void np_entity_store_tuple_get_latest_tid(TableScanDesc sscan, ItemPointer tid) {}
static bool np_entity_store_tuple_tid_valid(TableScanDesc scan, ItemPointer tid) { return false; }
static bool np_entity_store_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot) { return true; }

static void np_entity_store_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence, TransactionId *freezeXid, MultiXactId *minmulti) {
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
static void np_entity_store_relation_nontransactional_truncate(Relation rel) {}
static void np_entity_store_relation_copy_data(Relation rel, const RelFileLocator *newrlocator) {}
static void np_entity_store_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap, Relation OldIndex, bool use_sort, TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead) {}
static void np_entity_store_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy) {}
static bool np_entity_store_scan_analyze_next_block(TableScanDesc sscan, struct ReadStream *stream) { return false; }
static bool np_entity_store_scan_analyze_next_tuple(TableScanDesc sscan, TransactionId OldestXmin, double *deadrows, double *livetuples, TupleTableSlot *slot) { return false; }
static double np_entity_store_index_build_range_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, void (*callback)(Relation, ItemPointer, Datum *, bool *, bool, void *), void *callback_state, TableScanDesc scan) { return 0; }
static void np_entity_store_index_validate_scan(Relation heapRelation, Relation indexRelation, struct IndexInfo *indexInfo, Snapshot snapshot, struct ValidateIndexState *state) {}

static Size np_entity_store_parallelscan_estimate(Relation rel) { 
    return 0; 
}
static Size np_entity_store_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan) { 
    return 0; 
}
static void np_entity_store_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan) {}

static void np_entity_store_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid, ItemPointer maxtid) {}
static bool np_entity_store_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot) { 
    return false; 
}
static TransactionId np_entity_store_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate) {
    ereport(ERROR, (errmsg("np_entity_store: index_delete_tuples not implemented")));
    return InvalidTransactionId;
}
static const TableAmRoutine np_entity_store_methods = {
    .type = T_TableAmRoutine,

    .slot_callbacks = np_entity_store_slot_callbacks,

    .scan_begin = np_entity_store_scan_begin,
    .scan_end = np_entity_store_scan_end,
    .scan_rescan = np_entity_store_scan_rescan,
    .scan_getnextslot = np_entity_store_scan_getnextslot,
    .scan_set_tidrange = np_entity_store_scan_set_tidrange,
    .scan_getnextslot_tidrange = np_entity_store_scan_getnextslot_tidrange,

    .parallelscan_estimate = np_entity_store_parallelscan_estimate,
    .parallelscan_initialize = np_entity_store_parallelscan_initialize,
    .parallelscan_reinitialize = np_entity_store_parallelscan_reinitialize,
    .scan_bitmap_next_tuple = np_entity_store_scan_bitmap_next_tuple,
    .index_delete_tuples = np_entity_store_index_delete_tuples,
    .scan_sample_next_block = np_entity_store_scan_sample_next_block,
    .scan_sample_next_tuple = np_entity_store_scan_sample_next_tuple,

    .index_fetch_begin = np_entity_store_index_fetch_begin,
    .index_fetch_reset = np_entity_store_index_fetch_reset,
    .index_fetch_end = np_entity_store_index_fetch_end,
    .index_fetch_tuple = np_entity_store_index_fetch_tuple,

    .tuple_insert = np_entity_store_tuple_insert,
    .tuple_insert_speculative = np_entity_store_tuple_insert_speculative,
    .tuple_complete_speculative = np_entity_store_tuple_complete_speculative,
    .multi_insert = np_entity_store_multi_insert,
    .tuple_delete = np_entity_store_tuple_delete,
    .tuple_update = np_entity_store_tuple_update,
    .tuple_lock = np_entity_store_tuple_lock,

    .tuple_fetch_row_version = np_entity_store_tuple_fetch_row_version,
    .tuple_get_latest_tid = np_entity_store_tuple_get_latest_tid,
    .tuple_tid_valid = np_entity_store_tuple_tid_valid,
    .tuple_satisfies_snapshot = np_entity_store_tuple_satisfies_snapshot,

    .relation_set_new_filelocator = np_entity_store_relation_set_new_filelocator,
    
    .relation_nontransactional_truncate = np_entity_store_relation_nontransactional_truncate,
    .relation_copy_data = np_entity_store_relation_copy_data,
    
    .relation_copy_for_cluster = np_entity_store_relation_copy_for_cluster,
    .relation_vacuum = np_entity_store_relation_vacuum,
    .scan_analyze_next_block = np_entity_store_scan_analyze_next_block,
    .scan_analyze_next_tuple = np_entity_store_scan_analyze_next_tuple,
    .index_build_range_scan = np_entity_store_index_build_range_scan,
    .index_validate_scan = np_entity_store_index_validate_scan,

    .relation_size = np_entity_store_relation_size,
    .relation_needs_toast_table = np_entity_store_relation_needs_toast_table,
    .relation_toast_am = np_entity_store_relation_toast_am,
    .relation_fetch_toast_slice = np_entity_store_relation_fetch_toast_slice,
    .relation_estimate_size = np_entity_store_estimate_rel_size
};

PG_FUNCTION_INFO_V1(np_entity_store_handler);
Datum
np_entity_store_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&np_entity_store_methods);
}