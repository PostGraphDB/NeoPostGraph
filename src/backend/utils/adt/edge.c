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
#include "utuils/edge.h"
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

    memcpy(&e->props, &gt->root, VARSIZE(gt));

    SET_VARSIZE(e, VARSIZE(gt) + VARHDRSZ + sizeof(uint64) + (3 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_EDGE(e);
}

PG_FUNCTION_INFO_V1(edge_build);
Datum edge_build(PG_FUNCTION_ARGS) {
    gtype *gt = NP_GET_ARG_GTYPE_P(6);
    vertex *start_vertex = NP_GET_ARG_VERTEX(3);
    vertex *end_vertex = NP_GET_ARG_VERTEX(4);


    edge *e = palloc(sizeof(edge) + VARSIZE(gt));

    e->id = PG_GETARG_INT64(0);
    e->graph_id = PG_GETARG_INT32(1);
    e->label_id = PG_GETARG_INT32(2);
    e->dictionary_id = PG_GETARG_INT16(5);

    e->start_id = start_vertex->id;
    e->start_label = start_vertex->label;
    e->end_id = end_vertex->id;
    e->end_label = end_vertex->label;

    memcpy(&e->props, &gt->root, VARSIZE(gt));

    SET_VARSIZE(e, VARSIZE(gt) + VARHDRSZ + sizeof(uint64) + (3 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_EDGE(e);
}

PG_FUNCTION_INFO_V1(ltree_out);

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

    appendStringInfoString(buffer, ") \"edge_vertex\": (");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->end_label))));
   appendStringInfoString(buffer, ",");
   appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->end_id))));


    // label
    appendStringInfoString(buffer, "), \"label\": \"");
    if (v->graph_id != 0 && v->label_id != 0) {
        label_cache_data *cache = search_edge_label_graph_id_label_id_cache(v->graph_id, v->label_id);
        appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(cache->label)) + 2));
    }

    // properties
    appendStringInfoString(buffer, "\", \"properties\": ");

    if (e->dictionary_id != 0 && show_dictionary_keys) {
        appendStringInfoString(buffer, "{");

        const edge_dictionary_cache_data *dictionary_cache =
            search_vertex_dictionary_cache(e->graph_id, e->label_id, e->dictionary_id);

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
        gtype_to_cstring(buffer, &e->props, 0);
    }
    appendStringInfoString(buffer, "}");

    PG_RETURN_CSTRING(buffer->data);
}
/*
PG_FUNCTION_INFO_V1(edge_set_dictionary);
Datum edge_set_dictionary(PG_FUNCTION_ARGS) {
    edge *e = NP_GET_ARG_EDGE(0);
    int dictionary_id = PG_GETARG_INT32(1);

    const edge_dictionary_cache_data *dictionary_cache =
        search_edge_dictionary_cache(v->graph_id, v->label_id, dictionary_id);

    if (!dictionary_cache) NP_RETURN_EDGE(v);

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

    edge *return_e = palloc(sizeof(edge) + VARSIZE(props));

    return_e->id = e->id;
    return_e->graph_id = e->graph_id;
    return_e->label_id = e->label_id;
    return_e->dictionary_id = dictionary_id;

    memcpy(&return_e->props, &props->root, VARSIZE(props));

    SET_VARSIZE(return_v, VARSIZE(props) + VARHDRSZ + sizeof(uint64) + (3 * sizeof(uint32)) + sizeof(uint16));

    NP_RETURN_VERTEX(return_v);
}
*/

/*
PG_FUNCTION_INFO_V1(vertex_insert);
Datum vertex_set_dictionary(PG_FUNCTION_ARGS) {
    const vertex_dictionary_cache_data *dictionary_cache =
        search_vertex_dictionary_cache(v->graph_id, v->label_id, dictionary_id);



    PG_RETURN_VOID();
}
*/
