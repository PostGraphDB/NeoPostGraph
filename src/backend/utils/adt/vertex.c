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
#include "utils/gtype.h"
#include "utils/dictionary.h"
#include "utils/vertex.h"


bool show_dictionary_keys = true;
bool show_dictionary_nulls = false;

void
assign_show_dictionary_keys(bool newval, void *extra)
{
    show_dictionary_keys = newval;
    ereport(LOG, errmsg("neopostgraph: show_dictionary_keys = %s", newval ? "true" : "false"));
}

void
assign_show_dictionary_nulls(bool newval, void *extra)
{
    show_dictionary_nulls = newval;
    ereport(LOG, errmsg("neopostgraph: show_dictionary_nulls = %s", newval ? "true" : "false"));
}

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

    if (v->dictionary_id != 0 && show_dictionary_keys) {
        appendStringInfoString(buffer, "{");

        const vertex_dictionary_cache_data *dictionary_cache =
            search_vertex_dictionary_cache(v->graph_id, v->label_id, v->dictionary_id);

        gtype_iterator *it = gtype_iterator_init(&v->props);
        gtype_iterator *dict_it = gtype_iterator_init(&dictionary_cache->dict->array);
        gtype_value gtv;
        gtype_value dict_gtv;
        gtype_iterator_next(&it, &gtv, true);
        gtype_iterator_next(&dict_it, &dict_gtv, true);

        bool first_run = true;
        while (gtype_iterator_next(&dict_it, &dict_gtv, true) < WGT_END_ARRAY) {
            gtype_iterator_next(&it, &gtv, true);

            if (gtv.type == GTV_NULL && !show_dictionary_nulls)
                continue;

            if (!first_run)
                appendStringInfoString(buffer, ", ");
            else
                first_run = false;

            appendStringInfoString(buffer, dict_gtv.val.string.val);
            appendStringInfoString(buffer, ": ");

            if (gtv.type == GTV_BINARY)
                gtype_to_cstring(buffer, gtv.val.binary.data, gtv.val.binary.len);
            else
                gtype_put_escaped_value(buffer, &gtv);
        }

        if (gtype_iterator_next(&it, &gtv, true) != WGT_END_OBJECT && gtv.type != GTV_NULL) {

            gtype_iterator *extra_it = gtype_iterator_init(gtv.val.binary.data);
            gtype_iterator_next(&extra_it, &gtv, true);

            while (gtype_iterator_next(&extra_it, &gtv, true) < WGT_END_OBJECT) {
                if (!first_run)
                    appendStringInfoString(buffer, ", ");
                else
                    first_run = false;

                appendStringInfoString(buffer, gtv.val.string.val);
                appendStringInfoString(buffer, ": ");

                gtype_iterator_next(&extra_it, &gtv, true);

                if (gtv.type == GTV_BINARY)
                    gtype_to_cstring(buffer, gtv.val.binary.data, gtv.val.binary.len);
                else
                    gtype_put_escaped_value(buffer, &gtv);
            }
        }
        appendStringInfoString(buffer, "}");
    } else {
        gtype_to_cstring(buffer, &v->props, 0);
    }
    appendStringInfoString(buffer, "}");

    PG_RETURN_CSTRING(buffer->data);
}

PG_FUNCTION_INFO_V1(vertex_set_dictionary);
Datum vertex_set_dictionary(PG_FUNCTION_ARGS) {
    vertex *v = NP_GET_ARG_VERTEX(0);
    int dictionary_id = PG_GETARG_INT32(1);

    const vertex_dictionary_cache_data *dictionary_cache =
        search_vertex_dictionary_cache(v->graph_id, v->label_id, dictionary_id);

    if (!dictionary_cache) NP_RETURN_VERTEX(v);

    if (v->dictionary_id != 0)
        ereport(ERROR, errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("Cannot Set Vertex to have a dictionary if it already has a dictionary"));

    gtype_iterator *it = gtype_iterator_init(&v->props);
    gtype_iterator *dict_it = gtype_iterator_init(&dictionary_cache->dict->array);
    gtype_value gtv;
    gtype_value dict_gtv;
    gtype_iterator_token gtoken = gtype_iterator_next(&it, &gtv, true);
    gtype_iterator_token array_gtoken = gtype_iterator_next(&dict_it, &dict_gtv, true);
    Assert(gtoken == WGT_BEGIN_OBJECT && array_gtoken == WGT_BEGIN_ARRAY);

    gtype_in_state result;
    memset(&result, 0, sizeof(gtype_in_state));

    result.res = push_gtype_value(&result.parse_state, WGT_BEGIN_ARRAY, NULL);

    gtoken = gtype_iterator_next(&it, &gtv, true);
    bool extra_props = false;

    while ((array_gtoken = gtype_iterator_next(&dict_it, &dict_gtv, true)) < WGT_END_ARRAY &&
        gtoken != WGT_END_OBJECT) {
next_key:

        int cmp;
        if (( cmp = pg_strcasecmp(dict_gtv.val.string.val, gtv.val.string.val)) == 0) {
            gtoken = gtype_iterator_next(&it, &gtv, true);
            Assert(gtoken == WGT_VALUE);
            result.res = push_gtype_value(&result.parse_state, WGT_ELEM, &gtv);
            gtoken = gtype_iterator_next(&it, &gtv, true);
        } else if (cmp > 0) {
            gtoken = gtype_iterator_next(&it, &gtv, true);
            gtoken = gtype_iterator_next(&it, &gtv, true);
            extra_props = true;
            if (gtoken != WGT_END_OBJECT)
                goto next_key;
        } else {
            gtype_value gtv_null = { .type = GTV_NULL };
            result.res = push_gtype_value(&result.parse_state, WGT_ELEM, &gtv_null);
        }
    }

    if (gtoken != WGT_END_OBJECT)
        extra_props = true;


    if (extra_props) {
        it = gtype_iterator_init(&v->props);
        dict_it = gtype_iterator_init(&dictionary_cache->dict->array);
        gtoken = gtype_iterator_next(&it, &gtv, true);
        array_gtoken = gtype_iterator_next(&dict_it, &dict_gtv, true);

        result.res = push_gtype_value(&result.parse_state, WGT_BEGIN_OBJECT, NULL);

        gtoken = gtype_iterator_next(&it, &gtv, true);


        while ((array_gtoken = gtype_iterator_next(&dict_it, &dict_gtv, true)) < WGT_END_ARRAY && gtoken != WGT_END_OBJECT) {
next_key_extra:

            int cmp;
            if (gtv.type == GTV_NULL || ( cmp = pg_strcasecmp(dict_gtv.val.string.val, gtv.val.string.val)) == 0) {
                gtoken = gtype_iterator_next(&it, &gtv, true);
                gtoken = gtype_iterator_next(&it, &gtv, true);
            } else if (cmp > 0) {
                result.res = push_gtype_value(&result.parse_state, gtoken, &gtv);
                gtoken = gtype_iterator_next(&it, &gtv, true);
                result.res = push_gtype_value(&result.parse_state, gtoken, &gtv);
                gtoken = gtype_iterator_next(&it, &gtv, true);
                if (gtoken != WGT_END_OBJECT)
                    goto next_key_extra;
            }
        }

        while (gtoken != WGT_END_OBJECT) {
            result.res = push_gtype_value(&result.parse_state, gtoken, &gtv);
            gtoken = gtype_iterator_next(&it, &gtv, true);
        }

        result.res = push_gtype_value(&result.parse_state, WGT_END_OBJECT, NULL);
    } else {
        gtype_value gtv_null = { .type = GTV_NULL };
        result.res = push_gtype_value(&result.parse_state, WGT_ELEM, &gtv_null);
    }

    result.res = push_gtype_value(&result.parse_state, WGT_END_ARRAY, NULL);
    gtype *props = gtype_value_to_gtype(result.res);

    vertex *return_v = palloc(sizeof(vertex) + VARSIZE(props));

    return_v->id = v->id;
    return_v->graph_id = v->graph_id;
    return_v->label_id = v->label_id;
    return_v->dictionary_id = dictionary_id;

    memcpy(&return_v->props, &props->root, VARSIZE(props));

    SET_VARSIZE(return_v, VARSIZE(props) + VARHDRSZ + sizeof(uint64) + (3 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_VERTEX(return_v);
}
