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
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "commands/alter.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "catalog/np_graph.h"
#include "catalog/np_label.h"
#include "commands/label_commands.h"
#include "utils/np_cache.h"

void insert_graph(const Name graph_name, const Oid namespace, int graph_id, Oid vertex_label, Oid vertex_id_seq, Oid edge_label, Oid edge_id_seq, Oid schema_tbl, Oid schema_pmap);

PG_FUNCTION_INFO_V1(create_graph);
Datum create_graph(PG_FUNCTION_ARGS)
{
    // fetch the namespace the graph is created in
    Oid namespace;
    if (PG_ARGISNULL(1)) {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("create_graph requires a search path when namespace is not specified")));

        namespace = linitial_oid(search_path);
    } else if (!OidIsValid(namespace = get_namespace_oid(TextDatumGetCString(PG_GETARG_DATUM(1)), true))) {
        namespace = NamespaceCreate(TextDatumGetCString(PG_GETARG_DATUM(1)), GetUserId(), false);
        CommandCounterIncrement();
    }

    // fetch the graph name
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    char *graph_name = NameStr(*PG_GETARG_NAME(0));

    // only 1 graph per name per namespace
    if (search_graph_name_namespace_cache(graph_name, namespace))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_SCHEMA),
                errmsg("graph \"%s\" already exists in the namespace \"%s\".", graph_name, get_namespace_name(namespace)),
                PG_ARGISNULL(1) ?
                    errhint("When namespace is not specified, the graph is created in the first namespace in the search path. Consider changing the search path or specifying a namespace explicitly.") :
                    errhint("Use a different graph name or create the graph in a different namespace.")
                ));

    int graph_id = DatumGetInt32(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(get_relname_relid("np_graph_id_seq", np_namespace_id()))));

    Oid vertex_id_seq, vertex_label;
    setup_vertex_label_catalog(graph_id, namespace, &vertex_id_seq, &vertex_label);

    Oid edge_id_seq, edge_label;
    setup_edge_label_catalog(graph_id, namespace, &edge_id_seq, &edge_label);

    Oid schema_tbl = create_annotation_schema_table(graph_id, namespace);
    Oid schema_pmap = create_annotation_schema_phys_map_table(graph_id, namespace);

    create_label_catalog_table(graph_id);

    insert_graph(PG_GETARG_NAME(0), namespace, graph_id, vertex_label, vertex_id_seq, edge_label, edge_id_seq, schema_tbl, schema_pmap);

    ereport(NOTICE, (errmsg("graph \"%s\" has been created", graph_name)));

    PG_RETURN_VOID();
}

// INSERT INTO postgraph.np_graph VALUES (id, graph_name, namespace, vertex_id_seq)
void insert_graph(const Name graph_name, const Oid namespace, int graph_id, Oid vertex_label, Oid vertex_id_seq, Oid edge_label, Oid edge_id_seq, Oid schema_tbl, Oid schema_pmap)
{
    Relation rel = table_open(np_graph_relation_id(), RowExclusiveLock);

    Datum values[9] = {
        Int32GetDatum(graph_id),
        NameGetDatum(graph_name),
        ObjectIdGetDatum(namespace),
        ObjectIdGetDatum(vertex_label),
        ObjectIdGetDatum(vertex_id_seq),
        ObjectIdGetDatum(edge_label),
        ObjectIdGetDatum(edge_id_seq),
        ObjectIdGetDatum(schema_tbl),
        ObjectIdGetDatum(schema_pmap)
    };
    bool nulls[9] = { false, false, false, false, false, false, false, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
}

static void
np_append_oid(List **oids, Datum d, bool isnull)
{
    Oid oid;

    if (isnull)
        return;
    oid = DatumGetObjectId(d);
    if (OidIsValid(oid))
        *oids = lappend_oid(*oids, oid);
}

static void
np_collect_regclass_col(Relation rel, HeapTuple tuple, AttrNumber attno, List **oids)
{
    bool isnull;
    Datum d = heap_getattr(tuple, attno, RelationGetDescr(rel), &isnull);

    np_append_oid(oids, d, isnull);
}

static void
np_append_named_rel(List **oids, const char *name, Oid nsp)
{
    Oid oid = get_relname_relid(name, nsp);

    if (OidIsValid(oid))
        *oids = lappend_oid(*oids, oid);
}

static void
np_collect_linked_list_tables(Oid meta_oid, List **oids)
{
    Relation meta_rel;
    SysScanDesc scan;
    HeapTuple tuple;

    if (!OidIsValid(meta_oid))
        return;

    meta_rel = table_open(meta_oid, AccessShareLock);
    scan = systable_beginscan(meta_rel, InvalidOid, false, NULL, 0, NULL);
    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
        np_collect_regclass_col(meta_rel, tuple, 2, oids);
    systable_endscan(scan);
    table_close(meta_rel, AccessShareLock);
}

static List *
np_collect_graph_namespace_rels(const graph_cache_data *graph)
{
    List *oids = NIL;
    Relation cat;
    SysScanDesc scan;
    HeapTuple tuple;

    np_append_oid(&oids, ObjectIdGetDatum(graph->vertex_id_seq), false);
    np_append_oid(&oids, ObjectIdGetDatum(graph->edge_id_seq), false);
    np_append_oid(&oids, ObjectIdGetDatum(graph->annot_schema_tbl), false);
    np_append_oid(&oids, ObjectIdGetDatum(graph->annot_schema_phys_map), false);

    cat = table_open(graph->vertex_labels, AccessShareLock);
    scan = systable_beginscan(cat, InvalidOid, false, NULL, 0, NULL);
    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        bool isnull;
        Oid ll_meta;
        int32 label_id;

        /* tbl, phys_map, linked_list_meta, linked_list_seq, arraylist, annotations_tbl */
        np_collect_regclass_col(cat, tuple, 3, &oids);
        np_collect_regclass_col(cat, tuple, 4, &oids);
        np_collect_regclass_col(cat, tuple, 5, &oids);
        np_collect_regclass_col(cat, tuple, 6, &oids);
        np_collect_regclass_col(cat, tuple, 7, &oids);
        np_collect_regclass_col(cat, tuple, 8, &oids);

        ll_meta = DatumGetObjectId(heap_getattr(tuple, 5, RelationGetDescr(cat), &isnull));
        if (!isnull && OidIsValid(ll_meta))
            np_collect_linked_list_tables(ll_meta, &oids);

        label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(cat), &isnull));
        if (!isnull)
        {
            np_append_named_rel(&oids, psprintf("np_vertex_id_seq_%d_%d", (int) graph->id, label_id), np_namespace_id());
            np_append_named_rel(&oids, psprintf("np_vertex_property_dictionary_%d_%d", (int) graph->id, label_id), np_namespace_id());
            np_append_named_rel(&oids, psprintf("np_vertex_property_dictionary_seq_%d_%d", (int) graph->id, label_id), np_namespace_id());
        }
    }
    systable_endscan(scan);
    table_close(cat, AccessShareLock);

    cat = table_open(graph->edge_labels, AccessShareLock);
    scan = systable_beginscan(cat, InvalidOid, false, NULL, 0, NULL);
    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        bool isnull;
        int32 label_id;

        /* tbl, phys_map, annotations_tbl */
        np_collect_regclass_col(cat, tuple, 3, &oids);
        np_collect_regclass_col(cat, tuple, 4, &oids);
        np_collect_regclass_col(cat, tuple, 5, &oids);

        label_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(cat), &isnull));
        if (!isnull)
            np_append_named_rel(&oids, psprintf("np_edge_id_seq_%d_%d", (int) graph->id, label_id), np_namespace_id());
    }
    systable_endscan(scan);
    table_close(cat, AccessShareLock);

    np_append_oid(&oids, ObjectIdGetDatum(graph->vertex_labels), false);
    np_append_oid(&oids, ObjectIdGetDatum(graph->edge_labels), false);
    np_append_named_rel(&oids, psprintf("np_label_catalog_%d", (int) graph->id), np_namespace_id());

    return oids;
}

static void
np_drop_rel(Oid relid)
{
    ObjectAddress obj;

    if (!OidIsValid(relid))
        return;
    if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
        return;

    obj.classId = RelationRelationId;
    obj.objectId = relid;
    obj.objectSubId = 0;
    performDeletion(&obj, DROP_CASCADE, PERFORM_DELETION_INTERNAL);
    CommandCounterIncrement();
}

static void
np_move_rel_to_namespace(Oid relid, Oid old_nsp, Oid new_nsp, ObjectAddresses *moved)
{
    if (!OidIsValid(relid))
        return;
    if (get_rel_namespace(relid) != old_nsp)
        return;
    AlterObjectNamespace_oid(RelationRelationId, relid, new_nsp, moved);
}

PG_FUNCTION_INFO_V1(alter_graph);
Datum
alter_graph(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *new_nsp_str;
    Oid old_nsp;
    Oid new_nsp;
    const graph_cache_data *graph;
    List *oids;
    ListCell *lc;
    ObjectAddresses *moved;
    Relation np_graph;
    ScanKeyData skey[2];
    SysScanDesc scan;
    HeapTuple old_tup;
    HeapTuple new_tup;
    Datum values[9];
    bool nulls[9];
    bool replace[9];
    NameData name_data;
    int moved_count = 0;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("new namespace must not be NULL")));

    graph_name = NameStr(*PG_GETARG_NAME(0));
    new_nsp_str = TextDatumGetCString(PG_GETARG_DATUM(1));

    if (PG_ARGISNULL(2))
    {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("alter_graph requires a search path when namespace is not specified")));
        old_nsp = linitial_oid(search_path);
    }
    else
    {
        char *old_nsp_str = TextDatumGetCString(PG_GETARG_DATUM(2));
        old_nsp = get_namespace_oid(old_nsp_str, true);
        if (!OidIsValid(old_nsp))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("namespace \"%s\" does not exist", old_nsp_str)));
    }

    graph = search_graph_name_namespace_cache(graph_name, old_nsp);
    if (!graph)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" does not exist in the namespace \"%s\"",
                               graph_name, get_namespace_name(old_nsp))));

    new_nsp = get_namespace_oid(new_nsp_str, true);
    if (!OidIsValid(new_nsp))
    {
        new_nsp = NamespaceCreate(new_nsp_str, GetUserId(), false);
        CommandCounterIncrement();
    }

    if (new_nsp == old_nsp)
    {
        ereport(NOTICE, (errmsg("graph \"%s\" is already in namespace \"%s\"",
                                graph_name, get_namespace_name(new_nsp))));
        PG_RETURN_VOID();
    }

    if (search_graph_name_namespace_cache(graph_name, new_nsp))
        ereport(ERROR, (errcode(ERRCODE_DUPLICATE_SCHEMA),
                        errmsg("graph \"%s\" already exists in the namespace \"%s\"",
                               graph_name, get_namespace_name(new_nsp))));

    oids = np_collect_graph_namespace_rels(graph);
    moved = new_object_addresses();
    foreach(lc, oids)
    {
        Oid relid = lfirst_oid(lc);
        Oid before = get_rel_namespace(relid);

        np_move_rel_to_namespace(relid, old_nsp, new_nsp, moved);
        if (before == old_nsp && get_rel_namespace(relid) == new_nsp)
            moved_count++;
    }
    free_object_addresses(moved);
    list_free(oids);

    np_graph = table_open(np_graph_relation_id(), RowExclusiveLock);
    namestrcpy(&name_data, graph_name);
    ScanKeyInit(&skey[0], 2, BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&name_data));
    ScanKeyInit(&skey[1], 3, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(old_nsp));
    scan = systable_beginscan(np_graph, np_graph_name_namespace_index_id(), true, NULL, 2, skey);
    old_tup = systable_getnext(scan);
    if (!HeapTupleIsValid(old_tup))
    {
        systable_endscan(scan);
        table_close(np_graph, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" catalog row not found", graph_name)));
    }

    memset(values, 0, sizeof(values));
    memset(nulls, false, sizeof(nulls));
    memset(replace, false, sizeof(replace));
    values[2] = ObjectIdGetDatum(new_nsp);
    replace[2] = true;
    new_tup = heap_modify_tuple(old_tup, RelationGetDescr(np_graph), values, nulls, replace);
    CatalogTupleUpdate(np_graph, &old_tup->t_self, new_tup);
    heap_freetuple(new_tup);
    systable_endscan(scan);
    table_close(np_graph, RowExclusiveLock);
    CommandCounterIncrement();

    invalidate_graph_name_namespace_cache_entry(graph_name, old_nsp);

    ereport(NOTICE, (errmsg("graph \"%s\" moved from namespace \"%s\" to \"%s\" (%d relation(s))",
                            graph_name, get_namespace_name(old_nsp), get_namespace_name(new_nsp), moved_count)));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(rename_graph);
Datum
rename_graph(PG_FUNCTION_ARGS)
{
    char *graph_name;
    char *new_name;
    Oid nsp;
    const graph_cache_data *graph;
    Relation np_graph;
    ScanKeyData skey[2];
    SysScanDesc scan;
    HeapTuple old_tup;
    HeapTuple new_tup;
    Datum values[9];
    bool nulls[9];
    bool replace[9];
    NameData name_data;
    NameData new_name_data;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("new graph name must not be NULL")));

    graph_name = NameStr(*PG_GETARG_NAME(0));
    new_name = NameStr(*PG_GETARG_NAME(1));

    if (PG_ARGISNULL(2))
    {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("rename_graph requires a search path when namespace is not specified")));
        nsp = linitial_oid(search_path);
    }
    else
    {
        char *nsp_str = TextDatumGetCString(PG_GETARG_DATUM(2));
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

    if (namestrcmp(PG_GETARG_NAME(0), new_name) == 0)
    {
        ereport(NOTICE, (errmsg("graph \"%s\" is already named \"%s\"",
                                graph_name, new_name)));
        PG_RETURN_VOID();
    }

    if (search_graph_name_namespace_cache(new_name, nsp))
        ereport(ERROR, (errcode(ERRCODE_DUPLICATE_SCHEMA),
                        errmsg("graph \"%s\" already exists in the namespace \"%s\"",
                               new_name, get_namespace_name(nsp))));

    np_graph = table_open(np_graph_relation_id(), RowExclusiveLock);
    namestrcpy(&name_data, graph_name);
    ScanKeyInit(&skey[0], 2, BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&name_data));
    ScanKeyInit(&skey[1], 3, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
    scan = systable_beginscan(np_graph, np_graph_name_namespace_index_id(), true, NULL, 2, skey);
    old_tup = systable_getnext(scan);
    if (!HeapTupleIsValid(old_tup))
    {
        systable_endscan(scan);
        table_close(np_graph, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" catalog row not found", graph_name)));
    }

    memset(values, 0, sizeof(values));
    memset(nulls, false, sizeof(nulls));
    memset(replace, false, sizeof(replace));
    namestrcpy(&new_name_data, new_name);
    values[1] = NameGetDatum(&new_name_data);
    replace[1] = true;
    new_tup = heap_modify_tuple(old_tup, RelationGetDescr(np_graph), values, nulls, replace);
    CatalogTupleUpdate(np_graph, &old_tup->t_self, new_tup);
    heap_freetuple(new_tup);
    systable_endscan(scan);
    table_close(np_graph, RowExclusiveLock);
    CommandCounterIncrement();

    invalidate_graph_name_namespace_cache_entry(graph_name, nsp);

    ereport(NOTICE, (errmsg("graph \"%s\" has been renamed to \"%s\"",
                            graph_name, new_name)));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(drop_graph);
Datum
drop_graph(PG_FUNCTION_ARGS)
{
    char *graph_name;
    Oid nsp;
    const graph_cache_data *graph;
    List *oids;
    ListCell *lc;
    Relation np_graph;
    ScanKeyData skey[2];
    SysScanDesc scan;
    HeapTuple old_tup;
    NameData name_data;

    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph name must not be NULL")));

    graph_name = NameStr(*PG_GETARG_NAME(0));

    if (PG_ARGISNULL(1))
    {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("drop_graph requires a search path when namespace is not specified")));
        nsp = linitial_oid(search_path);
    }
    else
    {
        char *nsp_str = TextDatumGetCString(PG_GETARG_DATUM(1));
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

    oids = np_collect_graph_namespace_rels(graph);
    foreach(lc, oids)
        np_drop_rel(lfirst_oid(lc));
    list_free(oids);

    np_graph = table_open(np_graph_relation_id(), RowExclusiveLock);
    namestrcpy(&name_data, graph_name);
    ScanKeyInit(&skey[0], 2, BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&name_data));
    ScanKeyInit(&skey[1], 3, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
    scan = systable_beginscan(np_graph, np_graph_name_namespace_index_id(), true, NULL, 2, skey);
    old_tup = systable_getnext(scan);
    if (!HeapTupleIsValid(old_tup))
    {
        systable_endscan(scan);
        table_close(np_graph, RowExclusiveLock);
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                        errmsg("graph \"%s\" catalog row not found", graph_name)));
    }

    CatalogTupleDelete(np_graph, &old_tup->t_self);
    systable_endscan(scan);
    table_close(np_graph, RowExclusiveLock);
    CommandCounterIncrement();

    invalidate_graph_name_namespace_cache_entry(graph_name, nsp);

    ereport(NOTICE, (errmsg("graph \"%s\" has been dropped", graph_name)));
    PG_RETURN_VOID();
}
