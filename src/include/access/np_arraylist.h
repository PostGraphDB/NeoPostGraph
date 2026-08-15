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

#ifndef NP_ARRAY_LIST_AM_H
#define NP_ARRAY_LIST_AM_H

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

#include "utils/array.h"
#include "utils/edge.h"
#include "utils/vertex.h"

/* The physical representation of an arraylist block on disk */
typedef struct NeoArrayListRecord {
    FullTransactionId xmin;
    FullTransactionId xmax;
    CommandId cmin;
    CommandId cmax;
    uint16 flags;
    
    int64 owner_id;
    Oid prev_tbl;
    ItemPointerData prev_itemptr;
    ItemPointerData next_itemptr;
    
    /* Variable length adjacency list payload follows this header */
    char adj_list_data[FLEXIBLE_ARRAY_MEMBER]; 
} NeoArrayListRecord;

#define SizeOfNeoArrayListRecord offsetof(NeoArrayListRecord, adj_list_data)

#endif 