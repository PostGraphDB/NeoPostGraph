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

#include "catalog/np_catalog.h"

#include "ltree.h"

extern Datum ltree_out(PG_FUNCTION_ARGS); 

#define CATALOG_LTREE_ROOT_LABEL "_"

Oid create_default_vlabel(int graph_id, Oid vertex_id_seq, Oid namespace);
Oid create_default_elabel(int graph_id, Oid vertex_id_seq, Oid namespace);
void create_vlabel_from_array(int graph_id, ArrayType *labels, Oid vertex_id_seq);
Oid create_label_metadata_table(char *meta_tbl_name);
void create_metadata_btree_index(char *tbl_name);
void create_metadata_gist_index(char *tbl_name);

Oid create_label_sequence(char *seq_name, char *namespace);

Oid create_vertex_tables(int graphid, int vertex_id_seq, Oid namespace);

#endif
