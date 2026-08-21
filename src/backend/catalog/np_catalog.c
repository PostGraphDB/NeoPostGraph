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

#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "catalog/namespace.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "catalog/np_catalog.h"
#include "catalog/np_graph.h"


Oid np_namespace_id(void)
{
    return get_namespace_oid("neopostgraph", false);
}

Oid np_relation_id(const char *name, const char *kind)
{
    Oid id;

    if (!OidIsValid(id = get_relname_relid(name, np_namespace_id())))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                        errmsg("%s \"%s\" does not exist", kind, name)));

    return id;
}

Oid public_catalog_namespace_id(void)
{
    return get_namespace_oid("public", false);
}

Oid neopostgraph_catalog_namespace_id(void)
{
    return get_namespace_oid("neopostgraph", false);
}

// NOT NULL
Constraint *build_not_null_constraint(void)
{
    Constraint *not_null;

    not_null = makeNode(Constraint);
    not_null->contype = CONSTR_NOTNULL;
    not_null->location = -1;

    return not_null;
}

Constraint *build_unique_constraint(void)
{
    Constraint *not_null;

    not_null = makeNode(Constraint);
    not_null->contype = CONSTR_UNIQUE;
    not_null->location = -1;

    return not_null;
}

void
np_catalog_insert(Relation rel, HeapTuple tup)
{
	TupleTableSlot *slot;
	List	   *indexoidlist;
	ListCell   *lc;

	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsHeapTuple);
	ExecStoreHeapTuple(tup, slot, false);
	table_tuple_insert(rel, slot, GetCurrentCommandId(true), 0, NULL);

	indexoidlist = RelationGetIndexList(rel);
	foreach(lc, indexoidlist)
	{
		Oid			index_oid = lfirst_oid(lc);
		Relation	indexDesc = index_open(index_oid, RowExclusiveLock);
		IndexInfo  *indexInfo = BuildIndexInfo(indexDesc);
		Datum		idx_values[INDEX_MAX_KEYS];
		bool		idx_nulls[INDEX_MAX_KEYS];

		FormIndexDatum(indexInfo, slot, NULL, idx_values, idx_nulls);
		index_insert(indexDesc, idx_values, idx_nulls,
					 &slot->tts_tid, rel,
					 indexDesc->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
					 false, indexInfo);
		index_close(indexDesc, RowExclusiveLock);
	}
	list_free(indexoidlist);
	ExecDropSingleTupleTableSlot(slot);
}

HeapTuple
np_catalog_slot_getnext(TableScanDesc scan, TupleTableSlot *slot)
{
	if (!table_scan_getnextslot(scan, ForwardScanDirection, slot))
		return NULL;
	return ExecFetchSlotHeapTuple(slot, false, NULL);
}

void
np_catalog_delete(Relation rel, ItemPointer tid)
{
	TM_FailureData tmfd;
	TM_Result	result;

	result = table_tuple_delete(rel, tid, GetCurrentCommandId(true),
								GetActiveSnapshot(), InvalidSnapshot,
								true, &tmfd, false);
	if (result != TM_Ok)
		elog(ERROR, "NeoPostGraph: failed to delete catalog tuple");
}

