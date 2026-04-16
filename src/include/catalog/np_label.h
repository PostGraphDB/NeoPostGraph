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

#define CATALOG_LTREE_ROOT_LABEL "_"

#define np_vertex_label_relation_id() np_relation_id("np_vertex_label", "table")
#define np_vertex_label_graph_id_label_id() np_relation_id("np_vertex_label_graph_id_label", "index")

void create_default_vlabel(int graph_id, Oid vertex_id_seq);
void create_vlabel_from_array(int graph_id, ArrayType *labels, Oid vertex_id_seq);

Oid create_vlabel_sequence(int graph_id, char *namespace);

#endif