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

#ifndef NP_VERTEX_H
#define NP_VERTEX_H

#include "utils/gtype.h"
#include "catalog/np_catalog.h"

#define DATUM_GET_VERTEX(d) ((vertex *)PG_DETOAST_DATUM(d))
#define VERTEX_GET_DATUM(p) PointerGetDatum(p)
#define NP_GET_ARG_VERTEX(x) DATUM_GET_VERTEX(PG_GETARG_DATUM(x))
#define NP_RETURN_VERTEX(x) PG_RETURN_POINTER(x)



typedef struct {
    int32 vl_len_;
    int64 id;
    int32 graph_id;
    int32 label_id;
    int16 dictionary_id;
    gtype_container props;
} vertex;

extern bool show_dictionary_keys;
extern bool show_dictionary_nulls;

void assign_show_dictionary_keys(bool newval, void *extra);
void assign_show_dictionary_nulls(bool newval, void *extra);

vertex *
build_vertex_internal(int64 id, int32 graph_id, int32 label_id, int16 dictionary_id, gtype *gt);


#define VERTEXOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("vertex"), ObjectIdGetDatum(np_namespace_id())))

#define VERTEXARRAYOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("_vertex"), ObjectIdGetDatum(np_namespace_id())))


#endif
