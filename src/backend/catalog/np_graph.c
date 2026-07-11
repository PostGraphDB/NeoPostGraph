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
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "catalog/indexing.h"

#include "catalog/np_graph.h"
#include "catalog/np_label.h"
#include "utils/np_cache.h"

void insert_graph(const Name graph_name, const Oid namespace, int graph_id, Oid vertex_label, Oid vertex_id_seq, Oid edge_label, Oid edge_id_seq);

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

    // vertex labels setup
    Oid vertex_id_seq = create_label_sequence(psprintf("vertex_label_id_seq_%d",graph_id), get_namespace_name(namespace));
    char *vertex_meta_tbl = psprintf("np_vertex_label_%d", graph_id);
    Oid vertex_label = create_vertex_label_metadata_table(psprintf(vertex_meta_tbl));
    create_metadata_btree_index(vertex_meta_tbl);
    create_metadata_gist_index(vertex_meta_tbl);
    create_default_vlabel(graph_id, vertex_id_seq, namespace);

    // edge labels setup
    Oid edge_id_seq = create_label_sequence(psprintf("edge_label_id_seq_%d", graph_id), get_namespace_name(namespace));
    char *edge_meta_tbl = psprintf("np_edge_label_%d", graph_id);
    Oid edge_label = create_label_metadata_table(edge_meta_tbl);
    create_metadata_btree_index(edge_meta_tbl);
    create_metadata_gist_index(edge_meta_tbl);
    create_default_elabel(graph_id, edge_id_seq, namespace);
 

    insert_graph(PG_GETARG_NAME(0), namespace, graph_id, vertex_label, vertex_id_seq, edge_label, edge_id_seq);

    ereport(NOTICE, (errmsg("graph \"%s\" has been created", graph_name)));

    PG_RETURN_VOID();
}

// INSERT INTO postgraph.np_graph VALUES (id, graph_name, namespace, vertex_id_seq)
void insert_graph(const Name graph_name, const Oid namespace, int graph_id, Oid vertex_label, Oid vertex_id_seq, Oid edge_label, Oid edge_id_seq)
{
    Relation rel = table_open(np_graph_relation_id(), RowExclusiveLock);

    Datum values[7] = {
        Int32GetDatum(graph_id),
        NameGetDatum(graph_name),
        ObjectIdGetDatum(namespace),
        ObjectIdGetDatum(vertex_label),
        ObjectIdGetDatum(vertex_id_seq),
        ObjectIdGetDatum(edge_label),
        ObjectIdGetDatum(edge_id_seq)
    };
    bool nulls[7] = { false, false, false, false, false, false };

    CatalogTupleInsert(rel, heap_form_tuple(RelationGetDescr(rel), values, nulls));

    table_close(rel, RowExclusiveLock);

    CommandCounterIncrement();
}
