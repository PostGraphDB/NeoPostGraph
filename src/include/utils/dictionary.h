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


#ifndef NP_DICTIONARY_H
#define NP_DICTIONARY_H

#include "gtype.h"
#include "catalog/np_catalog.h"

#define DATUM_GET_DICTIONARY(d) ((dictionary *)PG_DETOAST_DATUM(d))
#define DICTIONARY_GET_DATUM(p) PointerGetDatum(p)
#define NP_GET_ARG_DICTIONARY(x) DATUM_GET_DICTIONARY(PG_GETARG_DATUM(x))
#define NP_RETURN_DICTIONARY(x) PG_RETURN_POINTER(x)

#define DICTIONARYOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("dictionary"), ObjectIdGetDatum(neopostgraph_catalog_namespace_id())))

typedef struct {
    uint32 vl_len_;
    uint64 dictionary_id;
    gtype_container array;
} dictionary;

Oid create_vertex_property_dictionary(int graph_id, int label_id);
void create_vertex_dictionary_metadata_btree_index(int graph_id, int label_id, int dictionary_id);
#endif