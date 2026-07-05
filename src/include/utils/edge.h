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

#ifndef NP_EDGE_H
#define NP_EDGE_H

#include "utils/gtype.h"
#include "utils/vertex.h"
#include "catalog/np_catalog.h"

#define DATUM_GET_EDGE(d) ((edge *)PG_DETOAST_DATUM(d))
#define EDGE_GET_DATUM(p) PointerGetDatum(p)
#define NP_GET_ARG_EDGE(x) DATUM_GET_EDGE(PG_GETARG_DATUM(x))
#define NP_RETURN_EDGE(x) PG_RETURN_POINTER(x)



typedef struct {
    int32 vl_len_;
    int64 id;
    int32 graph_id;
    int32 label_id;
    int16 dictionary_id;
    int32 start_label;
    int64 start_id;
    int32 end_id;
    int64 end_label;
    gtype_container props;
} edge;

#define EDGEOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("edge"), ObjectIdGetDatum(np_namespace_id())))

#define EDGEARRAYOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("_edge"), ObjectIdGetDatum(np_namespace_id())))


#endif
