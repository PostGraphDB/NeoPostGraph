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

#include "access/np_arraylist.h"
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
static int32 
resolve_or_create_target_edge_label(int32 graph_id, int32 current_label_id, const char *new_label_str, 
                                    const graph_cache_data *graph_cache, const char *raw_current_ltree);
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
static ArrayType *merge_and_dedupe_text_arrays(ArrayType *arr1, ArrayType *arr2)
{
    if (!arr1 && !arr2) return NULL;
    if (!arr1) return arr2;
    if (!arr2) return arr1;

    Datum *d1, *d2;
    bool *n1, *n2;
    int c1, c2;

    deconstruct_array(arr1, TEXTOID, -1, false, 'i', &d1, &n1, &c1);
    deconstruct_array(arr2, TEXTOID, -1, false, 'i', &d2, &n2, &c2);

    Datum *d_out = palloc((c1 + c2) * sizeof(Datum));
    int c_out = 0;

    for (int i = 0; i < c1; i++) {
        if (n1[i]) continue;
        d_out[c_out++] = d1[i];
    }

    for (int i = 0; i < c2; i++) {
        if (n2[i]) continue;
        char *s2 = TextDatumGetCString(d2[i]);
        bool duplicate = false;
        
        for (int j = 0; j < c_out; j++) {
            char *s1 = TextDatumGetCString(d_out[j]);
            if (strcmp(s1, s2) == 0) {
                duplicate = true;
                break;
            }
        }
        
        if (!duplicate) d_out[c_out++] = d2[i];
    }

    if (c_out == 0) return NULL;
    return construct_array(d_out, c_out, TEXTOID, -1, false, 'i');
}

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
        
        if (blk >= RelationGetNumberOfBlocks(rel)) {
            table_close(rel, AccessShareLock);
            break;
        }

        Buffer buf = ReadBuffer(rel, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&curr_tid));

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
            /* Linked List partition */
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

                np_internal_delete_edge(graph_id, e_lid, e_id, cid, current_fxid);

                if (e->start_id == old_v->id && e->start_label == old_v->label_id) {
                    e->start_id = new_v->id;
                    e->start_label = new_v->label_id;
                }
                if (e->end_id == old_v->id && e->end_label == old_v->label_id) {
                    e->end_id = new_v->id;
                    e->end_label = new_v->label_id;
                }

                vertex *actual_neighbor = (o_id == old_v->id && o_lid == old_v->label_id) ? new_v : neighbor_v;

                if (e->start_id == new_v->id && e->start_label == new_v->label_id) {
                    np_internal_insert_edge(new_v, actual_neighbor, e);
                } else {
                    np_internal_insert_edge(actual_neighbor, new_v, e);
                }

                pfree(neighbor_v);
                pfree(e);
            }
        } else if (desc->natts == 5) {
            NeoArrayListRecord *rec = (NeoArrayListRecord *) PageGetItem(page, lp);

            next_tbl = curr_tbl_id;
            next_tid = rec->next_itemptr;

            int32 raw_payload_size = (int32)ItemIdGetLength(lp) - (int32)offsetof(NeoArrayListRecord, adj_list_data);

            if (raw_payload_size <= 0 || raw_payload_size > 8192) {
                UnlockReleaseBuffer(buf);
                table_close(rel, AccessShareLock);
                break;
            }


            AdjList *adj_copy = (AdjList *) palloc(VARHDRSZ + raw_payload_size);
            SET_VARSIZE(adj_copy, VARHDRSZ + raw_payload_size);
            memcpy(VARDATA(adj_copy), rec->adj_list_data, raw_payload_size);

            UnlockReleaseBuffer(buf);
            table_close(rel, AccessShareLock);
/* Create arrays to hold the fetched payloads safely in memory */
            edge **migrated_edges = palloc(adj_copy->nitems * sizeof(edge *));
            vertex **migrated_neighbors = palloc(adj_copy->nitems * sizeof(vertex *));
            int migrated_count = 0;

            /* ==========================================
             * PASS 1: FETCH AND DELETE (ISOLATED)
             * ========================================== */
            for (int i = 0; i < adj_copy->nitems; i++) {
                if (FullTransactionIdIsValid(adj_copy->data[i].xmax)) continue;

                uint64 e_id = adj_copy->data[i].edge_id;
                int32 e_lid = adj_copy->data[i].edge_lid;
                int64 o_id = adj_copy->data[i].other_id;
                int32 o_lid = adj_copy->data[i].other_lid;

                vertex *neighbor_v = np_internal_fetch_vertex(graph_id, o_id, o_lid);
                edge *e = np_internal_fetch_edge(graph_id, e_id, e_lid);

                if (e == NULL || neighbor_v == NULL) {
                    if (e) pfree(e);
                    if (neighbor_v) pfree(neighbor_v);
                    continue;
                }

                /* Execute Delete. Array list WAL buffer is flushed and unpinned here. */
                np_internal_delete_edge(graph_id, e_lid, e_id, cid, current_fxid);

                /* Save the pristine payloads for Pass 2 */
                migrated_edges[migrated_count] = e;
                migrated_neighbors[migrated_count] = neighbor_v;
                migrated_count++;
            }

            /* ==========================================
             * PASS 2: MUTATE AND INSERT (ISOLATED)
             * ========================================== */
            for (int i = 0; i < migrated_count; i++) {
                edge *e = migrated_edges[i];
                vertex *neighbor_v = migrated_neighbors[i];

                /* Safely re-calculate o_id/o_lid from the saved neighbor */
                int64 o_id = neighbor_v->id;
                int32 o_lid = neighbor_v->label_id;

                if (e->start_id == old_v->id && e->start_label == old_v->label_id) {
                    e->start_id = new_v->id;
                    e->start_label = new_v->label_id;
                }
                if (e->end_id == old_v->id && e->end_label == old_v->label_id) {
                    e->end_id = new_v->id;
                    e->end_label = new_v->label_id;
                }

                vertex *actual_neighbor = (o_id == old_v->id && o_lid == old_v->label_id) ? new_v : neighbor_v;
                
                /* New Linked List buffers are pinned and written here, totally isolated from Delete */
                if (e->start_id == new_v->id && e->start_label == new_v->label_id)
                    np_internal_insert_edge(new_v, actual_neighbor, e);
                else 
                    np_internal_insert_edge(actual_neighbor, new_v, e);

                pfree(neighbor_v);
                pfree(e);
            }
            
            pfree(migrated_edges);
            pfree(migrated_neighbors);
            pfree(adj_copy);
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

    /* THE FIX: Let Postgres find the sequence dynamically via the search_path */
    RangeVar *rv = makeRangeVar(NULL, psprintf("vertex_label_id_seq_%d", graph_id), -1);
    Oid seq_oid = RangeVarGetRelid(rv, NoLock, false);
    
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

    /* 1. Open the Vertex Label Catalog for rewriting */
    Relation catalog_rel = table_open(graph->vertex_labels, RowExclusiveLock);
    TableScanDesc scan = table_beginscan(catalog_rel, GetActiveSnapshot(), 0, NULL);
    TupleDesc tupdesc = RelationGetDescr(catalog_rel);
    HeapTuple tuple;

    int dropped_count = 0;

    /* 2. Sequentially scan the catalog (O(1) relative to entity scale) */
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
        bool isnull;
        Datum ltree_datum = heap_getattr(tuple, 2, tupdesc, &isnull);
        char *path = DatumGetCString(DirectFunctionCall1(ltree_out, ltree_datum));

        /* Tokenize the ltree path by '.' */
        char *path_copy = pstrdup(path);
        char *tokens[256];
        int num_tokens = 0;
        char *tok = strtok(path_copy, ".");
        while (tok != NULL && num_tokens < 256) {
            tokens[num_tokens++] = tok;
            tok = strtok(NULL, ".");
        }

        bool modified = false;
        bool is_exact = false;
        
        StringInfoData new_path;
        initStringInfo(&new_path);

        /* 3. Rebuild the ltree path, omitting the dropped label */
        for (int i = 0; i < num_tokens; i++) {
            if (strcmp(tokens[i], label_str) == 0 && !modified) {
                modified = true;
                /* If the dropped label is the very last token, it is a leaf drop */
                if (i == num_tokens - 1) {
                    is_exact = true; 
                }
            } else {
                if (new_path.len > 0) appendStringInfoChar(&new_path, '.');
                appendStringInfoString(&new_path, tokens[i]);
            }
        }

        /* If the label dropped was the only label (e.g. _.person), fallback to root _ */
        if (new_path.len == 0) {
            appendStringInfoString(&new_path, "_");
        }

        /* 4. If the path contained the label, update the catalog tuple! */
        if (modified) {
            Datum values[10] = {0};
            bool nulls[10] = {0};
            bool replaces[10] = {0};

            /* Update Column 2 (Index 1): ltree */
            values[1] = DirectFunctionCall1(ltree_in, CStringGetDatum(new_path.data));
            replaces[1] = true;

            /* 
             * Update Column 10 (Index 9): is_primary
             * If it was a leaf drop, this table is no longer the primary storage
             * for its logical path. It becomes a secondary/archived table.
             */
            if (is_exact) {
                values[9] = BoolGetDatum(false);
                replaces[9] = true;
            }

            HeapTuple new_tuple = heap_modify_tuple(tuple, tupdesc, values, nulls, replaces);
            
            /* Use our safe table-AM updater to handle partial indexes correctly */
            np_catalog_update(catalog_rel, tuple, new_tuple);
            
            heap_freetuple(new_tuple);
            dropped_count++;
        }
        
        pfree(new_path.data);
        pfree(path_copy);
        pfree(path);
    }

    table_endscan(scan);
    table_close(catalog_rel, RowExclusiveLock);

    /* 5. Remove the structural label from the Global Label Catalog */
    char *cat_name = psprintf("np_label_catalog_%d", graph_id);
    Relation global_cat_rel = table_open(np_relation_id(cat_name, "table"), RowExclusiveLock);
    
    NameData name_val;
    namestrcpy(&name_val, label_str);
    ScanKeyData skey[2];
    ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&name_val));
    ScanKeyInit(&skey[1], 2, BTEqualStrategyNumber, F_CHAREQ, CharGetDatum('s'));
    
    SysScanDesc global_cat_scan = systable_beginscan(global_cat_rel, InvalidOid, false, NULL, 2, skey);
    HeapTuple cat_tuple;
    while (HeapTupleIsValid(cat_tuple = systable_getnext(global_cat_scan))) {
        CatalogTupleDelete(global_cat_rel, &cat_tuple->t_self);
    }
    systable_endscan(global_cat_scan);
    table_close(global_cat_rel, RowExclusiveLock);

    ereport(NOTICE, (errmsg("Structural label \"%s\" has been dropped. Modified %d physical partition(s) in O(1) time.", label_str, dropped_count)));

    pfree(label_str);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(rename_vlabel);
Datum
rename_vlabel(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *old_label;
    char *new_label;
    Oid nsp;
    const graph_cache_data *graph;
    Relation catalog_rel;
    TableScanDesc scan;
    TupleDesc tupdesc;
    HeapTuple tuple;
    char *cat_name;
    Relation global_cat_rel;
    ScanKeyData skey[1];
    SysScanDesc cat_scan;
    HeapTuple cat_tuple;
    Datum old_name_datum;
    Datum new_name_datum;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label must not be NULL")));
    if (PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("new label must not be NULL")));

    graph_name = NameStr(*PG_GETARG_NAME(0));
    old_label = text_to_cstring(PG_GETARG_TEXT_PP(1));
    new_label = text_to_cstring(PG_GETARG_TEXT_PP(2));

    if (old_label[0] == '\0' || new_label[0] == '\0')
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("label must not be empty")));
    if (strchr(old_label, '.') != NULL || strchr(new_label, '.') != NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("labels cannot contain dots")));
    if (strcmp(old_label, "_") == 0 || strcmp(new_label, "_") == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("cannot rename the root label \"_\"")));

    if (PG_ARGISNULL(3))
    {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("rename_vlabel requires a search path when namespace is not specified")));
        nsp = linitial_oid(search_path);
    }
    else
    {
        char *nsp_str = text_to_cstring(PG_GETARG_TEXT_PP(3));
        nsp = get_namespace_oid(nsp_str, true);
        if (!OidIsValid(nsp))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("namespace \"%s\" does not exist", nsp_str)));
    }

    graph = search_graph_name_namespace_cache(graph_name, nsp);
    if (!graph)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist in the namespace \"%s\"",
                               graph_name, get_namespace_name(nsp))));

    if (strcmp(old_label, new_label) == 0)
    {
        ereport(NOTICE, (errmsg("vlabel \"%s\" is already named \"%s\"",
                                old_label, new_label)));
        PG_RETURN_VOID();
    }

    cat_name = psprintf("np_label_catalog_%d", (int) graph->id);
    global_cat_rel = table_open(np_relation_id(cat_name, "table"), RowExclusiveLock);

    new_name_datum = CStringGetTextDatum(new_label);
    ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_TEXTEQ, new_name_datum);
    cat_scan = systable_beginscan(global_cat_rel, InvalidOid, false, NULL, 1, skey);
    if (HeapTupleIsValid(systable_getnext(cat_scan)))
    {
        systable_endscan(cat_scan);
        table_close(global_cat_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT),
                        errmsg("label \"%s\" already exists", new_label)));
    }
    systable_endscan(cat_scan);

    old_name_datum = CStringGetTextDatum(old_label);
    ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_TEXTEQ, old_name_datum);
    cat_scan = systable_beginscan(global_cat_rel, InvalidOid, false, NULL, 1, skey);
    cat_tuple = systable_getnext(cat_scan);
    if (!HeapTupleIsValid(cat_tuple))
    {
        systable_endscan(cat_scan);
        table_close(global_cat_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("vlabel \"%s\" does not exist", old_label)));
    }
    {
        bool isnull;
        char actual_type = DatumGetChar(heap_getattr(cat_tuple, 2, RelationGetDescr(global_cat_rel), &isnull));

        if (isnull || actual_type != 's')
        {
            systable_endscan(cat_scan);
            table_close(global_cat_rel, RowExclusiveLock);
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                            errmsg("vlabel \"%s\" does not exist", old_label)));
        }
    }

    catalog_rel = table_open(graph->vertex_labels, RowExclusiveLock);
    scan = table_beginscan(catalog_rel, GetActiveSnapshot(), 0, NULL);
    tupdesc = RelationGetDescr(catalog_rel);

    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool isnull;
        Datum ltree_datum = heap_getattr(tuple, 2, tupdesc, &isnull);
        char *path;
        char *path_copy;
        char *tokens[256];
        int num_tokens = 0;
        char *tok;
        bool modified = false;
        StringInfoData new_path;
        int32 label_id;

        if (isnull)
            continue;

        path = DatumGetCString(DirectFunctionCall1(ltree_out, ltree_datum));
        path_copy = pstrdup(path);
        tok = strtok(path_copy, ".");
        while (tok != NULL && num_tokens < 256)
        {
            tokens[num_tokens++] = tok;
            tok = strtok(NULL, ".");
        }

        initStringInfo(&new_path);
        for (int i = 0; i < num_tokens; i++)
        {
            const char *piece = tokens[i];

            if (strcmp(piece, old_label) == 0)
            {
                piece = new_label;
                modified = true;
            }
            if (new_path.len > 0)
                appendStringInfoChar(&new_path, '.');
            appendStringInfoString(&new_path, piece);
        }

        if (modified)
        {
            Datum values[10] = {0};
            bool nulls[10] = {0};
            bool replaces[10] = {0};
            HeapTuple new_tuple;

            values[1] = DirectFunctionCall1(ltree_in, CStringGetDatum(new_path.data));
            replaces[1] = true;
            new_tuple = heap_modify_tuple(tuple, tupdesc, values, nulls, replaces);
            np_catalog_update(catalog_rel, tuple, new_tuple);
            heap_freetuple(new_tuple);

            label_id = DatumGetInt32(heap_getattr(tuple, 1, tupdesc, &isnull));
            if (!isnull)
                invalidate_vertex_label_graph_id_label_id_cache_entry((int) graph->id, label_id);
        }

        pfree(new_path.data);
        pfree(path_copy);
        pfree(path);
    }

    table_endscan(scan);
    table_close(catalog_rel, RowExclusiveLock);

    {
        Datum values[2] = {0};
        bool nulls[2] = {0};
        bool replaces[2] = {0};
        HeapTuple new_cat;

        values[0] = new_name_datum;
        replaces[0] = true;
        new_cat = heap_modify_tuple(cat_tuple, RelationGetDescr(global_cat_rel), values, nulls, replaces);
        np_catalog_update(global_cat_rel, cat_tuple, new_cat);
        heap_freetuple(new_cat);
    }

    systable_endscan(cat_scan);
    table_close(global_cat_rel, RowExclusiveLock);
    CommandCounterIncrement();

    ereport(NOTICE, (errmsg("vlabel \"%s\" has been renamed to \"%s\"",
                            old_label, new_label)));
    PG_RETURN_VOID();
}

/*
 * drop_elabel — O(1) metadata-only drop of a structural edge-label token.
 *
 * Mirrors drop_vlabel: rewrite every ltree in np_edge_label_<G> that contains
 * the token, demote a leaf partition (is_primary = false), and leave entity
 * rows in their original physical tables.
 */
PG_FUNCTION_INFO_V1(drop_elabel);
Datum
drop_elabel(PG_FUNCTION_ARGS)
{
    Name graph_name = PG_GETARG_NAME(0);
    char *graph_name_str = NameStr(*graph_name);

    text *label_text = PG_GETARG_TEXT_PP(1);
    char *label_str = text_to_cstring(label_text);

    Oid namespace = linitial_oid(fetch_search_path(false));
    const graph_cache_data *graph = search_graph_name_namespace_cache(graph_name_str, namespace);
    if (!graph)
        ereport(ERROR, (errmsg("NeoPostGraph: Graph '%s' not found", graph_name_str)));

    int32 graph_id = graph->id;

    /* 1. Open the Edge Label Catalog for rewriting */
    Relation catalog_rel = table_open(graph->edge_labels, RowExclusiveLock);
    TableScanDesc scan = table_beginscan(catalog_rel, GetActiveSnapshot(), 0, NULL);
    TupleDesc tupdesc = RelationGetDescr(catalog_rel);
    HeapTuple tuple;

    int dropped_count = 0;

    /* 2. Sequentially scan the catalog (O(1) relative to entity scale) */
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
        bool isnull;
        Datum ltree_datum = heap_getattr(tuple, 2, tupdesc, &isnull);
        char *path = DatumGetCString(DirectFunctionCall1(ltree_out, ltree_datum));

        /* Tokenize the ltree path by '.' */
        char *path_copy = pstrdup(path);
        char *tokens[256];
        int num_tokens = 0;
        char *tok = strtok(path_copy, ".");
        while (tok != NULL && num_tokens < 256) {
            tokens[num_tokens++] = tok;
            tok = strtok(NULL, ".");
        }

        bool modified = false;
        bool is_exact = false;

        StringInfoData new_path;
        initStringInfo(&new_path);

        /* 3. Rebuild the ltree path, omitting the dropped label */
        for (int i = 0; i < num_tokens; i++) {
            if (strcmp(tokens[i], label_str) == 0 && !modified) {
                modified = true;
                /* If the dropped label is the very last token, it is a leaf drop */
                if (i == num_tokens - 1) {
                    is_exact = true;
                }
            } else {
                if (new_path.len > 0) appendStringInfoChar(&new_path, '.');
                appendStringInfoString(&new_path, tokens[i]);
            }
        }

        /* If the label dropped was the only label (e.g. _.knows), fallback to root _ */
        if (new_path.len == 0) {
            appendStringInfoString(&new_path, "_");
        }

        /* 4. If the path contained the label, update the catalog tuple */
        if (modified) {
            /* np_edge_label_<G> has 7 columns; is_primary is attnum 7 (index 6) */
            Datum values[7] = {0};
            bool nulls[7] = {0};
            bool replaces[7] = {0};

            /* Update Column 2 (Index 1): ltree */
            values[1] = DirectFunctionCall1(ltree_in, CStringGetDatum(new_path.data));
            replaces[1] = true;

            /*
             * Update Column 7 (Index 6): is_primary
             * If it was a leaf drop, this table is no longer the primary storage
             * for its logical path. It becomes a secondary/archived table.
             */
            if (is_exact) {
                values[6] = BoolGetDatum(false);
                replaces[6] = true;
            }

            HeapTuple new_tuple = heap_modify_tuple(tuple, tupdesc, values, nulls, replaces);

            np_catalog_update(catalog_rel, tuple, new_tuple);

            heap_freetuple(new_tuple);
            dropped_count++;
        }

        pfree(new_path.data);
        pfree(path_copy);
        pfree(path);
    }

    table_endscan(scan);
    table_close(catalog_rel, RowExclusiveLock);

    /* 5. Remove the structural label from the Global Label Catalog, if present */
    char *cat_name = psprintf("np_label_catalog_%d", graph_id);
    Relation global_cat_rel = table_open(np_relation_id(cat_name, "table"), RowExclusiveLock);

    NameData name_val;
    namestrcpy(&name_val, label_str);
    ScanKeyData skey[2];
    ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&name_val));
    ScanKeyInit(&skey[1], 2, BTEqualStrategyNumber, F_CHAREQ, CharGetDatum('s'));

    SysScanDesc global_cat_scan = systable_beginscan(global_cat_rel, InvalidOid, false, NULL, 2, skey);
    HeapTuple cat_tuple;
    while (HeapTupleIsValid(cat_tuple = systable_getnext(global_cat_scan))) {
        CatalogTupleDelete(global_cat_rel, &cat_tuple->t_self);
    }
    systable_endscan(global_cat_scan);
    table_close(global_cat_rel, RowExclusiveLock);

    ereport(NOTICE, (errmsg("Structural edge label \"%s\" has been dropped. Modified %d physical partition(s) in O(1) time.", label_str, dropped_count)));

    pfree(label_str);
    PG_RETURN_VOID();
}


PG_FUNCTION_INFO_V1(set_edge_label);
Datum
set_edge_label(PG_FUNCTION_ARGS)
{
    int64 edge_id = PG_GETARG_INT64(0);
    int32 current_label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);
    char *new_label_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    if (strchr(new_label_str, '.') != NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("labels cannot contain dots.")));

    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);
    const label_cache_data *current_label_cache = search_edge_label_graph_id_label_id_cache(graph_id, current_label_id);
    
    if (!current_label_cache) 
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("edge label %d not found", current_label_id)));

    /* SAFEGUARD 1: Extract OIDs to the local stack BEFORE target creation overwrites the cache struct */
    Oid safe_old_physmap_oid = current_label_cache->phys_map;
    Oid safe_old_edge_tbl_oid = current_label_cache->vertex_tbl;

    /* SAFEGUARD 2: Extract LTREE so the pointer isn't corrupted */
    char *raw_current_ltree = DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(current_label_cache->label)));
    
    /* 1. Resolve Target Edge Label */
    int32 target_label_id = resolve_or_create_target_edge_label(graph_id, current_label_id, new_label_str, graph_cache, raw_current_ltree);
    
    if (target_label_id == current_label_id) {
        pfree(raw_current_ltree);
        PG_RETURN_NULL();
    }
    pfree(raw_current_ltree);

    /* 2. Fetch Old Edge Payload via Edge Phys Map using SAFE OIDs */
    Relation old_physmap_rel = table_open(safe_old_physmap_oid, AccessShareLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(edge_id, pmap_tuples_per_page, &phys_map_tid);

    Buffer pmap_buf = ReadBuffer(old_physmap_rel, ItemPointerGetBlockNumber(&phys_map_tid));
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    NeoEdgePhysMapRecord pmap_rec = *((NeoEdgePhysMapRecord *) PageGetItem(BufferGetPage(pmap_buf), 
                                      PageGetItemId(BufferGetPage(pmap_buf), ItemPointerGetOffsetNumber(&phys_map_tid))));
    UnlockReleaseBuffer(pmap_buf);
    
    Relation old_e_rel = table_open(safe_old_edge_tbl_oid, AccessShareLock);
    Buffer old_e_buf = ReadBuffer(old_e_rel, ItemPointerGetBlockNumber(&pmap_rec.e_itemptr));
    LockBuffer(old_e_buf, BUFFER_LOCK_SHARE);
    
    NPEntityTupleHeader old_e_hdr = (NPEntityTupleHeader) PageGetItem(BufferGetPage(old_e_buf), 
                                    PageGetItemId(BufferGetPage(old_e_buf), ItemPointerGetOffsetNumber(&pmap_rec.e_itemptr)));

    /* Clone the payload in memory so we can safely mutate its label ID */
    edge *new_unpacked = (edge *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_e_hdr->serialized_entity));
    
    /* Safely cache the vertex routing info from the payload BEFORE we tombstone it */
    int64 start_v_id = new_unpacked->start_id;
    int32 start_v_label = new_unpacked->start_label;
    int64 end_v_id = new_unpacked->end_id;
    int32 end_v_label = new_unpacked->end_label;

    UnlockReleaseBuffer(old_e_buf);
    
    /* VERY IMPORTANT: Close relations so delete_edge doesn't self-deadlock */
    table_close(old_e_rel, AccessShareLock);
    table_close(old_physmap_rel, AccessShareLock);

    /* 3. Update Edge Label ID in memory */
    new_unpacked->label_id = target_label_id;

    /* 4. Tombstone the Old Edge AND its Adjacency Links */
    np_internal_delete_edge(graph_id, current_label_id, edge_id, cid, current_fxid);

    /* 5. Force Postgres to recognize the tombstone and newly forged tables */
    CommandCounterIncrement();
    AcceptInvalidationMessages();

    /* 6. Fetch Start and End Vertices (MUST BE DONE AFTER delete_edge to avoid stale lists!) */
    vertex *start_v = np_internal_fetch_vertex(graph_id, start_v_id, start_v_label);
    vertex *end_v = np_internal_fetch_vertex(graph_id, end_v_id, end_v_label);

    /* 7. Insert New Edge Payload & Wire Adjacency Lists */
    np_internal_insert_edge(start_v, end_v, new_unpacked); 

    pfree(start_v);
    pfree(end_v);

    PG_RETURN_DATUM(PointerGetDatum(new_unpacked));
}
static int32 
resolve_or_create_target_edge_label(int32 graph_id, int32 current_label_id, const char *new_label_str, 
                                    const graph_cache_data *graph_cache, const char *raw_current_ltree)
{
    char *base_ltree_str = psprintf("_.%s", new_label_str);
    Datum base_ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(base_ltree_str));
    int32 base_label_id = -1;

    /* 1. Find or Create Base Edge Label */
    PushActiveSnapshot(GetLatestSnapshot());
    Relation meta_rel = table_open(graph_cache->edge_labels, AccessShareLock);
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
        base_label_id = create_elabel_internal(NameStr(graph_cache->name), new_label_str, NULL, NULL, NULL); 
        CommandCounterIncrement();
    }
    
    /* 2. Determine target path */
    char *merged_ltree_str = NULL;

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

    /* 3. Find target edge label */
    Datum target_ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(merged_ltree_str));
    int32 target_label_id = -1;
    
    PushActiveSnapshot(GetLatestSnapshot());
    meta_rel = table_open(graph_cache->edge_labels, AccessShareLock);
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

    /* 4. FORGE TARGET DIRECTLY (Bypass the syscache entirely) */
    if (target_label_id == -1) {
        target_label_id = (int32) DatumGetInt64(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(graph_cache->edge_id_seq)));
        Oid edge_tbl = create_edge_tables(graph_cache->id, target_label_id, graph_cache->namespace);
        Oid phys_map = create_label_edge_physical_mapping_table(
                            psprintf("np_edge_%d_%d_phys_map", graph_cache->id, target_label_id), graph_cache->namespace);

        const label_cache_data *curr_lbl = search_edge_label_graph_id_label_id_cache(graph_id, current_label_id);
        const label_cache_data *base_lbl = search_edge_label_graph_id_label_id_cache(graph_id, base_label_id);
        
        ArrayType *merged_array = merge_and_dedupe_text_arrays(
            curr_lbl ? curr_lbl->annotation_map : NULL,
            base_lbl ? base_lbl->annotation_map : NULL
        );
        
        Datum merged_array_datum = (merged_array != NULL) ? PointerGetDatum(merged_array) : (Datum)0;
        Oid annot_tbl_oid = InvalidOid;
        
        if (merged_array != NULL) {
            int byte_alloc = (ArrayGetNItems(ARR_NDIM(merged_array), ARR_DIMS(merged_array)) + 7) / 8;
            annot_tbl_oid = create_vertex_label_annotation_table(
                                psprintf("np_edge_annotations_%d_%d", graph_cache->id, target_label_id), 
                                graph_cache->namespace, byte_alloc);
            insert_annotation_schema(graph_cache->id, target_label_id, merged_array, 
                                     graph_cache->annot_schema_tbl, graph_cache->annot_schema_phys_map);
        }
        /* ------------------------------------- */

        /* Write the EXACT computed string directly into the catalog */
        insert_label(
            psprintf("np_edge_label_%d", graph_cache->id),
            target_ltree_datum,
            target_label_id,
            edge_tbl,
            phys_map,
            annot_tbl_oid,
            merged_array_datum
        );

        CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
        char seq_name[NAMEDATALEN];
        snprintf(seq_name, NAMEDATALEN, "np_edge_id_seq_%d_%d", graph_cache->id, target_label_id);
        seq_stmt->sequence = makeRangeVar("neopostgraph", seq_name, -1);
        seq_stmt->options = NIL;
        seq_stmt->ownerId = GetUserId();
        seq_stmt->for_identity = false;
        seq_stmt->if_not_exists = false;

        DefineSequence(NULL, seq_stmt);
        CommandCounterIncrement();
    }
    
    pfree(merged_ltree_str);
    return target_label_id;
}

static int32 
resolve_target_edge_label_for_removal(int32 graph_id, int32 current_label_id, const char *label_to_remove, 
                                      const graph_cache_data *graph_cache, const char *raw_current_ltree)
{
    /* 1. Check if label is in path and build the new path */
    bool found = false;
    char *path_copy = pstrdup(raw_current_ltree);
    char *token = strtok(path_copy, ".");
    StringInfoData new_path;
    initStringInfo(&new_path);

    while (token != NULL) {
        if (strcmp(token, label_to_remove) == 0) {
            found = true;
        } else {
            if (new_path.len > 0) appendStringInfoChar(&new_path, '.');
            appendStringInfoString(&new_path, token);
        }
        token = strtok(NULL, ".");
    }
    pfree(path_copy);

    if (!found) {
        pfree(new_path.data);
        return current_label_id; /* Nothing to remove */
    }

    if (strcmp(new_path.data, "_") == 0) {
        pfree(new_path.data);
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), 
                errmsg("Cannot remove the base structural label of an edge")));
    }

    Datum target_ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(new_path.data));
    int32 target_label_id = -1;

    /* 2. Find target edge label in catalog */
    PushActiveSnapshot(GetLatestSnapshot());
    Relation meta_rel = table_open(graph_cache->edge_labels, AccessShareLock);
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, GetActiveSnapshot(), 0, NULL);
    HeapTuple tuple;
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

    /* 3. If the fallback path doesn't exist, forge it directly (Syscache bypass) */
    if (target_label_id == -1) {
        target_label_id = (int32) DatumGetInt64(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(graph_cache->edge_id_seq)));
        Oid edge_tbl = create_edge_tables(graph_cache->id, target_label_id, graph_cache->namespace);
        Oid phys_map = create_label_edge_physical_mapping_table(
                            psprintf("np_edge_%d_%d_phys_map", graph_cache->id, target_label_id), graph_cache->namespace);


        const label_cache_data *curr_lbl = search_edge_label_graph_id_label_id_cache(graph_id, current_label_id);
        ArrayType *merged_array = NULL;
        if (curr_lbl && curr_lbl->annotation_map) {
            merged_array = merge_and_dedupe_text_arrays(curr_lbl->annotation_map, NULL);
        }
        
        Datum merged_array_datum = (merged_array != NULL) ? PointerGetDatum(merged_array) : (Datum)0;
        Oid annot_tbl_oid = InvalidOid;

        if (merged_array != NULL) {
            int byte_alloc = (ArrayGetNItems(ARR_NDIM(merged_array), ARR_DIMS(merged_array)) + 7) / 8;
            annot_tbl_oid = create_vertex_label_annotation_table(
                                psprintf("np_edge_annotations_%d_%d", graph_cache->id, target_label_id), 
                                graph_cache->namespace, byte_alloc);
            insert_annotation_schema(graph_cache->id, target_label_id, merged_array, 
                                     graph_cache->annot_schema_tbl, graph_cache->annot_schema_phys_map);
        }

        insert_label(
            psprintf("np_edge_label_%d", graph_cache->id),
            target_ltree_datum,
            target_label_id,
            edge_tbl,
            phys_map,
            annot_tbl_oid,
            merged_array_datum
        );

        CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
        char seq_name[NAMEDATALEN];
        snprintf(seq_name, NAMEDATALEN, "np_edge_id_seq_%d_%d", graph_cache->id, target_label_id);
        seq_stmt->sequence = makeRangeVar("neopostgraph", seq_name, -1);
        seq_stmt->options = NIL;
        seq_stmt->ownerId = GetUserId();
        seq_stmt->for_identity = false;
        seq_stmt->if_not_exists = false;

        DefineSequence(NULL, seq_stmt);
        CommandCounterIncrement();
    }

    pfree(new_path.data);
    return target_label_id;
}
PG_FUNCTION_INFO_V1(remove_edge_label);
Datum
remove_edge_label(PG_FUNCTION_ARGS)
{
    int64 edge_id = PG_GETARG_INT64(0);
    int32 current_label_id = PG_GETARG_INT32(1);
    int32 graph_id = PG_GETARG_INT32(2);
    char *label_to_remove = text_to_cstring(PG_GETARG_TEXT_PP(3));

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);
    const label_cache_data *current_label_cache = search_edge_label_graph_id_label_id_cache(graph_id, current_label_id);
    
    if (!current_label_cache) 
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("edge label %d not found", current_label_id)));

    /* 1. Resolve Target Edge Label for Removal */
    char *raw_current_ltree = DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(current_label_cache->label)));
    
    int32 target_label_id = resolve_target_edge_label_for_removal(graph_id, current_label_id, label_to_remove, graph_cache, raw_current_ltree);
    
    if (target_label_id == current_label_id) {
        pfree(raw_current_ltree);
        pfree(label_to_remove);
        PG_RETURN_NULL(); /* Token not found, nothing to do */
    }
    pfree(raw_current_ltree);

    /* 2. Fetch Old Edge Payload via Edge Phys Map */
    Oid safe_old_physmap_oid = current_label_cache->phys_map;
    Oid safe_old_edge_tbl_oid = current_label_cache->vertex_tbl;

    Relation old_physmap_rel = table_open(safe_old_physmap_oid, AccessShareLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(edge_id, pmap_tuples_per_page, &phys_map_tid);

    Buffer pmap_buf = ReadBuffer(old_physmap_rel, ItemPointerGetBlockNumber(&phys_map_tid));
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    NeoEdgePhysMapRecord pmap_rec = *((NeoEdgePhysMapRecord *) PageGetItem(BufferGetPage(pmap_buf), 
                                      PageGetItemId(BufferGetPage(pmap_buf), ItemPointerGetOffsetNumber(&phys_map_tid))));
    UnlockReleaseBuffer(pmap_buf);
    
    Relation old_e_rel = table_open(safe_old_edge_tbl_oid, AccessShareLock);
    Buffer old_e_buf = ReadBuffer(old_e_rel, ItemPointerGetBlockNumber(&pmap_rec.e_itemptr));
    LockBuffer(old_e_buf, BUFFER_LOCK_SHARE);
    
    NPEntityTupleHeader old_e_hdr = (NPEntityTupleHeader) PageGetItem(BufferGetPage(old_e_buf), 
                                    PageGetItemId(BufferGetPage(old_e_buf), ItemPointerGetOffsetNumber(&pmap_rec.e_itemptr)));

    edge *new_unpacked = (edge *) PG_DETOAST_DATUM_COPY(PointerGetDatum(old_e_hdr->serialized_entity));
    
    /* Safely cache the vertex routing info BEFORE tombstoning */
    int64 start_v_id = new_unpacked->start_id;
    int32 start_v_label = new_unpacked->start_label;
    int64 end_v_id = new_unpacked->end_id;
    int32 end_v_label = new_unpacked->end_label;

    UnlockReleaseBuffer(old_e_buf);
    table_close(old_e_rel, AccessShareLock);
    table_close(old_physmap_rel, AccessShareLock);

    /* 3. Update Edge Label ID in memory */
    new_unpacked->label_id = target_label_id;

    /* 4. Tombstone Old Edge AND Adjacency Links */
    np_internal_delete_edge(graph_id, current_label_id, edge_id, cid, current_fxid);

    /* 5. Force Postgres to recognize the tombstone and newly forged tables */
    CommandCounterIncrement();
    AcceptInvalidationMessages();
    PopActiveSnapshot();
    PushActiveSnapshot(GetTransactionSnapshot());

    /* 6. Fetch fresh Start and End Vertices */
    vertex *start_v = np_internal_fetch_vertex(graph_id, start_v_id, start_v_label);
    vertex *end_v = np_internal_fetch_vertex(graph_id, end_v_id, end_v_label);

    /* 7. Insert New Edge Payload & Wire Adjacency Lists */
    np_internal_insert_edge(start_v, end_v, new_unpacked); 

    pfree(start_v);
    pfree(end_v);
    pfree(label_to_remove);

    PG_RETURN_DATUM(PointerGetDatum(new_unpacked));
}