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
#include "access/tableam.h"
#include "catalog/index.h"
#include "executor/tuptable.h"

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



Datum text_array_to_lxtquery(ArrayType *label_array);
Datum text_array_to_lxtquery_or(ArrayType *label_array);

Oid create_linked_list_table_sequence(char *seq_name, char *namespace);
Oid create_vertex_label_linked_list_metadata_table(char *tbl_name, Oid namespace);
int insert_vertex_ll_meta(char *table_name, Oid namespace, int ll_seq, Oid tbl);
Oid create_vertex_label_linked_list_table(char *tbl_name, Oid namespace);
int insert_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map);
Oid create_edge_tables(int graph_id, int label_id, Oid namespace);
Oid create_vertex_label_annotation_table(char *tbl_name, Oid namespace, int byte_allocation_size);

static int32 
resolve_or_create_target_label(int32 graph_id, int32 current_label_id, const char *new_label_str, 
                               const graph_cache_data *graph_cache, const char *raw_current_ltree);




/* 
 * Helper to safely extend and write to the schema phys map.
 * Mirrors the logic used for the edge physical map.
 */
void
np_place_physmap_record(Relation rel, ItemPointer tid, NeoPhysMapRecord *new_data)
{
    BlockNumber target_block = ItemPointerGetBlockNumber(tid);
    OffsetNumber target_offset = ItemPointerGetOffsetNumber(tid);
    BlockNumber nblocks;

    LockRelationForExtension(rel, ExclusiveLock);
    nblocks = RelationGetNumberOfBlocks(rel);
    while (nblocks <= target_block)
    {
        Buffer extend_buf = ReadBuffer(rel, P_NEW);
        LockBuffer(extend_buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(extend_buf);
        PageInit(page, BLCKSZ, 0);
        MarkBufferDirty(extend_buf);
        UnlockReleaseBuffer(extend_buf);
        nblocks++;
    }
    UnlockRelationForExtension(rel, ExclusiveLock);

    Buffer buffer = ReadBuffer(rel, target_block);
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    
    GenericXLogState *state = GenericXLogStart(rel);
    Page page = GenericXLogRegisterBuffer(state, buffer, 0);
    OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

    if (target_offset > maxoff)
    {
        NeoPhysMapRecord empty_pad;
        memset(&empty_pad, 0, sizeof(NeoPhysMapRecord));
        ItemPointerSetInvalid(&empty_pad.v_itemptr);
        ItemPointerSetInvalid(&empty_pad.e_itemptr);
        ItemPointerSetInvalid(&empty_pad.a_itemptr);
        empty_pad.e_tbl_id = InvalidOid;

        while (maxoff < target_offset - 1)
        {
            PageAddItemExtended(page, (Item) &empty_pad, sizeof(NeoPhysMapRecord), InvalidOffsetNumber, 0);
            maxoff++;
        }
        PageAddItemExtended(page, (Item) new_data, sizeof(NeoPhysMapRecord), InvalidOffsetNumber, 0);
    }
    else
    {
        ItemId lp = PageGetItemId(page, target_offset);
        if (ItemIdIsNormal(lp))
        {
            NeoPhysMapRecord *disk_rec = (NeoPhysMapRecord *) PageGetItem(page, lp);
            memcpy(disk_rec, new_data, sizeof(NeoPhysMapRecord));
        }
        else
        {
            PageAddItemExtended(page, (Item) new_data, sizeof(NeoPhysMapRecord), target_offset, PAI_OVERWRITE);
        }
    }

    GenericXLogFinish(state);
    UnlockReleaseBuffer(buffer);
}

/*
 * Inserts a new annotation schema array into the graph's schema entity store
 * and updates the schema physical map.
 */
void
insert_annotation_schema(int32 graph_id, int32 label_id, ArrayType *annot_array, Oid schema_tbl, Oid schema_pmap)
{
    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    /* 1. Write the array to the Schema Entity Store */
    Relation schema_rel = table_open(schema_tbl, RowExclusiveLock);

/* Ensure array is detoasted to a standard 4-byte varlena header before writing to disk */
ArrayType *clean_array = DatumGetArrayTypePCopy(PointerGetDatum(annot_array));
Size payload_size = VARSIZE(clean_array);
    Size total_tuple_size = MAXALIGN(SizeOfNPEntityTupleHeader + payload_size);

    char *tuple_buf = (char *) palloc0(total_tuple_size);
    NPEntityTupleHeader hdr = (NPEntityTupleHeader) tuple_buf;

    hdr->xmin = current_fxid;
    hdr->xmax = InvalidFullTransactionId;
    hdr->cmin = cid;
    hdr->cmax = InvalidCommandId;
    ItemPointerSetInvalid(&hdr->prev_itemptr);
    hdr->flags = 0;
    hdr->id = label_id;

memcpy(hdr->serialized_entity, clean_array, payload_size);
pfree(clean_array);

    ItemPointerData schema_tid;
    np_write_record_to_page(schema_rel, tuple_buf, total_tuple_size, &schema_tid);

    pfree(tuple_buf);
    table_close(schema_rel, RowExclusiveLock);

    /* 2. Write the O(1) routing pointer to the Schema Phys Map */
    Relation pmap_rel = table_open(schema_pmap, RowExclusiveLock);

    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(label_id, pmap_tuples_per_page, &phys_map_tid);

    NeoPhysMapRecord pmap_rec;
    memset(&pmap_rec, 0, sizeof(NeoPhysMapRecord));
    
    /* Store the pointer in v_itemptr to match our 4-column padding trick */
    pmap_rec.v_itemptr = schema_tid;
    ItemPointerSetInvalid(&pmap_rec.e_itemptr);
    ItemPointerSetInvalid(&pmap_rec.a_itemptr);
    pmap_rec.e_tbl_id = InvalidOid;

    np_place_physmap_record(pmap_rel, &phys_map_tid, &pmap_rec);

    table_close(pmap_rel, RowExclusiveLock);
}


PG_FUNCTION_INFO_V1(create_elabel);
Datum create_elabel(PG_FUNCTION_ARGS)
{
    // fetch the namespace the graph is created in
    Oid namespace;
    if (PG_ARGISNULL(2)) {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("create_vlabel requires a search path when namespace is not specified")));

        namespace = linitial_oid(search_path);
    } else if (!OidIsValid(namespace = get_namespace_oid(TextDatumGetCString(PG_GETARG_DATUM(2)), true))) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("namespace \"%s\" does not exist", TextDatumGetCString(PG_GETARG_DATUM(2)))));
    }

    // validate the label
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("ltree must not be NULL")));

    // fetch the graph name
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    char *graph_name = NameStr(*PG_GETARG_NAME(0));

    graph_cache_data *entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!entry)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                errmsg("graph \"%s\" does not exist \"%s\".", graph_name, get_namespace_name(namespace)),
                PG_ARGISNULL(1) ?
                    errhint("When namespace is not specified, the graph is created in the first namespace in the search path. Consider changing the search path or specifying a namespace explicitly.") :
                    errhint("Use a different graph name or create the graph.")
                ));

    Oid label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(entry->edge_id_seq)));
    Oid edge_tbl = create_edge_tables(entry->id, label_id, namespace);
    
    /* NEW: Spawn the router table */
    Oid phys_map = create_label_edge_physical_mapping_table(
                        psprintf("np_edge_%d_%d_phys_map", entry->id, label_id), namespace);
    
    insert_label(
        psprintf("np_edge_label_%d", entry->id),
        DirectFunctionCall2(ltree_addltree,
            DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)),
            PG_GETARG_DATUM(1)
        ),
        label_id,
        edge_tbl,
        phys_map /* Pass it to the catalog */
    );

    CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
    char seq_name[NAMEDATALEN];
    snprintf(seq_name, NAMEDATALEN, "np_edge_id_seq_%d_%d", entry->id, label_id);
    seq_stmt->sequence = makeRangeVar("neopostgraph", seq_name, -1);
    seq_stmt->options = NIL;
    seq_stmt->ownerId = GetUserId();
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;

    DefineSequence(NULL, seq_stmt);
    CommandCounterIncrement();

    ereport(NOTICE, (errmsg("elabel \"%s\" has been created", graph_name)));

    PG_RETURN_VOID();
}

Oid create_label_edge_physical_mapping_table(char *tbl_name, Oid namespace)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);
    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), tbl_name, -1);

    /* The optimized non-MVCC router: just a single physical pointer */
    create_stmt->tableElts = list_make1(makeColumnDef("e_itemptr", TIDOID, -1, InvalidOid));
    
    create_stmt->accessMethod = "np_mutable";
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver,
                   NULL);

    CommandCounterIncrement();

    return get_relname_relid(tbl_name, namespace);
}

Oid create_linked_list_table_sequence(char *seq_name, char *namespace)
{
    ParseState *pstate = make_parsestate(NULL);
    pstate->p_sourcetext = "(generated CREATE SEQUENCE command)";

    CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
    seq_stmt->sequence = makeRangeVar(namespace, seq_name, -1);
    seq_stmt->options = NIL;
    seq_stmt->ownerId = InvalidOid;
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;

    DefineSequence(pstate, seq_stmt);

    CommandCounterIncrement();

    return get_relname_relid(seq_name, get_namespace_oid(namespace, false));
}




int insert_vertex_ll_meta(char *table_name, Oid namespace, int ll_seq, Oid tbl)
{
    Relation rel = table_open(get_relname_relid(table_name, namespace), RowExclusiveLock);

    /* FIX: Pack the sequence ID first, and the table OID second */
    Datum values[4] = {
        Int32GetDatum(ll_seq),      /* Column 1: id (INT4OID) */
        ObjectIdGetDatum(tbl),      /* Column 2: tbl (REGCLASSOID) */
        BoolGetDatum(true),         /* Column 3: active (BOOLOID) */
        BoolGetDatum(false)         /* Column 4: compacted (BOOLOID) */
    };
    bool nulls[4] = { false, false, false, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
    return 0;
}

#include "access/tableam.h"
#include "catalog/index.h"
#include "executor/tuptable.h"

void insert_vertex_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map, Oid arraylist, Oid ll_seq, Oid ll_meta, Oid annotations_tbl, Datum annotation_map)
{
    Relation rel = table_open(np_relation_id(table_name, "table"), RowExclusiveLock);

    Datum values[10] = {
        ObjectIdGetDatum(label_id),
        label,
        ObjectIdGetDatum(tbl),
        ObjectIdGetDatum(phys_map),
        ObjectIdGetDatum(ll_meta),
        ObjectIdGetDatum(ll_seq),
        ObjectIdGetDatum(arraylist),
        ObjectIdGetDatum(annotations_tbl),
        annotation_map,
        BoolGetDatum(true) /* COL 10: is_primary = true */
    };
    
    bool nulls[10] = { false };
    if (annotation_map == (Datum)0)
        nulls[8] = true;

    HeapTuple tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

    /* Initialize slot FIRST, table_tuple_insert requires it */
    TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsHeapTuple);
    ExecStoreHeapTuple(tup, slot, false);

    /* 1. Insert directly into the table heap via slot */
    table_tuple_insert(rel, slot, GetCurrentCommandId(true), 0, NULL);

    /* 2. Manually insert into indexes */
    List *indexoidlist = RelationGetIndexList(rel);
    ListCell *lc;

    foreach(lc, indexoidlist) {
        Oid index_oid = lfirst_oid(lc);
        Relation indexDesc = index_open(index_oid, RowExclusiveLock);
        IndexInfo *indexInfo = BuildIndexInfo(indexDesc);

        Datum idx_values[INDEX_MAX_KEYS];
        bool idx_nulls[INDEX_MAX_KEYS];

        FormIndexDatum(indexInfo, slot, NULL, idx_values, idx_nulls);

        /* Missing 'false' for indexUnchanged parameter added */
        index_insert(indexDesc, idx_values, idx_nulls,
                     &(slot->tts_tid),
                     rel,
                     indexDesc->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
                     false,
                     indexInfo);

        index_close(indexDesc, RowExclusiveLock);
    }

    ExecDropSingleTupleTableSlot(slot);
    list_free(indexoidlist);
    heap_freetuple(tup);
    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
}

int insert_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map)
{
    Relation rel = table_open(np_relation_id(table_name, "table"), RowExclusiveLock);

    /* Update to 5 columns to account for the is_primary flag */
    Datum values[5] = {
        ObjectIdGetDatum(label_id),
        label,
        ObjectIdGetDatum(tbl),
        ObjectIdGetDatum(phys_map),
        BoolGetDatum(true) /* COL 5: is_primary = true */
    };
    
    /* Initialize all 5 to false safely */
    bool nulls[5] = { false };

    HeapTuple tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

    /* Create a slot specifically designed to hold an in-memory HeapTuple */
    TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsHeapTuple);
    ExecStoreHeapTuple(tup, slot, false);

    /* 1. Insert directly into the table heap via slot (Bypasses CatalogTupleInsert) */
    table_tuple_insert(rel, slot, GetCurrentCommandId(true), 0, NULL);

    /* 2. Manually insert into indexes, bypassing the CatalogIndexInsert assertion */
    List *indexoidlist = RelationGetIndexList(rel);
    ListCell *lc;

    foreach(lc, indexoidlist) {
        Oid index_oid = lfirst_oid(lc);
        Relation indexDesc = index_open(index_oid, RowExclusiveLock);
        IndexInfo *indexInfo = BuildIndexInfo(indexDesc);

        Datum idx_values[INDEX_MAX_KEYS];
        bool idx_nulls[INDEX_MAX_KEYS];

        /* Extract the specific index data from the tuple slot */
        FormIndexDatum(indexInfo, slot, NULL, idx_values, idx_nulls);

        /* Push into the index natively */
        index_insert(indexDesc, idx_values, idx_nulls,
                     &(slot->tts_tid),
                     rel,
                     indexDesc->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
                     false, /* indexUnchanged */
                     indexInfo);

        index_close(indexDesc, RowExclusiveLock);
    }

    /* Cleanup */
    ExecDropSingleTupleTableSlot(slot);
    list_free(indexoidlist);
    heap_freetuple(tup);
    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
    
    return 0;
}

typedef struct {
    SysScanDesc scan;
    Relation rel;
} GetVLabelContext;

PG_FUNCTION_INFO_V1(get_vlabel_ids_by_path);
Datum
get_vlabel_ids_by_path(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;

    if (SRF_IS_FIRSTCALL())
    {
        graph_cache_data *cache_entry;

        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        Oid namespace;
        if (PG_ARGISNULL(2)) 
        {
            List *search_path = fetch_search_path(false);
            if (list_length(search_path) < 1)
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("get_vlabel_ids requires a search path when namespace is not specified")));
            namespace = linitial_oid(search_path);
        } 
        else 
        {
            char *nsp_str = TextDatumGetCString(PG_GETARG_DATUM(2));
            namespace = get_namespace_oid(nsp_str, true);
            if (!OidIsValid(namespace))
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("namespace \"%s\" does not exist", nsp_str)));
        }

        if (PG_ARGISNULL(0))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph name must not be NULL")));
        char *graph_name = NameStr(*PG_GETARG_NAME(0));

        cache_entry = search_graph_name_namespace_cache(graph_name, namespace);
        if (!cache_entry)
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                            errmsg("graph \"%s\" does not exist in namespace \"%s\"", 
                            graph_name, get_namespace_name(namespace))));

        if (PG_ARGISNULL(1))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("label array must not be NULL")));

        Relation rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", cache_entry->id), "table"), AccessShareLock);

        ScanKeyData skey[1];
        ScanKeyInit(&skey[0], 2, 14,
            DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum("public.ltxtq_exec(public.ltree, public.ltxtquery)"))),
            text_array_to_lxtquery(PG_GETARG_ARRAYTYPE_P(1))
        ); 


        SysScanDesc scan = systable_beginscan(rel, np_relation_id(psprintf("np_vertex_label_%d_gist_idx", cache_entry->id), "index"), true, NULL, 1, skey);

        GetVLabelContext *fctx = palloc(sizeof(GetVLabelContext));
        fctx->scan = scan;
        fctx->rel = rel;
        funcctx->user_fctx = fctx;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    GetVLabelContext *fctx = (GetVLabelContext *) funcctx->user_fctx;
    SysScanDesc scan = fctx->scan;
    HeapTuple tuple;

    if ((tuple = systable_getnext(scan)) != NULL)
    {
        bool isnull;
        Datum id_val = heap_getattr(tuple, 1, RelationGetDescr(fctx->rel), &isnull);
        SRF_RETURN_NEXT(funcctx, id_val);
    }

    systable_endscan(scan);
    table_close(fctx->rel, AccessShareLock);
    SRF_RETURN_DONE(funcctx);
}

Datum
text_array_to_lxtquery(ArrayType *label_array)
{
    Datum *datums;
    bool *nulls;
    int count;
    deconstruct_array(label_array,
                      TEXTOID, -1, false, 'i',
                      &datums, &nulls, &count);

    if (count == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("label array must not be empty")));

    StringInfoData buf;
    initStringInfo(&buf);

    for (int i = 0; i < count; i++)
    {
        if (nulls[i])
            continue;

        char *label = TextDatumGetCString(datums[i]);

        if (buf.len > 0)
            appendStringInfoString(&buf, " & ");

        appendStringInfoString(&buf, label);
    }

    Datum result = DirectFunctionCall1(ltxtq_in, CStringGetDatum(buf.data));

    pfree(buf.data);
    pfree(datums);
    pfree(nulls);

    return result;
}

PG_FUNCTION_INFO_V1(get_or_vlabel_ids_by_path);
Datum
get_or_vlabel_ids_by_path(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;

    if (SRF_IS_FIRSTCALL())
    {
        graph_cache_data *cache_entry;

        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        Oid namespace;
        if (PG_ARGISNULL(2)) 
        {
            List *search_path = fetch_search_path(false);
            if (list_length(search_path) < 1)
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("get_vlabel_ids requires a search path when namespace is not specified")));
            namespace = linitial_oid(search_path);
        } 
        else 
        {
            char *nsp_str = TextDatumGetCString(PG_GETARG_DATUM(2));
            namespace = get_namespace_oid(nsp_str, true);
            if (!OidIsValid(namespace))
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("namespace \"%s\" does not exist", nsp_str)));
        }

        if (PG_ARGISNULL(0))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph name must not be NULL")));
        char *graph_name = NameStr(*PG_GETARG_NAME(0));

        cache_entry = search_graph_name_namespace_cache(graph_name, namespace);
        if (!cache_entry)
        {
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                            errmsg("graph \"%s\" does not exist in namespace \"%s\"", 
                            graph_name, get_namespace_name(namespace))));
        }

        if (PG_ARGISNULL(1))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("label array must not be NULL")));

        Relation rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", cache_entry->id), "table"), AccessShareLock);

        ScanKeyData skey[1];
        ScanKeyInit(&skey[0], 2, 14,
            DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum("public.ltxtq_exec(public.ltree, public.ltxtquery)"))),
            text_array_to_lxtquery_or(PG_GETARG_ARRAYTYPE_P(1))
        );

        SysScanDesc scan = systable_beginscan(rel, np_relation_id(psprintf("np_vertex_label_%d_gist_idx", cache_entry->id), "index"), true, NULL, 1, skey);

        GetVLabelContext *fctx = palloc(sizeof(GetVLabelContext));
        fctx->scan = scan;
        fctx->rel = rel;
        funcctx->user_fctx = fctx;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    GetVLabelContext *fctx = (GetVLabelContext *) funcctx->user_fctx;
    SysScanDesc scan = fctx->scan;
    HeapTuple tuple;

    if ((tuple = systable_getnext(scan)) != NULL)
    {
        bool isnull;
        Datum id_val = heap_getattr(tuple, 1, RelationGetDescr(fctx->rel), &isnull);
        SRF_RETURN_NEXT(funcctx, id_val);
    }
    else
    {
        systable_endscan(scan);
        table_close(fctx->rel, AccessShareLock);
        SRF_RETURN_DONE(funcctx);
    }
}

Datum
text_array_to_lxtquery_or(ArrayType *label_array)
{
    Datum *datums;
    bool *nulls;
    int count;

    deconstruct_array(label_array,
                      TEXTOID, -1, false, 'i',
                      &datums, &nulls, &count);

    if (count == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("label array must not be empty")));

    StringInfoData buf;
    initStringInfo(&buf);

    for (int i = 0; i < count; i++)
    {
        if (nulls[i])
            continue;

        char *label = TextDatumGetCString(datums[i]);

        if (buf.len > 0)
            appendStringInfoString(&buf, " | ");

        appendStringInfoString(&buf, label);
    }

    Datum result = DirectFunctionCall1(ltxtq_in, CStringGetDatum(buf.data));

    pfree(buf.data);
    pfree(datums);
    pfree(nulls);

    return result;
}



Oid
create_vertex_label_linked_list_table(char *tbl_name, Oid namespace)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), tbl_name, -1);
    
    //create_stmt->tableElts = NIL;
    create_stmt->tableElts = list_make1(makeColumnDef("id", INT8OID, -1, InvalidOid));
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("edge_lid", INT4OID, -1, InvalidOid)
    );
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("dir", CHAROID, -1, InvalidOid)
    );
    create_stmt->tableElts = lappend(create_stmt->tableElts, 
        makeColumnDef("owner_id", INT8OID, -1, InvalidOid)
    );
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("other_id", INT8OID, -1, InvalidOid)
    );    
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("other_lid", INT4OID, -1, InvalidOid)
    );
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("next_tbl", REGCLASSOID, -1, InvalidOid)
    );
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("next_itemptr", TIDOID, -1, InvalidOid)
    );
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("prev_tbl", REGCLASSOID, -1, InvalidOid)
    );
    create_stmt->tableElts = lappend(create_stmt->tableElts,
        makeColumnDef("prev_itemptr", TIDOID, -1, InvalidOid)
    );
    create_stmt->accessMethod = "nplinkedlist";
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    /* Wrap and execute */
    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated linked list CREATE TABLE)",
                   false, PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
                   None_Receiver, NULL);

    CommandCounterIncrement();

    return get_relname_relid(tbl_name, namespace);
}

Oid
create_vertex_label_linked_list_metadata_table(char *tbl_name, Oid namespace)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), tbl_name, -1);

    create_stmt->tableElts = list_make4(
        makeColumnDef("id", INT4OID, -1, InvalidOid),
        makeColumnDef("tbl", REGCLASSOID, -1, InvalidOid),
        makeColumnDef("active", BOOLOID, -1, InvalidOid),
        makeColumnDef("compacted", BOOLOID, -1, InvalidOid)
    );

    create_stmt->accessMethod = NULL;
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    /* Wrap and execute */
    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated arraylist CREATE TABLE)",
                   false, PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
                   None_Receiver, NULL);

    CommandCounterIncrement();

    return get_relname_relid(tbl_name, namespace);
}

Oid
create_vertex_label_arraylist_table(char *tbl_name, Oid namespace)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), tbl_name, -1);

    /* Fixed columns + one variable column (bytea for adjacency list) */
    create_stmt->tableElts = list_make5(
        makeColumnDef("id", INT8OID, -1, InvalidOid),
        makeColumnDef("prev_table", REGCLASSOID, 4, InvalidOid),
        makeColumnDef("prev_itemptr", TIDOID, -1, InvalidOid),
        makeColumnDef("adj_list", ADJLISTOID, -1, InvalidOid),
        makeColumnDef("next_itemptr", TIDOID, -1, InvalidOid)
    );

    create_stmt->accessMethod = "nparraylist";
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    /* Wrap and execute */
    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated arraylist CREATE TABLE)",
                   false, PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
                   None_Receiver, NULL);

    CommandCounterIncrement();

    return get_relname_relid(tbl_name, namespace);
}

Oid create_vertex_label_metadata_table(char *meta_tbl_name)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar("neopostgraph", meta_tbl_name, -1);
    
    ColumnDef *id = makeColumnDef("id", INT4OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    ColumnDef *ltree = makeColumnDef("ltree", LTREEOID, -1, InvalidOid);
    ltree->constraints = list_make1(build_not_null_constraint());
    ColumnDef *vertex_tbl = makeColumnDef("tbl", REGCLASSOID, -1, InvalidOid);
    vertex_tbl->constraints = list_make1(build_not_null_constraint());
    ColumnDef *phys_map = makeColumnDef("phys_map", REGCLASSOID, -1, InvalidOid);
    phys_map->constraints = list_make1(build_not_null_constraint());
    ColumnDef *linked_list_meta = makeColumnDef("linked_list_meta", REGCLASSOID, -1, InvalidOid);
    linked_list_meta->constraints = list_make1(build_not_null_constraint());
    ColumnDef *linked_list_seq = makeColumnDef("linked_list_seq", REGCLASSOID, -1, InvalidOid);
    linked_list_seq->constraints = list_make1(build_not_null_constraint());
    ColumnDef *arraylist = makeColumnDef("arraylist", REGCLASSOID, -1, InvalidOid);
    arraylist->constraints = list_make1(build_not_null_constraint());
    ColumnDef *annotations_tbl = makeColumnDef("annotations_tbl", REGCLASSOID, -1, InvalidOid);
    annotations_tbl->constraints = list_make1(build_not_null_constraint());
    ColumnDef *annotation_map = makeColumnDef("annotation_map", TEXTARRAYOID, -1, InvalidOid);
    ColumnDef *is_primary = makeColumnDef("is_primary", BOOLOID, -1, InvalidOid);
    is_primary->constraints = list_make1(build_not_null_constraint());

    List *tableElts = list_make5(id, ltree, vertex_tbl, phys_map, linked_list_meta);
    tableElts = lappend(tableElts, linked_list_seq);
    tableElts = lappend(tableElts, arraylist);
    tableElts = lappend(tableElts, annotations_tbl);
    tableElts = lappend(tableElts, annotation_map);
    tableElts = lappend(tableElts, is_primary);
    create_stmt->tableElts = tableElts;
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver,
                   NULL);
    
    CommandCounterIncrement();

    return get_relname_relid(meta_tbl_name, get_namespace_oid("neopostgraph", false));
}

void
enforce_and_insert_label_catalog(Relation cat_rel, Relation idx_rel, char *label_name, char expected_type)
{
    ScanKeyData skey[1];
    Datum name_datum = CStringGetTextDatum(label_name);
    
    ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_TEXTEQ, name_datum);

    SysScanDesc scan = systable_beginscan(cat_rel, RelationGetRelid(idx_rel), true, NULL, 1, skey);
    HeapTuple tuple = systable_getnext(scan);

    if (HeapTupleIsValid(tuple)) {
        bool isnull;
        char actual_type = DatumGetChar(heap_getattr(tuple, 2, RelationGetDescr(cat_rel), &isnull));
        
        if (actual_type != expected_type) {
            systable_endscan(scan);
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Label '%s' is already registered as a %s label", 
                           label_name, actual_type == 's' ? "structural" : "annotation")));
        }
    } else {
        /* Not found, safe to insert */
        Datum values[2] = { name_datum, CharGetDatum(expected_type) };
        bool nulls[2] = { false, false };

        HeapTuple newtup = heap_form_tuple(RelationGetDescr(cat_rel), values, nulls);
        CatalogTupleInsert(cat_rel, newtup);
        heap_freetuple(newtup);
    }
    
    systable_endscan(scan);
}

PG_FUNCTION_INFO_V1(add_vertex_annotation_label);
Datum
add_vertex_annotation_label(PG_FUNCTION_ARGS)
{
    /* 1. Extract Arguments */
    int64 vertex_id = PG_GETARG_INT64(0);
    int32 label_id  = PG_GETARG_INT32(1);
    int32 graph_id  = PG_GETARG_INT32(2);
    char *annot_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    /* 2. Look up the Label Cache */
    const label_cache_data *label_cache =
        search_vertex_label_graph_id_label_id_cache(graph_id, label_id);

    if (!label_cache || !OidIsValid(label_cache->annotations_tbl))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Structural label does not support annotations: graph_id=%d, label_id=%d",
                        graph_id, label_id)));

    /* 
     * 3. Resolve the Current Active Schema Array 
     * We load from the Entity Store via annot_schema_phys_map to ensure we see 
     * any annotations added via add_annotation_label DDL commands.
     */
    ArrayType *map_array = NULL;
    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);

    if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
        uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
        ItemPointerData schema_pmap_tid;
        np_id_to_tid(label_id, pmap_tuples_per_page, &schema_pmap_tid);

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

    /* Fallback to label_cache if Entity Store had no entry */
    if (map_array == NULL) {
        map_array = label_cache->annotation_map;
    }

    if (map_array == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("No annotation schema defined for this structural label")));

    /* 4. Map the requested annotation string to its bit position */
    Datum *map_d;
    bool *map_n;
    int map_count;
    deconstruct_array(map_array, TEXTOID, -1, false, 'i', &map_d, &map_n, &map_count);

    int bit_pos = -1;
    for (int i = 0; i < map_count; i++) {
        if (map_n[i]) continue;
        if (strcmp(annot_str, TextDatumGetCString(map_d[i])) == 0) {
            bit_pos = i;
            break;
        }
    }

    if (bit_pos == -1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Annotation label '%s' is not valid for this structural label", annot_str)));

    int byte_size = (map_count + 7) / 8;

    /* 5. Lookup the Vertex in its phys_map to get current a_itemptr */
    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(vertex_id, pmap_tuples_per_page, &phys_map_tid);

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

    ItemPointerData old_annot_tid = current_pmap_rec.a_itemptr;

    /* 6. Read Existing Bitset (or Allocate a Clean One) */
    bytea *bitset = (bytea *) palloc0(VARHDRSZ + byte_size);
    SET_VARSIZE(bitset, VARHDRSZ + byte_size);
    char *bits = VARDATA(bitset);

    Relation annot_rel = table_open(label_cache->annotations_tbl, RowExclusiveLock);

    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_check = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_check, BUFFER_LOCK_SHARE);
        Page opage_check = BufferGetPage(obuf_check);
        ItemId olp_check = PageGetItemId(opage_check, ItemPointerGetOffsetNumber(&old_annot_tid));

        if (ItemIdIsNormal(olp_check)) {
            NPEntityTupleHeader old_hdr = (NPEntityTupleHeader) PageGetItem(opage_check, olp_check);
            
            /* Concurrency Check: ensure no one deleted/updated this annotation bitset concurrently */
            if (FullTransactionIdIsValid(old_hdr->xmax)) {
                UnlockReleaseBuffer(obuf_check);
                table_close(annot_rel, RowExclusiveLock);
                table_close(pmap_rel, RowExclusiveLock);
                ereport(ERROR, (errmsg("Vertex ID %ld annotation bitset was concurrently updated", vertex_id)));
            }

            /* Copy existing bits into our buffer */
            bytea *old_bitset = (bytea *) old_hdr->serialized_entity;
            int old_len = VARSIZE(old_bitset) - VARHDRSZ;
            memcpy(bits, VARDATA(old_bitset), old_len > byte_size ? byte_size : old_len);
        }
        UnlockReleaseBuffer(obuf_check);
    }

    /* 7. Set the requested bit */
    bits[bit_pos / 8] |= (1 << (bit_pos % 8));

    /* 8. Construct & Write New Annotation Entity Tuple */
    Size annot_payload_size = VARSIZE(bitset);
    Size annot_total_size = MAXALIGN(SizeOfNPEntityTupleHeader + annot_payload_size);

    char *annot_tuple_buf = (char *) palloc0(annot_total_size);
    NPEntityTupleHeader annot_hdr = (NPEntityTupleHeader) annot_tuple_buf;

    annot_hdr->xmin = current_fxid;
    annot_hdr->xmax = InvalidFullTransactionId;
    annot_hdr->cmin = cid;
    annot_hdr->cmax = InvalidCommandId;
    annot_hdr->prev_itemptr = old_annot_tid; /* Link backwards to old version */
    annot_hdr->flags = 0;
    annot_hdr->id = vertex_id;

    memcpy(annot_hdr->serialized_entity, bitset, annot_payload_size);

    ItemPointerData new_annot_tid;
    np_write_record_to_page(annot_rel, annot_tuple_buf, annot_total_size, &new_annot_tid);
    pfree(annot_tuple_buf);

    /* 9. Update the Vertex phys_map to point to the new bitset tuple */
    current_pmap_rec.a_itemptr = new_annot_tid;
    np_overwrite_physmap_in_page(pmap_rel, &phys_map_tid, &current_pmap_rec);
    table_close(pmap_rel, RowExclusiveLock);

    /* 10. MVCC Tombstone the OLD Annotation Tuple (if one existed) */
    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_final = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_final, BUFFER_LOCK_EXCLUSIVE);

        GenericXLogState *state = GenericXLogStart(annot_rel);
        Page wal_page = GenericXLogRegisterBuffer(state, obuf_final, 0);

        ItemId olp_final = PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&old_annot_tid));
        NPEntityTupleHeader wal_old_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, olp_final);

        wal_old_hdr->xmax = current_fxid;
        wal_old_hdr->cmax = cid;

        GenericXLogFinish(state);
        UnlockReleaseBuffer(obuf_final);
    }

    table_close(annot_rel, RowExclusiveLock);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(remove_vertex_annotation_label);
Datum
remove_vertex_annotation_label(PG_FUNCTION_ARGS)
{
    /* 1. Extract Arguments */
    int64 vertex_id = PG_GETARG_INT64(0);
    int32 label_id  = PG_GETARG_INT32(1);
    int32 graph_id  = PG_GETARG_INT32(2);
    char *annot_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    /* 2. Look up the Label Cache */
    const label_cache_data *label_cache =
        search_vertex_label_graph_id_label_id_cache(graph_id, label_id);

    if (!label_cache || !OidIsValid(label_cache->annotations_tbl))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Structural label does not support annotations: graph_id=%d, label_id=%d",
                        graph_id, label_id)));

    /* 
     * 3. Resolve the Current Active Schema Array 
     * We load from the Entity Store via annot_schema_phys_map to ensure we see 
     * any annotations added via add_annotation_label DDL commands.
     */
    ArrayType *map_array = NULL;
    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);

    if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
        uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
        ItemPointerData schema_pmap_tid;
        np_id_to_tid(label_id, pmap_tuples_per_page, &schema_pmap_tid);

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

    /* Fallback to label_cache if Entity Store had no entry */
    if (map_array == NULL) {
        map_array = label_cache->annotation_map;
    }

    if (map_array == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("No annotation schema defined for this structural label")));

    /* 4. Map the requested annotation string to its bit position */
    Datum *map_d;
    bool *map_n;
    int map_count;
    deconstruct_array(map_array, TEXTOID, -1, false, 'i', &map_d, &map_n, &map_count);

    int bit_pos = -1;
    for (int i = 0; i < map_count; i++) {
        if (map_n[i]) continue;
        if (strcmp(annot_str, TextDatumGetCString(map_d[i])) == 0) {
            bit_pos = i;
            break;
        }
    }

    if (bit_pos == -1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Annotation label '%s' is not valid for this structural label", annot_str)));

    int byte_size = (map_count + 7) / 8;

    /* 5. Lookup the Vertex in its phys_map to get current a_itemptr */
    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(vertex_id, pmap_tuples_per_page, &phys_map_tid);

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

    ItemPointerData old_annot_tid = current_pmap_rec.a_itemptr;

    /* 6. Read Existing Bitset (or Allocate a Clean One) */
    bytea *bitset = (bytea *) palloc0(VARHDRSZ + byte_size);
    SET_VARSIZE(bitset, VARHDRSZ + byte_size);
    char *bits = VARDATA(bitset);

    Relation annot_rel = table_open(label_cache->annotations_tbl, RowExclusiveLock);

    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_check = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_check, BUFFER_LOCK_SHARE);
        Page opage_check = BufferGetPage(obuf_check);
        ItemId olp_check = PageGetItemId(opage_check, ItemPointerGetOffsetNumber(&old_annot_tid));

        if (ItemIdIsNormal(olp_check)) {
            NPEntityTupleHeader old_hdr = (NPEntityTupleHeader) PageGetItem(opage_check, olp_check);
            
            /* Concurrency Check: ensure no one deleted/updated this annotation bitset concurrently */
            if (FullTransactionIdIsValid(old_hdr->xmax)) {
                UnlockReleaseBuffer(obuf_check);
                table_close(annot_rel, RowExclusiveLock);
                table_close(pmap_rel, RowExclusiveLock);
                ereport(ERROR, (errmsg("Vertex ID %ld annotation bitset was concurrently updated", vertex_id)));
            }

            /* Copy existing bits into our buffer */
            bytea *old_bitset = (bytea *) old_hdr->serialized_entity;
            int old_len = VARSIZE(old_bitset) - VARHDRSZ;
            memcpy(bits, VARDATA(old_bitset), old_len > byte_size ? byte_size : old_len);
        }
        UnlockReleaseBuffer(obuf_check);
    }

    /* 
     * 7. Clear the requested bit using AND with inverted mask 
     * This turns off the bit while leaving all other bits unchanged.
     */
    bits[bit_pos / 8] &= ~(1 << (bit_pos % 8));

    /* 8. Construct & Write New Annotation Entity Tuple */
    Size annot_payload_size = VARSIZE(bitset);
    Size annot_total_size = MAXALIGN(SizeOfNPEntityTupleHeader + annot_payload_size);

    char *annot_tuple_buf = (char *) palloc0(annot_total_size);
    NPEntityTupleHeader annot_hdr = (NPEntityTupleHeader) annot_tuple_buf;

    annot_hdr->xmin = current_fxid;
    annot_hdr->xmax = InvalidFullTransactionId;
    annot_hdr->cmin = cid;
    annot_hdr->cmax = InvalidCommandId;
    annot_hdr->prev_itemptr = old_annot_tid; /* Link backwards to old version */
    annot_hdr->flags = 0;
    annot_hdr->id = vertex_id;

    memcpy(annot_hdr->serialized_entity, bitset, annot_payload_size);

    ItemPointerData new_annot_tid;
    np_write_record_to_page(annot_rel, annot_tuple_buf, annot_total_size, &new_annot_tid);
    pfree(annot_tuple_buf);

    /* 9. Update the Vertex phys_map to point to the new bitset tuple */
    current_pmap_rec.a_itemptr = new_annot_tid;
    np_overwrite_physmap_in_page(pmap_rel, &phys_map_tid, &current_pmap_rec);
    table_close(pmap_rel, RowExclusiveLock);

    /* 10. MVCC Tombstone the OLD Annotation Tuple (if one existed) */
    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_final = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_final, BUFFER_LOCK_EXCLUSIVE);

        GenericXLogState *state = GenericXLogStart(annot_rel);
        Page wal_page = GenericXLogRegisterBuffer(state, obuf_final, 0);

        ItemId olp_final = PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&old_annot_tid));
        NPEntityTupleHeader wal_old_hdr = (NPEntityTupleHeader) PageGetItem(wal_page, olp_final);

        wal_old_hdr->xmax = current_fxid;
        wal_old_hdr->cmax = cid;

        GenericXLogFinish(state);
        UnlockReleaseBuffer(obuf_final);
    }

    table_close(annot_rel, RowExclusiveLock);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(add_annotation_label);
Datum add_annotation_label(PG_FUNCTION_ARGS)
{
    /* 1. Resolve Namespace */
    Oid namespace;
    if (PG_ARGISNULL(3)) {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("add_annotation_label requires a search path when namespace is not specified")));
        namespace = linitial_oid(search_path);
    } else {
        char *nsp_str = TextDatumGetCString(PG_GETARG_DATUM(3));
        namespace = get_namespace_oid(nsp_str, true);
        if (!OidIsValid(namespace))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("namespace \"%s\" does not exist", nsp_str)));
    }

    /* 2. Extract Arguments */
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name, structural label, and new annotation label must not be NULL")));

    char *graph_name = NameStr(*PG_GETARG_NAME(0));
    char *struct_label_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *new_annot_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

    if (strchr(new_annot_str, '.') != NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Annotation labels cannot contain dots.")));

    /* 3. Look up Graph Cache */
    graph_cache_data *graph_entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!graph_entry)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist", graph_name)));

    /* 4. Open the Metadata Table and Find the Base Label's ltree */
    Relation meta_rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", graph_entry->id), "table"), RowExclusiveLock);
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple tuple;
    
    Datum target_ltree = (Datum)0;
    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        char *cached_str = DatumGetCString(DirectFunctionCall1(ltree_out, ltree_val));
        
        char *start = cached_str;
        if (strncmp(start, "_.", 2) == 0) start += 2;
        else if (strcmp(start, "_") == 0) start = "";

        if (strcmp(start, struct_label_str) == 0) {
            target_ltree = ltree_val;
            pfree(cached_str);
            break;
        }
        pfree(cached_str);
    }
    systable_endscan(scan);

    if (target_ltree == (Datum)0) {
        table_close(meta_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("Structural label \"%s\" not found in graph \"%s\"", struct_label_str, graph_name)));
    }
    target_ltree = PointerGetDatum(PG_DETOAST_DATUM(target_ltree));

    /* 5. Second Scan: Iterate over all labels in the hierarchy */
    
    scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));

    int updated_count = 0;

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        Datum row_ltree = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);

        /* 
         * Check if this row is an ltree descendant of target_ltree using ltree_isancestor (@>)
         * This matches _.person, _.person.employee, _.person.employee.engineer, etc.
         */
        bool is_descendant = DatumGetBool(DirectFunctionCall2(ltree_isparent, target_ltree, row_ltree));

        if (is_descendant) {
            int32 label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));

            /* A. Read Latest Schema from Entity Store for this label_id */
            /* Lock the physical vertex table so concurrent DML inserts wait for the schema commit */
            char *vertex_tbl_name = psprintf("np_vertex_%d_%d", graph_entry->id, label_id);
            Oid vertex_tbl_oid = get_relname_relid(vertex_tbl_name, namespace);
            if (OidIsValid(vertex_tbl_oid)) {
                LockRelationOid(vertex_tbl_oid, AccessExclusiveLock);
            }
            pfree(vertex_tbl_name);



            ItemPointerData schema_pmap_tid;
            np_id_to_tid(label_id, pmap_tuples_per_page, &schema_pmap_tid);

            Relation pmap_rel = table_open(graph_entry->annot_schema_phys_map, AccessShareLock);
            ItemPointerData latest_schema_tid;
            ItemPointerSetInvalid(&latest_schema_tid);

            /* SAFELY check if the block exists before reading */
            BlockNumber pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
            if (pmap_blk < RelationGetNumberOfBlocks(pmap_rel)) {
                Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
                LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
                Page pmap_page = BufferGetPage(pmap_buf);
                ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));

                if (ItemIdIsNormal(pmap_lp)) {
                    NeoPhysMapRecord *pmap_rec = (NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
                    latest_schema_tid = pmap_rec->v_itemptr;
                }
                UnlockReleaseBuffer(pmap_buf);
            }
            table_close(pmap_rel, AccessShareLock);

            ArrayType *old_array = NULL;
            if (ItemPointerIsValid(&latest_schema_tid)) {
                Relation schema_rel = table_open(graph_entry->annot_schema_tbl, AccessShareLock);
                Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
                LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
                Page schema_page = BufferGetPage(schema_buf);
                ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));

                if (ItemIdIsNormal(schema_lp)) {
                    NPEntityTupleHeader hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
                    old_array = DatumGetArrayTypePCopy(PointerGetDatum(hdr->serialized_entity));
                }
                UnlockReleaseBuffer(schema_buf);
                table_close(schema_rel, AccessShareLock);
            }

            /* Fallback to Column 9 (annotation_map) if no entity record exists yet */
            if (old_array == NULL) {
                Datum col9_datum = heap_getattr(tuple, 9, RelationGetDescr(meta_rel), &isnull);
                if (!isnull) {
                    old_array = DatumGetArrayTypePCopy(col9_datum);
                }
            }

            /* B. Deconstruct existing array and check if new_annot_str already exists */
            Datum *d_old = NULL;
            bool *n_old = NULL;
            int c_old = 0;
            bool already_exists = false;

            if (old_array != NULL && ARR_NDIM(old_array) > 0) {
                deconstruct_array(old_array, TEXTOID, -1, false, 'i', &d_old, &n_old, &c_old);
                for (int i = 0; i < c_old; i++) {
                    if (n_old[i]) continue;
                    if (strcmp(TextDatumGetCString(d_old[i]), new_annot_str) == 0) {
                        already_exists = true;
                        break;
                    }
                }
            }

            if (!already_exists) {
                /* C. Append new annotation string to this label's schema array */
                Datum *d_new = (Datum *) palloc0((c_old + 1) * sizeof(Datum));
                for (int i = 0; i < c_old; i++) {
                    d_new[i] = d_old[i];
                }
                d_new[c_old] = CStringGetTextDatum(new_annot_str);

                ArrayType *new_array = construct_array(d_new, c_old + 1, TEXTOID, -1, false, 'i');

                /* D. Write Temporal Schema Record to Entity Store */
                insert_annotation_schema(graph_entry->id, label_id, new_array, 
                                         graph_entry->annot_schema_tbl, 
                                         graph_entry->annot_schema_phys_map);

                /* E. In-Place Update column 9 (annotation_map) of this catalog tuple */
                int natts = RelationGetDescr(meta_rel)->natts;
                Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
                bool *nulls = (bool *) palloc0(natts * sizeof(bool));
                bool *replace = (bool *) palloc0(natts * sizeof(bool));

                replace[8] = true; /* index 8 = column 9 (annotation_map) */
                values[8] = PointerGetDatum(new_array);
                nulls[8] = false;

                HeapTuple updated_tup = heap_modify_tuple(tuple, RelationGetDescr(meta_rel), values, nulls, replace);
                np_catalog_update(meta_rel, tuple, updated_tup);
                heap_freetuple(updated_tup);

                pfree(values);
                pfree(nulls);
                pfree(replace);
                pfree(d_new);

                updated_count++;
            }
            if (old_array) pfree(old_array);
        }
    }

    systable_endscan(scan);
    table_close(meta_rel, RowExclusiveLock);

    /* 6. Register in Global Label Catalog */
    char *cat_name = psprintf("np_label_catalog_%d", graph_entry->id);
    char *idx_name = psprintf("np_label_catalog_%d_idx", graph_entry->id);
    Relation cat_rel = table_open(np_relation_id(cat_name, "table"), RowExclusiveLock);
    Relation idx_rel = index_open(np_relation_id(idx_name, "index"), RowExclusiveLock);
    
    enforce_and_insert_label_catalog(cat_rel, idx_rel, new_annot_str, 'a');

    index_close(idx_rel, RowExclusiveLock);
    table_close(cat_rel, RowExclusiveLock);

    ereport(NOTICE, (errmsg("Annotation label \"%s\" cascaded to %d structural label(s) under \"%s\"", new_annot_str, updated_count, struct_label_str)));

    PG_RETURN_VOID();
}

/*
 * Checks if any other independent root label (_.label_name) in the ltree hierarchy
 * of `row_ltree` also provides `annot_str`, excluding `target_ltree`.
 * 
 * Returns TRUE if another parent still provides the annotation (meaning we MUST NOT drop it).
 * Returns FALSE if no other parent provides it (meaning it is safe to drop).
 */
static bool
label_independently_owns_annotation(int32 graph_id, int32 label_id, Datum row_ltree, Datum target_ltree, const char *annot_str)
{
    /* 1. Convert row_ltree to a C-string so we can inspect its hierarchy */
    char *ltree_str = DatumGetCString(DirectFunctionCall1(ltree_out, row_ltree));
    char *target_str = DatumGetCString(DirectFunctionCall1(ltree_out, target_ltree));

    /* Strip root '_' prefix if present */
    char *path = ltree_str;
    if (strncmp(path, "_.", 2) == 0) path += 2;
    else if (strcmp(path, "_") == 0) path = "";

    char *target_name = target_str;
    if (strncmp(target_name, "_.", 2) == 0) target_name += 2;
    else if (strcmp(target_name, "_") == 0) target_name = "";

    /* If path has no dots, it is a single root label itself—it has no other parents */
    if (strchr(path, '.') == NULL) {
        pfree(ltree_str);
        pfree(target_str);
        return false;
    }

    /* 
     * 2. Split the ltree path by '.' and check if any other root label (_.component)
     * independently defines `annot_str`.
     */
    bool still_owned_by_other_parent = false;
    char *path_copy = pstrdup(path);
    char *token = strtok(path_copy, ".");

    Relation meta_rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", graph_id), "table"), AccessShareLock);

    while (token != NULL) {
        /* Skip the target label we are currently dropping from */
        if (strcmp(token, target_name) != 0) {
            
            /* Look up the independent root label for this token: _.token */
            char *root_ltree_str = psprintf("_.%s", token);
            SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
            HeapTuple tuple;

            while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
                bool isnull;
                Datum cat_ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
                char *cat_str = DatumGetCString(DirectFunctionCall1(ltree_out, cat_ltree_val));

                if (strcmp(cat_str, root_ltree_str) == 0) {
                    /* We found the root parent label! Check if it has annot_str in column 9 */
                    Datum array_datum = heap_getattr(tuple, 9, RelationGetDescr(meta_rel), &isnull);
                    if (!isnull) {
                        ArrayType *parent_array = DatumGetArrayTypeP(array_datum);
                        Datum *d_arr;
                        bool *n_arr;
                        int count;
                        deconstruct_array(parent_array, TEXTOID, -1, false, 'i', &d_arr, &n_arr, &count);

                        for (int i = 0; i < count; i++) {
                            if (n_arr[i]) continue;
                            if (strcmp(TextDatumGetCString(d_arr[i]), annot_str) == 0) {
                                /* MATCH! Another parent in the hierarchy still provides this annotation! */
                                still_owned_by_other_parent = true;
                                break;
                            }
                        }
                    }
                    pfree(cat_str);
                    break;
                }
                pfree(cat_str);
            }
            systable_endscan(scan);

            if (still_owned_by_other_parent) break;
        }
        token = strtok(NULL, ".");
    }

    table_close(meta_rel, AccessShareLock);
    pfree(path_copy);
    pfree(ltree_str);
    pfree(target_str);

    return still_owned_by_other_parent;
}

/*
 * Deletes an annotation label entry ('a') from the global np_label_catalog_<graph_id> table.
 */
static void
remove_from_label_catalog(Relation cat_rel, const char *label_str, char label_type)
{
    SysScanDesc scan;
    HeapTuple tuple;
    ScanKeyData skey[2];
NameData name_val;
namestrcpy(&name_val, label_str);
    /* 
     * Check if your np_label_catalog index uses F_NAMEEQ or F_TEXTEQ for col 1.
     * Assuming standard C-string/Name matching:
     */
    ScanKeyInit(&skey[0],
                1, /* Attribute 1: label name */
                BTEqualStrategyNumber,
                F_NAMEEQ,
                NameGetDatum(&name_val));

    ScanKeyInit(&skey[1],
                2, /* Attribute 2: label type ('a', 'v', 'e') */
                BTEqualStrategyNumber,
                F_CHAREQ,
                CharGetDatum(label_type));

    scan = systable_beginscan(cat_rel, InvalidOid, false, NULL, 2, skey);

    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        CatalogTupleDelete(cat_rel, &tuple->t_self);
    }

    systable_endscan(scan);
}

PG_FUNCTION_INFO_V1(drop_annotation_label);
Datum drop_annotation_label(PG_FUNCTION_ARGS)
{
    /* 1. Resolve Namespace */
    Oid namespace;
    if (PG_ARGISNULL(3)) {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("drop_annotation_label requires a search path when namespace is not specified")));
        namespace = linitial_oid(search_path);
    } else {
        char *nsp_str = TextDatumGetCString(PG_GETARG_DATUM(3));
        namespace = get_namespace_oid(nsp_str, true);
        if (!OidIsValid(namespace))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("namespace \"%s\" does not exist", nsp_str)));
    }

    /* 2. Extract Arguments */
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name, structural label, and annotation label to drop must not be NULL")));

    char *graph_name = NameStr(*PG_GETARG_NAME(0));
    char *struct_label_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *drop_annot_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

    /* 3. Look up Graph Cache */
    graph_cache_data *graph_entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!graph_entry)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist", graph_name)));

    /* 4. Open the Metadata Table and Find the Base Label's ltree */
    Relation meta_rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", graph_entry->id), "table"), RowExclusiveLock);
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple tuple;
    
    Datum target_ltree = (Datum)0;
    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        char *cached_str = DatumGetCString(DirectFunctionCall1(ltree_out, ltree_val));
        
        char *start = cached_str;
        if (strncmp(start, "_.", 2) == 0) start += 2;
        else if (strcmp(start, "_") == 0) start = "";

        if (strcmp(start, struct_label_str) == 0) {
            target_ltree = ltree_val;
            pfree(cached_str);
            break;
        }
        pfree(cached_str);
    }
    systable_endscan(scan);

    if (target_ltree == (Datum)0) {
        table_close(meta_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("Structural label \"%s\" not found in graph \"%s\"", struct_label_str, graph_name)));
    }

        target_ltree = PointerGetDatum(PG_DETOAST_DATUM(target_ltree));
    /* 5. Collect matching target label IDs first (avoids nested scan & buffer corruption) */
    scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));

    int natts = RelationGetDescr(meta_rel)->natts;
    int updated_count = 0;

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        Datum raw_ltree = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        if (isnull) continue;
        Datum row_ltree = PointerGetDatum(PG_DETOAST_DATUM(raw_ltree));

        bool is_descendant = DatumGetBool(DirectFunctionCall2(ltree_isparent, target_ltree, row_ltree));

        if (is_descendant) {
            int32 label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));

            /* Multiple-Inheritance Inline Check */
            if (DatumGetBool(DirectFunctionCall2(ltree_cmp, target_ltree, row_ltree)) != 0) {
                char *ltree_str = DatumGetCString(DirectFunctionCall1(ltree_out, row_ltree));
                char *path = ltree_str;
                if (strncmp(path, "_.", 2) == 0) path += 2;
                else if (strcmp(path, "_") == 0) path = "";

                bool still_owned_by_other_parent = false;
                char *path_copy = pstrdup(path);
                char *token = strtok(path_copy, ".");

                while (token != NULL) {
                    if (strcmp(token, struct_label_str) != 0) {
                        char *root_ltree_str = psprintf("_.%s", token);
                        SysScanDesc inner_scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
                        HeapTuple inner_tuple;

                        while (HeapTupleIsValid(inner_tuple = systable_getnext(inner_scan))) {
                            Datum raw_cat_ltree = heap_getattr(inner_tuple, 2, RelationGetDescr(meta_rel), &isnull);
                            if (isnull)
                                continue;
                            Datum cat_ltree = PointerGetDatum(PG_DETOAST_DATUM(raw_cat_ltree));
                            char *cat_str = DatumGetCString(DirectFunctionCall1(ltree_out, cat_ltree));

                            if (strcmp(cat_str, root_ltree_str) == 0) {
                                Datum arr_datum = heap_getattr(inner_tuple, 9, RelationGetDescr(meta_rel), &isnull);
                                if (!isnull) {
                                    ArrayType *p_arr = DatumGetArrayTypeP(arr_datum);
                                    if (ARR_NDIM(p_arr) > 0) {
                                        Datum *d_arr; bool *n_arr; int count;
                                        deconstruct_array(p_arr, TEXTOID, -1, false, 'i', &d_arr, &n_arr, &count);

                                        for (int i = 0; i < count; i++) {
                                            if (n_arr[i]) continue;
                                            if (strcmp(TextDatumGetCString(d_arr[i]), drop_annot_str) == 0) {
                                                still_owned_by_other_parent = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                                pfree(cat_str);
                                break;
                            }
                            pfree(cat_str);
                        }
                        systable_endscan(inner_scan);
                        if (still_owned_by_other_parent) break;
                    }
                    token = strtok(NULL, ".");
                }

                pfree(path_copy);
                pfree(ltree_str);

                if (still_owned_by_other_parent)
                    continue;
            }

            /* A. Read Latest Schema from Entity Store for this label_id */
            /* Lock the physical vertex table so concurrent DML inserts wait for the schema commit */
            char *vertex_tbl_name = psprintf("np_vertex_%d_%d", graph_entry->id, label_id);
            
            /* Use native get_relname_relid to avoid relkind 'table' vs 'partitioned table' crashes */
            Oid vertex_tbl_oid = get_relname_relid(vertex_tbl_name, namespace);
            if (OidIsValid(vertex_tbl_oid))
                LockRelationOid(vertex_tbl_oid, AccessExclusiveLock);
            pfree(vertex_tbl_name);   
            
            ItemPointerData schema_pmap_tid;
            np_id_to_tid(label_id, pmap_tuples_per_page, &schema_pmap_tid);

            Relation pmap_rel = table_open(graph_entry->annot_schema_phys_map, AccessShareLock);
            ItemPointerData latest_schema_tid;
            ItemPointerSetInvalid(&latest_schema_tid);

            /* SAFELY check if the block exists before reading */
            BlockNumber pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
            if (pmap_blk < RelationGetNumberOfBlocks(pmap_rel)) {
                Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
                LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
                Page pmap_page = BufferGetPage(pmap_buf);
                ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));

                if (ItemIdIsNormal(pmap_lp)) {
                    NeoPhysMapRecord *pmap_rec = (NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
                    latest_schema_tid = pmap_rec->v_itemptr;
                }
                UnlockReleaseBuffer(pmap_buf);
            }
            table_close(pmap_rel, AccessShareLock);

            ArrayType *old_array = NULL;
            if (ItemPointerIsValid(&latest_schema_tid)) {
                Relation schema_rel = table_open(graph_entry->annot_schema_tbl, AccessShareLock);
                Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
                LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
                Page schema_page = BufferGetPage(schema_buf);
                ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));

                if (ItemIdIsNormal(schema_lp)) {
                    NPEntityTupleHeader hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
                    old_array = DatumGetArrayTypePCopy(PointerGetDatum(hdr->serialized_entity));
                }
                UnlockReleaseBuffer(schema_buf);
                table_close(schema_rel, AccessShareLock);
            }

            /* Fallback to Column 9 (annotation_map) */
            if (old_array == NULL) {
                Datum col9_datum = heap_getattr(tuple, 9, RelationGetDescr(meta_rel), &isnull);
                if (!isnull) {
                    old_array = DatumGetArrayTypePCopy(col9_datum);
                }
            }

            /* B. Deconstruct existing array and CLEANLY drop the matching entry */
            if (old_array != NULL && ARR_NDIM(old_array) > 0) {
                Datum *d_old = NULL;
                bool *n_old = NULL;
                int c_old = 0;

                deconstruct_array(old_array, TEXTOID, -1, false, 'i', &d_old, &n_old, &c_old);

                if (c_old > 0) {
                    Datum *d_new = (Datum *) palloc0(c_old * sizeof(Datum));
                    int c_new = 0;
                    bool found_and_dropped = false;

                    for (int i = 0; i < c_old; i++) {
                        if (n_old[i]) continue;
                        char *curr_str = TextDatumGetCString(d_old[i]);
                        if (strcmp(curr_str, drop_annot_str) == 0) {
                            found_and_dropped = true;
                        } else {
                            d_new[c_new++] = d_old[i];
                        }
                    }

                    if (found_and_dropped) {
                        ArrayType *new_array;
                        if (c_new > 0) {
                            new_array = construct_array(d_new, c_new, TEXTOID, -1, false, 'i');
                        } else {
                            new_array = construct_empty_array(TEXTOID);
                        }

                        /* C. Write Temporal Schema Record to Entity Store */
                        insert_annotation_schema(graph_entry->id, label_id, new_array, 
                                                 graph_entry->annot_schema_tbl, 
                                                 graph_entry->annot_schema_phys_map);

                        /* D. In-Place Update column 9 (annotation_map) of this catalog tuple */
                        int natts = RelationGetDescr(meta_rel)->natts;
                        Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
                        bool *nulls = (bool *) palloc0(natts * sizeof(bool));
                        bool *replace = (bool *) palloc0(natts * sizeof(bool));

                        replace[8] = true; /* index 8 = column 9 (annotation_map) */
                        values[8] = PointerGetDatum(new_array);
                        nulls[8] = false;

                        HeapTuple updated_tup = heap_modify_tuple(tuple, RelationGetDescr(meta_rel), values, nulls, replace);
                        np_catalog_update(meta_rel, tuple, updated_tup);
                        heap_freetuple(updated_tup);

                        pfree(values);
                        pfree(nulls);
                        pfree(replace);
                        updated_count++;
                    }
                    pfree(d_new);
                }
                pfree(old_array);
            }
        }
    }
    systable_endscan(scan);
    table_close(meta_rel, RowExclusiveLock);
    /* 6. Remove from Global Label Catalog directly inline */
    char *cat_name = psprintf("np_label_catalog_%d", graph_entry->id);
    Relation cat_rel = table_open(np_relation_id(cat_name, "table"), RowExclusiveLock);
    NameData name_val;
namestrcpy(&name_val, drop_annot_str);
    ScanKeyData skey[2];
    ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&name_val));
    ScanKeyInit(&skey[1], 2, BTEqualStrategyNumber, F_CHAREQ, CharGetDatum('a'));
    
    SysScanDesc cat_scan = systable_beginscan(cat_rel, InvalidOid, false, NULL, 2, skey);
    HeapTuple cat_tuple;
    while (HeapTupleIsValid(cat_tuple = systable_getnext(cat_scan))) {
        CatalogTupleDelete(cat_rel, &cat_tuple->t_self);
    }
    systable_endscan(cat_scan);
    table_close(cat_rel, RowExclusiveLock);

    ereport(NOTICE, (errmsg("Annotation label \"%s\" dropped from %d structural label(s) under \"%s\"", drop_annot_str, updated_count, struct_label_str)));

    PG_RETURN_VOID();
}


/*
 * create_new_active_linked_list
 *
 * Creates a new linked list partition and makes it the active one.
 * The previous active partition (if any) is marked as inactive.
 */
Oid
create_new_active_linked_list(int graph_id, int label_id, Oid ll_seq_oid, Oid ll_meta_oid, Oid namespace_oid)
{
if (!OidIsValid(ll_seq_oid) || !OidIsValid(ll_meta_oid))
        ereport(ERROR, (errmsg("Invalid linked_list_seq or linked_list_meta OID")));

    Oid partition_id = DatumGetObjectId(
        DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(ll_seq_oid))
    );

    char *tbl_name = psprintf("np_vertex_%d_%d_%u_linked_list",
                              graph_id, label_id, partition_id);

    Oid new_list_oid = create_vertex_label_linked_list_table(tbl_name, namespace_oid);
    if (!OidIsValid(new_list_oid))
        ereport(ERROR, (errmsg("Failed to create linked list table")));

    Relation meta_rel = table_open(ll_meta_oid, RowExclusiveLock);

    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple tuple;

    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        bool isnull;
        bool active = DatumGetBool(heap_getattr(tuple, 3, RelationGetDescr(meta_rel), &isnull));

        if (active)
        {
            Datum values[4];
            bool nulls[4];
            bool replace[4] = {false, false, true, false}; 

            values[2] = BoolGetDatum(false);
            nulls[2] = false;

            HeapTuple newtup = heap_modify_tuple(tuple, RelationGetDescr(meta_rel),
                                                values, nulls, replace);
            np_catalog_update(meta_rel, tuple, newtup);
            heap_freetuple(newtup);
            break;
        }
    }
    systable_endscan(scan);

    Datum values[4];
    bool nulls[4] = {false, false, false, false};

    values[0] = Int32GetDatum(partition_id);     // id
    values[1] = ObjectIdGetDatum(new_list_oid);  // tbl
    values[2] = BoolGetDatum(true);              // active
    values[3] = BoolGetDatum(false);             // compacted

    HeapTuple newtup = heap_form_tuple(RelationGetDescr(meta_rel), values, nulls);
    CatalogTupleInsert(meta_rel, newtup);
    heap_freetuple(newtup);

    table_close(meta_rel, RowExclusiveLock);
    CommandCounterIncrement();

    return new_list_oid;
}

Oid 
rotate_active_linked_list_table_internal(const char *graph_name, int32 label_id)
{
    /* 1. Resolve Namespace */
    List *search_path = fetch_search_path(false);
    if (list_length(search_path) < 1) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Requires a search path to resolve namespace")));
    }
    Oid namespace = linitial_oid(search_path);

    /* 2. Fetch Graph Cache */
    const graph_cache_data *graph = search_graph_name_namespace_cache(graph_name, namespace);
    if (!graph) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), 
                        errmsg("graph \"%s\" does not exist", graph_name)));
    }

    /* 3. Fetch Label Cache */
    const label_cache_data *label = search_vertex_label_graph_id_label_id_cache(graph->id, label_id);
    if (!label || !OidIsValid(label->linked_list_meta) || !OidIsValid(label->linked_list_seq)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), 
                        errmsg("label does not have linked list setup")));
    }

    /* 4. Create and return the new active linked list table OID */
    return create_new_active_linked_list(
        graph->id,
        label_id,
        label->linked_list_seq,
        label->linked_list_meta,
        namespace
    );
}

PG_FUNCTION_INFO_V1(rotate_active_linked_list_table);
Datum
rotate_active_linked_list_table(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_name and label_id are required")));
    }

    /* 1. Extract arguments */
    char *graph_name = NameStr(*PG_GETARG_NAME(0));
    int32 label_id   = PG_GETARG_INT32(1);

    /* 2. Call internal logic */
    rotate_active_linked_list_table_internal(graph_name, label_id);

    PG_RETURN_VOID();
}


Oid
create_vertex_label_annotation_table(char *tbl_name, Oid namespace, int byte_allocation_size)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);
    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), tbl_name, -1);

    /* Column 1: Vertex ID */
    ColumnDef *id = makeColumnDef("id", INT8OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    
    /* Column 2: Ancestor Bitsets (Bitmap Set) */
    ColumnDef *annotations = makeColumnDef("annotations", BYTEAOID, byte_allocation_size, InvalidOid);
    
    create_stmt->tableElts = list_make2(id, annotations);
    
    create_stmt->accessMethod = "entity_store"; 
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated annotation CREATE TABLE)",
                   false, PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
                   None_Receiver, NULL);

    CommandCounterIncrement();

    return get_relname_relid(tbl_name, namespace);
}


void np_catalog_update(Relation rel, HeapTuple old_tup, HeapTuple new_tup)
{
    /* Create an in-memory slot for the new tuple */
    TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsHeapTuple);
    ExecStoreHeapTuple(new_tup, slot, false);

    TU_UpdateIndexes update_indexes;
    TM_FailureData tmfd;
    LockTupleMode lockmode;

    /* 1. Update the heap directly (Bypasses np_catalog_update) */
    TM_Result result = table_tuple_update(rel, &(old_tup->t_self), slot,
                                          GetCurrentCommandId(true),
                                          GetActiveSnapshot(),
                                          InvalidSnapshot,
                                          true, /* wait for commit */
                                          &tmfd, &lockmode, &update_indexes);

    if (result != TM_Ok) {
        elog(ERROR, "NeoPostGraph: failed to update catalog tuple");
    }

    /* 2. Manually update indexes if necessary */
    if (update_indexes != TU_None) {
        List *indexoidlist = RelationGetIndexList(rel);
        ListCell *lc;

        foreach(lc, indexoidlist) {
            Oid index_oid = lfirst_oid(lc);
            Relation indexDesc = index_open(index_oid, RowExclusiveLock);
            IndexInfo *indexInfo = BuildIndexInfo(indexDesc);

            Datum idx_values[INDEX_MAX_KEYS];
            bool idx_nulls[INDEX_MAX_KEYS];

            FormIndexDatum(indexInfo, slot, NULL, idx_values, idx_nulls);

            /* 
             * Check if the new tuple still satisfies the partial index predicate.
             * For our is_primary index, we check if is_primary is true.
             */
            bool satisfy_predicate = true;
            if (indexInfo->ii_Predicate != NIL) {
                /* 
                 * We can do a quick check: if the 10th column (is_primary) is false,
                 * skip index insertion for this specific partial GiST index.
                 */
                bool is_null;
                Datum is_primary_datum = heap_getattr(new_tup, 10, RelationGetDescr(rel), &is_null);
                if (!is_null && !DatumGetBool(is_primary_datum)) {
                    satisfy_predicate = false;
                }
            }

            if (satisfy_predicate) {
                index_insert(indexDesc, idx_values, idx_nulls,
                             &(slot->tts_tid), rel,
                             indexDesc->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
                             false, /* indexUnchanged */
                             indexInfo);
            }

            index_close(indexDesc, RowExclusiveLock);
        }
        list_free(indexoidlist);
    }

    ExecDropSingleTupleTableSlot(slot);
}