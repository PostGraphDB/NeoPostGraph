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
#include "utils/dictionary.h"

#define LTREEOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("ltree"), ObjectIdGetDatum(public_catalog_namespace_id())))



#define CATALOG_LTREE_ROOT_LABEL "_"


int insert_vlabel(int graph_id, Datum label, Oid vertex_id_seq);
Datum text_array_to_lxtquery(ArrayType *label_array);
Datum text_array_to_lxtquery_or(ArrayType *label_array);


PG_FUNCTION_INFO_V1(ltree_in);
PG_FUNCTION_INFO_V1(ltree_addltree);
PG_FUNCTION_INFO_V1(ltxtq_in);

void create_default_vlabel(int graph_id, Oid vertex_id_seq)
{
    int label_id = insert_vlabel(graph_id, DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)), vertex_id_seq);

    create_vertex_property_dictionary(graph_id, label_id);
}


PG_FUNCTION_INFO_V1(create_vlabel);
Datum create_vlabel(PG_FUNCTION_ARGS)
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

    // fetch the graph cache for the graph_id and vertex_id_seq
    graph_cache_data *entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!entry)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                errmsg("graph \"%s\" already exists in the namespace \"%s\".", graph_name, get_namespace_name(namespace)),
                PG_ARGISNULL(1) ?
                    errhint("When namespace is not specified, the graph is created in the first namespace in the search path. Consider changing the search path or specifying a namespace explicitly.") :
                    errhint("Use a different graph name or create the graph in a different namespace.")
                ));


    int label_id = insert_vlabel(
        entry->id,
        DirectFunctionCall2(ltree_addltree,
            DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)),
            PG_GETARG_DATUM(1)
        ),
        entry->vertex_id_seq);

    create_vertex_property_dictionary(entry->id, label_id);
    ereport(NOTICE, (errmsg("graph \"%s\" has been created", graph_name)));

    PG_RETURN_VOID();
}

void create_vlabel_from_array(int graph_id, ArrayType *labels, Oid vertex_id_seq)
{
    
}

Oid create_vlabel_sequence(int graph_id, char *namespace)
{
    ParseState *pstate = make_parsestate(NULL);
    pstate->p_sourcetext = "(generated CREATE SEQUENCE command)";

    char *seq_name = psprintf("vertex_label_id_seq_%d", graph_id);

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


int insert_vlabel(int graph_id, Datum label, Oid vertex_id_seq)
{
    Relation rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", graph_id), "table"), RowExclusiveLock);

    Datum label_id = DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(vertex_id_seq));
    Datum values[2] = {
        label_id,
        label
    };
    bool nulls[2] = { false, false};//, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();

    return DatumGetInt32(label_id);
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
        {
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                            errmsg("graph \"%s\" does not exist in namespace \"%s\"", 
                            graph_name, get_namespace_name(namespace))));
        }

        if (PG_ARGISNULL(1))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("label array must not be NULL")));

        Relation rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", cache_entry->id), "table"), AccessShareLock);

        ScanKeyData skey[1];
       // ScanKeyInit(&skey[0], 1, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(cache_entry->id));
        ScanKeyInit(&skey[0], 2, 14,
            DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum("public.ltxtq_exec(public.ltree, public.ltxtquery)"))),
            text_array_to_lxtquery(PG_GETARG_ARRAYTYPE_P(1))
        ); 


        SysScanDesc scan = systable_beginscan(rel, np_relation_id(psprintf("np_vertex_label_graph_id_label_%d", cache_entry->id), "index"), true, NULL, 1, skey);

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
        //ScanKeyInit(&skey[0], 2, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(cache_entry->id));
        ScanKeyInit(&skey[0], 2, 14,
            DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum("public.ltxtq_exec(public.ltree, public.ltxtquery)"))),
            text_array_to_lxtquery_or(PG_GETARG_ARRAYTYPE_P(1))
        );

        SysScanDesc scan = systable_beginscan(rel, np_relation_id(psprintf("np_vertex_label_graph_id_label_%d", cache_entry->id), "index"), true, NULL, 1, skey);

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

Oid create_vertex_label_metadata_table(int graph_id)
{
    CreateStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(CreateStmt);

    create_stmt->relation = makeRangeVar("neopostgraph", psprintf("np_vertex_label_%d", graph_id), -1);

    ColumnDef *id = makeColumnDef("id", INT4OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    ColumnDef *ltree = makeColumnDef("ltree", LTREEOID, -1, InvalidOid);
    ltree->constraints = list_make1(build_not_null_constraint());

    create_stmt->tableElts = list_make2(id, ltree);
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
    // CommandCounterIncrement() is called in ProcessUtility()
    return get_relname_relid(psprintf("np_vertex_label_%d", graph_id), get_namespace_oid("neopostgraph", false));
}

void create_vertex_label_metadata_btree_index(int graph_id)
{
    IndexStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(IndexStmt);

    create_stmt->idxname = psprintf("np_vertex_label_graph_id_id_index_%d", graph_id);
    create_stmt->relation = makeRangeVar("neopostgraph", psprintf("np_vertex_label_%d", graph_id), -1);

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
    create_stmt->concurrent = false; // Index and Table start empty
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

void create_vertex_label_metadata_gist_index(int graph_id)
{
    IndexStmt *create_stmt;
    PlannedStmt *wrapper;

    create_stmt = makeNode(IndexStmt);

    create_stmt->idxname = psprintf("np_vertex_label_graph_id_label_%d", graph_id);
    create_stmt->relation = makeRangeVar("neopostgraph", psprintf("np_vertex_label_%d", graph_id), -1);

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
    create_stmt->concurrent = false; // Index and Table start empty
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