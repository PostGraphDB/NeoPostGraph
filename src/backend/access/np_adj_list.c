#include "postgres.h"
#include "fmgr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "access/table.h"
#include "storage/bufmgr.h"

#include "utils/np_cache.h"
#include "utils/adj_list.h"
#include "access/np_linked_list.h"
#include "access/np_phys_map.h"

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
        HeapTupleData oldtup;
        oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
        oldtup.t_len = ItemIdGetLength(lp);
        oldtup.t_self = *target_tid;
        oldtup.t_tableOid = RelationGetRelid(array_rel);

        TupleDesc tupdesc = RelationGetDescr(array_rel);
        bool isnull;

        /* 1. Extract the next pointer in the chain to maintain continuity */
        Datum old_next = heap_getattr(&oldtup, 5, tupdesc, &isnull);
        if (!isnull)
        {
            ItemPointer ip = (ItemPointer) DatumGetPointer(old_next);
            next_tid = *ip;
        }

        /* 2. Extract, detoast, and merge the old adjacency list */
        Datum old_adj_datum = heap_getattr(&oldtup, 4, tupdesc, &isnull);
        if (!isnull)
        {
            AdjList *old_adj = DATUM_GET_ADJ_LIST(old_adj_datum);
            for (int i = 0; i < old_adj->nitems; i++)
            {
                /* Safely update the pointer via double indirection */
                *adj = np_append_adj_list(*adj, &old_adj->data[i]);
            }
        }
    }
    UnlockReleaseBuffer(buffer);

    /* 3. Delete the old compacted block since its data is now merged */
    simple_heap_delete(array_rel, target_tid);

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
    Datum values[5];
    bool nulls[5] = {false, false, false, false, false};
    ItemPointerData invalid_tid;
    ItemPointerSetInvalid(&invalid_tid);

    values[0] = Int64GetDatum(vertex_id);
    values[1] = OidIsValid(prev_tbl) ? ObjectIdGetDatum(prev_tbl) : ObjectIdGetDatum(InvalidOid);
    values[2] = ItemPointerIsValid(prev_tid) ? PointerGetDatum(prev_tid) : PointerGetDatum(&invalid_tid);
    values[3] = PointerGetDatum(adj);
    values[4] = ItemPointerIsValid(next_tid) ? PointerGetDatum(next_tid) : PointerGetDatum(&invalid_tid);

    HeapTuple newtup = heap_form_tuple(RelationGetDescr(array_rel), values, nulls);
    
    simple_heap_insert(array_rel, newtup);
    ItemPointerData new_array_tid = newtup->t_self;

    heap_freetuple(newtup);
    return new_array_tid;
}

static AdjList *
np_init_adj_list(int32 initial_capacity)
{
    Size size = offsetof(AdjList, data) + (initial_capacity * sizeof(AdjListMember));
    AdjList *list = (AdjList *) palloc0(size);
    SET_VARSIZE(list, size);
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
    SET_VARSIZE(list, offsetof(AdjList, data) + (list->nitems * sizeof(AdjListMember)));
    return list;
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

    if (ItemIdIsNormal(lp)) {
        NeoLinkedListRecord *disk_rec = (NeoLinkedListRecord *) PageGetItem(page, lp);
        disk_rec->next_tbl = new_next_tbl;
        disk_rec->next_itemptr = *new_next_tid;
        MarkBufferDirty(buffer);
    }
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

static ItemPointerData
np_merge_and_insert_arraylist_block(Oid arraylist_tbl_oid,
                                    uint64 vertex_id,
                                    AdjList *adj,
                                    Oid prev_tbl,
                                    ItemPointerData *prev_tid,
                                    ItemPointerData *downstream_tid,
                                    CommandId cid)
{
    Relation rel = table_open(arraylist_tbl_oid, RowExclusiveLock);
    TransactionId old_xmin = InvalidTransactionId;
    ItemPointerData next_tid;
    ItemPointerSetInvalid(&next_tid);

    if (ItemPointerIsValid(downstream_tid))
    {
        Buffer buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(downstream_tid));
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);
        ItemId lp = PageGetItemId(page, ItemPointerGetOffsetNumber(downstream_tid));
        
        if (ItemIdIsNormal(lp))
        {
            HeapTupleData oldtup;
            oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
            oldtup.t_len = ItemIdGetLength(lp);
            oldtup.t_self = *downstream_tid;
            oldtup.t_tableOid = RelationGetRelid(rel);

            old_xmin = HeapTupleHeaderGetXmin(oldtup.t_data);
            TupleDesc tupdesc = RelationGetDescr(rel);
            bool isnull;

            Datum old_next = heap_getattr(&oldtup, 5, tupdesc, &isnull);
            if (!isnull)
            {
                ItemPointer ip = (ItemPointer) DatumGetPointer(old_next);
                next_tid = *ip;
            }

            Datum old_adj_datum = heap_getattr(&oldtup, 4, tupdesc, &isnull);
            if (!isnull)
            {
                AdjList *old_adj = DATUM_GET_ADJ_LIST(old_adj_datum);
                for (int i = 0; i < old_adj->nitems; i++)
                {
                    adj = np_append_adj_list(adj, &old_adj->data[i]);
                }
            }
        }
        UnlockReleaseBuffer(buffer);
        simple_heap_delete(rel, downstream_tid);
    }

    ItemPointerData invalid_tid;
    ItemPointerSetInvalid(&invalid_tid);

    Datum values[5] = {
        Int64GetDatum(vertex_id),
        OidIsValid(prev_tbl) ? ObjectIdGetDatum(prev_tbl) : ObjectIdGetDatum(InvalidOid),
        ItemPointerIsValid(prev_tid) ? PointerGetDatum(prev_tid) : PointerGetDatum(&invalid_tid),
        PointerGetDatum(adj),
        ItemPointerIsValid(&next_tid) ? PointerGetDatum(&next_tid) : PointerGetDatum(&invalid_tid)

    };
    bool nulls[5] = {false, false, false, false, false};

    HeapTuple newtup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
    
    if (TransactionIdIsValid(old_xmin))
        HeapTupleHeaderSetXmin(newtup->t_data, old_xmin);

    simple_heap_insert(rel, newtup);
    ItemPointerData new_array_tid = newtup->t_self;

    heap_freetuple(newtup);
    table_close(rel, RowExclusiveLock);

    return new_array_tid;
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
    
    /* 
     * Auto-Rotate Logic: If there are no pending uncompacted tables, 
     * force the currently active table to rotate out, then grab it.
     */
    if (!OidIsValid(oldest_ll_oid))
    {
        create_new_active_linked_list(
            graph->id, 
            label_id, 
            label->linked_list_seq, 
            label->linked_list_meta, 
            namespace
        );

        /* Re-fetch the ID of the table we just forced into an inactive state */
        oldest_ll_oid = get_oldest_inactive_linked_list(label->linked_list_meta);
        
        /* If it's STILL invalid (e.g., catalog corruption), bail safely */
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

        /* 1. If we captured a downstream pointer, fetch and merge it */
        if (ItemPointerIsValid(&entry->downstream_tid))
        {
            next_array_tid = np_merge_existing_arraylist(array_rel, &entry->downstream_tid, &entry->adj);
        }

        /* 2. Insert the newly packed arraylist block */
        ItemPointerData new_array_tid = np_insert_arraylist_block(
            array_rel, entry->owner_id, entry->adj,
            entry->upstream_tbl, &entry->upstream_tid,
            &next_array_tid
        );

        table_close(array_rel, RowExclusiveLock);

        /* 3. Update upstream links to point at the new block */
        if (OidIsValid(entry->upstream_tbl)) {
            Relation upstream_rel = table_open(entry->upstream_tbl, RowExclusiveLock);
            np_update_next_pointer_inplace(upstream_rel, &entry->upstream_tid, 
                                           arraylist_oid, &new_array_tid, cid);
            table_close(upstream_rel, RowExclusiveLock);
        } else {
            Relation pmap_rel = table_open(pmap_oid, RowExclusiveLock);
            update_vertex_phys_map(pmap_rel, entry->owner_id, InvalidOid, &new_array_tid, cid);
            table_close(pmap_rel, RowExclusiveLock);
        }

        /* Because we used double indirection in the merge, this pfree is 100% safe */
        pfree(entry->adj);
    }

    hash_destroy(vertex_hash);

/* Mark the processed table as compacted */
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
            CatalogTupleUpdate(meta_rel, &meta_tuple->t_self, newtup);
            heap_freetuple(newtup);
            break;
        }
    }
    systable_endscan(meta_scan);
    table_close(meta_rel, RowExclusiveLock);

    PG_RETURN_VOID();
}