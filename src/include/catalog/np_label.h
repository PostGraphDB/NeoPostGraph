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

#ifndef NP_LABEL_H
#define NP_LABEL_H

#include "postgres.h"

#include "utils/array.h"

#include "access/np_phys_map.h"
#include "catalog/np_catalog.h"

#include "ltree.h"

extern Datum ltree_out(PG_FUNCTION_ARGS); 

#define CATALOG_LTREE_ROOT_LABEL "_"

#define LTREEOID \
(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("ltree"), ObjectIdGetDatum(public_catalog_namespace_id())))

#define InvalidLabelId -1

void insert_vertex_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map, Oid arraylist, Oid ll_seq, Oid ll_meta, Oid annotations_tbl, Datum annotation_map);
int insert_label(char *table_name, Datum label, Oid label_id, Oid tbl, Oid phys_map);
int insert_vertex_ll_meta(char *table_name, Oid namespace, int ll_seq, Oid tbl);

void insert_annotation_schema(int32 graph_id, int32 label_id, ArrayType *annot_array, Oid schema_tbl, Oid schema_pmap);
void
enforce_and_insert_label_catalog(Relation cat_rel, Relation idx_rel, char *label_name, char expected_type);
Oid create_label_edge_physical_mapping_table(char *tbl_name, Oid namespace);
void np_catalog_update(Relation rel, HeapTuple old_tup, HeapTuple new_tup);

#endif
