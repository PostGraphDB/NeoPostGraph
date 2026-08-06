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
    
    char *tuple_buf = (char *) palloc(total_tuple_size);
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

