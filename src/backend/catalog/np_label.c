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

int32 create_elabel_internal(const char *graph_name, const char *new_label_str, const char *namespace_name, ArrayType *annotations, Datum *out_id)
{
    Oid namespace;
    if (namespace_name) {
        namespace = get_namespace_oid(namespace_name, false);
    } else {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("create_elabel requires a search path when namespace is not specified")));
        namespace = linitial_oid(search_path);
    }

    graph_cache_data *entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!entry)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), errmsg("graph \"%s\" does not exist", graph_name)));

    int32 label_id = (int32) DatumGetInt64(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(entry->edge_id_seq)));
    Oid edge_tbl = create_edge_tables(entry->id, label_id, namespace);
    
    Oid phys_map = create_label_edge_physical_mapping_table(
                        psprintf("np_edge_%d_%d_phys_map", entry->id, label_id), namespace);

    /* --- SAFELY CREATE ANNOTATION TABLE AND WRITE TO ENTITY STORE --- */
    Oid annot_tbl_oid = InvalidOid;
    Datum annot_map_datum = (Datum)0; /* <--- Initialize as NULL */
    
    if (annotations != NULL) {
        annot_map_datum = PointerGetDatum(annotations); /* <--- Cast the array to a Datum */
        
        int num_elements;
        Datum *elements;
        bool *nulls;
        deconstruct_array(annotations, TEXTOID, -1, false, TYPALIGN_INT, &elements, &nulls, &num_elements);
        
        if (num_elements > 0) {
            int byte_allocation_size = (num_elements + 7) / 8;
            /* Physically create the annotation table for this edge */
            annot_tbl_oid = create_vertex_label_annotation_table(
                                psprintf("np_edge_annotations_%d_%d", entry->id, label_id), 
                                namespace, byte_allocation_size);
            
            /* Write the schema array to the Entity Store */
            insert_annotation_schema(entry->id, label_id, annotations, entry->annot_schema_tbl, entry->annot_schema_phys_map);
        }
    }
    /* ---------------------------------------------------------------- */

    insert_label(
        psprintf("np_edge_label_%d", entry->id),
        DirectFunctionCall2(ltree_addltree,
            DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)),
            DirectFunctionCall1(ltree_in, CStringGetDatum(new_label_str))
        ),
        label_id,
        edge_tbl,
        phys_map,
        annot_tbl_oid,  
        annot_map_datum /* <--- PASS IT CORRECTLY HERE */
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

    if (out_id) *out_id = Int32GetDatum(label_id);
    return label_id;
}
PG_FUNCTION_INFO_V1(create_elabel);
Datum create_elabel(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("label string must not be NULL")));

    char *graph_name = NameStr(*PG_GETARG_NAME(0));
    
    /* Safely detoast the label string */
    text *label_text = PG_GETARG_TEXT_PP(1);
    char *new_label_str = text_to_cstring(label_text);
    
    char *namespace_name = NULL;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2)) {
        namespace_name = TextDatumGetCString(PG_GETARG_DATUM(2));
    }

    ArrayType *annotations = NULL;
    if (PG_NARGS() > 3 && !PG_ARGISNULL(3)) {
        annotations = PG_GETARG_ARRAYTYPE_P(3);
    }

    create_elabel_internal(graph_name, new_label_str, namespace_name, annotations, NULL);
    
    pfree(new_label_str);
    ereport(NOTICE, (errmsg("elabel \"%s\" has been created", graph_name)));

    PG_RETURN_VOID();
}

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

PG_FUNCTION_INFO_V1(merge_elabels);
Datum merge_elabels(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("label IDs must not be NULL")));

    char *graph_name = NameStr(*PG_GETARG_NAME(0));
    int32 current_label_id = PG_GETARG_INT32(1);
    int32 base_label_id = PG_GETARG_INT32(2);
    
    char *namespace_name = NULL;
    if (PG_NARGS() > 3 && !PG_ARGISNULL(3)) {
        namespace_name = TextDatumGetCString(PG_GETARG_DATUM(3));
    }

    merge_elabels_internal(graph_name, current_label_id, base_label_id, namespace_name);
    
    ereport(NOTICE, (errmsg("Merged elabels %d and %d for graph \"%s\"", current_label_id, base_label_id, graph_name)));

    PG_RETURN_VOID();
}

int32
merge_elabels_internal(const char *graph_name, int32 current_label_id, int32 base_label_id, const char *namespace_name)
{
    Oid namespace;
    if (namespace_name) {
        namespace = get_namespace_oid(namespace_name, false);
    } else {
        List *search_path = fetch_search_path(false);
        namespace = linitial_oid(search_path);
    }

    graph_cache_data *graph_cache = search_graph_name_namespace_cache(graph_name, namespace);
    if (!graph_cache)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), errmsg("graph \"%s\" does not exist", graph_name)));

    const label_cache_data *current_label = search_edge_label_graph_id_label_id_cache(graph_cache->id, current_label_id);
    const label_cache_data *base_label = search_edge_label_graph_id_label_id_cache(graph_cache->id, base_label_id);

    if (!current_label || !base_label)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("One or more edge labels do not exist for merge")));

    /* Extract the exact string paths (e.g. '_.knows' and '_.colleague') */
    char *current_path = DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(current_label->label)));
    char *base_path = DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(base_label->label)));

    /* Strip the root '_.' from the base path to concatenate it cleanly */
    char *clean_base = strstr(base_path, "_.");
    if (clean_base) clean_base += 2;
    else clean_base = base_path;

    char *merged_path_str = psprintf("%s.%s", current_path, clean_base);
    Datum merged_ltree = DirectFunctionCall1(ltree_in, CStringGetDatum(merged_path_str));

    int32 new_label_id = (int32) DatumGetInt64(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(graph_cache->edge_id_seq)));
    Oid edge_tbl = create_edge_tables(graph_cache->id, new_label_id, namespace);
    Oid phys_map = create_label_edge_physical_mapping_table(
                        psprintf("np_edge_%d_%d_phys_map", graph_cache->id, new_label_id), namespace);

    /* --- Handle Annotations Inheritance (Just like Vertices) --- */
    ArrayType *merged_array = merge_and_dedupe_text_arrays(current_label->annotation_map, base_label->annotation_map);
    Datum merged_array_datum = (merged_array != NULL) ? PointerGetDatum(merged_array) : (Datum)0;

    Oid annot_tbl_oid = InvalidOid;
    
    if (merged_array != NULL) {
        int byte_allocation_size = (ArrayGetNItems(ARR_NDIM(merged_array), ARR_DIMS(merged_array)) + 7) / 8;
        
        annot_tbl_oid = create_vertex_label_annotation_table(
                            psprintf("np_edge_annotations_%d_%d", graph_cache->id, new_label_id), 
                            namespace, byte_allocation_size);
        
        insert_annotation_schema(graph_cache->id, new_label_id, merged_array, graph_cache->annot_schema_tbl, graph_cache->annot_schema_phys_map);
    }
    /* ----------------------------------------------------------- */

    /* Insert directly with the pre-merged ltree AND merged annotations */
    insert_label(
        psprintf("np_edge_label_%d", graph_cache->id),
        merged_ltree,
        new_label_id,
        edge_tbl,
        phys_map,
        annot_tbl_oid,      /* The combined annotation physical table */
        merged_array_datum  /* The inherited annotation schema map */
    );

    CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
    char seq_name[NAMEDATALEN];
    snprintf(seq_name, NAMEDATALEN, "np_edge_id_seq_%d_%d", graph_cache->id, new_label_id);
    seq_stmt->sequence = makeRangeVar("neopostgraph", seq_name, -1);
    seq_stmt->options = NIL;
    seq_stmt->ownerId = GetUserId();
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;

    DefineSequence(NULL, seq_stmt);
    CommandCounterIncrement();

    pfree(current_path);
    pfree(base_path);
    pfree(merged_path_str);

    return new_label_id;
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

int insert_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map, Oid annot_tbl, Datum annot_map)
{
    Relation rel = table_open(np_relation_id(table_name, "table"), RowExclusiveLock);

    /* Updated to 7 columns to match vertex catalog parity */
    Datum values[7] = {
        ObjectIdGetDatum(label_id),
        label,
        ObjectIdGetDatum(tbl),
        ObjectIdGetDatum(phys_map),
        ObjectIdGetDatum(annot_tbl),
        annot_map,
        BoolGetDatum(true) /* COL 7: is_primary = true */
    };
    
    bool nulls[7] = { false };
    if (!OidIsValid(annot_tbl)) nulls[4] = true;
    if (annot_map == (Datum)0) nulls[5] = true;

    HeapTuple tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

    /* Create a slot specifically designed to hold an in-memory HeapTuple */
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

static Datum
np_find_struct_ltree(Relation meta_rel, const char *struct_label_str)
{
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple tuple;
    Datum found = (Datum) 0;

    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        bool isnull;
        Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        if (isnull)
            continue;

        char *cached_str = DatumGetCString(DirectFunctionCall1(ltree_out, ltree_val));
        char *start = cached_str;
        if (strncmp(start, "_.", 2) == 0)
            start += 2;
        else if (strcmp(start, "_") == 0)
            start = "";

        if (strcmp(start, struct_label_str) == 0)
        {
            found = PointerGetDatum(PG_DETOAST_DATUM_COPY(ltree_val));
            pfree(cached_str);
            break;
        }
        pfree(cached_str);
    }

    systable_endscan(scan);
    return found;
}

/* Heap-scan every catalog row, including is_primary = false partitions. */
static List *
np_collect_ltree_descendants(Relation meta_rel, Datum target_ltree)
{
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    List *tuples = NIL;
    HeapTuple tuple;

    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        bool isnull;
        Datum row_ltree = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
        if (isnull)
            continue;

        if (DatumGetBool(DirectFunctionCall2(ltree_isparent, target_ltree, row_ltree)))
            tuples = lappend(tuples, heap_copytuple(tuple));
    }

    systable_endscan(scan);
    return tuples;
}

static bool
np_apply_add_annotation_to_tuple(graph_cache_data *graph_entry,
                                 Relation meta_rel,
                                 HeapTuple tuple,
                                 char *new_annot_str,
                                 Oid namespace,
                                 int annot_map_attno,
                                 int annot_tbl_attno,
                                 const char *entity_prefix,
                                 const char *annot_tbl_fmt)
{
    bool isnull;
    int32 label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));

    char *entity_tbl_name = psprintf("%s_%d_%d", entity_prefix, graph_entry->id, label_id);
    Oid entity_tbl_oid = get_relname_relid(entity_tbl_name, namespace);
    if (OidIsValid(entity_tbl_oid))
        LockRelationOid(entity_tbl_oid, AccessExclusiveLock);
    pfree(entity_tbl_name);

    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) /
        (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData schema_pmap_tid;
    np_id_to_tid(label_id, pmap_tuples_per_page, &schema_pmap_tid);

    Relation pmap_rel = table_open(graph_entry->annot_schema_phys_map, AccessShareLock);
    ItemPointerData latest_schema_tid;
    ItemPointerSetInvalid(&latest_schema_tid);

    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
    if (pmap_blk < RelationGetNumberOfBlocks(pmap_rel))
    {
        Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
        LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
        Page pmap_page = BufferGetPage(pmap_buf);
        ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));

        if (ItemIdIsNormal(pmap_lp))
        {
            NeoPhysMapRecord *pmap_rec = (NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
            latest_schema_tid = pmap_rec->v_itemptr;
        }
        UnlockReleaseBuffer(pmap_buf);
    }
    table_close(pmap_rel, AccessShareLock);

    ArrayType *old_array = NULL;
    if (ItemPointerIsValid(&latest_schema_tid))
    {
        Relation schema_rel = table_open(graph_entry->annot_schema_tbl, AccessShareLock);
        Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
        LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
        Page schema_page = BufferGetPage(schema_buf);
        ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));

        if (ItemIdIsNormal(schema_lp))
        {
            NPEntityTupleHeader hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
            old_array = DatumGetArrayTypePCopy(PointerGetDatum(hdr->serialized_entity));
        }
        UnlockReleaseBuffer(schema_buf);
        table_close(schema_rel, AccessShareLock);
    }

    if (old_array == NULL)
    {
        Datum map_datum = heap_getattr(tuple, annot_map_attno, RelationGetDescr(meta_rel), &isnull);
        if (!isnull)
            old_array = DatumGetArrayTypePCopy(map_datum);
    }

    Datum *d_old = NULL;
    bool *n_old = NULL;
    int c_old = 0;
    bool already_exists = false;

    if (old_array != NULL && ARR_NDIM(old_array) > 0)
    {
        deconstruct_array(old_array, TEXTOID, -1, false, 'i', &d_old, &n_old, &c_old);
        for (int i = 0; i < c_old; i++)
        {
            if (n_old[i])
                continue;
            if (strcmp(TextDatumGetCString(d_old[i]), new_annot_str) == 0)
            {
                already_exists = true;
                break;
            }
        }
    }

    if (already_exists)
    {
        if (old_array)
            pfree(old_array);
        return false;
    }

    Datum *d_new = (Datum *) palloc0((c_old + 1) * sizeof(Datum));
    for (int i = 0; i < c_old; i++)
        d_new[i] = d_old[i];
    d_new[c_old] = CStringGetTextDatum(new_annot_str);

    ArrayType *new_array = construct_array(d_new, c_old + 1, TEXTOID, -1, false, 'i');

    insert_annotation_schema(graph_entry->id, label_id, new_array,
                             graph_entry->annot_schema_tbl,
                             graph_entry->annot_schema_phys_map);

    int natts = RelationGetDescr(meta_rel)->natts;
    Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
    bool *nulls = (bool *) palloc0(natts * sizeof(bool));
    bool *replace = (bool *) palloc0(natts * sizeof(bool));

    replace[annot_map_attno - 1] = true;
    values[annot_map_attno - 1] = PointerGetDatum(new_array);
    nulls[annot_map_attno - 1] = false;

    Datum annot_tbl_datum = heap_getattr(tuple, annot_tbl_attno, RelationGetDescr(meta_rel), &isnull);
    if (isnull || !OidIsValid(DatumGetObjectId(annot_tbl_datum)))
    {
        int byte_alloc = (c_old + 1 + 7) / 8;
        char *annot_name = psprintf(annot_tbl_fmt, graph_entry->id, label_id);
        Oid annot_oid = create_vertex_label_annotation_table(annot_name, namespace, byte_alloc);
        pfree(annot_name);

        replace[annot_tbl_attno - 1] = true;
        values[annot_tbl_attno - 1] = ObjectIdGetDatum(annot_oid);
        nulls[annot_tbl_attno - 1] = false;
    }

    HeapTuple updated_tup = heap_modify_tuple(tuple, RelationGetDescr(meta_rel), values, nulls, replace);
    np_catalog_update(meta_rel, tuple, updated_tup);
    heap_freetuple(updated_tup);

    pfree(values);
    pfree(nulls);
    pfree(replace);
    pfree(d_new);
    if (old_array)
        pfree(old_array);

    return true;
}

static bool
np_other_parent_owns_annotation(Relation meta_rel, Datum row_ltree, const char *struct_label_str,
                                const char *annot_str, int annot_map_attno)
{
    char *ltree_str = DatumGetCString(DirectFunctionCall1(ltree_out, row_ltree));
    char *path = ltree_str;
    if (strncmp(path, "_.", 2) == 0)
        path += 2;
    else if (strcmp(path, "_") == 0)
        path = "";

    if (strchr(path, '.') == NULL)
    {
        pfree(ltree_str);
        return false;
    }

    bool still_owned = false;
    char *path_copy = pstrdup(path);
    char *token = strtok(path_copy, ".");

    while (token != NULL)
    {
        if (strcmp(token, struct_label_str) != 0)
        {
            char *root_ltree_str = psprintf("_.%s", token);
            SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
            HeapTuple tuple;

            while (HeapTupleIsValid(tuple = systable_getnext(scan)))
            {
                bool isnull;
                Datum cat_ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
                if (isnull)
                    continue;
                char *cat_str = DatumGetCString(DirectFunctionCall1(ltree_out, cat_ltree_val));

                if (strcmp(cat_str, root_ltree_str) == 0)
                {
                    Datum array_datum = heap_getattr(tuple, annot_map_attno, RelationGetDescr(meta_rel), &isnull);
                    if (!isnull)
                    {
                        ArrayType *parent_array = DatumGetArrayTypeP(array_datum);
                        if (ARR_NDIM(parent_array) > 0)
                        {
                            Datum *d_arr;
                            bool *n_arr;
                            int count;
                            deconstruct_array(parent_array, TEXTOID, -1, false, 'i', &d_arr, &n_arr, &count);
                            for (int i = 0; i < count; i++)
                            {
                                if (n_arr[i])
                                    continue;
                                if (strcmp(TextDatumGetCString(d_arr[i]), annot_str) == 0)
                                {
                                    still_owned = true;
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
            systable_endscan(scan);
            pfree(root_ltree_str);
            if (still_owned)
                break;
        }
        token = strtok(NULL, ".");
    }

    pfree(path_copy);
    pfree(ltree_str);
    return still_owned;
}

static bool
np_apply_drop_annotation_to_tuple(graph_cache_data *graph_entry,
                                  Relation meta_rel,
                                  HeapTuple tuple,
                                  Datum target_ltree,
                                  const char *struct_label_str,
                                  const char *drop_annot_str,
                                  Oid namespace,
                                  int annot_map_attno,
                                  const char *entity_prefix)
{
    bool isnull;
    int32 label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
    Datum raw_ltree = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
    Datum row_ltree = PointerGetDatum(PG_DETOAST_DATUM(raw_ltree));

    if (DatumGetBool(DirectFunctionCall2(ltree_cmp, target_ltree, row_ltree)) != 0 &&
        np_other_parent_owns_annotation(meta_rel, row_ltree, struct_label_str, drop_annot_str, annot_map_attno))
        return false;

    char *entity_tbl_name = psprintf("%s_%d_%d", entity_prefix, graph_entry->id, label_id);
    Oid entity_tbl_oid = get_relname_relid(entity_tbl_name, namespace);
    if (OidIsValid(entity_tbl_oid))
        LockRelationOid(entity_tbl_oid, AccessExclusiveLock);
    pfree(entity_tbl_name);

    uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) /
        (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData schema_pmap_tid;
    np_id_to_tid(label_id, pmap_tuples_per_page, &schema_pmap_tid);

    Relation pmap_rel = table_open(graph_entry->annot_schema_phys_map, AccessShareLock);
    ItemPointerData latest_schema_tid;
    ItemPointerSetInvalid(&latest_schema_tid);

    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
    if (pmap_blk < RelationGetNumberOfBlocks(pmap_rel))
    {
        Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
        LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
        Page pmap_page = BufferGetPage(pmap_buf);
        ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));

        if (ItemIdIsNormal(pmap_lp))
        {
            NeoPhysMapRecord *pmap_rec = (NeoPhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
            latest_schema_tid = pmap_rec->v_itemptr;
        }
        UnlockReleaseBuffer(pmap_buf);
    }
    table_close(pmap_rel, AccessShareLock);

    ArrayType *old_array = NULL;
    if (ItemPointerIsValid(&latest_schema_tid))
    {
        Relation schema_rel = table_open(graph_entry->annot_schema_tbl, AccessShareLock);
        Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&latest_schema_tid));
        LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
        Page schema_page = BufferGetPage(schema_buf);
        ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&latest_schema_tid));

        if (ItemIdIsNormal(schema_lp))
        {
            NPEntityTupleHeader hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
            old_array = DatumGetArrayTypePCopy(PointerGetDatum(hdr->serialized_entity));
        }
        UnlockReleaseBuffer(schema_buf);
        table_close(schema_rel, AccessShareLock);
    }

    if (old_array == NULL)
    {
        Datum map_datum = heap_getattr(tuple, annot_map_attno, RelationGetDescr(meta_rel), &isnull);
        if (!isnull)
            old_array = DatumGetArrayTypePCopy(map_datum);
    }

    if (old_array == NULL || ARR_NDIM(old_array) <= 0)
    {
        if (old_array)
            pfree(old_array);
        return false;
    }

    Datum *d_old = NULL;
    bool *n_old = NULL;
    int c_old = 0;
    deconstruct_array(old_array, TEXTOID, -1, false, 'i', &d_old, &n_old, &c_old);

    Datum *d_new = (Datum *) palloc0(c_old * sizeof(Datum));
    int c_new = 0;
    bool found_and_dropped = false;

    for (int i = 0; i < c_old; i++)
    {
        if (n_old[i])
            continue;
        if (strcmp(TextDatumGetCString(d_old[i]), drop_annot_str) == 0)
            found_and_dropped = true;
        else
            d_new[c_new++] = d_old[i];
    }

    if (!found_and_dropped)
    {
        pfree(d_new);
        pfree(old_array);
        return false;
    }

    ArrayType *new_array = (c_new > 0)
        ? construct_array(d_new, c_new, TEXTOID, -1, false, 'i')
        : construct_empty_array(TEXTOID);

    insert_annotation_schema(graph_entry->id, label_id, new_array,
                             graph_entry->annot_schema_tbl,
                             graph_entry->annot_schema_phys_map);

    int natts = RelationGetDescr(meta_rel)->natts;
    Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
    bool *nulls = (bool *) palloc0(natts * sizeof(bool));
    bool *replace = (bool *) palloc0(natts * sizeof(bool));

    replace[annot_map_attno - 1] = true;
    values[annot_map_attno - 1] = PointerGetDatum(new_array);
    nulls[annot_map_attno - 1] = false;

    HeapTuple updated_tup = heap_modify_tuple(tuple, RelationGetDescr(meta_rel), values, nulls, replace);
    np_catalog_update(meta_rel, tuple, updated_tup);
    heap_freetuple(updated_tup);

    pfree(values);
    pfree(nulls);
    pfree(replace);
    pfree(d_new);
    pfree(old_array);
    return true;
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

    /* 4. Heap-scan vertex AND edge catalogs, including is_primary = false rows. */
    int updated_count = 0;
    bool found = false;
    ListCell *lc;

    Relation v_rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", graph_entry->id), "table"), RowExclusiveLock);
    Datum v_target = np_find_struct_ltree(v_rel, struct_label_str);
    if (v_target != (Datum) 0)
    {
        List *v_tups = np_collect_ltree_descendants(v_rel, v_target);
        found = true;
        foreach(lc, v_tups)
        {
            HeapTuple tuple = (HeapTuple) lfirst(lc);
            if (np_apply_add_annotation_to_tuple(graph_entry, v_rel, tuple, new_annot_str, namespace,
                                                 9, 8, "np_vertex", "np_vertex_%d_%d_annotations"))
                updated_count++;
            heap_freetuple(tuple);
        }
        list_free(v_tups);
    }
    table_close(v_rel, RowExclusiveLock);

    Relation e_rel = table_open(np_relation_id(psprintf("np_edge_label_%d", graph_entry->id), "table"), RowExclusiveLock);
    Datum e_target = np_find_struct_ltree(e_rel, struct_label_str);
    if (e_target != (Datum) 0)
    {
        List *e_tups = np_collect_ltree_descendants(e_rel, e_target);
        found = true;
        foreach(lc, e_tups)
        {
            HeapTuple tuple = (HeapTuple) lfirst(lc);
            if (np_apply_add_annotation_to_tuple(graph_entry, e_rel, tuple, new_annot_str, namespace,
                                                 6, 5, "np_edge", "np_edge_annotations_%d_%d"))
                updated_count++;
            heap_freetuple(tuple);
        }
        list_free(e_tups);
    }
    table_close(e_rel, RowExclusiveLock);

    if (!found)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("Structural label \"%s\" not found in graph \"%s\"", struct_label_str, graph_name)));

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


static Oid
np_resolve_edge_annotations_tbl(const label_cache_data *cache, int32 graph_id, int32 label_id)
{
    if (cache && OidIsValid(cache->annotations_tbl))
        return cache->annotations_tbl;

    Relation rel = table_open(np_relation_id(psprintf("np_edge_label_%d", graph_id), "table"), AccessShareLock);
    ScanKeyData skey[1];
    ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(label_id));
    SysScanDesc scan = systable_beginscan(rel,
                        np_relation_id(psprintf("np_edge_label_%d_btree_idx", graph_id), "index"),
                        true, NULL, 1, skey);
    HeapTuple tuple = systable_getnext(scan);
    Oid annot_tbl = InvalidOid;

    if (HeapTupleIsValid(tuple))
    {
        bool isnull;
        Datum d = heap_getattr(tuple, 5, RelationGetDescr(rel), &isnull);
        if (!isnull)
            annot_tbl = DatumGetObjectId(d);
    }

    systable_endscan(scan);
    table_close(rel, AccessShareLock);
    return annot_tbl;
}

PG_FUNCTION_INFO_V1(add_edge_annotation);
Datum
add_edge_annotation(PG_FUNCTION_ARGS)
{
    /* 1. Extract Arguments */
    int64 edge_id = PG_GETARG_INT64(0);
    int32 label_id  = PG_GETARG_INT32(1);
    int32 graph_id  = PG_GETARG_INT32(2);
    char *annot_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    /* 2. Look up the Edge Label Cache */
    const label_cache_data *label_cache =
        search_edge_label_graph_id_label_id_cache(graph_id, label_id);

    Oid annotations_tbl = np_resolve_edge_annotations_tbl(label_cache, graph_id, label_id);

    if (!label_cache || !OidIsValid(annotations_tbl))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Structural edge label does not support annotations: graph_id=%d, label_id=%d",
                        graph_id, label_id)));

    /* 3. Resolve the Current Active Schema Array */
    ArrayType *map_array = NULL;
    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);

    if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
        uint32 schema_pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
        ItemPointerData schema_pmap_tid;
        np_id_to_tid(label_id, schema_pmap_tuples_per_page, &schema_pmap_tid);

        Relation schema_pmap_rel = table_open(graph_cache->annot_schema_phys_map, AccessShareLock);
        BlockNumber schema_pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
        
        ItemPointerData latest_schema_tid;
        ItemPointerSetInvalid(&latest_schema_tid);

        if (schema_pmap_blk < RelationGetNumberOfBlocks(schema_pmap_rel)) {
            Buffer schema_pmap_buf = ReadBuffer(schema_pmap_rel, schema_pmap_blk);
            LockBuffer(schema_pmap_buf, BUFFER_LOCK_SHARE);
            Page schema_pmap_page = BufferGetPage(schema_pmap_buf);
            ItemId schema_pmap_lp = PageGetItemId(schema_pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));

            if (ItemIdIsNormal(schema_pmap_lp)) {
                NeoPhysMapRecord *pmap_rec = (NeoPhysMapRecord *) PageGetItem(schema_pmap_page, schema_pmap_lp);
                latest_schema_tid = pmap_rec->v_itemptr;
            }
            UnlockReleaseBuffer(schema_pmap_buf);
        }
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

    if (map_array == NULL) {
        map_array = label_cache->annotation_map;
    }

    if (map_array == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("No annotation schema defined for this structural edge label")));

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
                 errmsg("Annotation label '%s' is not valid for this structural edge label", annot_str)));

    int byte_size = (map_count + 7) / 8;

    /* 5. Lookup the Edge in its phys_map to get current a_itemptr */
    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);
    uint32 edge_pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(edge_id, edge_pmap_tuples_per_page, &phys_map_tid);

    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);

    if (pmap_blk >= RelationGetNumberOfBlocks(pmap_rel)) {
        table_close(pmap_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Edge ID not found in phys_map (table empty)")));
    }

    Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    if (!ItemIdIsNormal(pmap_lp)) {
        UnlockReleaseBuffer(pmap_buf);
        table_close(pmap_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Edge ID not found in phys_map")));
    }

    NeoEdgePhysMapRecord *disk_pmap_rec = (NeoEdgePhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
    NeoEdgePhysMapRecord current_pmap_rec = *disk_pmap_rec;
    UnlockReleaseBuffer(pmap_buf);

    ItemPointerData old_annot_tid = current_pmap_rec.a_itemptr;
    Relation annot_rel = table_open(annotations_tbl, RowExclusiveLock);

    /* DEFENSE: Instantly drop garbage memory pointers from old insert_edge calls */
    if (ItemPointerIsValid(&old_annot_tid)) {
        if (ItemPointerGetBlockNumber(&old_annot_tid) >= RelationGetNumberOfBlocks(annot_rel)) {
            ItemPointerSetInvalid(&old_annot_tid);
        }
    }

    /* 6. Read Existing Bitset (or Allocate a Clean One) */
    bytea *bitset = (bytea *) palloc0(VARHDRSZ + byte_size);
    SET_VARSIZE(bitset, VARHDRSZ + byte_size);
    char *bits = VARDATA(bitset);

    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_check = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_check, BUFFER_LOCK_SHARE);
        Page opage_check = BufferGetPage(obuf_check);
        ItemId olp_check = PageGetItemId(opage_check, ItemPointerGetOffsetNumber(&old_annot_tid));

        if (ItemIdIsNormal(olp_check)) {
            NPEntityTupleHeader old_hdr = (NPEntityTupleHeader) PageGetItem(opage_check, olp_check);
            
            if (FullTransactionIdIsValid(old_hdr->xmax)) {
                UnlockReleaseBuffer(obuf_check);
                table_close(annot_rel, RowExclusiveLock);
                table_close(pmap_rel, RowExclusiveLock);
                ereport(ERROR, (errmsg("Edge ID %ld annotation bitset was concurrently updated", edge_id)));
            }

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
    annot_hdr->prev_itemptr = old_annot_tid;
    annot_hdr->flags = 0;
    annot_hdr->id = edge_id;

    memcpy(annot_hdr->serialized_entity, bitset, annot_payload_size);

    ItemPointerData new_annot_tid;
    np_write_record_to_page(annot_rel, annot_tuple_buf, annot_total_size, &new_annot_tid);
    pfree(annot_tuple_buf);

    /* 9. Update the Edge phys_map safely using WAL */
    Buffer overwrite_buf = ReadBuffer(pmap_rel, ItemPointerGetBlockNumber(&phys_map_tid));
    LockBuffer(overwrite_buf, BUFFER_LOCK_EXCLUSIVE);
    
    GenericXLogState *state = GenericXLogStart(pmap_rel);
    Page wal_page = GenericXLogRegisterBuffer(state, overwrite_buf, 0);
    NeoEdgePhysMapRecord *wal_rec = (NeoEdgePhysMapRecord *) PageGetItem(wal_page, 
                                      PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&phys_map_tid)));
    
    wal_rec->a_itemptr = new_annot_tid;
    
    GenericXLogFinish(state);
    UnlockReleaseBuffer(overwrite_buf);
    table_close(pmap_rel, RowExclusiveLock);

    /* 10. MVCC Tombstone the OLD Annotation Tuple */
    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_final = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_final, BUFFER_LOCK_EXCLUSIVE);

        state = GenericXLogStart(annot_rel);
        wal_page = GenericXLogRegisterBuffer(state, obuf_final, 0);

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

PG_FUNCTION_INFO_V1(remove_edge_annotation);
Datum
remove_edge_annotation(PG_FUNCTION_ARGS)
{
    /* 1. Extract Arguments */
    int64 edge_id = PG_GETARG_INT64(0);
    int32 label_id  = PG_GETARG_INT32(1);
    int32 graph_id  = PG_GETARG_INT32(2);
    char *annot_str = text_to_cstring(PG_GETARG_TEXT_PP(3));

    CommandId cid = GetCurrentCommandId(true);
    FullTransactionId current_fxid = GetTopFullTransactionId();

    /* 2. Look up the Edge Label Cache */
    const label_cache_data *label_cache =
        search_edge_label_graph_id_label_id_cache(graph_id, label_id);

    Oid annotations_tbl = np_resolve_edge_annotations_tbl(label_cache, graph_id, label_id);

    if (!label_cache || !OidIsValid(annotations_tbl))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Structural edge label does not support annotations: graph_id=%d, label_id=%d",
                        graph_id, label_id)));

    /* 3. Resolve the Current Active Schema Array */
    ArrayType *map_array = NULL;
    const graph_cache_data *graph_cache = search_graph_id_cache(graph_id);

    if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
        uint32 schema_pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
        ItemPointerData schema_pmap_tid;
        np_id_to_tid(label_id, schema_pmap_tuples_per_page, &schema_pmap_tid);

        Relation schema_pmap_rel = table_open(graph_cache->annot_schema_phys_map, AccessShareLock);
        BlockNumber schema_pmap_blk = ItemPointerGetBlockNumber(&schema_pmap_tid);
        
        ItemPointerData latest_schema_tid;
        ItemPointerSetInvalid(&latest_schema_tid);

        if (schema_pmap_blk < RelationGetNumberOfBlocks(schema_pmap_rel)) {
            Buffer schema_pmap_buf = ReadBuffer(schema_pmap_rel, schema_pmap_blk);
            LockBuffer(schema_pmap_buf, BUFFER_LOCK_SHARE);
            Page schema_pmap_page = BufferGetPage(schema_pmap_buf);
            ItemId schema_pmap_lp = PageGetItemId(schema_pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));

            if (ItemIdIsNormal(schema_pmap_lp)) {
                NeoPhysMapRecord *pmap_rec = (NeoPhysMapRecord *) PageGetItem(schema_pmap_page, schema_pmap_lp);
                latest_schema_tid = pmap_rec->v_itemptr;
            }
            UnlockReleaseBuffer(schema_pmap_buf);
        }
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

    if (map_array == NULL) {
        map_array = label_cache->annotation_map;
    }

    if (map_array == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("No annotation schema defined for this structural edge label")));

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
                 errmsg("Annotation label '%s' is not valid for this structural edge label", annot_str)));

    int byte_size = (map_count + 7) / 8;

    /* 5. Lookup the Edge in its phys_map to get current a_itemptr */
    Relation pmap_rel = table_open(label_cache->phys_map, RowExclusiveLock);
    uint32 edge_pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
    ItemPointerData phys_map_tid;
    np_id_to_tid(edge_id, edge_pmap_tuples_per_page, &phys_map_tid);

    BlockNumber pmap_blk = ItemPointerGetBlockNumber(&phys_map_tid);

    if (pmap_blk >= RelationGetNumberOfBlocks(pmap_rel)) {
        table_close(pmap_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Edge ID not found in phys_map (table empty)")));
    }

    Buffer pmap_buf = ReadBuffer(pmap_rel, pmap_blk);
    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
    Page pmap_page = BufferGetPage(pmap_buf);
    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));

    if (!ItemIdIsNormal(pmap_lp)) {
        UnlockReleaseBuffer(pmap_buf);
        table_close(pmap_rel, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("Edge ID not found in phys_map")));
    }

    NeoEdgePhysMapRecord *disk_pmap_rec = (NeoEdgePhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
    NeoEdgePhysMapRecord current_pmap_rec = *disk_pmap_rec;
    UnlockReleaseBuffer(pmap_buf);

    ItemPointerData old_annot_tid = current_pmap_rec.a_itemptr;
    Relation annot_rel = table_open(annotations_tbl, RowExclusiveLock);

    /* DEFENSE: Instantly drop garbage memory pointers from old insert_edge calls */
    if (ItemPointerIsValid(&old_annot_tid)) {
        if (ItemPointerGetBlockNumber(&old_annot_tid) >= RelationGetNumberOfBlocks(annot_rel)) {
            ItemPointerSetInvalid(&old_annot_tid);
        }
    }

    /* 6. Read Existing Bitset */
    bytea *bitset = (bytea *) palloc0(VARHDRSZ + byte_size);
    SET_VARSIZE(bitset, VARHDRSZ + byte_size);
    char *bits = VARDATA(bitset);

    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_check = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_check, BUFFER_LOCK_SHARE);
        Page opage_check = BufferGetPage(obuf_check);
        ItemId olp_check = PageGetItemId(opage_check, ItemPointerGetOffsetNumber(&old_annot_tid));

        if (ItemIdIsNormal(olp_check)) {
            NPEntityTupleHeader old_hdr = (NPEntityTupleHeader) PageGetItem(opage_check, olp_check);
            
            if (FullTransactionIdIsValid(old_hdr->xmax)) {
                UnlockReleaseBuffer(obuf_check);
                table_close(annot_rel, RowExclusiveLock);
                table_close(pmap_rel, RowExclusiveLock);
                ereport(ERROR, (errmsg("Edge ID %ld annotation bitset was concurrently updated", edge_id)));
            }

            bytea *old_bitset = (bytea *) old_hdr->serialized_entity;
            int old_len = VARSIZE(old_bitset) - VARHDRSZ;
            memcpy(bits, VARDATA(old_bitset), old_len > byte_size ? byte_size : old_len);
        }
        UnlockReleaseBuffer(obuf_check);
    }

    /* 7. Clear the requested bit using AND with inverted mask */
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
    annot_hdr->prev_itemptr = old_annot_tid;
    annot_hdr->flags = 0;
    annot_hdr->id = edge_id;

    memcpy(annot_hdr->serialized_entity, bitset, annot_payload_size);

    ItemPointerData new_annot_tid;
    np_write_record_to_page(annot_rel, annot_tuple_buf, annot_total_size, &new_annot_tid);
    pfree(annot_tuple_buf);

    /* 9. Update the Edge phys_map safely using WAL */
    Buffer overwrite_buf = ReadBuffer(pmap_rel, ItemPointerGetBlockNumber(&phys_map_tid));
    LockBuffer(overwrite_buf, BUFFER_LOCK_EXCLUSIVE);
    
    GenericXLogState *state = GenericXLogStart(pmap_rel);
    Page wal_page = GenericXLogRegisterBuffer(state, overwrite_buf, 0);
    NeoEdgePhysMapRecord *wal_rec = (NeoEdgePhysMapRecord *) PageGetItem(wal_page, 
                                      PageGetItemId(wal_page, ItemPointerGetOffsetNumber(&phys_map_tid)));
    
    wal_rec->a_itemptr = new_annot_tid;
    
    GenericXLogFinish(state);
    UnlockReleaseBuffer(overwrite_buf);
    table_close(pmap_rel, RowExclusiveLock);

    /* 10. MVCC Tombstone the OLD Annotation Tuple */
    if (ItemPointerIsValid(&old_annot_tid) && ItemPointerGetOffsetNumber(&old_annot_tid) != 0) {
        Buffer obuf_final = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&old_annot_tid));
        LockBuffer(obuf_final, BUFFER_LOCK_EXCLUSIVE);

        state = GenericXLogStart(annot_rel);
        wal_page = GenericXLogRegisterBuffer(state, obuf_final, 0);

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

    /* 4. Heap-scan vertex AND edge catalogs, including is_primary = false rows. */
    int updated_count = 0;
    bool found = false;
    ListCell *lc;

    Relation v_rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", graph_entry->id), "table"), RowExclusiveLock);
    Datum v_target = np_find_struct_ltree(v_rel, struct_label_str);
    if (v_target != (Datum) 0)
    {
        List *v_tups = np_collect_ltree_descendants(v_rel, v_target);
        found = true;
        foreach(lc, v_tups)
        {
            HeapTuple tuple = (HeapTuple) lfirst(lc);
            if (np_apply_drop_annotation_to_tuple(graph_entry, v_rel, tuple, v_target,
                                                  struct_label_str, drop_annot_str, namespace,
                                                  9, "np_vertex"))
                updated_count++;
            heap_freetuple(tuple);
        }
        list_free(v_tups);
    }
    table_close(v_rel, RowExclusiveLock);

    Relation e_rel = table_open(np_relation_id(psprintf("np_edge_label_%d", graph_entry->id), "table"), RowExclusiveLock);
    Datum e_target = np_find_struct_ltree(e_rel, struct_label_str);
    if (e_target != (Datum) 0)
    {
        List *e_tups = np_collect_ltree_descendants(e_rel, e_target);
        found = true;
        foreach(lc, e_tups)
        {
            HeapTuple tuple = (HeapTuple) lfirst(lc);
            if (np_apply_drop_annotation_to_tuple(graph_entry, e_rel, tuple, e_target,
                                                  struct_label_str, drop_annot_str, namespace,
                                                  6, "np_edge"))
                updated_count++;
            heap_freetuple(tuple);
        }
        list_free(e_tups);
    }
    table_close(e_rel, RowExclusiveLock);

    if (!found)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("Structural label \"%s\" not found in graph \"%s\"", struct_label_str, graph_name)));
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
             * Vertex catalog: is_primary is attnum 10.
             * Edge catalog:   is_primary is attnum 7.
             */
            bool satisfy_predicate = true;
            if (indexInfo->ii_Predicate != NIL) {
                bool is_null;
                TupleDesc desc = RelationGetDescr(rel);
                Datum is_primary_datum;

                if (desc->natts >= 10)
                    is_primary_datum = heap_getattr(new_tup, 10, desc, &is_null);
                else
                    is_primary_datum = heap_getattr(new_tup, 7, desc, &is_null);

                if (!is_null && !DatumGetBool(is_primary_datum))
                    satisfy_predicate = false;
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