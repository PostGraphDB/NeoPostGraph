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

#ifndef NP_CATALOG_H
#define NP_CATALOG_H

#include "postgres.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "catalog/namespace.h"
#include "executor/tuptable.h"
#include "utils/rel.h"

Oid np_namespace_id(void);

Oid np_relation_id(const char *name, const char *kind);

Oid public_catalog_namespace_id(void);
Oid neopostgraph_catalog_namespace_id(void);

Constraint *build_not_null_constraint(void);
Constraint *build_unique_constraint(void);

void np_catalog_insert(Relation rel, HeapTuple tup);
void np_catalog_delete(Relation rel, ItemPointer tid);
HeapTuple np_catalog_slot_getnext(TableScanDesc scan, TupleTableSlot *slot);

#endif