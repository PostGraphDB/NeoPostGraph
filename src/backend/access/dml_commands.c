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

#include <string.h>
#include <assert.h>

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/heapam.h"
#include "access/hio.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "varatt.h"

#include "utils/np_cache.h"
#include "utils/gtype.h"
#include "utils/dictionary.h"
#include "utils/vertex.h"
#include "catalog/np_label.h"
#include "access/np_mutable.h"
#include "utils/edge.h"

static Datum
itemptr_to_bytea(ItemPointer iptr);
void np_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid);
void np_heap_insert(Relation relation, HeapTuple tup, CommandId cid, int options, BulkInsertState bistate);

static void insert_edge_one_direction(vertex *owner_v,
                                      vertex *other_v,
                                      edge *e,
                                      uint8 direction,
                                      CommandId cid);

static Oid get_active_linked_list_oid(Oid linked_list_meta_oid);

static ItemPointerData get_current_head_tid(Relation pmap_rel, uint64 vertex_id);

static void update_edge_prev_pointer(Relation list_rel,
                                     ItemPointer old_head_tid,
                                     ItemPointer new_tid,
                                     CommandId cid);


PG_FUNCTION_INFO_V1(insert_vertex);
Datum
insert_vertex(PG_FUNCTION_ARGS)
{
    vertex *v = NP_GET_ARG_VERTEX(0);
    CommandId cid = GetCurrentCommandId(true);

    const label_cache_data *label_cache =
        search_vertex_label_graph_id_label_id_cache(v->graph_id, v->label_id);

    if (!label_cache)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("label not found: graph_id=%d, label_id=%d",
                        v->graph_id, v->label_id)));

    Relation rel;
    HeapTuple tup;
    Datum values[2];
    bool nulls[2] = {false, false};
    ItemPointerData v_itemptr;

    /* === 1. Insert into main vertex table === */
    rel = table_open(label_cache->vertex_tbl, RowExclusiveLock);

    values[0] = Int64GetDatum(v->id);
    values[1] = PointerGetDatum(v);

    tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
    tup->t_tableOid = RelationGetRelid(rel);

    np_heap_insert(rel, tup, cid, 0, NULL);

    v_itemptr = tup->t_self;

    table_close(rel, RowExclusiveLock);

    /* === 2. Insert into phys_map === */
    Relation pmap_rel;
    HeapTuple pmap_tup;
    Datum pmap_values[3];
    bool pmap_nulls[3] = {false, true, true};

    pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);

    pmap_values[0] = itemptr_to_bytea(&v_itemptr);
    pmap_values[1] = (Datum)0;
    pmap_values[2] = (Datum)0;

    pmap_tup = heap_form_tuple(RelationGetDescr(pmap_rel), pmap_values, pmap_nulls);
    pmap_tup->t_tableOid = RelationGetRelid(pmap_rel);

    np_heap_insert(pmap_rel, pmap_tup, cid, 0, NULL);

    table_close(pmap_rel, RowExclusiveLock);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(insert_edge);
Datum
insert_edge(PG_FUNCTION_ARGS)
{
    vertex *start_v = NP_GET_ARG_VERTEX(0);
    vertex *end_v   = NP_GET_ARG_VERTEX(1);
    edge   *e       = NP_GET_ARG_EDGE(2);

    CommandId cid = GetCurrentCommandId(true);

    /* === 1. Insert into the main per-label edge table === */
    const label_cache_data *edge_label =
        search_edge_label_graph_id_label_id_cache(e->graph_id, e->label_id);

    if (!edge_label || !OidIsValid(edge_label->vertex_tbl))
        ereport(ERROR, (errmsg("Edge label table not found")));

    Relation edge_rel = table_open(edge_label->vertex_tbl, RowExclusiveLock);

    HeapTuple edge_tup;
    Datum edge_values[2];
    bool edge_nulls[2] = {false, false};

    edge_values[0] = Int64GetDatum(e->id);
    edge_values[1] = PointerGetDatum(e);           // store the full 'edge' composite

    edge_tup = heap_form_tuple(RelationGetDescr(edge_rel), edge_values, edge_nulls);
    edge_tup->t_tableOid = RelationGetRelid(edge_rel);

    np_heap_insert(edge_rel, edge_tup, cid, 0, NULL);
    table_close(edge_rel, RowExclusiveLock);


    insert_edge_one_direction(start_v, end_v, e, 0, cid); // outgoing
    insert_edge_one_direction(end_v, start_v, e, 1, cid); // incoming

    PG_RETURN_VOID();
}

/*
 * np_calculate_tuples_per_page
 *
 * Calculates the exact number of fixed-width tuples that fit on a standard 
 * 8KB PostgreSQL page. This MUST match the behavior of RelationPutHeapTuple.
 */
uint32
np_calculate_tuples_per_page(Size payload_size)
{
    /* Standard MVCC Header (usually 24 bytes aligned) + Payload */
    Size tuple_size = MAXALIGN(SizeofHeapTupleHeader + payload_size);
    
    /* Each tuple requires a 4-byte Line Pointer */
    Size space_per_item = tuple_size + sizeof(ItemIdData);
    
    /* Available space = 8192 - 24 byte page header */
    Size available_space = BLCKSZ - SizeOfPageHeaderData;
    
    return available_space / space_per_item;
}

/*
 * np_id_to_tid
 *
 * The O(1) Positional Router. 
 * Converts a sequential 1-indexed ID directly into a physical disk pointer.
 */
void
np_id_to_tid(uint64 id, uint32 tuples_per_page, ItemPointerData *tid)
{
    uint64 zero_based_id;

    Assert(id > 0);
    zero_based_id = id - 1;

    BlockNumber block  = (BlockNumber)(zero_based_id / tuples_per_page);
    OffsetNumber offset = (OffsetNumber)((zero_based_id % tuples_per_page) + 1);

    ItemPointerSet(tid, block, offset);
}

void np_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid) {
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
np_heap_insert(Relation relation, HeapTuple tup, CommandId cid,
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
 * update_vertex_phys_map
 *
 * Uses positional math (np_id_to_tid) to directly locate the phys_map row
 * for a vertex and performs an in-place update of its edge pointer.
 */
static void
update_vertex_phys_map(Relation pmap_rel,
                       uint64 vertex_id,
                       ItemPointer new_edge_tid,
                       CommandId cid)
{
    TupleDesc tupdesc = RelationGetDescr(pmap_rel);

    Size payload_size = sizeof(ItemPointerData) + sizeof(Oid) + sizeof(ItemPointerData);
    uint32 tuples_per_page = np_calculate_tuples_per_page(payload_size);

    ItemPointerData target_tid;
    np_id_to_tid(vertex_id, tuples_per_page, &target_tid);

    /* Fetch old tuple */
    Buffer buffer = ReadBuffer(pmap_rel, ItemPointerGetBlockNumber(&target_tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&target_tid));

    HeapTupleData oldtup_data;
    oldtup_data.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    oldtup_data.t_len  = ItemIdGetLength(lp);
    oldtup_data.t_self = target_tid;
    oldtup_data.t_tableOid = RelationGetRelid(pmap_rel);

    HeapTuple oldtup = &oldtup_data;

    /* Build new tuple */
    bytea *e_itemptr_bytea = (bytea *)itemptr_to_bytea(new_edge_tid);

    Datum values[3];
    bool nulls[3];
    bool replace[3] = {false, true, true};

    values[1] = ObjectIdGetDatum(RelationGetRelid(pmap_rel));
    values[2] = PointerGetDatum(e_itemptr_bytea);

    HeapTuple newtup = heap_modify_tuple(oldtup, tupdesc, values, nulls, replace);

    /* === IMPORTANT: Release the lock before calling np_update_inplace === */
    UnlockReleaseBuffer(buffer);

    /* Now safe to call */
    np_update_inplace(pmap_rel, &target_tid, newtup, cid);
}
static Datum
itemptr_to_bytea(ItemPointer iptr)
{
    bytea *result;

    if (!ItemPointerIsValid(iptr))
        return (Datum)0;

    result = (bytea *) palloc(VARHDRSZ + sizeof(ItemPointerData));
    SET_VARSIZE(result, VARHDRSZ + sizeof(ItemPointerData));
    memcpy(VARDATA(result), iptr, sizeof(ItemPointerData));

    return PointerGetDatum(result);
}

static void
insert_edge_one_direction(vertex *owner_v,
                          vertex *other_v,
                          edge *e,
                          uint8 direction,
                          CommandId cid)
{
    const label_cache_data *owner_label =
        search_vertex_label_graph_id_label_id_cache(owner_v->graph_id, owner_v->label_id);

    if (!owner_label || !OidIsValid(owner_label->phys_map) || !OidIsValid(owner_label->linked_list_meta))
        ereport(ERROR, (errmsg("Missing phys_map or linked_list_meta")));

    Oid active_list_oid = get_active_linked_list_oid(owner_label->linked_list_meta);
    if (!OidIsValid(active_list_oid))
        ereport(ERROR, (errmsg("No active linked list")));

    Relation list_rel = table_open(active_list_oid, RowExclusiveLock);
    Relation pmap_rel = table_open(owner_label->phys_map, RowExclusiveLock);

    ItemPointerData old_head = get_current_head_tid(pmap_rel, owner_v->id);

    bytea *dir = (bytea *) palloc(VARHDRSZ + 1);
    SET_VARSIZE(dir, VARHDRSZ + 1);
    *VARDATA(dir) = direction;

    HeapTuple new_tup;
    Datum values[9];
    bool nulls[9] = {false};

    values[0] = Int64GetDatum(e->id);
    values[1] = Int32GetDatum(e->label_id);
    values[2] = PointerGetDatum(dir);
    values[3] = Int64GetDatum(other_v->id);
    values[4] = Int32GetDatum(other_v->label_id);

    if (ItemPointerIsValid(&old_head)) {
        values[5] = ObjectIdGetDatum(RelationGetRelid(list_rel));
        values[6] = PointerGetDatum(&old_head);
    } else {
        values[5] = ObjectIdGetDatum(InvalidOid);
        values[6] = (Datum)0;
    }

    values[7] = ObjectIdGetDatum(InvalidOid);
    values[8] = (Datum)0;

    new_tup = heap_form_tuple(RelationGetDescr(list_rel), values, nulls);
    new_tup->t_tableOid = RelationGetRelid(list_rel);

    np_heap_insert(list_rel, new_tup, cid, 0, NULL);
    ItemPointerData new_tid = new_tup->t_self;

    /* Update old head's prev pointer (if existed) */
    if (ItemPointerIsValid(&old_head)) {
        update_edge_prev_pointer(list_rel, &old_head, &new_tid, cid);
    }

    table_close(list_rel, RowExclusiveLock);

    /* Update phys_map */
    update_vertex_phys_map(pmap_rel, owner_v->id, &new_tid, cid);
    table_close(pmap_rel, RowExclusiveLock);
}


static void
update_edge_prev_pointer(Relation list_rel,
                         ItemPointer old_head_tid,
                         ItemPointer new_tid,
                         CommandId cid)
{
    HeapTuple oldtup, newtup;
    TupleDesc tupdesc = RelationGetDescr(list_rel);
    Datum values[9];
    bool nulls[9];
    bool replace[9] = {false};

    /* We need the old tuple. Since we have the TID, fetch it directly */
    Buffer buffer;
    Page page;
    ItemId lp;
    HeapTupleData oldtup_data;

    buffer = ReadBuffer(list_rel, ItemPointerGetBlockNumber(old_head_tid));
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(old_head_tid));

    oldtup_data.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    oldtup_data.t_len  = ItemIdGetLength(lp);
    oldtup_data.t_self = *old_head_tid;
    oldtup_data.t_tableOid = RelationGetRelid(list_rel);

    oldtup = &oldtup_data;

    replace[7] = true; // prev_tbl
    replace[8] = true; // prev_itemptr

    values[7] = ObjectIdGetDatum(RelationGetRelid(list_rel));
    values[8] = PointerGetDatum(new_tid);

    newtup = heap_modify_tuple(oldtup, tupdesc, values, nulls, replace);

    np_update_inplace(list_rel, old_head_tid, newtup, cid);

    UnlockReleaseBuffer(buffer);
}
/*
 * get_active_linked_list_oid
 *
 * Reads the linked_list_meta table and returns the OID of the
 * currently active linked list partition.
 */
static Oid
get_active_linked_list_oid(Oid linked_list_meta_oid)
{
    Relation meta_rel;
    SysScanDesc scan;
    HeapTuple tuple;
    Oid active_oid = InvalidOid;

    if (!OidIsValid(linked_list_meta_oid))
        return InvalidOid;

    meta_rel = table_open(linked_list_meta_oid, AccessShareLock);
    scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);

    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        bool isnull;
        bool active = DatumGetBool(heap_getattr(tuple, 3, RelationGetDescr(meta_rel), &isnull)); // column 3 = active

        if (active && !isnull)
        {
            active_oid = DatumGetObjectId(heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull)); // column 2 = tbl (regclass)
            break;
        }
    }

    systable_endscan(scan);
    table_close(meta_rel, AccessShareLock);

    return active_oid;
}

/*
 * get_current_head_tid
 *
 * Uses positional math to locate the phys_map row for a vertex
 * and returns the current head edge pointer (e_itemptr).
 */
static ItemPointerData
get_current_head_tid(Relation pmap_rel, uint64 vertex_id)
{
    ItemPointerData head_tid = {0};   // returns invalid TID if no head
    TupleDesc tupdesc = RelationGetDescr(pmap_rel);
    bool isnull;

    /* Calculate how many tuples fit per page in phys_map */
    Size payload_size = sizeof(ItemPointerData) + sizeof(Oid) + sizeof(ItemPointerData);
    uint32 tuples_per_page = np_calculate_tuples_per_page(payload_size);

    /* Compute exact physical TID of this vertex's row in phys_map */
    ItemPointerData target_tid;
    np_id_to_tid(vertex_id, tuples_per_page, &target_tid);

    /* Fetch the tuple directly */
    Buffer buffer = ReadBuffer(pmap_rel, ItemPointerGetBlockNumber(&target_tid));
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buffer);
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&target_tid));

    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        return head_tid;
    }

    HeapTupleData tup_data;
    tup_data.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    tup_data.t_len  = ItemIdGetLength(lp);
    tup_data.t_self = target_tid;
    tup_data.t_tableOid = RelationGetRelid(pmap_rel);

    /* Get e_itemptr (column 3) safely */
    Datum e_itemptr_datum = heap_getattr(&tup_data, 3, tupdesc, &isnull);

    if (!isnull && e_itemptr_datum != 0) {
        bytea *e_itemptr_bytea = DatumGetByteaPCopy(e_itemptr_datum);

        if (VARSIZE_ANY_EXHDR(e_itemptr_bytea) == sizeof(ItemPointerData)) {
            memcpy(&head_tid, VARDATA_ANY(e_itemptr_bytea), sizeof(ItemPointerData));
        }
    }

    UnlockReleaseBuffer(buffer);
    return head_tid;
}
