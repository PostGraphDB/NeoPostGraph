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
#include "access/stratnum.h"
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
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "tsearch/ts_utils.h"

#include "parser/parse_type.h"
#include "catalog/pg_type.h"

#include "ltree.h"

#include "catalog/np_graph.h"
#include "catalog/np_label.h"
#include "utils/np_cache.h"

#define CATALOG_LTREE_ROOT_LABEL "_"

void insert_vlabel(int graph_id, Datum label, Oid vertex_id_seq);
Datum text_array_to_lxtquery(ArrayType *label_array);
Datum text_array_to_lxtquery_or(ArrayType *label_array);

PG_FUNCTION_INFO_V1(ltree_in);
PG_FUNCTION_INFO_V1(ltree_addltree);
PG_FUNCTION_INFO_V1(ltxtq_in);

void create_default_vlabel(int graph_id, Oid vertex_id_seq)
{
    insert_vlabel(graph_id, DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)), vertex_id_seq);
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


    insert_vlabel(
        entry->id,
        DirectFunctionCall2(ltree_addltree,
            DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)),
            PG_GETARG_DATUM(1)
        ),
        entry->vertex_id_seq);

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

void insert_vlabel(int graph_id, Datum label, Oid vertex_id_seq)
{
    Relation rel = table_open(np_vertex_label_relation_id(), RowExclusiveLock);

    Datum values[3] = {
        DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(vertex_id_seq)),
        Int32GetDatum(graph_id),
        label
    };
    bool nulls[3] = { false, false, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
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

        Relation rel = table_open(np_vertex_label_relation_id(), AccessShareLock);

        ScanKeyData skey[2];
        ScanKeyInit(&skey[0], 2, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(cache_entry->id));
        ScanKeyInit(&skey[1], 3, 14,
            DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum("public.ltxtq_exec(public.ltree, public.ltxtquery)"))),
            text_array_to_lxtquery(PG_GETARG_ARRAYTYPE_P(1))
        ); 


        SysScanDesc scan = systable_beginscan(rel, np_vertex_label_graph_id_label_id(), true, NULL, 2, skey);

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

        Relation rel = table_open(np_vertex_label_relation_id(), AccessShareLock);

        ScanKeyData skey[2];
        ScanKeyInit(&skey[0], 2, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(cache_entry->id));
        ScanKeyInit(&skey[1], 3, 14,
            DatumGetObjectId(DirectFunctionCall1(regprocedurein, CStringGetDatum("public.ltxtq_exec(public.ltree, public.ltxtquery)"))),
            text_array_to_lxtquery_or(PG_GETARG_ARRAYTYPE_P(1))
        ); 


        SysScanDesc scan = systable_beginscan(rel, np_vertex_label_graph_id_label_id(), true, NULL, 2, skey);

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