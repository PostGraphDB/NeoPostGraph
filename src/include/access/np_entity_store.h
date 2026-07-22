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

#ifndef NP_ENTITY_STORE_H
#define NP_ENTITY_STORE_H

#include "postgres.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "storage/itemptr.h"

//extern Datum np_entity_store_handler(PG_FUNCTION_ARGS);

typedef struct NPEntityTupleHeaderData {
    FullTransactionId xmin;
    FullTransactionId xmax;
    CommandId cmin;
    CommandId cmax;
    ItemPointerData prev_itemptr;
    uint16 flags;
    uint64 id;
    char serialized_entity[];
} NPEntityTupleHeaderData;

typedef NPEntityTupleHeaderData *NPEntityTupleHeader;


typedef struct NPEntityScanDescData
{
    TableScanDescData rs_base;
    
    BlockNumber rs_nblocks;
    BlockNumber rs_cblock;
    OffsetNumber rs_coffset;
    Buffer rs_cbuf;
} NPEntityScanDescData;

typedef NPEntityScanDescData *NPEntityScanDesc;

#define SizeOfNPEntityTupleHeader offsetof(NPEntityTupleHeaderData, serialized_entity)

#endif /* NP_ENTITY_STORE_H */