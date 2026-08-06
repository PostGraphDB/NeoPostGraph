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
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"
#include "varatt.h"

#include "utils/np_cache.h"
#include "utils/gtype.h"
#include "utils/dictionary.h"
#include "utils/vertex.h"
#include "catalog/np_label.h"
#include "utils/adj_list.h"
#include "access/np_phys_map.h"
#include "utils/edge.h"

#include "access/np_linked_list.h"
#include "access/np_entity_store.h"
void
np_internal_delete_edge(int32 graph_id, int32 label_id, int64 edge_id, CommandId cid, FullTransactionId current_fxid);
static Datum
itemptr_to_bytea(ItemPointer iptr);
void np_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid);

static void insert_edge_one_direction(vertex *owner_v,
                                      vertex *other_v,
                                      edge *e,
                                      uint8 direction,
                                      CommandId cid);

static Oid get_active_linked_list_oid(Oid linked_list_meta_oid);

static ItemPointerData get_current_head_tid(Relation pmap_rel, uint64 vertex_id, Oid *head_tbl);

static void update_edge_prev_pointer(Relation list_rel,
                                     ItemPointer old_head_tid,
                                     Oid prev_table_oid,
                                     ItemPointer new_tid,
                                     CommandId cid);

void 
np_write_record_to_page(Relation rel, char *data, Size data_size, ItemPointer out_tid)
{
    Buffer buffer;
    Page page;
    OffsetNumber offnum;
    BlockNumber blockNum;
    GenericXLogState *state;
    int flags = 0;

    blockNum = RelationGetNumberOfBlocks(rel);
    
    if (blockNum == 0) {
        buffer = ReadBuffer(rel, P_NEW);
    } else {
        buffer = ReadBuffer(rel, blockNum - 1);
    }

    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(buffer);

    Size space_needed = MAXALIGN(data_size) + sizeof(ItemIdData);
    if (!PageIsNew(page) && PageGetFreeSpace(page) < space_needed) {
        UnlockReleaseBuffer(buffer);
        
        buffer = ReadBuffer(rel, P_NEW);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(buffer);
    }

    state = GenericXLogStart(rel);

    if (PageIsNew(page)) {
        flags = GENERIC_XLOG_FULL_IMAGE;
    }
    
    page = GenericXLogRegisterBuffer(state, buffer, flags);

    if (PageIsNew(page)) {
        PageInit(page, BufferGetPageSize(buffer), 0);
    }

    offnum = PageAddItem(page, (Item) data, data_size, InvalidOffsetNumber, false, false);

    if (offnum == InvalidOffsetNumber) {
        GenericXLogAbort(state);
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: Failed to add tuple to new page despite free space check");
    }

    GenericXLogFinish(state);

    ItemPointerSet(out_tid, BufferGetBlockNumber(buffer), offnum);
    UnlockReleaseBuffer(buffer);
}
static bytea *
make_itemptr_bytea(const ItemPointerData *ip)
{
    bytea *b = (bytea *) palloc(VARHDRSZ + sizeof(ItemPointerData));
    SET_VARSIZE(b, VARHDRSZ + sizeof(ItemPointerData));
    memcpy(VARDATA(b), ip, sizeof(ItemPointerData));
    return b;
}

PG_FUNCTION_INFO_V1(insert_vertex);
Datum
insert_vertex(PG_FUNCTION_ARGS)
{
    vertex *v = NP_GET_ARG_VERTEX(0);
ArrayType *input_annots = PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1);

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    const label_cache_data *label_cache =
        search_vertex_label_graph_id_label_id_cache(v->graph_id, v->label_id);

    if (!label_cache)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("label not found: graph_id=%d, label_id=%d",
                        v->graph_id, v->label_id)));

    /* 1. Open the custom entity_store table */
    Relation rel = table_open(label_cache->vertex_tbl, RowExclusiveLock);

    /* 
     * 2. Calculate explicit byte size.
     * Assuming 'vertex' is a standard varlena, VARSIZE() gives us the payload length.
     */
    Size payload_size = VARSIZE(v);
    Size total_tuple_size = MAXALIGN(SizeOfNPEntityTupleHeader + payload_size);
    
    /* 3. Allocate and format the custom physical tuple */
    char *tuple_buf = (char *) palloc0(total_tuple_size);
    NPEntityTupleHeader hdr = (NPEntityTupleHeader) tuple_buf;

    hdr->xmin = current_fxid;
    hdr->xmax = InvalidFullTransactionId;
    hdr->cmin = cid;
    hdr->cmax = InvalidCommandId;
    ItemPointerSetInvalid(&hdr->prev_itemptr);
    hdr->flags = 0;
    hdr->id = v->id;

    /* Drop the serialized vertex payload directly behind the 40-byte header */
    memcpy(hdr->serialized_entity, v, payload_size);

    /* 4. Write directly to the page using your existing primitive */
    ItemPointerData v_itemptr;
    np_write_record_to_page(rel, tuple_buf, total_tuple_size, &v_itemptr);

    pfree(tuple_buf);
    table_close(rel, RowExclusiveLock);


    /* 5. Process Annotations via Entity Store */
    ItemPointerData a_itemptr;
    ItemPointerSetInvalid(&a_itemptr); /* Default to safely Invalid */

    Oid annotations_tbl = label_cache->annotations_tbl;

    if (OidIsValid(annotations_tbl) && input_annots != NULL) {
        
        /* 
         * A. Load the LATEST Schema Array dynamically from the Schema Entity Store 
         * instead of using the frozen label_cache->annotation_map
         */
        ArrayType *map_array = NULL;
        const graph_cache_data *graph_cache = search_graph_id_cache(v->graph_id);

        if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
            uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
            ItemPointerData schema_pmap_tid;
            np_id_to_tid(v->label_id, pmap_tuples_per_page, &schema_pmap_tid);

            Relation schema_pmap_rel = table_open(graph_cache->annot_schema_phys_map, AccessShareLock);
            Buffer schema_pmap_buf = ReadBuffer(schema_pmap_rel, ItemPointerGetBlockNumber(&schema_pmap_tid));
            LockBuffer(schema_pmap_buf, BUFFER_LOCK_SHARE);
            Page schema_pmap_page = BufferGetPage(schema_pmap_buf);
            ItemId schema_pmap_lp = PageGetItemId(schema_pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));

            ItemPointerData latest_schema_tid;
            ItemPointerSetInvalid(&latest_schema_tid);
            if (ItemIdIsNormal(schema_pmap_lp)) {
                NeoPhysMapRecord *pmap_rec = (NeoPhysMapRecord *) PageGetItem(schema_pmap_page, schema_pmap_lp);
                latest_schema_tid = pmap_rec->v_itemptr; /* Pointer stored in v_itemptr */
            }
            UnlockReleaseBuffer(schema_pmap_buf);
            table_close(schema_pmap_rel, AccessShareLock);

            if (ItemPointerIsValid(&latest_schema_tid)) {
                Relation schema_rel = table_open(graph_cache->annot_schema_tbl, AccessShareLock);
                Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
                LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
                Page schema_page = BufferGetPage(schema_buf);
                ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));

                if (ItemIdIsNormal(schema_lp)) {
                    NPEntityTupleHeader schema_hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
                    map_array = DatumGetArrayTypePCopy(PointerGetDatum(schema_hdr->serialized_entity));
                }
                UnlockReleaseBuffer(schema_buf);
                table_close(schema_rel, AccessShareLock);
            }
        }

        /* Fallback to cache if entity store wasn't initialized yet */
        if (map_array == NULL) {
            map_array = label_cache->annotation_map;
        }

        if (map_array == NULL) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("This structural label does not support annotations")));
        }

        /* B. Calculate dynamic byte size from the loaded schema array */
        int num_labels = ArrayGetNItems(ARR_NDIM(map_array), ARR_DIMS(map_array));
        int byte_size = (num_labels + 7) / 8;

        /* Allocate a bytea for the bitset */
        bytea *bitset = (bytea *) palloc0(VARHDRSZ + byte_size);
        SET_VARSIZE(bitset, VARHDRSZ + byte_size);
        char *bits = VARDATA(bitset);

        /* C. Map inputs to bit positions */
        Datum *input_d, *map_d;
        bool *input_n, *map_n;
        int input_count, map_count;
        
        deconstruct_array(input_annots, TEXTOID, -1, false, 'i', &input_d, &input_n, &input_count);
        deconstruct_array(map_array, TEXTOID, -1, false, 'i', &map_d, &map_n, &map_count);
        
        for (int i = 0; i < input_count; i++) {
            if (input_n[i]) continue;
            char *in_str = TextDatumGetCString(input_d[i]);
            int bit_pos = -1;
            
            for (int j = 0; j < map_count; j++) {
                if (map_n[j]) continue;
                if (strcmp(in_str, TextDatumGetCString(map_d[j])) == 0) {
                    bit_pos = j;
                    break;
                }
            }
            
            if (bit_pos != -1) {
                /* Flip the bit for the matched annotation index */
                bits[bit_pos / 8] |= (1 << (bit_pos % 8));
            } else {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Annotation label '%s' is not valid for this structural label", in_str)));
            }
        }

        /* D. Wrap the bytea bitset in the Entity Store physical tuple layout */
        Size annot_payload_size = VARSIZE(bitset);
        Size annot_total_size = MAXALIGN(SizeOfNPEntityTupleHeader + annot_payload_size);
        
        char *annot_tuple_buf = (char *) palloc0(annot_total_size);
        NPEntityTupleHeader annot_hdr = (NPEntityTupleHeader) annot_tuple_buf;

        annot_hdr->xmin = current_fxid;
        annot_hdr->xmax = InvalidFullTransactionId;
        annot_hdr->cmin = cid;
        annot_hdr->cmax = InvalidCommandId;
        ItemPointerSetInvalid(&annot_hdr->prev_itemptr);
        annot_hdr->flags = 0;
        annot_hdr->id = v->id;

        memcpy(annot_hdr->serialized_entity, bitset, annot_payload_size);

        /* Write the annotation directly to the page using the Entity Store TAM */
        Relation annot_rel = table_open(annotations_tbl, RowExclusiveLock);
        np_write_record_to_page(annot_rel, annot_tuple_buf, annot_total_size, &a_itemptr);
        
        pfree(annot_tuple_buf);
        table_close(annot_rel, RowExclusiveLock);
        
    } else if (input_annots != NULL) {
        /* User passed annotations to a label that doesn't support them */
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("This structural label does not support annotations")));
    }


    /* 5. Update the phys_map with the new topology pointer */
    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);
    
    
    NeoPhysMapRecord rec = {
        .v_itemptr = v_itemptr,
        .e_tbl_id = InvalidOid,
        .a_itemptr = a_itemptr
    };
    ItemPointerSetInvalid(&rec.e_itemptr);


    ItemPointerData dummy_tid;
    np_write_record_to_page(pmap_rel, (char *) &rec, sizeof(NeoPhysMapRecord), &dummy_tid);

    table_close(pmap_rel, RowExclusiveLock);
    
    PG_RETURN_VOID();
}

static void
np_overwrite_edge_physmap_in_page(Relation rel, ItemPointer tid, NeoEdgePhysMapRecord *new_data)
{
    Buffer buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    
    GenericXLogState *state = GenericXLogStart(rel);
    Page page = GenericXLogRegisterBuffer(state, buffer, 0);
    
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));

    /* If the page is completely new, you might need PageAddItem logic here, 
       but assuming the file is pre-extended or you handle that in np_id_to_tid: */
    if (!ItemIdIsNormal(lp)) {
        GenericXLogAbort(state);
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: attempted to update invalid edge phys_map tuple");
    }

    NeoEdgePhysMapRecord *disk_rec = (NeoEdgePhysMapRecord *) PageGetItem(page, lp);

    /* Pure overwrite */
    memcpy(disk_rec, new_data, sizeof(NeoEdgePhysMapRecord));

    GenericXLogFinish(state);
    UnlockReleaseBuffer(buffer);
}

PG_FUNCTION_INFO_V1(insert_edge);
Datum
insert_edge(PG_FUNCTION_ARGS)
{
    vertex *start_v = NP_GET_ARG_VERTEX(0);
    vertex *end_v = NP_GET_ARG_VERTEX(1);
    edge *e = NP_GET_ARG_EDGE(2);

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    const label_cache_data *edge_label =
        search_edge_label_graph_id_label_id_cache(e->graph_id, e->label_id);

    /* (Note: I changed vertex_tbl to edge_tbl here just in case that was a typo in your struct access!) */
    if (!edge_label || !OidIsValid(edge_label->vertex_tbl))
        ereport(ERROR, (errmsg("Edge label table not found")));

    /* 1. Open the custom entity_store edge table */
    Relation edge_rel = table_open(edge_label->vertex_tbl, RowExclusiveLock);

    /* 2. Calculate explicit byte size. */
    Size payload_size = VARSIZE(e);
    Size total_tuple_size = MAXALIGN(SizeOfNPEntityTupleHeader + payload_size);

    /* 3. Allocate and format the custom physical tuple */
    char *tuple_buf = (char *) palloc0(total_tuple_size);
    NPEntityTupleHeader hdr = (NPEntityTupleHeader) tuple_buf;

    hdr->xmin = current_fxid;
    hdr->xmax = InvalidFullTransactionId;
    hdr->cmin = cid;
    hdr->cmax = InvalidCommandId;
    ItemPointerSetInvalid(&hdr->prev_itemptr);
    hdr->flags = 0;
    hdr->id = e->id;

    /* Drop the serialized edge payload directly behind the 40-byte header */
    memcpy(hdr->serialized_entity, e, payload_size);

    /* 4. Write directly to the page using your existing primitive */
    ItemPointerData edge_tid;
    np_write_record_to_page(edge_rel, tuple_buf, total_tuple_size, &edge_tid);

    pfree(tuple_buf);
    table_close(edge_rel, RowExclusiveLock);

    if (OidIsValid(edge_label->phys_map)) 
    {
        Relation pmap_rel = table_open(edge_label->phys_map, RowExclusiveLock);
        
        uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
        ItemPointerData phys_map_tid;
        np_id_to_tid(e->id, pmap_tuples_per_page, &phys_map_tid);

        NeoEdgePhysMapRecord pmap_rec;
        pmap_rec.e_itemptr = edge_tid;

        /* Safely Upsert: Extends file, pads array, and sets the pointer */
        np_set_edge_physmap_record(pmap_rel, &phys_map_tid, &pmap_rec);
        
        table_close(pmap_rel, RowExclusiveLock);
    }

    /* 5. Update the doubly-linked adjacency lists on the start and end vertices */
    insert_edge_one_direction(start_v, end_v, e, 0, cid);
    insert_edge_one_direction(end_v, start_v, e, 1, cid);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(update_edge);
Datum
update_edge(PG_FUNCTION_ARGS)
{
    int64 edge_id = PG_GETARG_INT64(0);
    int32 label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);
    gtype *new_properties = NP_GET_ARG_GTYPE_P(3); 

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    const label_cache_data *label_cache =
        search_edge_label_graph_id_label_id_cache(graph_id, label_id);

    if (!label_cache || !OidIsValid(label_cache->vertex_tbl))
        ereport(ERROR, (errmsg("edge label not found: graph_id=%d, label_id=%d", graph_id, label_id)));

    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);

    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(edge_id, pmap_tuples_per_page, &phys_map_tid);

    /* 2. Read the current phys_map record */
    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);

    /* If the file is completely empty, the vertex definitely doesn't exist */
    if (pmap_blk >= RelationGetNumberOfBlocks(pmap_rel)) {
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map (table empty)")));
    }

    Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    if (!ItemIdIsNormal(pmap_lp)) {
        UnlockReleaseBuffer(pmap_buf);
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map")));
    }

    NeoEdgePhysMapRecord *disk_pmap_rec = (NeoEdgePhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
    ItemPointerData old_edge_tid = disk_pmap_rec->e_itemptr;
    UnlockReleaseBuffer(pmap_buf);

    /* 3. Concurrency Check & Extract old edge data */
    Relation rel = table_open(label_cache->vertex_tbl, RowExclusiveLock);
    Buffer obuf_check = ReadBuffer(rel, ItemPointerGetBlockNumber(&old_edge_tid));
    LockBuffer(obuf_check, BUFFER_LOCK_SHARE);
    
    Page opage_check = BufferGetPage(obuf_check);
    ItemId olp_check = PageGetItemId(opage_check, ItemPointerGetOffsetNumber(&old_edge_tid));

    if (!ItemIdIsNormal(olp_check)) {
        UnlockReleaseBuffer(obuf_check);
        ereport(ERROR, (errmsg("Corrupted phys_map: Pointer to empty line pointer for edge %ld", edge_id)));
    }

    NPEntityTupleHeader old_hdr_check = (NPEntityTupleHeader) PageGetItem(opage_check, olp_check);
    if (FullTransactionIdIsValid(old_hdr_check->xmax)) {
        UnlockReleaseBuffer(obuf_check);
        ereport(ERROR, (errmsg("Edge ID %ld was concurrently deleted or updated", edge_id)));
    }

    edge *old_e = (edge *) old_hdr_check->serialized_entity;
    
    // TODO: Dictionary Compression
    int16 current_dict_id = old_e->dictionary_id; 
    
    int64 start_vid = old_e->start_id;  
    int64 end_vid = old_e->end_id;     


    UnlockReleaseBuffer(obuf_check);

    Size gt_size = VARSIZE(new_properties);
    Size fixed_size = offsetof(edge, props); 
    
    edge *new_e = (edge *) palloc(fixed_size + gt_size);
    memcpy(new_e, old_e, fixed_size);

    memcpy((char *)new_e + fixed_size, &new_properties->root, gt_size);

    SET_VARSIZE(new_e, fixed_size + gt_size);

    Size actual_payload_size = VARSIZE(new_e);
    Size total_tuple_size = MAXALIGN(SizeOfNPEntityTupleHeader + actual_payload_size);
    
    char *tuple_buf = (char *) palloc0(total_tuple_size);
    NPEntityTupleHeader new_hdr = (NPEntityTupleHeader) tuple_buf;

    new_hdr->xmin = current_fxid;
    new_hdr->xmax = InvalidFullTransactionId;
    new_hdr->cmin = cid;
    new_hdr->cmax = InvalidCommandId;
    new_hdr->flags = 0;
    new_hdr->id = edge_id;
    new_hdr->prev_itemptr = old_edge_tid; 
    
    memcpy(new_hdr->serialized_entity, new_e, actual_payload_size);
    pfree(new_e);

    ItemPointerData new_edge_tid;
    np_write_record_to_page(rel, tuple_buf, total_tuple_size, &new_edge_tid);
    pfree(tuple_buf);

    NeoEdgePhysMapRecord pmap_rec;
    pmap_rec.e_itemptr = new_edge_tid;
    np_set_edge_physmap_record(pmap_rel, &phys_map_tid, &pmap_rec);
    table_close(pmap_rel, RowExclusiveLock);

    Buffer obuf_final = ReadBuffer(rel, ItemPointerGetBlockNumber(&old_edge_tid));
    LockBuffer(obuf_final, BUFFER_LOCK_EXCLUSIVE);
    
    GenericXLogState *state = GenericXLogStart(rel);
    Page wal_page = GenericXLogRegisterBuffer(state, obuf_final, 0);
    
    ItemId olp_final = PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&old_edge_tid));
    NPEntityTupleHeader wal_old_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, olp_final);
    
    wal_old_hdr->xmax = current_fxid;
    wal_old_hdr->cmax = cid;
    
    GenericXLogFinish(state);
    
    UnlockReleaseBuffer(obuf_final);
    table_close(rel, RowExclusiveLock);

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

    if (!ItemIdIsNormal(lp)) {
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

    if (result == TM_SelfModified)
        result = TM_Ok;

    if (result != TM_Ok) {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("NeoPostGraph: Unexpected visibility failure (%d) during in-place overwrite.", result)));
    }

    if (newtup->t_len != oldtup.t_len) {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: In-place update requires identical raw tuple sizes.%i %i",newtup->t_len, oldtup.t_len )));
    }

    START_CRIT_SECTION();

    uint8 header_size = oldtup.t_data->t_hoff;
    Size payload_size = oldtup.t_len - header_size;

    memcpy((char *) oldtup.t_data + header_size, 
           (char *) newtup->t_data + newtup->t_data->t_hoff, 
           payload_size);

    oldtup.t_data->t_ctid = oldtup.t_self;

    MarkBufferDirty(buffer);

    if (RelationNeedsWAL(relation)) {
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

/*
 * update_vertex_phys_map
 *
 * Uses positional math (np_id_to_tid) to directly locate the phys_map row
 * for a vertex and performs an in-place update of its edge pointer.
 */
void
update_vertex_phys_map(Relation pmap_rel, uint64 vertex_id, Oid new_edge_table_oid, ItemPointer new_edge_tid, CommandId cid)
{
    Size payload_size = sizeof(NeoPhysMapRecord);
    uint32 tuples_per_page = np_calculate_tuples_per_page(payload_size);

    ItemPointerData target_tid;
    np_id_to_tid(vertex_id, tuples_per_page, &target_tid);

    Buffer buffer = ReadBuffer(pmap_rel, ItemPointerGetBlockNumber(&target_tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&target_tid));

    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: phys_map positional update failed");
    }

    GenericXLogState *state = GenericXLogStart(pmap_rel);
    
    page = GenericXLogRegisterBuffer(state, buffer, 0);

    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&target_tid));
    NeoPhysMapRecord *disk_rec = (NeoPhysMapRecord *) PageGetItem(page, lp);
    
    disk_rec->e_tbl_id = new_edge_table_oid;
    disk_rec->e_itemptr = *new_edge_tid;

    GenericXLogFinish(state);

    UnlockReleaseBuffer(buffer);
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
    Oid old_head_tbl = InvalidOid;
    ItemPointerData old_head = get_current_head_tid(pmap_rel, owner_v->id, &old_head_tbl);

    ItemPointerData next_ip;
    ItemPointerData prev_ip;

    TupleTableSlot *slot = table_slot_create(list_rel, NULL);
    ExecClearTuple(slot);

    memset(slot->tts_isnull, false, 10 * sizeof(bool));

    slot->tts_values[0] = Int64GetDatum(e->id);
    slot->tts_values[1] = Int32GetDatum(e->label_id);
    slot->tts_values[2] = CharGetDatum((char) direction);
    slot->tts_values[3] = Int64GetDatum(owner_v->id); 
    slot->tts_values[4] = Int64GetDatum(other_v->id);
    slot->tts_values[5] = Int32GetDatum(other_v->label_id);

    if (ItemPointerIsValid(&old_head)) {
        slot->tts_values[6] = ObjectIdGetDatum(old_head_tbl); 
        next_ip = old_head;
        slot->tts_values[7] = PointerGetDatum(&next_ip);
    } else {
        slot->tts_values[6] = ObjectIdGetDatum(InvalidOid);
        
        ItemPointerSetInvalid(&next_ip);
        slot->tts_values[7] = PointerGetDatum(&next_ip);
    }

    slot->tts_values[8] = ObjectIdGetDatum(InvalidOid);    
    ItemPointerSetInvalid(&prev_ip);
    slot->tts_values[9] = PointerGetDatum(&prev_ip);

    ExecStoreVirtualTuple(slot);

    table_tuple_insert(list_rel, slot, cid, 0, NULL);
    
    ItemPointerData new_tid = slot->tts_tid;

    ExecDropSingleTupleTableSlot(slot);

    if (ItemPointerIsValid(&old_head) && OidIsValid(old_head_tbl)) {
        Relation old_head_rel;
        bool close_rel = false;
        
        if (old_head_tbl == active_list_oid) {
            old_head_rel = list_rel;
        } else {
            old_head_rel = table_open(old_head_tbl, RowExclusiveLock);
            close_rel = true;
        }

        update_edge_prev_pointer(old_head_rel, &old_head, active_list_oid, &new_tid, cid);

        if (close_rel) {
            table_close(old_head_rel, RowExclusiveLock);
        }
    }

    update_vertex_phys_map(pmap_rel, owner_v->id, active_list_oid, &new_tid, cid);

    table_close(list_rel, RowExclusiveLock);
    table_close(pmap_rel, RowExclusiveLock);
}

PG_FUNCTION_INFO_V1(update_vertex);
Datum
update_vertex(PG_FUNCTION_ARGS)
{
    int64 id = PG_GETARG_INT64(0);
    int32 label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);
    gtype *new_properties = NP_GET_ARG_GTYPE_P(3); 

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    const label_cache_data *label_cache =
        search_vertex_label_graph_id_label_id_cache(graph_id, label_id);

    if (!label_cache)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("label not found: graph_id=%d, label_id=%d", graph_id, label_id)));

    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);

    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    
    ItemPointerData phys_map_tid;
    np_id_to_tid(id, pmap_tuples_per_page, &phys_map_tid);

    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);

    /* If the file is completely empty, the vertex definitely doesn't exist */
    if (pmap_blk >= RelationGetNumberOfBlocks(pmap_rel)) {
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map (table empty)")));
    }

    Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    if (!ItemIdIsNormal(pmap_lp)) {
        UnlockReleaseBuffer(pmap_buf);
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map")));
    }
    NeoPhysMapRecord *disk_pmap_rec = (NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
    NeoPhysMapRecord current_pmap_rec = *disk_pmap_rec; 
    UnlockReleaseBuffer(pmap_buf);

    ItemPointerData old_vertex_tid = current_pmap_rec.v_itemptr;

    Relation rel = table_open(label_cache->vertex_tbl, RowExclusiveLock);
    Buffer obuf_check = ReadBuffer(rel, ItemPointerGetBlockNumber(&old_vertex_tid));
    LockBuffer(obuf_check, BUFFER_LOCK_SHARE);
    
    Page opage_check = BufferGetPage(obuf_check);
    ItemId olp_check = PageGetItemId(opage_check, ItemPointerGetOffsetNumber(&old_vertex_tid));

    if (!ItemIdIsNormal(olp_check)) {
        UnlockReleaseBuffer(obuf_check);
        ereport(ERROR, (errmsg("Corrupted phys_map: Pointer to empty line pointer")));
    }

    NPEntityTupleHeader old_hdr_check = (NPEntityTupleHeader) PageGetItem(opage_check, olp_check);
    if (FullTransactionIdIsValid(old_hdr_check->xmax)) {
        UnlockReleaseBuffer(obuf_check);
        ereport(ERROR, (errmsg("Vertex ID %ld was concurrently deleted or updated", id)));
    }

    vertex *old_v = (vertex *) old_hdr_check->serialized_entity;
    int16 current_dictionary_id = old_v->dictionary_id;

    UnlockReleaseBuffer(obuf_check);

    vertex *new_v = build_vertex_internal(id, graph_id, label_id, current_dictionary_id, new_properties);

    Size actual_payload_size = VARSIZE(new_v);
    Size total_tuple_size = MAXALIGN(SizeOfNPEntityTupleHeader + actual_payload_size);
    
    char *tuple_buf = (char *) palloc0(total_tuple_size);
    NPEntityTupleHeader new_hdr = (NPEntityTupleHeader) tuple_buf;

    new_hdr->xmin = current_fxid;
    new_hdr->xmax = InvalidFullTransactionId;
    new_hdr->cmin = cid;
    new_hdr->cmax = InvalidCommandId;
    new_hdr->flags = 0;
    new_hdr->id = id;
    new_hdr->prev_itemptr = old_vertex_tid; 
    
    memcpy(new_hdr->serialized_entity, new_v, actual_payload_size);
    pfree(new_v);

    ItemPointerData new_vertex_tid;
    np_write_record_to_page(rel, tuple_buf, total_tuple_size, &new_vertex_tid);
    pfree(tuple_buf);

    current_pmap_rec.v_itemptr = new_vertex_tid;
    np_overwrite_physmap_in_page(pmap_rel, &phys_map_tid, &current_pmap_rec);
    table_close(pmap_rel, RowExclusiveLock);

    Buffer obuf_final = ReadBuffer(rel, ItemPointerGetBlockNumber(&old_vertex_tid));
    LockBuffer(obuf_final, BUFFER_LOCK_EXCLUSIVE);
    
    GenericXLogState *state = GenericXLogStart(rel);
    Page wal_page = GenericXLogRegisterBuffer(state, obuf_final, 0);
    
    ItemId olp_final = PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&old_vertex_tid));
    NPEntityTupleHeader wal_old_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, olp_final);
    
    wal_old_hdr->xmax = current_fxid;
    wal_old_hdr->cmax = cid;
    
    GenericXLogFinish(state);
    
    UnlockReleaseBuffer(obuf_final);
    table_close(rel, RowExclusiveLock);

    PG_RETURN_VOID();
}
static void
np_delete_edge_from_adj_list(int32 graph_id, int64 vertex_id, int32 vertex_label_id, int64 target_edge_id, CommandId cid, FullTransactionId current_fxid)
{
    const label_cache_data *v_label = search_vertex_label_graph_id_label_id_cache(graph_id, vertex_label_id);
    
    if (!v_label || !OidIsValid(v_label->phys_map)) 
        return;

    Relation pmap_rel = table_open(v_label->phys_map, AccessShareLock);
    
    Oid current_tbl = InvalidOid;
    ItemPointerData current_tid = get_current_head_tid(pmap_rel, vertex_id, &current_tbl);
    
    table_close(pmap_rel, AccessShareLock);

    /* Walk the doubly-linked list until we find the target edge */
    while (ItemPointerIsValid(&current_tid) && OidIsValid(current_tbl))
    {
        Relation list_rel = table_open(current_tbl, RowExclusiveLock);
        Buffer buf = ReadBuffer(list_rel, ItemPointerGetBlockNumber(&current_tid));
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        
        Page page = BufferGetPage(buf);
        ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&current_tid));
        
        if (!ItemIdIsNormal(lp)) {
            UnlockReleaseBuffer(buf);
            table_close(list_rel, RowExclusiveLock);
            elog(ERROR, "NeoPostGraph: Traversed into an invalid line pointer in linked list");
        }
        
        NeoLinkedListRecord *rec = (NeoLinkedListRecord *) PageGetItem(page, lp);
        
        /* If we found the edge and it isn't already deleted */
        if (rec->id == target_edge_id && !FullTransactionIdIsValid(rec->xmax)) 
        {
            GenericXLogState *state = GenericXLogStart(list_rel);
            page = GenericXLogRegisterBuffer(state, buf, 0);
            
            lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&current_tid));
            NeoLinkedListRecord *wal_rec = (NeoLinkedListRecord *) PageGetItem(page, lp);
            
            /* Stamp the MVCC tombstone directly on the adjacency record */
            wal_rec->xmax = current_fxid;
            wal_rec->cmax = cid;
            
            GenericXLogFinish(state);
            UnlockReleaseBuffer(buf);
            table_close(list_rel, RowExclusiveLock);
            return; /* Successfully unlinked */
        }
        
        /* Not a match, extract the next pointers before dropping the lock */
        Oid next_tbl = rec->next_tbl;
        ItemPointerData next_tid = rec->next_itemptr;
        
        UnlockReleaseBuffer(buf);
        table_close(list_rel, RowExclusiveLock);
        
        current_tbl = next_tbl;
        current_tid = next_tid;
    }
}



typedef struct {
    int64 edge_id;
    int32 edge_lid;
} EdgeToDelete;

PG_FUNCTION_INFO_V1(delete_vertex);
Datum
delete_vertex(PG_FUNCTION_ARGS)
{
    int64 vertex_id = PG_GETARG_INT64(0);
    int32 label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);
    bool detach = PG_GETARG_BOOL(3);

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    const label_cache_data *v_label = search_vertex_label_graph_id_label_id_cache(graph_id, label_id);

    if (!v_label || !OidIsValid(v_label->vertex_tbl))
        ereport(ERROR, (errmsg("Vertex label not found: graph_id=%d, label_id=%d", graph_id, label_id)));

    /* 1. O(1) Address Calculation for Vertex PhysMap */
    Relation pmap_rel = table_open(v_label->phys_map, AccessShareLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(vertex_id, pmap_tuples_per_page, &phys_map_tid);

    /* Read phys_map to get target TID and Adjacency List Head */
    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);

    /* If the file is completely empty, the vertex definitely doesn't exist */
    if (pmap_blk >= RelationGetNumberOfBlocks(pmap_rel)) {
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map (table empty)")));
    }

    Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    if (!ItemIdIsNormal(pmap_lp)) {
        UnlockReleaseBuffer(pmap_buf);
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map")));
    }
    NeoPhysMapRecord *disk_pmap_rec = (NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
    ItemPointerData target_vertex_tid = disk_pmap_rec->v_itemptr;
    
    Oid current_tbl = disk_pmap_rec->e_tbl_id;
    ItemPointerData current_tid = disk_pmap_rec->e_itemptr;
    
    UnlockReleaseBuffer(pmap_buf);
    table_close(pmap_rel, AccessShareLock);

    /* 2. The DETACH Check: Walk the Adjacency Lists */
    EdgeToDelete *edges_to_delete = NULL;
    int num_edges = 0;
    int max_edges = 0;

    while (ItemPointerIsValid(&current_tid) && OidIsValid(current_tbl))
    {
        if (current_tbl == v_label->arraylist)
        {
            Relation array_rel = table_open(current_tbl, AccessShareLock);
            Buffer buf = ReadBuffer(array_rel, ItemPointerGetBlockNumber(&current_tid));
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            
            Page page = BufferGetPage(buf);
            ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&current_tid));
            
            HeapTupleData oldtup;
            oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
            oldtup.t_len = ItemIdGetLength(lp);
            oldtup.t_tableOid = RelationGetRelid(array_rel);
            oldtup.t_self = current_tid;
            
            TupleDesc tupdesc = RelationGetDescr(array_rel);
            Datum values[5];
            bool nulls[5];
            heap_deform_tuple(&oldtup, tupdesc, values, nulls);
            
            if (!nulls[3]) {
                AdjList *adj = DATUM_GET_ADJ_LIST(values[3]);
                for (int i = 0; i < adj->nitems; i++) {
                    if (!FullTransactionIdIsValid(adj->data[i].xmax)) {
                        if (!detach) {
                            UnlockReleaseBuffer(buf);
                            table_close(array_rel, AccessShareLock);
                            ereport(ERROR, (errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION), 
                                            errmsg("Cannot delete vertex %ld because it still has active edges. Use DETACH DELETE.", vertex_id)));
                        }
                        
                        /* Collect edge for deletion */
                        if (num_edges >= max_edges) {
                            max_edges = max_edges == 0 ? 16 : max_edges * 2;
                            edges_to_delete = repalloc(edges_to_delete, max_edges * sizeof(EdgeToDelete));
                        }
                        edges_to_delete[num_edges].edge_id = adj->data[i].edge_id;
                        edges_to_delete[num_edges].edge_lid = adj->data[i].edge_lid;
                        num_edges++;
                    }
                }
            }
            
            ItemPointerData next_tid;
            ItemPointerSetInvalid(&next_tid);
            if (!nulls[4]) {
                ItemPointer ip = (ItemPointer) DatumGetPointer(values[4]);
                next_tid = *ip;
            }
            
            UnlockReleaseBuffer(buf);
            table_close(array_rel, AccessShareLock);
            current_tid = next_tid;
        }
        else
        {
            Relation list_rel = table_open(current_tbl, AccessShareLock);
            Buffer buf = ReadBuffer(list_rel, ItemPointerGetBlockNumber(&current_tid));
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            
            Page page = BufferGetPage(buf);
            ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&current_tid));
            NeoLinkedListRecord *rec = (NeoLinkedListRecord *) PageGetItem(page, lp);
            
            if (!FullTransactionIdIsValid(rec->xmax)) {
                if (!detach) {
                    UnlockReleaseBuffer(buf);
                    table_close(list_rel, AccessShareLock);
                    ereport(ERROR, (errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION), 
                                    errmsg("Cannot delete vertex %ld because it still has active edges. Use DETACH DELETE.", vertex_id)));
                }
                
                if (num_edges >= max_edges) {
                    max_edges = max_edges == 0 ? 16 : max_edges * 2;
                    
                    if (edges_to_delete == NULL) {
                        // First allocation must use palloc
                        edges_to_delete = palloc(max_edges * sizeof(EdgeToDelete));
                    } else {
                        // Subsequent re-allocations use repalloc
                        edges_to_delete = repalloc(edges_to_delete, max_edges * sizeof(EdgeToDelete));
                    }
                }
                edges_to_delete[num_edges].edge_id = rec->id;
                edges_to_delete[num_edges].edge_lid = rec->edge_lid;
                num_edges++;
            }
            
            Oid next_tbl = rec->next_tbl;
            ItemPointerData next_tid = rec->next_itemptr;
            
            UnlockReleaseBuffer(buf);
            table_close(list_rel, AccessShareLock);
            current_tbl = next_tbl;
            current_tid = next_tid;
        }
    }

    /* 3. Cascade Delete to Collected Edges */
    for (int i = 0; i < num_edges; i++) {
        /* Call an internal C function instead of the SQL-exposed one to bypass type conversions */
        np_internal_delete_edge(graph_id, edges_to_delete[i].edge_lid, edges_to_delete[i].edge_id, cid, current_fxid);
    }
    if (edges_to_delete) pfree(edges_to_delete);

    /* 4. Tombstone the Vertex Tuple in the Heap */
    Relation rel = table_open(v_label->vertex_tbl, RowExclusiveLock);
    Buffer obuf = ReadBuffer(rel, ItemPointerGetBlockNumber(&target_vertex_tid));
    LockBuffer(obuf, BUFFER_LOCK_EXCLUSIVE);

    GenericXLogState *state = GenericXLogStart(rel);
    Page wal_page = GenericXLogRegisterBuffer(state, obuf, 0);

    ItemId wal_lp = PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&target_vertex_tid));
    
    if (!ItemIdIsNormal(wal_lp)) {
        GenericXLogAbort(state);
        UnlockReleaseBuffer(obuf);
        table_close(rel, RowExclusiveLock);
        ereport(ERROR, (errmsg("Corrupted phys_map: Pointer to empty line pointer for vertex %ld", vertex_id)));
    }

    NPEntityTupleHeader wal_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, wal_lp);

    if (FullTransactionIdIsValid(wal_hdr->xmax)) {
        GenericXLogAbort(state);
        UnlockReleaseBuffer(obuf);
        table_close(rel, RowExclusiveLock);
        ereport(ERROR, (errmsg("Vertex ID %ld was concurrently deleted or updated", vertex_id)));
    }

    /* Mark as deleted */
    wal_hdr->xmax = current_fxid;
    wal_hdr->cmax = cid;

    GenericXLogFinish(state);
    UnlockReleaseBuffer(obuf);
    table_close(rel, RowExclusiveLock);

    PG_RETURN_VOID();
}

void
np_internal_delete_edge(int32 graph_id, int32 label_id, int64 edge_id, CommandId cid, FullTransactionId current_fxid)
{
    const label_cache_data *label_cache =
        search_edge_label_graph_id_label_id_cache(graph_id, label_id);

    /* Fallback to vertex_tbl check if you are using that naming convention for edges */
    if (!label_cache || !OidIsValid(label_cache->vertex_tbl))
        ereport(ERROR, (errmsg("edge label not found: graph_id=%d, label_id=%d", graph_id, label_id)));

    /* 1. O(1) Address Calculation for Edge PhysMap */
    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(edge_id, pmap_tuples_per_page, &phys_map_tid);

    /* 2. Read the current phys_map record to get target TID */
    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);

    /* If the file is completely empty, the vertex definitely doesn't exist */
    if (pmap_blk >= RelationGetNumberOfBlocks(pmap_rel)) {
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map (table empty)")));
    }

    Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    if (!ItemIdIsNormal(pmap_lp)) {
        UnlockReleaseBuffer(pmap_buf);
        table_close(pmap_rel, RowExclusiveLock); /* Use the exact lock level you opened it with */
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Vertex ID not found in phys_map")));
    }

    NeoEdgePhysMapRecord *disk_pmap_rec = (NeoEdgePhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
    ItemPointerData target_edge_tid = disk_pmap_rec->e_itemptr;
    
    UnlockReleaseBuffer(pmap_buf);
    table_close(pmap_rel, RowExclusiveLock);

    /* 3. Tombstone the Edge Tuple in the Heap */
    Relation rel = table_open(label_cache->vertex_tbl, RowExclusiveLock);
    Buffer obuf = ReadBuffer(rel, ItemPointerGetBlockNumber(&target_edge_tid));
    LockBuffer(obuf, BUFFER_LOCK_EXCLUSIVE);

    /* 4. WAL Logging and MVCC Tombstoning */
    GenericXLogState *state = GenericXLogStart(rel);
    Page wal_page = GenericXLogRegisterBuffer(state, obuf, 0);

    ItemId wal_lp = PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&target_edge_tid));
    
    if (!ItemIdIsNormal(wal_lp)) {
        GenericXLogAbort(state);
        UnlockReleaseBuffer(obuf);
        table_close(rel, RowExclusiveLock);
        ereport(ERROR, (errmsg("Corrupted phys_map: Pointer to empty line pointer for edge %ld", edge_id)));
    }

    NPEntityTupleHeader wal_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, wal_lp);

    /* Concurrency Check */
    if (FullTransactionIdIsValid(wal_hdr->xmax)) {
        GenericXLogAbort(state);
        UnlockReleaseBuffer(obuf);
        table_close(rel, RowExclusiveLock);
        ereport(ERROR, (errmsg("Edge ID %ld was concurrently deleted or updated", edge_id)));
    }

    /* Extract vertex routing metadata before we lose access to the payload */
    edge *deleted_e = (edge *) wal_hdr->serialized_entity;
    int64 start_vid = deleted_e->start_id;  
    int32 start_label = deleted_e->start_label;
    int64 end_vid = deleted_e->end_id;
    int32 end_label = deleted_e->end_label;

    /* Mark as deleted in the heap */
    wal_hdr->xmax = current_fxid;
    wal_hdr->cmax = cid;

    GenericXLogFinish(state);
    UnlockReleaseBuffer(obuf);
    table_close(rel, RowExclusiveLock);

    /* 5. Adjacency List Unlinking */
    /* Cascade the tombstone into the doubly-linked lists and arrays */
    np_delete_edge_from_adj_list(graph_id, start_vid, start_label, edge_id, cid, current_fxid);
    np_delete_edge_from_adj_list(graph_id, end_vid, end_label, edge_id, cid, current_fxid);
}
PG_FUNCTION_INFO_V1(delete_edge);
Datum
delete_edge(PG_FUNCTION_ARGS)
{
    int64 edge_id = PG_GETARG_INT64(0);
    int32 label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    /* Delegate everything to the internal C API */
    np_internal_delete_edge(graph_id, label_id, edge_id, cid, current_fxid);

    PG_RETURN_VOID();
}

static void
update_edge_prev_pointer(Relation rel, ItemPointer old_head_tid, Oid prev_table_oid, ItemPointer new_head_tid, CommandId cid)
{
    Buffer buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(old_head_tid));
    
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);

    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(old_head_tid));

    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: attempted to update prev pointer on a dead or invalid tuple");
    }

    GenericXLogState *state = GenericXLogStart(rel);
    
    page = GenericXLogRegisterBuffer(state, buffer, 0);

    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(old_head_tid));
    NeoLinkedListRecord *disk_rec = (NeoLinkedListRecord *) PageGetItem(page, lp);

    disk_rec->prev_tbl = prev_table_oid;
    disk_rec->prev_itemptr = *new_head_tid;

    GenericXLogFinish(state);

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
static ItemPointerData get_current_head_tid(Relation pmap_rel, uint64 vertex_id, Oid *head_tbl)
{
    ItemPointerData head_tid;
    ItemPointerSetInvalid(&head_tid);
    if (head_tbl) *head_tbl = InvalidOid;

    Size payload_size = sizeof(NeoPhysMapRecord);
    uint32 tuples_per_page = np_calculate_tuples_per_page(payload_size);

    ItemPointerData target_tid;
    np_id_to_tid(vertex_id, tuples_per_page, &target_tid);
    
    BlockNumber target_blk = ItemPointerGetBlockNumber(&target_tid);
    if (target_blk >= RelationGetNumberOfBlocks(pmap_rel)) {
        return head_tid; 
    }

    Buffer buffer = ReadBuffer(pmap_rel, target_blk);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buffer);
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&target_tid));

    if (ItemIdIsNormal(lp)) {
        NeoPhysMapRecord *disk_rec = (NeoPhysMapRecord *) PageGetItem(page, lp);
        head_tid = disk_rec->e_itemptr; 
        if (head_tbl) *head_tbl = disk_rec->e_tbl_id; /* Capture the table! */
    }
    UnlockReleaseBuffer(buffer);
    return head_tid;
}