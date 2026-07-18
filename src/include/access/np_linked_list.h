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

#ifndef NP_LINKED_LIST_AM_H
#define NP_LINKED_LIST_AM_H

#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "storage/itemptr.h"
#include "utils/rel.h"
#include "executor/tuptable.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"

extern const TableAmRoutine np_linked_list_methods;


typedef struct __attribute__((packed)) NeoLinkedListRecord
{
    FullTransactionId xmin;           
    FullTransactionId xmax;
    CommandId       cmin;
    CommandId       cmax;
    int64 id;
    int32 edge_lid;
    char dir;
    int64 owner_id;
    int64 other_id;
    int32 other_lid;
    Oid next_tbl;
    ItemPointerData next_itemptr;
    Oid prev_tbl;
    ItemPointerData prev_itemptr;
} NeoLinkedListRecord;

// The lightweight in-memory wrapper that points to your raw disk bytes
typedef struct NeoTupleData
{
    uint32          t_len;      // Total byte length of the record
    ItemPointerData t_self;     // The physical Block/Offset on disk
    Oid             t_tableOid; // The OID of the table
    void           *t_data;     // Pointer directly to the raw disk record (e.g., NeoLinkedListRecord)
} NeoTupleData;

typedef NeoTupleData *NeoTuple;

// Your custom Table Slot
typedef struct NeoTupleTableSlot
{
    TupleTableSlot  base;       // MUST be first! This allows PG to cast it back and forth
    Buffer          buffer;     // Tracks the pinned 8KB disk buffer
    NeoTuple        tuple;      // Pointer to your custom in-memory wrapper
} NeoTupleTableSlot;

TM_Result np_linked_list_update_inplace(Relation relation, const ItemPointerData *otid, HeapTuple newtup, CommandId cid);

#endif // NP_LINKED_LIST_AM_H