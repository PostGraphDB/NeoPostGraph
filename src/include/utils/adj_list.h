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

#ifndef NP_ADJ_LIST_H
#define NP_ADJ_LIST_H

#include "utils/gtype.h"
#include "utils/vertex.h"
#include "utils/edge.h"
#include "catalog/np_catalog.h"

#define DATUM_GET_ADJ_LIST(d) ((AdjList *)PG_DETOAST_DATUM(d))
#define ADJ_LIST_GET_DATUM(p) PointerGetDatum(p)
#define NP_GET_ARG_ADJ_LIST(x) DATUM_GET_ADJ_LIST(PG_GETARG_DATUM(x))
#define NP_RETURN_ADJ_LIST(x) PG_RETURN_POINTER(x)


typedef struct AdjListMember
{
    int64 edge_id;
    int32 edge_lid;
    int64 other_id;
    int32 other_lid;
    uint8 dir;

    FullTransactionId xmin;
    FullTransactionId xmax;
    CommandId cmin;
    CommandId cmax;

    uint8 flags; 
    uint8 padding[2];
} AdjListMember;

typedef struct AdjList
{
    int32 vl_len_;
    int32 nitems;
    int32 maxitems;
    AdjListMember data[FLEXIBLE_ARRAY_MEMBER];
} AdjList;


#define ADJLISTOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("adj_list"), ObjectIdGetDatum(np_namespace_id())))

#define ADJLISTARRAYOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("_adj_list"), ObjectIdGetDatum(np_namespace_id())))


#endif
