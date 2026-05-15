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

#include "access/genam.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "utils/np_cache.h"
#include "utils/gtype.h"
#include "utils/dictionary.h"
#include "utils/vertex.h"

PG_FUNCTION_INFO_V1(vertex_in);
Datum vertex_in(PG_FUNCTION_ARGS) {
    char *str = PG_GETARG_CSTRING(0);
    gtype_value *val = gtype_value_from_cstring(str, strlen(str));

    if (val->type != GTV_OBJECT)
        ereport(ERROR, errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("invalid format for properties, expects object"));

    gtype *gt = gtype_value_to_gtype(val);

    vertex *v = palloc(sizeof(vertex) + VARSIZE(gt));
    v->id = 0;
    v->dictionary_id = 0;
    v->label_id = 0;
    v->graph_id = 0;

    memcpy(&v->props, &gt->root, VARSIZE(gt));

    SET_VARSIZE(v, VARSIZE(gt) + VARHDRSZ + sizeof(uint64) + (3 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_VERTEX(v);
}

PG_FUNCTION_INFO_V1(vertex_build);
Datum vertex_build(PG_FUNCTION_ARGS) {
    gtype *gt = NP_GET_ARG_GTYPE_P(4);

    vertex *v = palloc(sizeof(vertex) + VARSIZE(gt));

    v->id = PG_GETARG_INT64(0);
    v->graph_id = PG_GETARG_INT32(1);
    v->label_id = PG_GETARG_INT32(2);
    v->dictionary_id = PG_GETARG_INT16(3);

    memcpy(&v->props, &gt->root, VARSIZE(gt));

    SET_VARSIZE(v, VARSIZE(gt) + VARHDRSZ + sizeof(uint64) + (3 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_VERTEX(v);
}

PG_FUNCTION_INFO_V1(ltree_out);

PG_FUNCTION_INFO_V1(vertex_out);
Datum vertex_out(PG_FUNCTION_ARGS) {
    vertex *v = NP_GET_ARG_VERTEX(0);
    StringInfoData *buffer = makeStringInfo();

    // id
    appendStringInfoString(buffer, "{\"id\": ");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(v->id))));

    // label
    appendStringInfoString(buffer, ", \"label\": \"");
    if (v->graph_id != 0 && v->label_id != 0) {
        vertex_label_cache_data *cache = search_vertex_label_graph_id_label_id_cache(v->graph_id, v->label_id);
        appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(cache->label)) + 2));
    }

    // properties
    appendStringInfoString(buffer, "\", \"properties\": ");
    gtype_to_cstring(buffer, &v->props, 0);
    appendStringInfoString(buffer, "}");
}
