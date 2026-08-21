/*
 * PostGraph
 * Copyright (C) 2026 by PostGraph
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef DML_INSERT_H
#define DML_INSERT_H

#include "postgres.h"
#include "fmgr.h"
#include "storage/itemptr.h"
#include "utils/array.h"

#include "utils/vertex.h"
#include "utils/edge.h"


/* =====================================================================
 * INTERNAL C API
 * ===================================================================== */

/*
 * Inserts a vertex payload into the heap, processes its annotations, 
 * and writes the routing record to the structural phys_map.
 */
void np_internal_insert_vertex(vertex *v, gtype *props, ArrayType *input_annots, ItemPointerData *forwarded_a_itemptr);

/*
 * Inserts an edge payload into the heap, updates the edge phys_map,
 * and handles all doubly-linked adjacency list mutations for both endpoints.
 */
void np_internal_insert_edge(vertex *start_v, vertex *end_v, edge *e, gtype *props);

ItemPointerData get_current_head_tid(Relation pmap_rel, uint64 vertex_id, Oid *head_tbl);

/* =====================================================================
 * SQL API WRAPPERS
 * ===================================================================== */

Datum insert_vertex(PG_FUNCTION_ARGS);
Datum insert_edge(PG_FUNCTION_ARGS);



#endif /* DML_INSERT_H */