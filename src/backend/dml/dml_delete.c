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

#include "dml/dml_insert.h"
#include "access/np_linked_list.h"
#include "access/np_entity_store.h"

void np_delete_edge_from_adj_list(int32 graph_id, int64 vertex_id, int32 vertex_label_id, int64 target_edge_id, CommandId cid, FullTransactionId current_fxid);

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



