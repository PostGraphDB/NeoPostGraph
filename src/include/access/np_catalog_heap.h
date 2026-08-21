/*
 * PostGraph
 * Copyright (C) 2026 by PostGraph
 *
 * Heap-shaped catalog AM: 64-bit xids, no vacuum.
 */

#ifndef NP_CATALOG_HEAP_H
#define NP_CATALOG_HEAP_H

#include "postgres.h"
#include "access/htup.h"
#include "access/transam.h"
#include "storage/itemptr.h"

/*
 * On-disk item: FullTransactionId visibility prefix, then a standard
 * HeapTupleHeader + user data so heap_getattr / systable_getnext work.
 */
typedef struct NPCatalogItemHeaderData
{
	FullTransactionId xmin;
	FullTransactionId xmax;
	CommandId		cmin;
	CommandId		cmax;
	ItemPointerData prev_itemptr;
	uint16			flags;
	char			heap_tuple[FLEXIBLE_ARRAY_MEMBER];
} NPCatalogItemHeaderData;

typedef NPCatalogItemHeaderData *NPCatalogItemHeader;

#define SizeOfNPCatalogItemHeader \
	MAXALIGN(offsetof(NPCatalogItemHeaderData, heap_tuple))

#endif /* NP_CATALOG_HEAP_H */
