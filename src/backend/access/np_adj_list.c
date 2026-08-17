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
#include "fmgr.h"
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "access/table.h"
#include "access/tableam.h"
#include "storage/bufmgr.h"

#include "utils/np_cache.h"
#include "utils/adj_list.h"
#include "access/np_linked_list.h"
#include "access/np_arraylist.h"
#include "access/np_phys_map.h"
#include "catalog/np_label.h"

typedef struct CompactedVertexEntry {
    uint64 owner_id;                
    AdjList *adj;                   
    Oid upstream_tbl;               
    ItemPointerData upstream_tid;
    Oid downstream_tbl;             
    ItemPointerData downstream_tid;
} CompactedVertexEntry;

static AdjList *
np_append_adj_list(AdjList *list, AdjListMember *member);
extern Oid create_new_active_linked_list(int graph_id, int label_id, Oid ll_seq_oid, Oid ll_meta_oid, Oid namespace_oid);


/* 
 * Safely delete an arraylist record on the page directly since 
 * np_arraylist_am disabled table_tuple_delete.
 */
static void
delete_arraylist_record(Relation array_rel, ItemPointerData *target_tid)
{
    Buffer buffer = ReadBuffer(array_rel, ItemPointerGetBlockNumber(target_tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(target_tid));

    if (ItemIdIsNormal(lp))
    {
        GenericXLogState *state = GenericXLogStart(array_rel);
        page = GenericXLogRegisterBuffer(state, buffer, 0);
        lp = PageGetItemId(page, ItemPointerGetOffsetNumber(target_tid));
        
        NeoArrayListRecord *rec = (NeoArrayListRecord *) PageGetItem(page, lp);
        rec->xmax = GetTopFullTransactionId();
        rec->cmax = GetCurrentCommandId(true);
        
        GenericXLogFinish(state);
    }
    UnlockReleaseBuffer(buffer);
}

/*
 * np_merge_existing_arraylist
 * Fetches an existing arraylist block, appends its contents to the new AdjList,
 * deletes the old block, and returns the next pointer in the arraylist chain.
 */
static ItemPointerData
np_merge_existing_arraylist(Relation array_rel, ItemPointerData *target_tid, AdjList **adj)
{
    ItemPointerData next_tid;
    ItemPointerSetInvalid(&next_tid);

    if (!ItemPointerIsValid(target_tid))
        return next_tid;

    Buffer buffer = ReadBuffer(array_rel, ItemPointerGetBlockNumber(target_tid));
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buffer);
    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(target_tid));

    if (ItemIdIsNormal(lp))
    {
        NeoArrayListRecord *rec = (NeoArrayListRecord *) PageGetItem(page, lp);
        next_tid = rec->next_itemptr;

        Size raw_payload_size = ItemIdGetLength(lp) - offsetof(NeoArrayListRecord, adj_list_data);
        
        /* RESTORED: Your original, flawless re-padding logic */
        AdjList *old_adj = (AdjList *) palloc(VARHDRSZ + raw_payload_size);
        SET_VARSIZE(old_adj, VARHDRSZ + raw_payload_size);
        memcpy(VARDATA(old_adj), rec->adj_list_data, raw_payload_size);

        for (int i = 0; i < old_adj->nitems; i++)
        {
            *adj = np_append_adj_list(*adj, &old_adj->data[i]);
        }
        pfree(old_adj);
    }
    UnlockReleaseBuffer(buffer);

    delete_arraylist_record(array_rel, target_tid);

    return next_tid;
}

/*
 * np_insert_arraylist_block
 * Inserts a fully formed arraylist into the table and returns its physical TID.
 */
static ItemPointerData
np_insert_arraylist_block(Relation array_rel,
                          uint64 vertex_id,
                          AdjList *adj,
                          Oid prev_tbl,
                          ItemPointerData *prev_tid,
                          ItemPointerData *next_tid)
{
    TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(array_rel), &TTSOpsVirtual);
    ExecClearTuple(slot);

    ItemPointerData invalid_tid;
    ItemPointerSetInvalid(&invalid_tid);

    slot->tts_values[0] = Int64GetDatum(vertex_id);
    slot->tts_isnull[0] = false;
    
    slot->tts_values[1] = OidIsValid(prev_tbl) ? ObjectIdGetDatum(prev_tbl) : ObjectIdGetDatum(InvalidOid);
    slot->tts_isnull[1] = false;

    if (ItemPointerIsValid(prev_tid)) {
        slot->tts_values[2] = PointerGetDatum(prev_tid);
        slot->tts_isnull[2] = false;
    } else {
        slot->tts_isnull[2] = true;
    }

    slot->tts_values[3] = PointerGetDatum(adj);
    slot->tts_isnull[3] = false;

    if (ItemPointerIsValid(next_tid)) {
        slot->tts_values[4] = PointerGetDatum(next_tid);
        slot->tts_isnull[4] = false;
    } else {
        slot->tts_isnull[4] = true;
    }

    ExecStoreVirtualTuple(slot);
    
    table_tuple_insert(array_rel, slot, GetCurrentCommandId(true), 0, NULL);
    ItemPointerData new_array_tid = slot->tts_tid;

    ExecDropSingleTupleTableSlot(slot);
    return new_array_tid;
}

static AdjList *
np_init_adj_list(int32 initial_capacity)
{
    Size alloc_size = offsetof(AdjList, data) + (initial_capacity * sizeof(AdjListMember));
    AdjList *list = (AdjList *) palloc0(alloc_size);
    /* Set logical VARSIZE to exactly 0 items. It grows safely in np_append_adj_list */
    SET_VARSIZE(list, offsetof(AdjList, data));
    list->nitems = 0;
    list->maxitems = initial_capacity;
    return list;
}

static AdjList *
np_append_adj_list(AdjList *list, AdjListMember *member)
{
    if (list->nitems >= list->maxitems)
    {
        list->maxitems *= 2;
        Size new_size = offsetof(AdjList, data) + (list->maxitems * sizeof(AdjListMember));
        list = (AdjList *) repalloc(list, new_size);
    }
    list->data[list->nitems] = *member;
    list->nitems++;
    /* Keeps VARSIZE exactly pinned to the actual data count */
    SET_VARSIZE(list, offsetof(AdjList, data) + (list->nitems * sizeof(AdjListMember)));
    return list;
}

/*
 * np_trim_adj_list
 * Synchronizes maxitems with nitems so when this struct is read back from disk,
 * the application doesn't try to append to unmapped padding. No repalloc needed
 * because the TAM only reads up to VARSIZE.
 */
static void
np_trim_adj_list(AdjList *list)
{
    list->maxitems = list->nitems;
}

static void
np_update_next_pointer_inplace(Relation rel, ItemPointer tid, 
                               Oid new_next_tbl, ItemPointer new_next_tid, 
                               CommandId cid)
{
    Buffer buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);

    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));

    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: attempted in-place update on invalid or dead tuple during compaction");
    }

    GenericXLogState *state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(state, buffer, 0);
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    NeoLinkedListRecord *disk_rec = (NeoLinkedListRecord *) PageGetItem(page, lp);

    disk_rec->next_tbl = new_next_tbl;
    disk_rec->next_itemptr = *new_next_tid;

    GenericXLogFinish(state);

    UnlockReleaseBuffer(buffer);
}

static Oid
get_oldest_inactive_linked_list(Oid meta_oid)
{
    Relation meta_rel = table_open(meta_oid, AccessShareLock);
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple tuple;
    
    Oid oldest_oid = InvalidOid;
    int64 oldest_id = PG_INT64_MAX;
    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        bool isnull; 

        bool active = DatumGetBool(heap_getattr(tuple, 3, RelationGetDescr(meta_rel), &isnull));
        bool compacted = DatumGetBool(heap_getattr(tuple, 4, RelationGetDescr(meta_rel), &isnull));
        
        if (!active && !compacted)
        {
            int64 current_id = DatumGetInt64(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
            
            if (current_id < oldest_id)
            {
                oldest_id = current_id;
                oldest_oid = DatumGetObjectId(heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull));
            }
        }
    }
    
    systable_endscan(scan);
    table_close(meta_rel, AccessShareLock);
    
    return oldest_oid;
}
/* 
 * Calculate the exact maximum number of edges that can physically fit 
 * on a standard PostgreSQL 8KB page without triggering TOAST.
 */
#define MAX_EDGES_PER_BLOCK ((MaxHeapTupleSize - SizeOfNeoArrayListRecord) / sizeof(AdjListMember))

/*
 * In-place update for Arraylist chains. 
 * Reaches back to the previously inserted chunk and wires up its next_itemptr.
 */
static void
np_update_arraylist_next_pointer_inplace(Relation rel, ItemPointer tid, ItemPointer new_next_tid)
{
    Buffer buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    Page page = BufferGetPage(buffer);

    ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    if (!ItemIdIsNormal(lp)) {
        UnlockReleaseBuffer(buffer);
        elog(ERROR, "NeoPostGraph: attempted in-place update on invalid arraylist tuple");
    }

    GenericXLogState *state = GenericXLogStart(rel);
    page = GenericXLogRegisterBuffer(state, buffer, 0);
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    
    NeoArrayListRecord *disk_rec = (NeoArrayListRecord *) PageGetItem(page, lp);
    disk_rec->next_itemptr = *new_next_tid;

    GenericXLogFinish(state);
    UnlockReleaseBuffer(buffer);
}

/*
 * Slices a massive merged AdjList into perfectly maximized 8KB chunks.
 * Inserts them sequentially, wiring up a doubly-linked chain across disk blocks.
 */
static ItemPointerData
np_insert_chunked_arraylist(Relation array_rel,
                            uint64 vertex_id,
                            AdjList *adj,
                            Oid prev_tbl,
                            ItemPointerData *prev_tid,
                            ItemPointerData *downstream_tid)
{
    int total_edges = adj->nitems;
    
    if (total_edges == 0) {
        ItemPointerData invalid_tid;
        ItemPointerSetInvalid(&invalid_tid);
        return invalid_tid;
    }

    int chunks = (total_edges + MAX_EDGES_PER_BLOCK - 1) / MAX_EDGES_PER_BLOCK;
    
    Oid current_prev_tbl = prev_tbl;
    ItemPointerData current_prev_tid = *prev_tid;
    
    ItemPointerData first_chunk_tid;
    ItemPointerSetInvalid(&first_chunk_tid);
    
    ItemPointerData last_inserted_tid;
    ItemPointerSetInvalid(&last_inserted_tid);

    for (int i = 0; i < chunks; i++)
    {
        int edges_in_chunk = Min(MAX_EDGES_PER_BLOCK, total_edges - (i * MAX_EDGES_PER_BLOCK));
        AdjList *chunk_adj = np_init_adj_list(edges_in_chunk);
        
        for (int j = 0; j < edges_in_chunk; j++) {
            chunk_adj = np_append_adj_list(chunk_adj, &adj->data[(i * MAX_EDGES_PER_BLOCK) + j]);
        }
        np_trim_adj_list(chunk_adj);

        ItemPointerData next_tid;
        ItemPointerSetInvalid(&next_tid);
        
        /* The final tail chunk links to the existing downstream block */
        if (i == chunks - 1) {
            next_tid = *downstream_tid;
        }

        ItemPointerData new_tid = np_insert_arraylist_block(
            array_rel, vertex_id, chunk_adj, 
            current_prev_tbl, &current_prev_tid, &next_tid
        );

        if (i == 0) {
            first_chunk_tid = new_tid; /* The head of the chain */
        } else {
            /* Reach back and point the previous chunk to this new chunk */
            np_update_arraylist_next_pointer_inplace(array_rel, &last_inserted_tid, &new_tid);
        }

        current_prev_tbl = RelationGetRelid(array_rel);
        current_prev_tid = new_tid;
        last_inserted_tid = new_tid;
        
        pfree(chunk_adj);
    }

    return first_chunk_tid;
}

PG_FUNCTION_INFO_V1(compact_oldest_linked_list_table);
Datum
compact_oldest_linked_list_table(PG_FUNCTION_ARGS)
{
    Name graph_name = PG_GETARG_NAME(0);
    int32 label_id = PG_GETARG_INT32(1);
    CommandId cid = GetCurrentCommandId(true);

    Oid namespace = linitial_oid(fetch_search_path(false));

    const graph_cache_data *graph = search_graph_name_namespace_cache(NameStr(*graph_name), namespace);    
    if (!graph)
        ereport(ERROR, (errmsg("NeoPostGraph: Graph '%s' not found", NameStr(*graph_name))));

    const label_cache_data *label = search_vertex_label_graph_id_label_id_cache(graph->id, label_id);
    if (!label)
        ereport(ERROR, (errmsg("NeoPostGraph: Label %d not found for graph '%s'", label_id, NameStr(*graph_name))));

    Oid pmap_oid = label->phys_map;
    Oid arraylist_oid = label->arraylist;
    
    Oid oldest_ll_oid = get_oldest_inactive_linked_list(label->linked_list_meta);
    if (!OidIsValid(oldest_ll_oid))
    {
        create_new_active_linked_list(
            graph->id, 
            label_id, 
            label->linked_list_seq, 
            label->linked_list_meta, 
            namespace
        );

        oldest_ll_oid = get_oldest_inactive_linked_list(label->linked_list_meta);

        if (!OidIsValid(oldest_ll_oid))
            ereport(ERROR, (errmsg("NeoPostGraph: Compaction failed to successfully rotate the label")));
    }

    HASHCTL hash_ctl;
    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(uint64);
    hash_ctl.entrysize = sizeof(CompactedVertexEntry);
    
    HTAB *vertex_hash = hash_create("Compacted Vertices Hash", 1024, &hash_ctl, HASH_ELEM | HASH_BLOBS);

    Relation old_ll_rel = table_open(oldest_ll_oid, AccessShareLock);
    BlockNumber nblocks = RelationGetNumberOfBlocks(old_ll_rel);

    for (BlockNumber blk = 0; blk < nblocks; blk++)
    {
        Buffer buffer = ReadBuffer(old_ll_rel, blk);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);
        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
        {
            ItemId lp = PageGetItemId(page, off);
            if (!ItemIdIsNormal(lp)) continue;

            NeoLinkedListRecord *rec = (NeoLinkedListRecord *) PageGetItem(page, lp);

            bool found;
            CompactedVertexEntry *entry = hash_search(vertex_hash, &rec->owner_id, HASH_ENTER, &found);
            
            if (!found) {
                entry->adj = np_init_adj_list(4);
                entry->upstream_tbl = InvalidOid;
                ItemPointerSetInvalid(&entry->upstream_tid);
                entry->downstream_tbl = InvalidOid;
                ItemPointerSetInvalid(&entry->downstream_tid);
            }

            AdjListMember member = {
                .xmin = rec->xmin,         .xmax = rec->xmax,
                .cmin = rec->cmin,         .cmax = rec->cmax,
                .flags = 0,                .dir = rec->dir,
                .edge_id = rec->id,        .edge_lid = rec->edge_lid,
                .other_id = rec->other_id, .other_lid = rec->other_lid
            };

            entry->adj = np_append_adj_list(entry->adj, &member);

            if (rec->prev_tbl != oldest_ll_oid) {
                entry->upstream_tbl = rec->prev_tbl;
                entry->upstream_tid = rec->prev_itemptr;
            }

            if (rec->next_tbl != oldest_ll_oid && rec->next_tbl != InvalidOid) {
                Assert(!OidIsValid(rec->next_tbl) || rec->next_tbl == arraylist_oid);
                entry->downstream_tbl = rec->next_tbl;
                entry->downstream_tid = rec->next_itemptr;
            }
        }
        UnlockReleaseBuffer(buffer);
    }
    table_close(old_ll_rel, AccessShareLock);

    HASH_SEQ_STATUS hash_seq;
    CompactedVertexEntry *entry;
    hash_seq_init(&hash_seq, vertex_hash);

while ((entry = hash_seq_search(&hash_seq)) != NULL)
    {
        Relation array_rel = table_open(arraylist_oid, RowExclusiveLock);
        
        ItemPointerData next_array_tid;
        ItemPointerSetInvalid(&next_array_tid);

        /* 1. Extract the old disk block and append it to our RAM list */
        if (ItemPointerIsValid(&entry->downstream_tid))
        {
            next_array_tid = np_merge_existing_arraylist(array_rel, &entry->downstream_tid, &entry->adj);
        }

        /* 2. Trim the padding so we don't write dead space */
        np_trim_adj_list(entry->adj);

        /* 
         * 3. THE PLUG-IN POINT:
         * We used to call np_insert_arraylist_block here, which would explode 
         * if the varlena was > 8KB. 
         * Now, we hand the massive RAM list to the chunker. It slices it into 
         * MAX_EDGES_PER_BLOCK sizes, inserts them sequentially, wires up the 
         * doubly-linked next_itemptr/prev_itemptr chain, and returns the CTID 
         * of the VERY FIRST block it inserted (the head of the chain).
         */
        ItemPointerData new_array_tid = np_insert_chunked_arraylist(
            array_rel, entry->owner_id, entry->adj,
            entry->upstream_tbl, &entry->upstream_tid,
            &next_array_tid
        );

        table_close(array_rel, RowExclusiveLock);
/* 4. Wire the upstream pointer (Phys Map, Active Linked List, or Array List) to the head of the chain */
        if (OidIsValid(entry->upstream_tbl)) {
            Relation upstream_rel = table_open(entry->upstream_tbl, RowExclusiveLock);
            TupleDesc up_desc = RelationGetDescr(upstream_rel);
            
            /* DYNAMIC ROUTING: Never cast an Array List to a Linked List! */
            if (up_desc->natts == 10) { /* It's a Linked List */
                np_update_next_pointer_inplace(upstream_rel, &entry->upstream_tid, 
                                               arraylist_oid, &new_array_tid, cid);
            } 
            else if (up_desc->natts == 5) { /* It's an Array List */
                np_update_arraylist_next_pointer_inplace(upstream_rel, &entry->upstream_tid, 
                                                         &new_array_tid);
            }
            else {
                elog(ERROR, "NeoPostGraph: Unknown upstream table format in compactor");
            }
            
            table_close(upstream_rel, RowExclusiveLock);
        } else {
            Relation pmap_rel = table_open(pmap_oid, RowExclusiveLock);
            update_vertex_phys_map(pmap_rel, entry->owner_id, arraylist_oid, &new_array_tid, cid);
            table_close(pmap_rel, RowExclusiveLock);
        }
        pfree(entry->adj);
    }

    hash_destroy(vertex_hash);

    Relation meta_rel = table_open(label->linked_list_meta, RowExclusiveLock);
    SysScanDesc meta_scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple meta_tuple;

    while (HeapTupleIsValid(meta_tuple = systable_getnext(meta_scan)))
    {
        bool isnull;
        Oid tbl = DatumGetObjectId(heap_getattr(meta_tuple, 2, RelationGetDescr(meta_rel), &isnull));
        
        if (tbl == oldest_ll_oid)
        {
            Datum values[4];
            bool nulls[4];
            bool replace[4] = {false, false, false, true};

            values[3] = BoolGetDatum(true);
            nulls[3] = false;

            HeapTuple newtup = heap_modify_tuple(meta_tuple, RelationGetDescr(meta_rel), 
                                                 values, nulls, replace);
            np_catalog_update(meta_rel, meta_tuple, newtup);
            heap_freetuple(newtup);
            break;
        }
    }
    systable_endscan(meta_scan);
    table_close(meta_rel, RowExclusiveLock);

    PG_RETURN_VOID();
}