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

#ifndef NP_MAP_AM_H
#define NP_MAP_AM_H

#include "postgres.h"
#include "access/tableam.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "commands/vacuum.h"

extern const TableAmRoutine np_methods;

void np_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid);
void np_heap_insert(Relation relation, HeapTuple tup, CommandId cid, int options, BulkInsertState bistate);
void np_id_to_tid(uint64 id, uint32 tuples_per_page, ItemPointerData *tid);
uint32 np_calculate_tuples_per_page(Size payload_size);

#endif