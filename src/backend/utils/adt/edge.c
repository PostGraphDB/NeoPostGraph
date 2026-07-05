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

#include "access/genam.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "utils/np_cache.h"
#include "catalog/np_label.h"
#include "utils/gtype.h"
#include "utils/dictionary.h"
#include "utils/edge.h"
#include "utils/vertex.h"


PG_FUNCTION_INFO_V1(edge_in);
Datum edge_in(PG_FUNCTION_ARGS) {
    char *str = PG_GETARG_CSTRING(0);
    gtype_value *val = gtype_value_from_cstring(str, strlen(str));

    if (val->type != GTV_OBJECT)
        ereport(ERROR, errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("invalid format for properties, expects object"));

    gtype *gt = gtype_value_to_gtype(val);

    edge *e = palloc(sizeof(edge) + VARSIZE(gt));
    e->id = 0;
    e->dictionary_id = 0;
    e->label_id = 0;
    e->graph_id = 0;

    e->start_id = 0;
    e->start_label = 0;
    e->end_id = 0;
    e->end_label = 0;

    memcpy(&e->props, &gt->root, VARSIZE(gt));

    SET_VARSIZE(e, VARSIZE(gt) + VARHDRSZ + (3 * sizeof(uint64)) + (6 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_EDGE(e);
}

PG_FUNCTION_INFO_V1(edge_build);
Datum edge_build(PG_FUNCTION_ARGS) {
    gtype *gt = NP_GET_ARG_GTYPE_P(6);
    vertex *start_vertex = NP_GET_ARG_VERTEX(4);
    vertex *end_vertex = NP_GET_ARG_VERTEX(5);

    edge *e = palloc(sizeof(edge) + VARSIZE(gt));

    e->id = PG_GETARG_INT64(0);
    e->graph_id = PG_GETARG_INT32(1);
    e->label_id = PG_GETARG_INT32(2);
    e->dictionary_id = PG_GETARG_INT16(3);

    e->start_id = start_vertex->id;
    e->start_label = start_vertex->label_id;
    e->end_id = end_vertex->id;
    e->end_label = end_vertex->label_id;

    memcpy(&e->props, &gt->root, VARSIZE(gt));

    SET_VARSIZE(e, VARSIZE(gt) + VARHDRSZ + (3 * sizeof(uint64)) + (6 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_EDGE(e);
}


PG_FUNCTION_INFO_V1(edge_out);
Datum edge_out(PG_FUNCTION_ARGS) {
    edge *e = NP_GET_ARG_EDGE(0);
    StringInfoData *buffer = makeStringInfo();

    // id
    appendStringInfoString(buffer, "{\"id\": ");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->id))));

    // start id
    appendStringInfoString(buffer, ", \"start_vertex\": (");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int4out, Int64GetDatum(e->start_label))));
     appendStringInfoString(buffer, ",");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->start_id))));

    appendStringInfoString(buffer, "), \"edge_vertex\": (");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->end_label))));
    appendStringInfoString(buffer, ",");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->end_id))));


    // label
    appendStringInfoString(buffer, "), \"label\": \"");
    if (e->graph_id != 0 && e->label_id != 0) {
        label_cache_data *cache = search_edge_label_graph_id_label_id_cache(e->graph_id, e->label_id);
        appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(cache->label)) + 2));
    }

    // properties
    appendStringInfoString(buffer, "\", \"properties\": ");
    gtype_to_cstring(buffer, &e->props, 0);
    appendStringInfoString(buffer, "}");

    PG_RETURN_CSTRING(buffer->data);
}
