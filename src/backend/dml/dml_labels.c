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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "funcapi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "storage/lockdefs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "tcop/utility.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "access/generic_xlog.h"
#include "utils/array.h"
#include "access/skey.h"
#include "utils/fmgroids.h"
#include "commands/sequence.h"
#include "nodes/parsenodes.h"
#include "nodes/makefuncs.h"
#include "miscadmin.h"
#include "utils/inval.h"

#include "utils/datum.h"

#include "ltree.h"

#include "catalog/np_label.h"
#include "commands/label_commands.h"
#include "utils/np_cache.h"
#include "utils/adj_list.h"
#include "access/np_phys_map.h"
#include "access/np_linked_list.h"
#include "utils/dictionary.h"
#include "utils/edge.h"
#include "utils/vertex.h"
#include "dml/dml_insert.h"

#include "access/np_entity_store.h"
#include "access/np_phys_map.h"


extern Datum ltree_in(PG_FUNCTION_ARGS);
extern Datum ltree_addltree(PG_FUNCTION_ARGS);
extern Datum ltxtq_in(PG_FUNCTION_ARGS);
extern Datum ltree_out(PG_FUNCTION_ARGS);
extern Datum ltree_isparent(PG_FUNCTION_ARGS);
extern Datum ltree_cmp(PG_FUNCTION_ARGS);
extern Datum ltxtq_exec(PG_FUNCTION_ARGS);

static int32 
resolve_reduced_target_label(int32 graph_id, int32 current_label_id, const char *label_to_remove, 
                             const graph_cache_data *graph_cache, const char *raw_current_ltree);
static int32 create_vlabel_from_path_internal(int32 graph_id, const char *ltree_path);

/* =====================================================================
 * MIGRATION STRUCTS & HELPER SIGNATURES
 * ===================================================================== */
typedef struct {
    uint64 edge_id;
    int32 edge_lid;
    uint8 dir;
    uint64 other_id;
    int32 other_lid;
    FullTransactionId xmin;
    FullTransactionId xmax;
    CommandId cmin;
    CommandId cmax;
    uint16 flags;
} MigratorAdjListMember;

typedef struct {
    int32 vl_len_;
    int32 nitems;
    int32 maxitems;
    MigratorAdjListMember data[FLEXIBLE_ARRAY_MEMBER];
} MigratorAdjList;

static int32 
resolve_or_create_target_label(int32 graph_id, int32 current_label_id, const char *new_label_str, 
                               const graph_cache_data *graph_cache, const char *raw_current_ltree)
{
    char *base_ltree_str = psprintf("_.%s", new_label_str);
    Datum base_ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(base_ltree_str));
    int32 base_label_id = -1;

    /* 1. Find or create base label */
    PushActiveSnapshot(GetLatestSnapshot());
    Relation meta_rel = table_open(graph_cache->vertex_labels, AccessShareLock);
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, GetActiveSnapshot(), 0, NULL);
    HeapTuple tuple;
    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        if (DatumGetInt32(DirectFunctionCall2(ltree_cmp, ltree_val, base_ltree_datum)) == 0) {
            base_label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
            break;
        }
    }
    systable_endscan(scan);
    table_close(meta_rel, AccessShareLock);
    PopActiveSnapshot();

    if (base_label_id == -1) {
        create_vlabel_internal(NameStr(graph_cache->name), new_label_str, NULL, NULL); 
        CommandCounterIncrement(); /* CACHE INVALIDATION HAPPENS HERE */

        PushActiveSnapshot(GetLatestSnapshot());
        meta_rel = table_open(graph_cache->vertex_labels, AccessShareLock);
        scan = systable_beginscan(meta_rel, InvalidOid, false, GetActiveSnapshot(), 0, NULL);
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            bool isnull;
            Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
            if (DatumGetInt32(DirectFunctionCall2(ltree_cmp, ltree_val, base_ltree_datum)) == 0) {
                base_label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
                break;
            }
        }
        systable_endscan(scan);
        table_close(meta_rel, AccessShareLock);
        PopActiveSnapshot();
    }

    /* 2. Determine target path */
    char *merged_ltree_str = NULL;
    int32 target_label_id = -1;

    if (strcmp(raw_current_ltree, "_") == 0) {
        merged_ltree_str = pstrdup(base_ltree_str);
    } else {
        bool already_has_label = false;
        char *path_copy = pstrdup(raw_current_ltree);
        char *token = strtok(path_copy, ".");
        while (token != NULL) {
            if (strcmp(token, new_label_str) == 0) { 
                already_has_label = true; 
                break; 
            }
            token = strtok(NULL, ".");
        }
        pfree(path_copy);

        if (already_has_label) {
            pfree(base_ltree_str);
            return current_label_id;
        }
        else merged_ltree_str = psprintf("%s.%s", raw_current_ltree, new_label_str);
    }
    pfree(base_ltree_str);

    /* 3. Find or merge target label */
    Datum target_ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(merged_ltree_str));
    
    PushActiveSnapshot(GetLatestSnapshot());
    meta_rel = table_open(graph_cache->vertex_labels, AccessShareLock);
    scan = systable_beginscan(meta_rel, InvalidOid, false, GetActiveSnapshot(), 0, NULL);
    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        if (DatumGetInt32(DirectFunctionCall2(ltree_cmp, ltree_val, target_ltree_datum)) == 0) {
            target_label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
            break;
        }
    }
    systable_endscan(scan);
    table_close(meta_rel, AccessShareLock);
    PopActiveSnapshot();

    if (target_label_id == -1) {
        merge_vlabels_internal(NameStr(graph_cache->name), current_label_id, base_label_id, NULL);
        CommandCounterIncrement(); /* CACHE INVALIDATION HAPPENS HERE TOO */

        PushActiveSnapshot(GetLatestSnapshot());
        meta_rel = table_open(graph_cache->vertex_labels, AccessShareLock);
        scan = systable_beginscan(meta_rel, InvalidOid, false, GetActiveSnapshot(), 0, NULL);
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            bool isnull;
            Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
            if (DatumGetInt32(DirectFunctionCall2(ltree_cmp, ltree_val, target_ltree_datum)) == 0) {
                target_label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
                break;
            }
        }
        systable_endscan(scan);
        table_close(meta_rel, AccessShareLock);
        PopActiveSnapshot();
    }
    pfree(merged_ltree_str);

    return target_label_id;
}

static vertex* 
np_internal_fetch_vertex(int32 graph_id, int64 v_id, int32 v_label) 
{
    const label_cache_data *cache = search_vertex_label_graph_id_label_id_cache(graph_id, v_label);
    Relation pmap = table_open(cache->phys_map, AccessShareLock);
    uint32 tpp = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData tid;
    np_id_to_tid(v_id, tpp, &tid);
    
    Buffer buf = ReadBuffer(pmap, ItemPointerGetBlockNumber(&tid));
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    NeoPhysMapRecord *rec = (NeoPhysMapRecord *)PageGetItem(BufferGetPage(buf), PageGetItemId(BufferGetPage(buf), ItemPointerGetOffsetNumber(&tid)));
    ItemPointerData v_tid = rec->v_itemptr;
    UnlockReleaseBuffer(buf);
    table_close(pmap, AccessShareLock);

    Relation v_rel = table_open(cache->vertex_tbl, AccessShareLock);
    Buffer v_buf = ReadBuffer(v_rel, ItemPointerGetBlockNumber(&v_tid));
    LockBuffer(v_buf, BUFFER_LOCK_SHARE);
    NPEntityTupleHeader hdr = (NPEntityTupleHeader)PageGetItem(BufferGetPage(v_buf), PageGetItemId(BufferGetPage(v_buf), ItemPointerGetOffsetNumber(&v_tid)));
    
    vertex *v = (vertex *)PG_DETOAST_DATUM_COPY(PointerGetDatum(hdr->serialized_entity));
    
    UnlockReleaseBuffer(v_buf);
    table_close(v_rel, AccessShareLock);
    return v;
}

static edge* 
np_internal_fetch_edge(int32 graph_id, int64 e_id, int32 e_label) 
{
    const label_cache_data *cache = search_edge_label_graph_id_label_id_cache(graph_id, e_label);
    Relation pmap = table_open(cache->phys_map, AccessShareLock);
    uint32 tpp = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData tid;
    np_id_to_tid(e_id, tpp, &tid);
    
    Buffer buf = ReadBuffer(pmap, ItemPointerGetBlockNumber(&tid));
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    NeoEdgePhysMapRecord *rec = (NeoEdgePhysMapRecord *)PageGetItem(BufferGetPage(buf), PageGetItemId(BufferGetPage(buf), ItemPointerGetOffsetNumber(&tid)));
    ItemPointerData e_tid = rec->e_itemptr;
    UnlockReleaseBuffer(buf);
    table_close(pmap, AccessShareLock);

    Relation e_rel = table_open(cache->vertex_tbl, AccessShareLock); 
    Buffer e_buf = ReadBuffer(e_rel, ItemPointerGetBlockNumber(&e_tid));
    LockBuffer(e_buf, BUFFER_LOCK_SHARE);
    NPEntityTupleHeader hdr = (NPEntityTupleHeader)PageGetItem(BufferGetPage(e_buf), PageGetItemId(BufferGetPage(e_buf), ItemPointerGetOffsetNumber(&e_tid)));
    
    edge *e = (edge *)PG_DETOAST_DATUM_COPY(PointerGetDatum(hdr->serialized_entity));
    
    UnlockReleaseBuffer(e_buf);
    table_close(e_rel, AccessShareLock);
    return e;
}

static void 
migrate_vertex_edges(int32 graph_id, vertex *old_v, vertex *new_v,
                     Oid old_e_tbl_id, ItemPointerData old_e_itemptr,
                     const graph_cache_data *graph_cache)
{
    if (!OidIsValid(old_e_tbl_id) || !ItemPointerIsValid(&old_e_itemptr)) {
        return;
    }

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    Oid curr_tbl_id = old_e_tbl_id;
    ItemPointerData curr_tid = old_e_itemptr;

    while (OidIsValid(curr_tbl_id) && ItemPointerIsValid(&curr_tid)) {

        Relation rel = table_open(curr_tbl_id, AccessShareLock);
        BlockNumber blk = ItemPointerGetBlockNumber(&curr_tid);
        
        /* Safe bounds check BEFORE reading to prevent Unpin panics */
        if (blk >= RelationGetNumberOfBlocks(rel)) {
            table_close(rel, AccessShareLock);
            break;
        }

        Buffer buf = ReadBuffer(rel, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&curr_tid));

        /* 
         * THE CRITICAL FIX: 
         * Postgres physically prunes dead line pointers. We must ensure the pointer 
         * hasn't been pruned before attempting to cast or deform its memory.
         */
        if (!ItemIdIsNormal(lp)) {
            UnlockReleaseBuffer(buf);
            table_close(rel, AccessShareLock);
            break;
        }

        TupleDesc desc = RelationGetDescr(rel);

        Oid next_tbl = InvalidOid;
        ItemPointerData next_tid;
        ItemPointerSetInvalid(&next_tid);

        if (desc->natts == 10) {
            /* 10 columns means this is a Linked List partition */
            NeoLinkedListRecord *rec = (NeoLinkedListRecord *) PageGetItem(page, lp);
            next_tbl = rec->next_tbl;
            next_tid = rec->next_itemptr;

            uint64 e_id = rec->id;
            int32 e_lid = rec->edge_lid;
            int64 o_id = rec->other_id;
            int32 o_lid = rec->other_lid;
            bool is_deleted = FullTransactionIdIsValid(rec->xmax);

            UnlockReleaseBuffer(buf);
            table_close(rel, AccessShareLock);

            if (!is_deleted) {
                vertex *neighbor_v = np_internal_fetch_vertex(graph_id, o_id, o_lid);
                edge *e = np_internal_fetch_edge(graph_id, e_id, e_lid);

                /* Canonical delete to handle WAL/tombstones for the old vertex */
                np_internal_delete_edge(graph_id, e_lid, e_id, cid, current_fxid);

                /* Safely update both sides (handles self-loops correctly) */
                if (e->start_id == old_v->id && e->start_label == old_v->label_id) {
                    e->start_id = new_v->id;
                    e->start_label = new_v->label_id;
                }
                if (e->end_id == old_v->id && e->end_label == old_v->label_id) {
                    e->end_id = new_v->id;
                    e->end_label = new_v->label_id;
                }

                /* Determine if the neighbor is us */
                vertex *actual_neighbor = (o_id == old_v->id && o_lid == old_v->label_id) ? new_v : neighbor_v;

                /* Let the definitively updated edge dictate the exact insertion order */
                if (e->start_id == new_v->id && e->start_label == new_v->label_id) {
                    np_internal_insert_edge(new_v, actual_neighbor, e);
                } else {
                    np_internal_insert_edge(actual_neighbor, new_v, e);
                }

                pfree(neighbor_v);
                pfree(e);
            }
        } else if (desc->natts == 5) {
            HeapTupleData tup;
            tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
            tup.t_len = ItemIdGetLength(lp);
            tup.t_self = curr_tid;

            Datum vals[5] = {0}; bool nulls[5] = {0};
            heap_deform_tuple(&tup, desc, vals, nulls);

            next_tbl = curr_tbl_id;
            if (!nulls[4]) {
                next_tid = *((ItemPointer) DatumGetPointer(vals[4]));
            } else {
                ItemPointerSetInvalid(&next_tid);
            }

            MigratorAdjList *adj_copy = NULL; 
            if (!nulls[3]) {
                /* 
                 * DatumGetByteaPCopy GUARANTEES the 1-byte short header 
                 * is expanded back into a standard 4-byte VARHDRSZ,
                 * perfectly aligning it with your struct layout.
                 */
                adj_copy = (MigratorAdjList *) DatumGetByteaPCopy(vals[3]);
            }

            UnlockReleaseBuffer(buf);
            table_close(rel, AccessShareLock);

            if (adj_copy) {
                
                for (int i = 0; i < adj_copy->nitems; i++) {
                    if (FullTransactionIdIsValid(adj_copy->data[i].xmax)) {
                        continue;
                    }

                    uint64 e_id = adj_copy->data[i].edge_id;
                    int32 e_lid = adj_copy->data[i].edge_lid;
                    int64 o_id = adj_copy->data[i].other_id;
                    int32 o_lid = adj_copy->data[i].other_lid;
                    uint8 dir = adj_copy->data[i].dir;


                    vertex *neighbor_v = np_internal_fetch_vertex(graph_id, o_id, o_lid);
                    edge *e = np_internal_fetch_edge(graph_id, e_id, e_lid);

                    np_internal_delete_edge(graph_id, e_lid, e_id, cid, current_fxid);

                    if (e->start_id == old_v->id) {
                        e->start_id = new_v->id;
                        e->start_label = new_v->label_id;
                    }
                    if (e->end_id == old_v->id) {
                        e->end_id = new_v->id;
                        e->end_label = new_v->label_id;
                    }

                    vertex *actual_neighbor = (o_id == old_v->id) ? new_v : neighbor_v;
                    
                    if (e->start_id == new_v->id)
                        np_internal_insert_edge(new_v, actual_neighbor, e);
                    else 
                        np_internal_insert_edge(actual_neighbor, new_v, e);


                    pfree(neighbor_v);
                    pfree(e);
                }
                pfree(adj_copy);
            }
        } else {
            UnlockReleaseBuffer(buf);
            table_close(rel, AccessShareLock);
            break;
        }

        curr_tbl_id = next_tbl;
        curr_tid = next_tid;
    }
}
PG_FUNCTION_INFO_V1(set_vertex_label);
Datum
set_vertex_label(PG_FUNCTION_ARGS)
{
    int64 old_vid = PG_GETARG_INT64(0);
    int32 current_label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);
    char *new_label_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

    if (strchr(new_label_str, '.') != NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("labels cannot contain dots.")));

    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);
    if (!graph_cache) 
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), errmsg("graph id %d not found", graph_id)));

    const label_cache_data *current_label_cache = search_vertex_label_graph_id_label_id_cache(graph_id, current_label_id);
    if (!current_label_cache) 
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("label %d not found", current_label_id)));

    /* SAFEGUARD 1: Extract LTREE so the pointer isn't corrupted */
    char *raw_current_ltree = DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(current_label_cache->label)));

    /* SAFEGUARD 2: Extract OIDs to the local stack BEFORE target creation overwrites the cache struct */
    Oid safe_old_physmap_oid = current_label_cache->phys_map;
    Oid safe_old_vertex_tbl_oid = current_label_cache->vertex_tbl;

    /* 1. Resolve Target Label (This will overwrite the static cache memory!) */
    int32 target_label_id = resolve_or_create_target_label(graph_id, current_label_id, new_label_str, graph_cache, raw_current_ltree);
    
    if (target_label_id == current_label_id) {
        pfree(raw_current_ltree);
        PG_RETURN_NULL();
    }
    pfree(raw_current_ltree);

    /* 3. Fetch Old Vertex from Phys Map using the SAFE local OID */
    Relation old_physmap_rel = table_open(safe_old_physmap_oid, RowExclusiveLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(old_vid, pmap_tuples_per_page, &phys_map_tid);

    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);
    Buffer pmap_buf = ReadBuffer(old_physmap_rel, pmap_blk);
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);

    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    /* Safe local stack copy of the phys map record */
    NeoPhysMapRecord pmap_rec = *((NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp));
    UnlockReleaseBuffer(pmap_buf);

    // Open the entity store with plans to delete the row using the SAFE local OID
    Relation old_v_rel = table_open(safe_old_vertex_tbl_oid, RowExclusiveLock);

    // Get the buffer the vertex is on and lock it for READING ONLY
    Buffer old_v_buf = ReadBuffer(old_v_rel, ItemPointerGetBlockNumber(&pmap_rec.v_itemptr));
    LockBuffer(old_v_buf, BUFFER_LOCK_SHARE); // <-- MUST BE SHARE LOCK TO PREVENT DEADLOCKS

    // Get the old vertex in the entity store
    NPEntityTupleHeader old_v_hdr = (NPEntityTupleHeader) 
        PageGetItem(BufferGetPage(old_v_buf), 
                    PageGetItemId(BufferGetPage(old_v_buf), ItemPointerGetOffsetNumber(&pmap_rec.v_itemptr)));

    /* 4. Prepare New Vertex Structs */
    vertex *old_unpacked = (vertex *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_v_hdr->serialized_entity));
    vertex *new_unpacked = (vertex *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_v_hdr->serialized_entity));

    // RELEASE THE BUFFER LOCK IMMEDIATELY SO EDGE MIGRATION CAN READ THE PAGE
    UnlockReleaseBuffer(old_v_buf);

    int64 new_vertex_id = DatumGetInt64(DirectFunctionCall1(nextval_oid, 
                                ObjectIdGetDatum(get_relname_relid(psprintf("np_vertex_id_seq_%d_%d", graph_id, target_label_id),
                                                                   get_namespace_oid("neopostgraph", false)))));

    /* Ensure both the struct and the header inside the varlena receive the new ID */
    new_unpacked->id = new_vertex_id;
    new_unpacked->label_id = target_label_id;

    /* =====================================================================
     * 4.5 EXTRACT AND PORT ANNOTATIONS
     * ===================================================================== */
    ArrayType *extracted_annots = NULL;
    
    if (ItemPointerIsValid(&pmap_rec.a_itemptr) && OidIsValid(current_label_cache->annotations_tbl)) {
        /* A. Read the old bitset from the old annotation table */
        Relation old_annot_rel = table_open(current_label_cache->annotations_tbl, AccessShareLock);
        Buffer old_annot_buf = ReadBuffer(old_annot_rel, ItemPointerGetBlockNumber(&pmap_rec.a_itemptr));
        LockBuffer(old_annot_buf, BUFFER_LOCK_SHARE);
        Page old_annot_page = BufferGetPage(old_annot_buf);
        ItemId old_annot_lp = PageGetItemId(old_annot_page, ItemPointerGetOffsetNumber(&pmap_rec.a_itemptr));
        
        NPEntityTupleHeader old_annot_hdr = (NPEntityTupleHeader) PageGetItem(old_annot_page, old_annot_lp);
        bytea *old_bitset = (bytea *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_annot_hdr->serialized_entity));
        
        UnlockReleaseBuffer(old_annot_buf);
        table_close(old_annot_rel, AccessShareLock);

        /* B. Read the old annotation schema */
        ArrayType *old_map_array = NULL;
        if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
            uint32 spmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
            ItemPointerData schema_pmap_tid;
            np_id_to_tid(current_label_id, spmap_tuples_per_page, &schema_pmap_tid);
            
            Relation schema_pmap_rel = table_open(graph_cache->annot_schema_phys_map, AccessShareLock);
            BlockNumber schema_pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
            
            if (schema_pmap_blk < RelationGetNumberOfBlocks(schema_pmap_rel)) {
                Buffer schema_pmap_buf = ReadBuffer(schema_pmap_rel, schema_pmap_blk);
                LockBuffer(schema_pmap_buf, BUFFER_LOCK_SHARE);
                Page schema_pmap_page = BufferGetPage(schema_pmap_buf);
                ItemId schema_pmap_lp = PageGetItemId(schema_pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));
                
                ItemPointerData latest_schema_tid;
                ItemPointerSetInvalid(&latest_schema_tid);
                if (ItemIdIsNormal(schema_pmap_lp)) {
                    NeoPhysMapRecord *spmap_rec = (NeoPhysMapRecord *) PageGetItem(schema_pmap_page, schema_pmap_lp);
                    latest_schema_tid = spmap_rec->v_itemptr;
                }
                UnlockReleaseBuffer(schema_pmap_buf);
                
                if (ItemPointerIsValid(&latest_schema_tid)) {
                    Relation schema_rel = table_open(graph_cache->annot_schema_tbl, AccessShareLock);
                    Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
                    LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
                    Page schema_page = BufferGetPage(schema_buf);
                    ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));
                    
                    if (ItemIdIsNormal(schema_lp)) {
                        NPEntityTupleHeader schema_hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
                        old_map_array = DatumGetArrayTypePCopy(PointerGetDatum(schema_hdr->serialized_entity));
                    }
                    UnlockReleaseBuffer(schema_buf);
                    table_close(schema_rel, AccessShareLock);
                }
                table_close(schema_pmap_rel, AccessShareLock); // Fixed close on success
            } else {
                /* Unlock if block doesn't exist */
                table_close(schema_pmap_rel, AccessShareLock); // Fixed close on failure
            }
        }
        
        if (old_map_array == NULL) old_map_array = current_label_cache->annotation_map;
        
        /* C. Decode the bitset to an Array of Strings */
        if (old_map_array != NULL) {
            Datum *map_d;
            bool *map_n;
            int map_count;
            deconstruct_array(old_map_array, TEXTOID, -1, false, 'i', &map_d, &map_n, &map_count);
            
            char *bits = VARDATA(old_bitset);
            Datum *extracted_d = palloc(map_count * sizeof(Datum));
            int extracted_count = 0;
            
            for (int i = 0; i < map_count; i++) {
                if (map_n[i]) continue;
                /* Check if the bit for this annotation is flipped */
                if (bits[i / 8] & (1 << (i % 8))) {
                    extracted_d[extracted_count++] = map_d[i];
                }
            }
            
            if (extracted_count > 0) {
                /* Construct standard PostgreSQL text array to feed into the internal insert */
                extracted_annots = construct_array(extracted_d, extracted_count, TEXTOID, -1, false, 'i');
            }
        }
        pfree(old_bitset);
    }

    /* 5. Insert New Vertex (Handles payload and initializes the physical map) */
    np_internal_insert_vertex(new_unpacked, extracted_annots, NULL);

    /* 6. Edge Migration (Uses np_internal_insert_edge, which automatically updates the new phys map!) */
    migrate_vertex_edges(graph_id, old_unpacked, new_unpacked,
                         pmap_rec.e_tbl_id, pmap_rec.e_itemptr,
                         graph_cache);

    /* 7. Tombstone Old Vertex Payload Natively (Re-acquire lock EXCLUSIVELY) */
    old_v_buf = ReadBuffer(old_v_rel, ItemPointerGetBlockNumber(&pmap_rec.v_itemptr));
    LockBuffer(old_v_buf, BUFFER_LOCK_EXCLUSIVE);

    GenericXLogState *state = GenericXLogStart(old_v_rel);
    Page wal_page = GenericXLogRegisterBuffer(state, old_v_buf, 0);
    NPEntityTupleHeader wal_old_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&pmap_rec.v_itemptr)));
    wal_old_hdr->xmax = GetTopFullTransactionId();
    wal_old_hdr->cmax = GetCurrentCommandId(true);
    GenericXLogFinish(state);
    
    UnlockReleaseBuffer(old_v_buf);
    table_close(old_v_rel, RowExclusiveLock);

    /* 8. Clear Old Phys Map */
    Buffer pbuf2 = ReadBuffer(old_physmap_rel, pmap_blk);
    LockBuffer(pbuf2, BUFFER_LOCK_EXCLUSIVE);
    
    state = GenericXLogStart(old_physmap_rel);
    Page ppage2 = GenericXLogRegisterBuffer(state, pbuf2, 0);
    NeoPhysMapRecord *wal_prec = (NeoPhysMapRecord *) PageGetItem(ppage2, PageGetItemId(ppage2, ItemPointerGetOffsetNumber(&phys_map_tid)));
    ItemPointerSetInvalid(&wal_prec->v_itemptr);
    GenericXLogFinish(state);
    
    UnlockReleaseBuffer(pbuf2);
    table_close(old_physmap_rel, RowExclusiveLock);

    pfree(old_unpacked);
    PG_RETURN_DATUM(PointerGetDatum(new_unpacked));
}

Datum np_internal_remove_vertex_label(int64 old_vid, int32 current_label_id, int32 graph_id, const char *label_to_remove, Buffer external_pmap_buf);

PG_FUNCTION_INFO_V1(remove_vertex_label);
Datum
remove_vertex_label(PG_FUNCTION_ARGS)
{
    int64 v_id = PG_GETARG_INT64(0);
    int32 old_label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);
    
    /* THE FIX: Safely detoast and null-terminate the text datum */
    text *label_text = PG_GETARG_TEXT_PP(3);
    char *label_str = text_to_cstring(label_text);
    
    Datum ret = np_internal_remove_vertex_label(v_id, old_label_id, graph_id, label_str, InvalidBuffer);
    
    pfree(label_str);
    return ret;
}

Datum
np_internal_remove_vertex_label(int64 old_vid, int32 current_label_id, int32 graph_id, const char *label_to_remove, Buffer external_pmap_buf)
{
    if (strchr(label_to_remove, '.') != NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("labels cannot contain dots.")));

    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);
    if (!graph_cache) 
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), errmsg("graph id %d not found", graph_id)));

    const label_cache_data *current_label_cache = search_vertex_label_graph_id_label_id_cache(graph_id, current_label_id);
    if (!current_label_cache) 
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("label %d not found", current_label_id)));

    /* SAFEGUARD 1: Extract LTREE so the pointer isn't corrupted */
    char *raw_current_ltree = DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(current_label_cache->label)));

    /* SAFEGUARD 2: Extract OIDs to the local stack BEFORE target creation overwrites the cache struct */
    Oid safe_old_physmap_oid = current_label_cache->phys_map;
    Oid safe_old_vertex_tbl_oid = current_label_cache->vertex_tbl;

    /* 1. Resolve Target Reduced Label */
    int32 target_label_id = resolve_reduced_target_label(graph_id, current_label_id, label_to_remove, graph_cache, raw_current_ltree);
    
    pfree(raw_current_ltree);

    /* 3. Fetch Old Vertex from Phys Map using the SAFE local OID */
    Relation old_physmap_rel = table_open(safe_old_physmap_oid, RowExclusiveLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(old_vid, pmap_tuples_per_page, &phys_map_tid);

    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);
    
    bool has_ext_buf = BufferIsValid(external_pmap_buf);
    Buffer pmap_buf = has_ext_buf ? external_pmap_buf : ReadBuffer(old_physmap_rel, pmap_blk);

    if (!has_ext_buf) {
        LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    }

    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    /* Safe local stack copy of the phys map record */
    NeoPhysMapRecord pmap_rec = *((NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp));

    /* SAFEGUARD: Catch double-deletions before they crash the engine */
    if (!ItemPointerIsValid(&pmap_rec.v_itemptr)) {
        if (!has_ext_buf) UnlockReleaseBuffer(pmap_buf);
        table_close(old_physmap_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("Vertex %ld not found or already migrated", old_vid)));
    }

    if (!has_ext_buf) {
        UnlockReleaseBuffer(pmap_buf);
    }

    // Open the entity store with plans to delete the row using the SAFE local OID
    Relation old_v_rel = table_open(safe_old_vertex_tbl_oid, RowExclusiveLock);

    // Get the buffer the vertex is on and lock it for READING ONLY
    Buffer old_v_buf = ReadBuffer(old_v_rel, ItemPointerGetBlockNumber(&pmap_rec.v_itemptr));
    LockBuffer(old_v_buf, BUFFER_LOCK_SHARE); // <-- MUST BE SHARE LOCK TO PREVENT DEADLOCKS

    // Get the old vertex in the entity store
    NPEntityTupleHeader old_v_hdr = (NPEntityTupleHeader) 
        PageGetItem(BufferGetPage(old_v_buf), 
                    PageGetItemId(BufferGetPage(old_v_buf), ItemPointerGetOffsetNumber(&pmap_rec.v_itemptr)));

    /* 4. Prepare New Vertex Structs */
    vertex *old_unpacked = (vertex *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_v_hdr->serialized_entity));
    vertex *new_unpacked = (vertex *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_v_hdr->serialized_entity));

    // RELEASE THE BUFFER LOCK IMMEDIATELY SO EDGE MIGRATION CAN READ THE PAGE
    UnlockReleaseBuffer(old_v_buf);

    int64 new_vertex_id = DatumGetInt64(DirectFunctionCall1(nextval_oid, 
                                ObjectIdGetDatum(get_relname_relid(psprintf("np_vertex_id_seq_%d_%d", graph_id, target_label_id),
                                                                   get_namespace_oid("neopostgraph", false)))));

    /* Ensure both the struct and the header inside the varlena receive the new ID */
    new_unpacked->id = new_vertex_id;
    new_unpacked->label_id = target_label_id;

    /* =====================================================================
     * 4.5 EXTRACT AND PORT ANNOTATIONS
     * ===================================================================== */
    ArrayType *extracted_annots = NULL;
    
    if (ItemPointerIsValid(&pmap_rec.a_itemptr) && OidIsValid(current_label_cache->annotations_tbl)) {
        /* A. Read the old bitset from the old annotation table */
        Relation old_annot_rel = table_open(current_label_cache->annotations_tbl, AccessShareLock);
        Buffer old_annot_buf = ReadBuffer(old_annot_rel, ItemPointerGetBlockNumber(&pmap_rec.a_itemptr));
        LockBuffer(old_annot_buf, BUFFER_LOCK_SHARE);
        Page old_annot_page = BufferGetPage(old_annot_buf);
        ItemId old_annot_lp = PageGetItemId(old_annot_page, ItemPointerGetOffsetNumber(&pmap_rec.a_itemptr));
        
        NPEntityTupleHeader old_annot_hdr = (NPEntityTupleHeader) PageGetItem(old_annot_page, old_annot_lp);
        bytea *old_bitset = (bytea *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_annot_hdr->serialized_entity));
        
        UnlockReleaseBuffer(old_annot_buf);
        table_close(old_annot_rel, AccessShareLock);

        /* B. Read the old annotation schema */
        ArrayType *old_map_array = NULL;
        if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
            uint32 spmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
            ItemPointerData schema_pmap_tid;
            np_id_to_tid(current_label_id, spmap_tuples_per_page, &schema_pmap_tid);
            
            Relation schema_pmap_rel = table_open(graph_cache->annot_schema_phys_map, AccessShareLock);
            BlockNumber schema_pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
            
            if (schema_pmap_blk < RelationGetNumberOfBlocks(schema_pmap_rel)) {
                Buffer schema_pmap_buf = ReadBuffer(schema_pmap_rel, schema_pmap_blk);
                LockBuffer(schema_pmap_buf, BUFFER_LOCK_SHARE);
                Page schema_pmap_page = BufferGetPage(schema_pmap_buf);
                ItemId schema_pmap_lp = PageGetItemId(schema_pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));
                
                ItemPointerData latest_schema_tid;
                ItemPointerSetInvalid(&latest_schema_tid);
                if (ItemIdIsNormal(schema_pmap_lp)) {
                    NeoPhysMapRecord *spmap_rec = (NeoPhysMapRecord *) PageGetItem(schema_pmap_page, schema_pmap_lp);
                    latest_schema_tid = spmap_rec->v_itemptr;
                }
                UnlockReleaseBuffer(schema_pmap_buf);
                
                if (ItemPointerIsValid(&latest_schema_tid)) {
                    Relation schema_rel = table_open(graph_cache->annot_schema_tbl, AccessShareLock);
                    Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
                    LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
                    Page schema_page = BufferGetPage(schema_buf);
                    ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));
                    
                    if (ItemIdIsNormal(schema_lp)) {
                        NPEntityTupleHeader schema_hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
                        old_map_array = DatumGetArrayTypePCopy(PointerGetDatum(schema_hdr->serialized_entity));
                    }
                    UnlockReleaseBuffer(schema_buf);
                    table_close(schema_rel, AccessShareLock);
                }
                table_close(schema_pmap_rel, AccessShareLock);
            } else {
                table_close(schema_pmap_rel, AccessShareLock);
            }
        }
        
        if (old_map_array == NULL) old_map_array = current_label_cache->annotation_map;
        
        /* C. Decode the bitset to an Array of Strings */
        if (old_map_array != NULL) {
            Datum *map_d;
            bool *map_n;
            int map_count;
            deconstruct_array(old_map_array, TEXTOID, -1, false, 'i', &map_d, &map_n, &map_count);
            
            char *bits = VARDATA(old_bitset);
            Datum *extracted_d = palloc(map_count * sizeof(Datum));
            int extracted_count = 0;
            
            for (int i = 0; i < map_count; i++) {
                if (map_n[i]) continue;
                if (bits[i / 8] & (1 << (i % 8))) {
                    extracted_d[extracted_count++] = map_d[i];
                }
            }
            
            /* D. Filter out annotations no longer supported by the new label */
            const label_cache_data *target_cache = search_vertex_label_graph_id_label_id_cache(graph_id, target_label_id);
            
            if (!target_cache || !OidIsValid(target_cache->annotations_tbl)) {
                /* Target supports NO annotations at all */
                extracted_count = 0;
            } else if (extracted_count > 0) {
                /* Target supports annotations: Fetch target schema for intersection */
                ArrayType *target_map_array = NULL;
                
                if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
                    uint32 spmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
                    ItemPointerData schema_pmap_tid;
                    np_id_to_tid(target_label_id, spmap_tuples_per_page, &schema_pmap_tid);
                    
                    Relation schema_pmap_rel = table_open(graph_cache->annot_schema_phys_map, AccessShareLock);
                    BlockNumber schema_pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
                    
                    if (schema_pmap_blk < RelationGetNumberOfBlocks(schema_pmap_rel)) {
                        Buffer schema_pmap_buf = ReadBuffer(schema_pmap_rel, schema_pmap_blk);
                        LockBuffer(schema_pmap_buf, BUFFER_LOCK_SHARE);
                        Page schema_pmap_page = BufferGetPage(schema_pmap_buf);
                        ItemId schema_pmap_lp = PageGetItemId(schema_pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));
                        
                        ItemPointerData latest_schema_tid;
                        ItemPointerSetInvalid(&latest_schema_tid);
                        if (ItemIdIsNormal(schema_pmap_lp)) {
                            NeoPhysMapRecord *spmap_rec = (NeoPhysMapRecord *) PageGetItem(schema_pmap_page, schema_pmap_lp);
                            latest_schema_tid = spmap_rec->v_itemptr;
                        }
                        UnlockReleaseBuffer(schema_pmap_buf);
                        
                        if (ItemPointerIsValid(&latest_schema_tid)) {
                            Relation schema_rel = table_open(graph_cache->annot_schema_tbl, AccessShareLock);
                            Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
                            LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
                            Page schema_page = BufferGetPage(schema_buf);
                            ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));
                            
                            if (ItemIdIsNormal(schema_lp)) {
                                NPEntityTupleHeader schema_hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
                                target_map_array = DatumGetArrayTypePCopy(PointerGetDatum(schema_hdr->serialized_entity));
                            }
                            UnlockReleaseBuffer(schema_buf);
                            table_close(schema_rel, AccessShareLock);
                        }
                    }
                    table_close(schema_pmap_rel, AccessShareLock);
                }
                
                if (target_map_array == NULL) target_map_array = target_cache->annotation_map;
                
                if (target_map_array != NULL) {
                    Datum *target_map_d;
                    bool *target_map_n;
                    int target_map_count;
                    deconstruct_array(target_map_array, TEXTOID, -1, false, 'i', &target_map_d, &target_map_n, &target_map_count);
                    
                    int valid_count = 0;
                    for (int i = 0; i < extracted_count; i++) {
                        char *in_str = TextDatumGetCString(extracted_d[i]);
                        bool is_valid = false;
                        for (int j = 0; j < target_map_count; j++) {
                            if (target_map_n[j]) continue;
                            if (strcmp(in_str, TextDatumGetCString(target_map_d[j])) == 0) {
                                is_valid = true;
                                break;
                            }
                        }
                        /* Only carry over annotations that exist in the new label's schema */
                        if (is_valid) {
                            extracted_d[valid_count++] = extracted_d[i];
                        }
                    }
                    extracted_count = valid_count;
                } else {
                    extracted_count = 0;
                }
            }
            
            if (extracted_count > 0) {
                /* Construct standard PostgreSQL text array to feed into the internal insert */
                extracted_annots = construct_array(extracted_d, extracted_count, TEXTOID, -1, false, 'i');
            }
        
        }
        pfree(old_bitset);
    }

    /* 5. Insert New Vertex (Handles payload and initializes the physical map) */
    np_internal_insert_vertex(new_unpacked, extracted_annots, NULL);

    /* 6. Edge Migration (Uses np_internal_insert_edge, which automatically updates the new phys map!) */
    migrate_vertex_edges(graph_id, old_unpacked, new_unpacked,
                         pmap_rec.e_tbl_id, pmap_rec.e_itemptr,
                         graph_cache);

    /* 7. Tombstone Old Vertex Payload Natively (Re-acquire lock EXCLUSIVELY) */
    old_v_buf = ReadBuffer(old_v_rel, ItemPointerGetBlockNumber(&pmap_rec.v_itemptr));
    LockBuffer(old_v_buf, BUFFER_LOCK_EXCLUSIVE);

    GenericXLogState *state = GenericXLogStart(old_v_rel);
    Page wal_page = GenericXLogRegisterBuffer(state, old_v_buf, 0);
    NPEntityTupleHeader wal_old_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&pmap_rec.v_itemptr)));
    wal_old_hdr->xmax = GetTopFullTransactionId();
    wal_old_hdr->cmax = GetCurrentCommandId(true);
    GenericXLogFinish(state);
    
    UnlockReleaseBuffer(old_v_buf);
    table_close(old_v_rel, RowExclusiveLock);

    /* 8. Clear Old Phys Map */
    if (has_ext_buf) {
        state = GenericXLogStart(old_physmap_rel);
        Page ppage2 = GenericXLogRegisterBuffer(state, external_pmap_buf, 0);
        NeoPhysMapRecord *wal_prec = (NeoPhysMapRecord *) PageGetItem(ppage2, PageGetItemId(ppage2, ItemPointerGetOffsetNumber(&phys_map_tid)));
        ItemPointerSetInvalid(&wal_prec->v_itemptr);
        GenericXLogFinish(state);
    } else {
        Buffer pbuf2 = ReadBuffer(old_physmap_rel, pmap_blk);
        LockBuffer(pbuf2, BUFFER_LOCK_EXCLUSIVE);
        
        state = GenericXLogStart(old_physmap_rel);
        Page ppage2 = GenericXLogRegisterBuffer(state, pbuf2, 0);
        NeoPhysMapRecord *wal_prec = (NeoPhysMapRecord *) PageGetItem(ppage2, PageGetItemId(ppage2, ItemPointerGetOffsetNumber(&phys_map_tid)));
        ItemPointerSetInvalid(&wal_prec->v_itemptr);
        GenericXLogFinish(state);
        
        UnlockReleaseBuffer(pbuf2);
    }

    table_close(old_physmap_rel, RowExclusiveLock);

    pfree(old_unpacked);
    return (PointerGetDatum(new_unpacked));
}

static int32
create_vlabel_from_path_internal(int32 graph_id, const char *ltree_path)
{
    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);
    if (!graph_cache) {
        elog(ERROR, "Graph %d not found during dynamic label creation", graph_id);
    }

    Oid nsp_oid = get_namespace_oid("neopostgraph", false);

    /* Get the new Label ID directly from the graph's sequence */
    Oid seq_oid = get_relname_relid(psprintf("np_label_id_seq_%d", graph_id), nsp_oid);
    if (!OidIsValid(seq_oid)) {
        elog(ERROR, "Could not find sequence for graph %d labels", graph_id);
    }
    
    int32 new_label_id = (int32) DatumGetInt64(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(seq_oid)));
    
    Datum ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(ltree_path));

    build_vertex_label_infrastructure(
        graph_id, 
        new_label_id, 
        nsp_oid,
        ltree_datum, 
        0,        
        (Datum)0, 
        NULL,     
        graph_cache->annot_schema_tbl, 
        graph_cache->annot_schema_phys_map
    );

    /* Return the generated ID directly to the caller */
    return new_label_id;
}

static int32 
resolve_reduced_target_label(int32 graph_id, int32 current_label_id, const char *label_to_remove, 
                             const graph_cache_data *graph_cache, const char *raw_current_ltree)
{
    char *path_copy = pstrdup(raw_current_ltree);
    char *token = strtok(path_copy, ".");
    
    StringInfoData new_path;
    initStringInfo(&new_path);
    bool removed = false;
    
    while (token != NULL) {
        if (strcmp(token, label_to_remove) == 0 && !removed) {
            removed = true; 
        } else {
            if (new_path.len > 0) appendStringInfoChar(&new_path, '.');
            appendStringInfoString(&new_path, token);
        }
        token = strtok(NULL, ".");
    }
    pfree(path_copy);
    
    if (!removed) {
        pfree(new_path.data);
        return current_label_id;
    }

    if (new_path.len == 0) {
        appendStringInfoString(&new_path, "_");
    }

    Datum target_ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(new_path.data));
    int32 target_id = -1;

    /* 1. Translate the LTREE string to an integer ID using the catalog */
    PushActiveSnapshot(GetLatestSnapshot());
    Relation meta_rel = table_open(graph_cache->vertex_labels, AccessShareLock);
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, GetActiveSnapshot(), 0, NULL);
    HeapTuple tuple;
    
    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        if (DatumGetInt32(DirectFunctionCall2(ltree_cmp, ltree_val, target_ltree_datum)) == 0) {
            target_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
            break;
        }
    }
    systable_endscan(scan);
    table_close(meta_rel, AccessShareLock);
    PopActiveSnapshot();

    /* 2. If the path didn't exist in the catalog, forge the tables and use the new ID directly */
    if (target_id == -1) {
        target_id = create_vlabel_from_path_internal(graph_id, new_path.data);
        CommandCounterIncrement(); 
    }
    
    pfree(new_path.data);
    return target_id;
}

uint64
np_tid_to_id(ItemPointer tid, uint32 tuples_per_page)
{
    BlockNumber block = ItemPointerGetBlockNumber(tid);
    OffsetNumber offset = ItemPointerGetOffsetNumber(tid);

    uint64 zero_based_id = ((uint64)block * tuples_per_page) + (offset - 1);
    
    return zero_based_id + 1;
}
PG_FUNCTION_INFO_V1(drop_vlabel);
Datum
drop_vlabel(PG_FUNCTION_ARGS)
{
    /* Arg 0 is `name` (64-byte struct), Arg 1 is `text` (varlena) */
    Name graph_name = PG_GETARG_NAME(0);
    char *graph_name_str = NameStr(*graph_name);
    
    text *label_text = PG_GETARG_TEXT_PP(1);
    char *label_str = text_to_cstring(label_text);

    Oid namespace = linitial_oid(fetch_search_path(false));
    const graph_cache_data *graph = search_graph_name_namespace_cache(graph_name_str, namespace);    
    if (!graph)
        ereport(ERROR, (errmsg("NeoPostGraph: Graph '%s' not found", graph_name_str)));

    int32 graph_id = graph->id;

    /* PASS 1: Native GiST Index Scan */
    Relation catalog_rel = table_open(graph->vertex_labels, AccessShareLock);

    ScanKeyData skey[1];
    ScanKeyInit(&skey[0], 2, 14,
        DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum("public.ltxtq_exec(public.ltree, public.ltxtquery)"))),
        DirectFunctionCall1(ltxtq_in, CStringGetDatum(label_str))
    );

    Oid idx_oid = np_relation_id(psprintf("np_vertex_label_%d_gist_idx", graph_id), "index");
    SysScanDesc cat_scan = systable_beginscan(catalog_rel, idx_oid, true, GetActiveSnapshot(), 1, skey);

    int max_drops = 128;
    int drop_count = 0;
    int32 *drop_list = palloc(sizeof(int32) * max_drops);

    HeapTuple cat_tuple;
    while (HeapTupleIsValid(cat_tuple = systable_getnext(cat_scan))) {
        bool isnull;
        int32 match_id = DatumGetInt32(heap_getattr(cat_tuple, 1, RelationGetDescr(catalog_rel), &isnull));
        
        if (drop_count >= max_drops) {
            max_drops *= 2;
            drop_list = repalloc(drop_list, sizeof(int32) * max_drops);
        }
        drop_list[drop_count++] = match_id;
    }
    systable_endscan(cat_scan);
    table_close(catalog_rel, AccessShareLock);

    /* PASS 2: Inline execution passing external_pmap_buf down */
    for (int i = 0; i < drop_count; i++) {
        int32 old_label_id = drop_list[i];
        
        const label_cache_data *label_cache = search_vertex_label_graph_id_label_id_cache(graph_id, old_label_id);
        Relation pmap = table_open(label_cache->phys_map, AccessShareLock);
        
        BlockNumber nblocks = RelationGetNumberOfBlocks(pmap);
        uint32 tpp = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
        
        for (BlockNumber blk = 0; blk < nblocks; blk++) {
            Buffer buf = ReadBuffer(pmap, blk);
            
            /* Acquire EXCLUSIVE lock once for the entire page */
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            
            Page page = BufferGetPage(buf);
            OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

            for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++) {
                ItemId lp = PageGetItemId(page, off);
                if (!ItemIdIsNormal(lp)) continue;

                NeoPhysMapRecord *rec = (NeoPhysMapRecord *) PageGetItem(page, lp);
                if (!ItemPointerIsValid(&rec->v_itemptr)) continue;

                /* THE FIX: Reconstruct the exact VID using the formal inverse function */
                ItemPointerData pmap_tid;
                ItemPointerSet(&pmap_tid, blk, off);
                int64 v_id = (int64) np_tid_to_id(&pmap_tid, tpp);
                
                /* Execute inline immediately - pass the exclusively locked buffer straight down */
                np_internal_remove_vertex_label(v_id, old_label_id, graph_id, label_str, buf);
            }
            
            /* Safely release when the page is completely migrated */
            UnlockReleaseBuffer(buf);
        }
        
        table_close(pmap, AccessShareLock);
    }

    pfree(drop_list);
    pfree(label_str);
    PG_RETURN_VOID();
}