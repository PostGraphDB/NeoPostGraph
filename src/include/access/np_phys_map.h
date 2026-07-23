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

#ifndef NP_MUTABLE_AM_H
#define NP_MUTABLE_AM_H

#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "storage/itemptr.h"
#include "utils/rel.h"
#include "executor/tuptable.h"

typedef struct __attribute__((packed)) NeoPhysMapRecord
{
    ItemPointerData v_itemptr;
    Oid e_tbl_id;
    ItemPointerData e_itemptr;
} NeoPhysMapRecord;

typedef struct  __attribute__((packed)) NeoEdgePhysMapRecord {
    ItemPointerData e_itemptr;
} NeoEdgePhysMapRecord;

void np_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid);
void np_id_to_tid(uint64 id, uint32 tuples_per_page, ItemPointerData *tid);
uint32 np_calculate_tuples_per_page(Size payload_size);
void update_vertex_phys_map(Relation pmap_rel, uint64 vertex_id, Oid new_edge_table_oid, ItemPointer new_edge_tid, CommandId cid);
ItemPointerData get_phys_map_vpointer(Relation pmap_rel, ItemPointer pmap_tid);
void np_overwrite_physmap_in_page(Relation rel, ItemPointer tid, NeoPhysMapRecord *new_data);
void np_set_edge_physmap_record(Relation rel, ItemPointer tid, NeoEdgePhysMapRecord *new_data);

#endif // NP_MUTABLE_AM_H