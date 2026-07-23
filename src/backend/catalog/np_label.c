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

#include "ltree.h"

#include "catalog/np_label.h"
#include "utils/np_cache.h"
#include "utils/adj_list.h"
#include "utils/dictionary.h"
#include "utils/edge.h"
#include "utils/vertex.h"

#define LTREEOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("ltree"), ObjectIdGetDatum(public_catalog_namespace_id())))

#define CATALOG_LTREE_ROOT_LABEL "_"

PG_FUNCTION_INFO_V1(ltree_in);
PG_FUNCTION_INFO_V1(ltree_addltree);
PG_FUNCTION_INFO_V1(ltxtq_in);
PG_FUNCTION_INFO_V1(ltree_out);

Datum text_array_to_lxtquery(ArrayType *label_array);
Datum text_array_to_lxtquery_or(ArrayType *label_array);

Oid create_linked_list_table_sequence(char *seq_name, char *namespace);
Oid create_vertex_label_linked_list_metadata_table(char *tbl_name, Oid namespace);
int insert_vertex_ll_meta(char *table_name, Oid namespace, int ll_seq, Oid tbl);
Oid create_vertex_label_linked_list_table(char *tbl_name, Oid namespace);
Oid create_label_edge_physical_mapping_table(char *tbl_name, Oid namespace);
int insert_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map);
Oid create_edge_tables(int graph_id, int label_id, Oid namespace);

Oid create_default_elabel(int graph_id, Oid edge_id_seq, Oid namespace)
{
    Oid label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(edge_id_seq)));

    Oid edge_tbl = create_edge_tables(graph_id, label_id, namespace);
    Oid phys_map = create_label_edge_physical_mapping_table(
                        psprintf("np_edge_%d_%d_phys_map", graph_id, label_id), namespace);

    insert_label(psprintf("np_edge_label_%d", graph_id), 
                 DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)), 
                 label_id, edge_tbl, phys_map);
    //TODO
    //Oid dict_id = create_vertex_property_dictionary(graph_id, label_id);
    //create_vertex_dictionary_metadata_btree_index(graph_id, label_id, dict_id);

    return edge_tbl;
}


Oid create_default_vlabel(int graph_id, Oid vertex_id_seq, Oid namespace)
{
    Oid label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(vertex_id_seq)));

    Oid vertex_tbl = create_vertex_tables(graph_id, label_id, namespace);
    Oid phys_map   = create_label_vertex_physical_mapping_table(
                        psprintf("np_vertex_%d_%d_phys_map", graph_id, label_id), namespace);
    Oid arraylist  = create_vertex_label_arraylist_table(
                        psprintf("np_vertex_%d_%d_arraylist", graph_id, label_id), namespace);

    Oid ll_seq = create_linked_list_table_sequence(
        psprintf("np_vertex_%d_%d_linked_list_seq", graph_id, label_id), 
        "neopostgraph"
    );

    Oid ll_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(ll_seq)));

    char *ll_table_name = psprintf("np_vertex_%d_%d_%d_linked_list", 
                                   graph_id, label_id, ll_id);
    Oid ll_table = create_vertex_label_linked_list_table(ll_table_name, namespace);

    char *ll_meta_table = psprintf("np_vertex_%d_%d_linked_list_meta", graph_id, label_id);
    Oid ll_meta = create_vertex_label_linked_list_metadata_table(ll_meta_table, namespace);

    insert_vertex_ll_meta(ll_meta_table, namespace,  ll_table, ll_id);

    insert_vertex_label(
        psprintf("np_vertex_label_%d", graph_id),
        DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)),
        label_id,
        vertex_tbl,
        phys_map,
        arraylist,
        ll_seq,
        ll_meta
    );

    Oid dict_id = create_vertex_property_dictionary(graph_id, label_id);
    create_vertex_dictionary_metadata_btree_index(graph_id, label_id, dict_id);

    return vertex_tbl;
}

PG_FUNCTION_INFO_V1(create_vlabel);
Datum create_vlabel(PG_FUNCTION_ARGS)
{
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
                        errmsg("label must not be NULL")));

    // fetch the graph name
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    char *graph_name = NameStr(*PG_GETARG_NAME(0));

    // fetch the graph cache for the graph_id and vertex_id_seq
    graph_cache_data *entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!entry)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                errmsg("graph \"%s\" does not exist \"%s\".", graph_name, get_namespace_name(namespace)),
                PG_ARGISNULL(1) ?
                    errhint("When namespace is not specified, the graph is created in the first namespace in the search path. Consider changing the search path or specifying a namespace explicitly.") :
                    errhint("Use a different graph name or create the graph.")
                ));

    Oid label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(entry->vertex_id_seq)));
    Oid vertex_tbl = create_vertex_tables(entry->id, label_id, namespace);
    Oid phys_map = create_label_vertex_physical_mapping_table(psprintf("np_vertex_%d_%d_phys_map", entry->id, label_id), namespace);
    Oid arraylist = create_vertex_label_arraylist_table(psprintf("np_vertex_%d_%d_arraylist", entry->id, label_id), namespace);
    
    // linked list
    Oid ll_seq = create_linked_list_table_sequence(psprintf("np_vertex_%d_%d_linked_list_seq", entry->id, label_id), "neopostgraph");    
    Oid ll_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(ll_seq)));
    char *ll_meta_table = psprintf("np_vertex_%d_%d_linked_list_meta", entry->id, label_id);
    Oid ll_meta = create_vertex_label_linked_list_metadata_table(ll_meta_table, namespace);
    Oid ll = create_vertex_label_linked_list_table(psprintf("np_vertex_%d_%d_%d_linked_list", entry->id, label_id, ll_id), namespace);
    insert_vertex_ll_meta(ll_meta_table, namespace, ll, ll_id);

    insert_vertex_label(
        psprintf("np_vertex_label_%d", entry->id),
        DirectFunctionCall2(ltree_addltree,
            DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)),
            PG_GETARG_DATUM(1)
        ),
        label_id,
        vertex_tbl,
        phys_map,
        arraylist, ll_seq, ll_meta);

    create_vertex_property_dictionary(entry->id, vertex_tbl);
    //create_vertex_tables(entry->id, label_id, namespace);
    ereport(NOTICE, (errmsg("vlabel \"%s\" has been created", graph_name)));

    PG_RETURN_VOID();
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

    ereport(NOTICE, (errmsg("elabel \"%s\" has been created", graph_name)));

    PG_RETURN_VOID();
}

Oid create_vertex_tables(int graph_id, int label_id, Oid namespace) {
    CreateStmt *create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), psprintf("np_vertex_%d_%d", graph_id, label_id), -1);

    ColumnDef *id = makeColumnDef("id", INT8OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    ColumnDef *vertex = makeColumnDef("vertex", VERTEXOID, -1, InvalidOid);
    vertex->constraints = list_make1(build_not_null_constraint());

    create_stmt->tableElts = list_make2(id, vertex);
    create_stmt->accessMethod = "entity_store";
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    PlannedStmt *wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver,
                   NULL);

    CommandCounterIncrement();

    return get_relname_relid(psprintf("np_vertex_%d_%d", graph_id, label_id), namespace);
}

Oid create_edge_tables(int graph_id, int label_id, Oid namespace) {
    CreateStmt *create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), psprintf("np_edge_%d_%d", graph_id, label_id), -1);

    ColumnDef *id = makeColumnDef("id", INT8OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    ColumnDef *edge = makeColumnDef("edge", EDGEOID, -1, InvalidOid);
    edge->constraints = list_make1(build_not_null_constraint());

    create_stmt->tableElts = list_make2(id, edge);
    create_stmt->accessMethod = "entity_store";
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    PlannedStmt *wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver,
                   NULL);
    CommandCounterIncrement();

    return get_relname_relid(psprintf("np_edge_%d_%d", graph_id, label_id), namespace);
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


Oid create_label_sequence(char *seq_name, char *namespace)
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

    Datum values[4] = {
        ObjectIdGetDatum(tbl),
        Int32GetDatum(ll_seq),
        BoolGetDatum(true),
        BoolGetDatum(false)
    };
    bool nulls[4] = { false, false, false, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
}

int insert_vertex_label(char *table_name, Datum label,Oid label_id, Oid tbl, Oid phys_map, Oid arraylist, Oid ll_seq, Oid ll_meta)
{
    Relation rel = table_open(np_relation_id(table_name, "table"), RowExclusiveLock);

    Datum values[7] = {
        ObjectIdGetDatum(label_id),
        label,
        ObjectIdGetDatum(tbl),
        ObjectIdGetDatum(phys_map),
        ObjectIdGetDatum(ll_meta),
        ObjectIdGetDatum(ll_seq),
        ObjectIdGetDatum(arraylist)
    };
    bool nulls[7] = { false, false, false, false, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
}

int insert_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map)
{
    Relation rel = table_open(np_relation_id(table_name, "table"), RowExclusiveLock);

    Datum values[4] = {
        ObjectIdGetDatum(label_id),
        label,
        ObjectIdGetDatum(tbl),
        ObjectIdGetDatum(phys_map)
    };
    bool nulls[4] = { false, false, false, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);
    CommandCounterIncrement();
    
    return 0; /* Just returning 0 since the signature expects int */
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

    ProcessUtility(wrapper, "(generated arraylist CREATE TABLE)",
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
        makeColumnDef("adj_list", ADJLISTOID, -1, InvalidOid), //TODO replace with array list type
        makeColumnDef("next_itemptr", TIDOID, -1, InvalidOid)
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


Oid create_label_vertex_physical_mapping_table(char *tbl_name, Oid namespace)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar(get_namespace_name(namespace), tbl_name, -1);

    create_stmt->tableElts = list_make3(makeColumnDef("v_itemptr", TIDOID, -1, InvalidOid),
                                        makeColumnDef("e_tbl_id", REGCLASSOID, -1, InvalidOid),
                                        makeColumnDef("e_itemptr", TIDOID, -1, InvalidOid));
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

    List *tableElts = list_make5(id, ltree, vertex_tbl, phys_map, linked_list_meta);
    tableElts = lappend(tableElts, linked_list_seq);
    tableElts = lappend(tableElts, arraylist);
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

Oid create_label_metadata_table(char *meta_tbl_name)
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
    ltree->constraints = list_make1(build_not_null_constraint());
    ColumnDef *phys_map = makeColumnDef("phys_map", REGCLASSOID, -1, InvalidOid);
    phys_map->constraints = list_make1(build_not_null_constraint());

    create_stmt->tableElts = list_make4(id, ltree, vertex_tbl, phys_map);
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

void create_metadata_btree_index(char *tbl_name)
{
    IndexStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(IndexStmt);

    create_stmt->idxname = psprintf("%s_btree_idx", tbl_name);
    create_stmt->relation = makeRangeVar("neopostgraph", tbl_name, -1);

    IndexElem *id = makeNode(IndexElem);
    id->name = "id";

    create_stmt->accessMethod = "btree";
    create_stmt->tableSpace = NULL;
    create_stmt->indexParams = list_make1(id);
    create_stmt->indexIncludingParams = NIL;
    create_stmt->options = NIL;
    create_stmt->whereClause = NULL;
    create_stmt->excludeOpNames = NIL;
    create_stmt->idxcomment = "primary index for autogenerated label metadata table";
    create_stmt->indexOid = InvalidOid;

    create_stmt->unique = true;
    create_stmt->primary = true;
    create_stmt->isconstraint = true;
    create_stmt->concurrent = false;
    create_stmt->deferrable = false;
    create_stmt->initdeferred = false;
    create_stmt->if_not_exists = false;
    create_stmt->reset_default_tblspc = false;


    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver, NULL);

    CommandCounterIncrement();
}

void create_metadata_gist_index(char *tbl_name)
{
    IndexStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(IndexStmt);

    create_stmt->idxname = psprintf("%s_gist_idx", tbl_name);
    create_stmt->relation = makeRangeVar("neopostgraph", tbl_name, -1);

    IndexElem *ltree = makeNode(IndexElem);
    ltree->name = "ltree";

    create_stmt->accessMethod = "gist";
    create_stmt->tableSpace = NULL;
    create_stmt->indexParams = list_make1(ltree);
    create_stmt->indexIncludingParams = NIL;
    create_stmt->options = NIL;
    create_stmt->whereClause = NULL;
    create_stmt->excludeOpNames = NIL;
    create_stmt->idxcomment = "label gist index for autogenerated label metadata table";
    create_stmt->indexOid = InvalidOid;

    create_stmt->unique = false;
    create_stmt->primary = false;
    create_stmt->isconstraint = false;
    create_stmt->concurrent = false;
    create_stmt->deferrable = false;
    create_stmt->initdeferred = false;
    create_stmt->if_not_exists = false;
    create_stmt->reset_default_tblspc = false;

    wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver, NULL);
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
            CatalogTupleUpdate(meta_rel, &tuple->t_self, newtup);
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

PG_FUNCTION_INFO_V1(rotate_active_linked_list_table);
Datum
rotate_active_linked_list_table(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_name and label_id are required")));

    char *graph_name = NameStr(*PG_GETARG_NAME(0));
    int32 label_id   = PG_GETARG_INT32(1);

    Oid namespace = linitial_oid(fetch_search_path(false));

    const graph_cache_data *graph = search_graph_name_namespace_cache(graph_name, namespace);
    if (!graph)
        ereport(ERROR, (errmsg("graph \"%s\" does not exist", graph_name)));

    const label_cache_data *label = search_vertex_label_graph_id_label_id_cache(graph->id, label_id);
    if (!label || !OidIsValid(label->linked_list_meta) || !OidIsValid(label->linked_list_seq))
        ereport(ERROR, (errmsg("label does not have linked list setup")));

    create_new_active_linked_list(
        graph->id,
        label_id,
        label->linked_list_seq,
        label->linked_list_meta,
        namespace
    );

    PG_RETURN_VOID();
}