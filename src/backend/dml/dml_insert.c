/*
 * PostGraph
 * Copyright (C) 2026 by PostGraph
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "postgres.h"

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

#include "access/generic_xlog.h"
#include "access/table.h"
#include "access/xact.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "dml/dml_insert.h"
#include "access/np_entity_store.h"
#include "access/np_phys_map.h"
#include "access/np_linked_list.h"
#include "access/np_arraylist.h"
#include "catalog/np_label.h"
#include "utils/np_cache.h"
#include "utils/dictionary.h"


static Oid get_active_linked_list_oid(Oid linked_list_meta_oid);
static void update_edge_prev_pointer(Relation list_rel, ItemPointer old_head_tid, Oid prev_table_oid, ItemPointer new_tid, CommandId cid);
static void insert_edge_one_direction(vertex *owner_v, vertex *other_v, edge *e, uint8 direction, CommandId cid);

PG_FUNCTION_INFO_V1(insert_vertex);
Datum
insert_vertex(PG_FUNCTION_ARGS)
{
    vertex *v = NP_GET_ARG_VERTEX(0);
    gtype *props = PG_ARGISNULL(1) ? np_empty_gtype_object() : NP_GET_ARG_GTYPE_P(1);
    ArrayType *input_annots = PG_ARGISNULL(2) ? NULL : PG_GETARG_ARRAYTYPE_P(2);

    np_internal_insert_vertex(v, props, input_annots, NULL);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(insert_edge);
Datum
insert_edge(PG_FUNCTION_ARGS)
{
    /* 1. Extract standard arguments */
    vertex *start_v = NP_GET_ARG_VERTEX(0);
    vertex *end_v = NP_GET_ARG_VERTEX(1);
    edge *e = NP_GET_ARG_EDGE(2);
    gtype *props = PG_ARGISNULL(3) ? np_empty_gtype_object() : NP_GET_ARG_GTYPE_P(3);

    /* 2. Call internal routing */
    np_internal_insert_edge(start_v, end_v, e, props);

    PG_RETURN_VOID();
}

void
np_internal_insert_vertex(vertex *v, gtype *props, ArrayType *input_annots, ItemPointerData *forwarded_a_itemptr)
{
    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    int32 graph_id = v->graph_id;
    int32 label_id = v->label_id;
    const label_cache_data *label_cache = search_vertex_label_graph_id_label_id_cache(graph_id, label_id);

    if (!label_cache)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("label not found: graph_id=%d, label_id=%d", graph_id, label_id)));

    /* 1. Open the custom entity_store table */
    Relation rel = table_open(label_cache->vertex_tbl, RowExclusiveLock);

    if (props == NULL)
        props = np_empty_gtype_object();

    /* 2. Calculate explicit byte size. */
    Size payload_size = np_entity_pack_size((struct varlena *) v, (struct varlena *) props);
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

    np_entity_pack(hdr->serialized_entity, (struct varlena *) v, (struct varlena *) props);

    /* 4. Write directly to the page using your existing primitive */
    ItemPointerData v_itemptr;
    np_write_record_to_page(rel, tuple_buf, total_tuple_size, &v_itemptr);

    pfree(tuple_buf);
    table_close(rel, RowExclusiveLock);


    /* 5. Process Annotations */
    ItemPointerData a_itemptr;
    ItemPointerSetInvalid(&a_itemptr);

    /* If migrating, carry over the old annotation pointer. Otherwise, generate new. */
    if (forwarded_a_itemptr && ItemPointerIsValid(forwarded_a_itemptr)) {
        a_itemptr = *forwarded_a_itemptr;
    } else if (input_annots != NULL) {
        Oid annotations_tbl = label_cache->annotations_tbl;

        if (OidIsValid(annotations_tbl)) {
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
                    latest_schema_tid = pmap_rec->v_itemptr;
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

            if (map_array == NULL) map_array = label_cache->annotation_map;

            if (map_array == NULL) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("This structural label does not support annotations")));
            }

            int num_labels = ArrayGetNItems(ARR_NDIM(map_array), ARR_DIMS(map_array));
            int byte_size = (num_labels + 7) / 8;

            bytea *bitset = (bytea *) palloc0(VARHDRSZ + byte_size);
            SET_VARSIZE(bitset, VARHDRSZ + byte_size);
            char *bits = VARDATA(bitset);

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
                    bits[bit_pos / 8] |= (1 << (bit_pos % 8));
                } else {
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Annotation label '%s' is not valid for this structural label", in_str)));
                }
            }

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

            Relation annot_rel = table_open(annotations_tbl, RowExclusiveLock);
            np_write_record_to_page(annot_rel, annot_tuple_buf, annot_total_size, &a_itemptr);
            
            pfree(annot_tuple_buf);
            table_close(annot_rel, RowExclusiveLock);
            
        } else {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("This structural label does not support annotations")));
        }
    }

    /* 6. Update the phys_map with the new topology pointer */
    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);
    
    NeoPhysMapRecord rec = {
        .v_itemptr = v_itemptr,
        .e_tbl_id = InvalidOid,
        .a_itemptr = a_itemptr
    };
    ItemPointerSetInvalid(&rec.e_itemptr);

    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(v->id, pmap_tuples_per_page, &phys_map_tid);

    /* Safely Upsert */
    np_place_physmap_record(pmap_rel, &phys_map_tid, &rec);
    table_close(pmap_rel, RowExclusiveLock);
}

void
np_internal_insert_edge(vertex *start_v, vertex *end_v, edge *e, gtype *props)
{
    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    const label_cache_data *edge_label = search_edge_label_graph_id_label_id_cache(e->graph_id, e->label_id);

    if (!edge_label) {
        ereport(ERROR, (errmsg("Edge label table not found")));
    }

    /* 1. Open the custom entity_store edge table */
    Relation edge_rel = table_open(edge_label->vertex_tbl, RowExclusiveLock);

    if (props == NULL)
        props = np_empty_gtype_object();

    /* 2. Calculate explicit byte size. */
    Size payload_size = np_entity_pack_size((struct varlena *) e, (struct varlena *) props);
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

    np_entity_pack(hdr->serialized_entity, (struct varlena *) e, (struct varlena *) props);

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
}
/*
 * In-place update for Arraylist chains. 
 * Updates the prev_table and prev_itemptr of an array list block.
 */
static void
np_update_arraylist_prev_pointer_inplace(Relation rel, ItemPointer tid, Oid new_prev_tbl, ItemPointer new_prev_tid)
{
    Buffer buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);

    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: attempted in-place prev update on invalid arraylist tuple");
    }

    GenericXLogState *state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(state, buffer, 0);
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    
    NeoArrayListRecord *disk_rec = (NeoArrayListRecord *) PageGetItem(page, lp);
    disk_rec->prev_tbl = new_prev_tbl;
    disk_rec->prev_itemptr = *new_prev_tid;

    GenericXLogFinish(state);
    UnlockReleaseBuffer(buffer);
}

static void
insert_edge_one_direction(vertex *owner_v, vertex *other_v, edge *e, uint8 direction, CommandId cid)
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

        TupleDesc old_desc = RelationGetDescr(old_head_rel);
        if (old_desc->natts == 10) { 
            update_edge_prev_pointer(old_head_rel, &old_head, active_list_oid, &new_tid, cid);
        }
        else if (old_desc->natts == 5) { 
np_update_arraylist_prev_pointer_inplace(old_head_rel, &old_head, active_list_oid, &new_tid);
        }
        else {
            elog(ERROR, "NeoPostGraph: Unknown old head format during insert_edge");
        }

        if (close_rel) {
            table_close(old_head_rel, RowExclusiveLock);
        }
    }

    update_vertex_phys_map(pmap_rel, owner_v->id, active_list_oid, &new_tid, cid);

    table_close(list_rel, RowExclusiveLock);
    table_close(pmap_rel, RowExclusiveLock);
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

ItemPointerData 
get_current_head_tid(Relation pmap_rel, uint64 vertex_id, Oid *head_tbl)
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
        if (head_tbl) *head_tbl = disk_rec->e_tbl_id;
    }
    UnlockReleaseBuffer(buffer);
    return head_tid;
}