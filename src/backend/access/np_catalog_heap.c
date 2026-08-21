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

/*
 * Catalog table AM modeled on heapam_handler.c: heap tuple payload,
 * FullTransactionId xmin/xmax, vacuum is a no-op so old catalog
 * versions remain for annotation-label history.
 */
#include "postgres.h"
#include "fmgr.h"

#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/tsmapi.h"
#include "access/valid.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#include "access/np_catalog_heap.h"
#include "access/np_entity_store.h"
#include "access/np_phys_map.h"

/*
 * Snapshot visibility for the overlay FullTransactionId header.
 *
 * SNAPSHOT_MVCC matches heap: the current command's inserts are invisible
 * (cmin >= curcid) and the current command's deletes stay visible
 * (cmax >= curcid).  Without that, a seq scan that COW-updates a catalog
 * row sees the new item later on the same page and rewrites it again.
 *
 * SNAPSHOT_ANY still uses the live-tuple predicate: catalogs are never
 * vacuumed, so index builds must not pick up xmax'd history.
 */
static bool
np_catalog_tuple_visible(NPCatalogItemHeader hdr, Snapshot snapshot)
{
	FullTransactionId fxmin = hdr->xmin;
	FullTransactionId fxmax = hdr->xmax;
	TransactionId xmin;
	TransactionId xmax;

	if (snapshot == NULL ||
		snapshot->snapshot_type == SNAPSHOT_ANY ||
		snapshot->snapshot_type == SNAPSHOT_TOAST)
		return np_entity_tuple_is_live((NPEntityTupleHeader) hdr);

	if (snapshot->snapshot_type == SNAPSHOT_SELF ||
		snapshot->snapshot_type == SNAPSHOT_DIRTY)
		return np_entity_tuple_is_live((NPEntityTupleHeader) hdr);

	if (!FullTransactionIdIsValid(fxmin))
		return false;

	xmin = XidFromFullTransactionId(fxmin);

	if (TransactionIdIsCurrentTransactionId(xmin))
	{
		if (hdr->cmin >= snapshot->curcid)
			return false;

		if (FullTransactionIdIsValid(fxmax) &&
			TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(fxmax)))
		{
			if (hdr->cmax >= snapshot->curcid)
				return true;
			return false;
		}
		return true;
	}

	if (XidInMVCCSnapshot(xmin, snapshot))
		return false;
	if (TransactionIdIsInProgress(xmin) || !TransactionIdDidCommit(xmin))
		return false;

	if (!FullTransactionIdIsValid(fxmax))
		return true;

	xmax = XidFromFullTransactionId(fxmax);

	if (TransactionIdIsCurrentTransactionId(xmax))
	{
		if (hdr->cmax >= snapshot->curcid)
			return true;
		return false;
	}

	if (XidInMVCCSnapshot(xmax, snapshot))
		return true;
	if (TransactionIdIsInProgress(xmax) || !TransactionIdDidCommit(xmax))
		return true;

	return false;
}

struct ReadStream;

typedef struct NPCatalogScanDescData
{
	TableScanDescData rs_base;
	BlockNumber		rs_nblocks;
	BlockNumber		rs_startblock;
	BlockNumber		rs_cblock;
	OffsetNumber	rs_coffset;
	Buffer			rs_cbuf;
	bool			rs_inited;
	BufferAccessStrategy rs_strategy;
	uint32			rs_cindex;
	uint32			rs_ntuples;
	OffsetNumber	rs_vistuples[MaxHeapTuplesPerPage];
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;
} NPCatalogScanDescData;

typedef NPCatalogScanDescData *NPCatalogScanDesc;

typedef struct NPCatalogIndexFetchData
{
	IndexFetchTableData xs_base;
	Buffer			xs_cbuf;
} NPCatalogIndexFetchData;

static bool
np_catalog_item_is_fetchable(Page page, OffsetNumber off)
{
	ItemId		lp;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return false;
	lp = PageGetItemId(page, off);
	if (!ItemIdIsNormal(lp))
		return false;
	return ItemIdGetLength(lp) >= SizeOfNPCatalogItemHeader + SizeofHeapTupleHeader;
}

static NPCatalogItemHeader
np_catalog_item_header(Page page, OffsetNumber off)
{
	return (NPCatalogItemHeader) PageGetItem(page, PageGetItemId(page, off));
}

static void
np_catalog_store_heap_tuple(Page page, OffsetNumber off,
							ItemPointer tid, Oid reloid, TupleTableSlot *slot)
{
	ItemId		lp = PageGetItemId(page, off);
	NPCatalogItemHeader hdr = (NPCatalogItemHeader) PageGetItem(page, lp);
	HeapTupleData htup;
	HeapTuple	copy;
	MemoryContext old;

	htup.t_data = (HeapTupleHeader) ((char *) hdr + SizeOfNPCatalogItemHeader);
	htup.t_len = ItemIdGetLength(lp) - SizeOfNPCatalogItemHeader;
	htup.t_self = *tid;
	htup.t_tableOid = reloid;

	old = MemoryContextSwitchTo(slot->tts_mcxt);
	copy = heap_copytuple(&htup);
	MemoryContextSwitchTo(old);
	copy->t_self = *tid;
	copy->t_tableOid = reloid;
	ExecStoreHeapTuple(copy, slot, true);
	slot->tts_tid = *tid;
	slot->tts_tableOid = reloid;
}

static Size
np_catalog_pack_size(HeapTuple tuple)
{
	return SizeOfNPCatalogItemHeader + tuple->t_len;
}

static void
np_catalog_pack_item(char *dest, HeapTuple tuple, FullTransactionId xmin,
					 FullTransactionId xmax, CommandId cmin, CommandId cmax,
					 ItemPointer prev)
{
	NPCatalogItemHeader hdr = (NPCatalogItemHeader) dest;

	hdr->xmin = xmin;
	hdr->xmax = xmax;
	hdr->cmin = cmin;
	hdr->cmax = cmax;
	if (prev)
		hdr->prev_itemptr = *prev;
	else
		ItemPointerSetInvalid(&hdr->prev_itemptr);
	hdr->flags = 0;
	memcpy((char *) hdr + SizeOfNPCatalogItemHeader, tuple->t_data, tuple->t_len);
}

static const TupleTableSlotOps *
np_catalog_slot_callbacks(Relation rel)
{
	return &TTSOpsHeapTuple;
}

static TableScanDesc
np_catalog_scan_begin(Relation rel, Snapshot snapshot, int nkeys,
					  struct ScanKeyData *key, ParallelTableScanDesc pscan,
					  uint32 flags)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) palloc0(sizeof(NPCatalogScanDescData));

	scan->rs_base.rs_rd = rel;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_key = key;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = pscan;
	scan->rs_nblocks = RelationGetNumberOfBlocks(rel);
	scan->rs_startblock = 0;
	scan->rs_cblock = 0;
	scan->rs_coffset = FirstOffsetNumber;
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_inited = false;
	scan->rs_cindex = 0;
	scan->rs_ntuples = 0;
	scan->rs_strategy = NULL;
	scan->rs_parallelworkerdata = NULL;
	if (flags & SO_ALLOW_STRAT)
		scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	if (pscan != NULL)
		scan->rs_parallelworkerdata = palloc(sizeof(ParallelBlockTableScanWorkerData));
	return &scan->rs_base;
}

static void
np_catalog_scan_end(TableScanDesc sscan)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;

	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);
	if (scan->rs_strategy)
		FreeAccessStrategy(scan->rs_strategy);
	if (scan->rs_parallelworkerdata)
		pfree(scan->rs_parallelworkerdata);
	if (sscan->rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(sscan->rs_snapshot);
	pfree(scan);
}

static void
np_catalog_scan_rescan(TableScanDesc sscan, struct ScanKeyData *key,
					   bool set_params, bool allow_strat, bool allow_sync,
					   bool allow_pagemode)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;

	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}
	if (key != NULL)
		scan->rs_base.rs_key = key;
	scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_base.rs_rd);
	scan->rs_startblock = 0;
	scan->rs_cblock = 0;
	scan->rs_coffset = FirstOffsetNumber;
	scan->rs_inited = false;
	scan->rs_cindex = 0;
	scan->rs_ntuples = 0;
}

static bool
np_catalog_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
							TupleTableSlot *slot)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;
	Relation	rel = sscan->rs_rd;

	for (;;)
	{
		if (!BufferIsValid(scan->rs_cbuf))
		{
			if (scan->rs_cblock >= scan->rs_nblocks)
				return false;
			scan->rs_cbuf = ReadBuffer(rel, scan->rs_cblock);
			scan->rs_coffset = FirstOffsetNumber;
		}

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
		{
			Page		page = BufferGetPage(scan->rs_cbuf);
			OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

			while (scan->rs_coffset <= maxoff)
			{
				OffsetNumber off = scan->rs_coffset++;

				if (!np_catalog_item_is_fetchable(page, off))
					continue;

				{
					NPCatalogItemHeader hdr = np_catalog_item_header(page, off);
					ItemPointerData tid;

					if (!np_catalog_tuple_visible(hdr, sscan->rs_snapshot))
						continue;

					ItemPointerSet(&tid, scan->rs_cblock, off);
					np_catalog_store_heap_tuple(page, off, &tid,
												RelationGetRelid(rel), slot);
					if (scan->rs_base.rs_nkeys > 0)
					{
						HeapTuple	htup = ExecFetchSlotHeapTuple(slot, false, NULL);

						if (!HeapKeyTest(htup, RelationGetDescr(rel),
										 scan->rs_base.rs_nkeys,
										 scan->rs_base.rs_key))
							continue;
					}
					LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
					return true;
				}
			}
		}
		UnlockReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
		scan->rs_cblock++;
	}
}

static void
np_catalog_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
						int options, BulkInsertState bistate)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	FullTransactionId xmin = GetTopFullTransactionId();
	Size		total;
	char	   *buf;
	ItemPointerData tid;

	slot->tts_tableOid = RelationGetRelid(rel);
	tuple->t_tableOid = slot->tts_tableOid;

	HeapTupleHeaderSetXmin(tuple->t_data, XidFromFullTransactionId(xmin));
	HeapTupleHeaderSetCmin(tuple->t_data, cid);
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);

	total = np_catalog_pack_size(tuple);
	buf = (char *) palloc0(total);
	np_catalog_pack_item(buf, tuple, xmin, InvalidFullTransactionId,
						 cid, InvalidCommandId, NULL);
	np_write_record_to_page(rel, buf, total, &tid);
	pfree(buf);

	tuple->t_self = tid;
	slot->tts_tid = tid;
	if (shouldFree)
		pfree(tuple);
}

static void
np_catalog_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
									CommandId cid, int options,
									BulkInsertState bistate, uint32 specToken)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("np_catalog: speculative insert is not supported")));
}

static void
np_catalog_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
									  uint32 specToken, bool succeeded)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("np_catalog: speculative insert is not supported")));
}

static void
np_catalog_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
						CommandId cid, int options, BulkInsertState bistate)
{
	int			i;

	for (i = 0; i < nslots; i++)
		np_catalog_tuple_insert(rel, slots[i], cid, options, bistate);
}

static TM_Result
np_catalog_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
						CommandId cid, Snapshot snapshot, Snapshot crosscheck,
						bool wait, TM_FailureData *tmfd,
						LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	FullTransactionId fxid = GetTopFullTransactionId();
	bool		shouldFree = true;
	HeapTuple	tuple;
	Buffer		obuf;
	Page		opage;
	ItemId		olp;
	NPCatalogItemHeader old_hdr;
	Size		total;
	char	   *buf;
	ItemPointerData new_tid;

	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	slot->tts_tableOid = RelationGetRelid(rel);
	tuple->t_tableOid = slot->tts_tableOid;
	HeapTupleHeaderSetXmin(tuple->t_data, XidFromFullTransactionId(fxid));
	HeapTupleHeaderSetCmin(tuple->t_data, cid);
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);

	obuf = ReadBuffer(rel, ItemPointerGetBlockNumber(otid));
	LockBuffer(obuf, BUFFER_LOCK_EXCLUSIVE);
	opage = BufferGetPage(obuf);
	olp = PageGetItemId(opage, ItemPointerGetOffsetNumber(otid));
	old_hdr = (NPCatalogItemHeader) PageGetItem(opage, olp);

	if (!np_entity_tuple_is_live((NPEntityTupleHeader) old_hdr))
	{
		UnlockReleaseBuffer(obuf);
		if (shouldFree)
			pfree(tuple);
		return TM_Invisible;
	}
	if (np_entity_tuple_xmax_other((NPEntityTupleHeader) old_hdr))
	{
		UnlockReleaseBuffer(obuf);
		if (shouldFree)
			pfree(tuple);
		return TM_Updated;
	}

	UnlockReleaseBuffer(obuf);

	total = np_catalog_pack_size(tuple);
	buf = (char *) palloc0(total);
	np_catalog_pack_item(buf, tuple, fxid, InvalidFullTransactionId,
						 cid, InvalidCommandId, otid);
	np_write_record_to_page(rel, buf, total, &new_tid);
	pfree(buf);

	obuf = ReadBuffer(rel, ItemPointerGetBlockNumber(otid));
	LockBuffer(obuf, BUFFER_LOCK_EXCLUSIVE);
	{
		GenericXLogState *state = GenericXLogStart(rel);
		Page		wal_page = GenericXLogRegisterBuffer(state, obuf, 0);
		NPCatalogItemHeader wal_hdr;

		wal_hdr = (NPCatalogItemHeader) PageGetItem(wal_page,
					PageGetItemId(wal_page, ItemPointerGetOffsetNumber(otid)));
		wal_hdr->xmax = fxid;
		wal_hdr->cmax = cid;
		((HeapTupleHeader) ((char *) wal_hdr + SizeOfNPCatalogItemHeader))->t_ctid = new_tid;
		GenericXLogFinish(state);
	}
	UnlockReleaseBuffer(obuf);

	tuple->t_self = new_tid;
	slot->tts_tid = new_tid;
	*update_indexes = TU_All;
	if (shouldFree)
		pfree(tuple);
	return TM_Ok;
}

static TM_Result
np_catalog_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
						Snapshot snapshot, Snapshot crosscheck, bool wait,
						TM_FailureData *tmfd, bool changingPart)
{
	FullTransactionId fxid = GetTopFullTransactionId();
	Buffer		buf;
	NPCatalogItemHeader hdr;

	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	hdr = (NPCatalogItemHeader) PageGetItem(BufferGetPage(buf),
			PageGetItemId(BufferGetPage(buf), ItemPointerGetOffsetNumber(tid)));

	if (!np_entity_tuple_is_live((NPEntityTupleHeader) hdr))
	{
		UnlockReleaseBuffer(buf);
		return TM_Invisible;
	}
	if (np_entity_tuple_xmax_other((NPEntityTupleHeader) hdr))
	{
		UnlockReleaseBuffer(buf);
		return TM_Updated;
	}

	{
		GenericXLogState *state = GenericXLogStart(rel);
		Page		wal_page = GenericXLogRegisterBuffer(state, buf, 0);
		NPCatalogItemHeader wal_hdr;

		wal_hdr = (NPCatalogItemHeader) PageGetItem(wal_page,
					PageGetItemId(wal_page, ItemPointerGetOffsetNumber(tid)));
		wal_hdr->xmax = fxid;
		wal_hdr->cmax = cid;
		GenericXLogFinish(state);
	}
	UnlockReleaseBuffer(buf);
	return TM_Ok;
}

static TM_Result
np_catalog_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
					  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
					  LockWaitPolicy wait_policy, uint8 flags,
					  TM_FailureData *tmfd)
{
	Buffer		buf;
	Page		page;
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	NPCatalogItemHeader hdr;

	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!np_catalog_item_is_fetchable(page, off))
	{
		UnlockReleaseBuffer(buf);
		return TM_Invisible;
	}
	hdr = np_catalog_item_header(page, off);
	if (!np_catalog_tuple_visible(hdr, snapshot))
	{
		UnlockReleaseBuffer(buf);
		return TM_Invisible;
	}
	np_catalog_store_heap_tuple(page, off, tid, RelationGetRelid(rel), slot);
	UnlockReleaseBuffer(buf);
	return TM_Ok;
}

static IndexFetchTableData *
np_catalog_index_fetch_begin(Relation rel)
{
	NPCatalogIndexFetchData *scan = palloc0(sizeof(NPCatalogIndexFetchData));

	scan->xs_base.rel = rel;
	scan->xs_cbuf = InvalidBuffer;
	return &scan->xs_base;
}

static void
np_catalog_index_fetch_reset(IndexFetchTableData *scan)
{
	NPCatalogIndexFetchData *hscan = (NPCatalogIndexFetchData *) scan;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
	}
}

static void
np_catalog_index_fetch_end(IndexFetchTableData *scan)
{
	np_catalog_index_fetch_reset(scan);
	pfree(scan);
}

static bool
np_catalog_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid,
							 Snapshot snapshot, TupleTableSlot *slot,
							 bool *call_again, bool *all_dead)
{
	NPCatalogIndexFetchData *hscan = (NPCatalogIndexFetchData *) scan;
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	Page		page;
	NPCatalogItemHeader hdr;

	*call_again = false;
	if (all_dead)
		*all_dead = false;

	hscan->xs_cbuf = ReleaseAndReadBuffer(hscan->xs_cbuf, hscan->xs_base.rel,
										  ItemPointerGetBlockNumber(tid));
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(hscan->xs_cbuf);
	if (!np_catalog_item_is_fetchable(page, off))
	{
		if (all_dead)
			*all_dead = true;
		LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
		return false;
	}
	hdr = np_catalog_item_header(page, off);
	if (!np_entity_tuple_is_live((NPEntityTupleHeader) hdr))
	{
		if (all_dead)
			*all_dead = true;
		LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
		return false;
	}
	if (!np_catalog_tuple_visible(hdr, snapshot))
	{
		LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
		return false;
	}

	np_catalog_store_heap_tuple(page, off, tid,
								RelationGetRelid(hscan->xs_base.rel), slot);
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
	return true;
}

static bool
np_catalog_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
							 TupleTableSlot *slot)
{
	Buffer		buf;
	Page		page;
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	NPCatalogItemHeader hdr;

	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!np_catalog_item_is_fetchable(page, off))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	hdr = np_catalog_item_header(page, off);
	if (!np_catalog_tuple_visible(hdr, snapshot))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	np_catalog_store_heap_tuple(page, off, tid, RelationGetRelid(rel), slot);
	UnlockReleaseBuffer(buf);
	return true;
}

static void
np_catalog_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
	Relation	rel = sscan->rs_rd;
	Snapshot	snapshot = sscan->rs_snapshot;
	ItemPointerData ctid = *tid;

	Assert(ItemPointerIsValid(tid));

	for (;;)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		NPCatalogItemHeader hdr;
		HeapTupleHeader htup;
		ItemPointerData next;

		buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&ctid));
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		off = ItemPointerGetOffsetNumber(&ctid);
		if (!np_catalog_item_is_fetchable(page, off))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		hdr = np_catalog_item_header(page, off);
		if (np_catalog_tuple_visible(hdr, snapshot))
			*tid = ctid;
		htup = (HeapTupleHeader) ((char *) hdr + SizeOfNPCatalogItemHeader);
		next = htup->t_ctid;
		UnlockReleaseBuffer(buf);

		if (!ItemPointerIsValid(&next) || ItemPointerEquals(&ctid, &next))
			break;
		ctid = next;
	}
}

static bool
np_catalog_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	NPCatalogScanDesc s = (NPCatalogScanDesc) scan;

	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < s->rs_nblocks;
}

static bool
np_catalog_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
									Snapshot snapshot)
{
	Buffer		buf;
	Page		page;
	OffsetNumber off;
	bool		visible;

	if (TTS_EMPTY(slot) || !ItemPointerIsValid(&slot->tts_tid))
		return false;

	off = ItemPointerGetOffsetNumber(&slot->tts_tid);
	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&slot->tts_tid));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (!np_catalog_item_is_fetchable(page, off))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	visible = np_catalog_tuple_visible(np_catalog_item_header(page, off), snapshot);
	UnlockReleaseBuffer(buf);
	return visible;
}

static TransactionId
np_catalog_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	int			i;

	for (i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		Buffer		buf;
		Page		page;
		OffsetNumber off;

		buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&ideltid->tid));
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		off = ItemPointerGetOffsetNumber(&ideltid->tid);
		if (!np_catalog_item_is_fetchable(page, off) ||
			!np_entity_tuple_is_live((NPEntityTupleHeader)
									 np_catalog_item_header(page, off)))
			istatus->knowndeletable = true;
		UnlockReleaseBuffer(buf);
	}
	return InvalidTransactionId;
}

static void
np_catalog_relation_set_new_filelocator(Relation rel,
										const RelFileLocator *newrlocator,
										char persistence,
										TransactionId *freezeXid,
										MultiXactId *minmulti)
{
	SMgrRelation srel;

	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	srel = RelationCreateStorage(*newrlocator, persistence, true);
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
	}
	smgrclose(srel);
}

static void
np_catalog_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
}

static void
np_catalog_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;
	ForkNumber	forkNum;

	FlushRelationBuffers(rel);
	dstrel = RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, true);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);
	for (forkNum = MAIN_FORKNUM + 1; forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
np_catalog_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
									 Relation OldIndex, bool use_sort,
									 TransactionId OldestXmin,
									 TransactionId *xid_cutoff,
									 MultiXactId *multi_cutoff,
									 double *num_tuples, double *tups_vacuumed,
									 double *tups_recently_dead)
{
	TableScanDesc scan;
	TupleTableSlot *slot;
	double		n = 0;

	(void) OldIndex;
	(void) use_sort;
	(void) OldestXmin;
	(void) xid_cutoff;
	(void) multi_cutoff;

	*num_tuples = 0;
	*tups_vacuumed = 0;
	*tups_recently_dead = 0;

	slot = table_slot_create(OldHeap, NULL);
	scan = table_beginscan(OldHeap, SnapshotAny, 0, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		table_tuple_insert(NewHeap, slot, GetCurrentCommandId(true), 0, NULL);
		n++;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	*num_tuples = n;
}

static void
np_catalog_relation_vacuum(Relation rel, struct VacuumParams *params,
						   BufferAccessStrategy bstrategy)
{
	(void) rel;
	(void) params;
	(void) bstrategy;
}

static bool
np_catalog_scan_analyze_next_block(TableScanDesc sscan, struct ReadStream *stream)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;

	scan->rs_cbuf = read_stream_next_buffer(stream, NULL);
	if (!BufferIsValid(scan->rs_cbuf))
		return false;
	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
	scan->rs_cblock = BufferGetBlockNumber(scan->rs_cbuf);
	scan->rs_cindex = FirstOffsetNumber;
	return true;
}

static bool
np_catalog_scan_analyze_next_tuple(TableScanDesc sscan, TransactionId OldestXmin,
								   double *deadrows, double *livetuples,
								   TupleTableSlot *slot)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;
	Page		page;
	OffsetNumber maxoff;

	(void) OldestXmin;
	page = BufferGetPage(scan->rs_cbuf);
	maxoff = PageGetMaxOffsetNumber(page);

	for (; scan->rs_cindex <= maxoff; scan->rs_cindex++)
	{
		OffsetNumber off = (OffsetNumber) scan->rs_cindex;
		ItemId		lp;
		NPCatalogItemHeader hdr;
		ItemPointerData tid;

		lp = PageGetItemId(page, off);
		if (!ItemIdIsNormal(lp))
			continue;
		if (ItemIdGetLength(lp) < SizeOfNPCatalogItemHeader + SizeofHeapTupleHeader)
			continue;
		hdr = np_catalog_item_header(page, off);
		if (!np_entity_tuple_is_live((NPEntityTupleHeader) hdr))
		{
			*deadrows += 1;
			continue;
		}
		*livetuples += 1;
		ItemPointerSet(&tid, scan->rs_cblock, off);
		np_catalog_store_heap_tuple(page, off, &tid,
									RelationGetRelid(sscan->rs_rd), slot);
		scan->rs_cindex++;
		return true;
	}

	UnlockReleaseBuffer(scan->rs_cbuf);
	scan->rs_cbuf = InvalidBuffer;
	ExecClearTuple(slot);
	return false;
}

static double
np_catalog_index_build_range_scan(Relation heapRelation, Relation indexRelation,
								  struct IndexInfo *indexInfo, bool allow_sync,
								  bool anyvisible, bool progress,
								  BlockNumber start_blockno, BlockNumber numblocks,
								  IndexBuildCallback callback, void *callback_state,
								  TableScanDesc scan)
{
	TableScanDesc tscan;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *predicate;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples = 0;

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);
	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	tscan = table_beginscan(heapRelation, SnapshotAny, 0, NULL);
	while (table_scan_getnextslot(tscan, ForwardScanDirection, slot))
	{
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		callback(indexRelation, &slot->tts_tid, values, isnull, true, callback_state);
		reltuples += 1;
	}
	table_endscan(tscan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	return reltuples;
}

static void
np_catalog_index_validate_scan(Relation heapRelation, Relation indexRelation,
							   struct IndexInfo *indexInfo, Snapshot snapshot,
							   struct ValidateIndexState *state)
{
	TableScanDesc scan;
	NPCatalogScanDesc hscan;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *predicate;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ItemPointer indexcursor = NULL;
	ItemPointerData decoded;
	bool		tuplesort_empty = false;
	BlockNumber previous_blkno = InvalidBlockNumber;

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);
	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	scan = table_beginscan_strat(heapRelation, snapshot, 0, NULL, true, false);
	hscan = (NPCatalogScanDesc) scan;
	pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL, hscan->rs_nblocks);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		ItemPointer heapcursor = &slot->tts_tid;

		CHECK_FOR_INTERRUPTS();
		state->htups += 1;

		if (previous_blkno != hscan->rs_cblock)
		{
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 hscan->rs_cblock);
			previous_blkno = hscan->rs_cblock;
		}

		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, heapcursor) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
												  false, &ts_val, &ts_isnull,
												  NULL);
			Assert(tuplesort_empty || !ts_isnull);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;
			}
			else
				indexcursor = NULL;
		}

		if (tuplesort_empty ||
			ItemPointerCompare(indexcursor, heapcursor) > 0)
		{
			MemoryContextReset(econtext->ecxt_per_tuple_memory);
			if (predicate != NULL && !ExecQual(predicate, econtext))
				continue;
			FormIndexDatum(indexInfo, slot, estate, values, isnull);
			index_insert(indexRelation, values, isnull, heapcursor,
						 heapRelation,
						 indexInfo->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 false, indexInfo);
			state->tups_inserted += 1;
		}
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}

static Size
np_catalog_parallelscan_estimate(Relation rel)
{
	return table_block_parallelscan_estimate(rel);
}

static Size
np_catalog_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	return table_block_parallelscan_initialize(rel, pscan);
}

static void
np_catalog_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	table_block_parallelscan_reinitialize(rel, pscan);
}

static void
np_catalog_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
							 ItemPointer maxtid)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;
	ItemPointerData highest;
	ItemPointerData lowest;

	if (scan->rs_nblocks == 0)
		return;

	ItemPointerSet(&highest, scan->rs_nblocks - 1, MaxOffsetNumber);
	ItemPointerSet(&lowest, 0, FirstOffsetNumber);

	if (ItemPointerCompare(maxtid, &highest) < 0)
		ItemPointerCopy(maxtid, &highest);
	if (ItemPointerCompare(mintid, &lowest) > 0)
		ItemPointerCopy(mintid, &lowest);

	if (ItemPointerCompare(&highest, &lowest) < 0)
	{
		scan->rs_cblock = 0;
		scan->rs_nblocks = 0;
		return;
	}

	scan->rs_startblock = ItemPointerGetBlockNumberNoCheck(&lowest);
	scan->rs_cblock = scan->rs_startblock;
	scan->rs_coffset = FirstOffsetNumber;
	scan->rs_nblocks = ItemPointerGetBlockNumberNoCheck(&highest) + 1;
	ItemPointerCopy(&lowest, &sscan->st.tidrange.rs_mintid);
	ItemPointerCopy(&highest, &sscan->st.tidrange.rs_maxtid);
}

static bool
np_catalog_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
									 TupleTableSlot *slot)
{
	ItemPointer mintid = &sscan->st.tidrange.rs_mintid;
	ItemPointer maxtid = &sscan->st.tidrange.rs_maxtid;

	for (;;)
	{
		if (!np_catalog_scan_getnextslot(sscan, direction, slot))
			return false;
		if (ItemPointerCompare(&slot->tts_tid, mintid) < 0)
		{
			if (ScanDirectionIsBackward(direction))
				return false;
			continue;
		}
		if (ItemPointerCompare(&slot->tts_tid, maxtid) > 0)
		{
			if (ScanDirectionIsForward(direction))
				return false;
			continue;
		}
		return true;
	}
}

static bool
np_catalog_bitmap_next_block(TableScanDesc sscan, bool *recheck,
							 uint64 *lossy_pages, uint64 *exact_pages)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;
	TBMIterateResult tbmres;
	OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
	int			noffsets = -1;
	int			ntup = 0;
	Page		page;

	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	if (!tbm_iterate(&sscan->st.rs_tbmiterator, &tbmres))
		return false;

	*recheck = tbmres.recheck;
	if (!tbmres.lossy)
		noffsets = tbm_extract_page_tuple(&tbmres, offsets,
										  TBM_MAX_TUPLES_PER_PAGE);

	scan->rs_cbuf = ReadBuffer(sscan->rs_rd, tbmres.blockno);
	scan->rs_cblock = tbmres.blockno;
	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(scan->rs_cbuf);

	if (!tbmres.lossy)
	{
		int			i;

		for (i = 0; i < noffsets; i++)
		{
			OffsetNumber off = offsets[i];

			if (!np_catalog_item_is_fetchable(page, off))
				continue;
			if (!np_catalog_tuple_visible(np_catalog_item_header(page, off),
										  sscan->rs_snapshot))
				continue;
			scan->rs_vistuples[ntup++] = off;
		}
	}
	else
	{
		OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
		OffsetNumber off;

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			if (!np_catalog_item_is_fetchable(page, off))
				continue;
			if (!np_catalog_tuple_visible(np_catalog_item_header(page, off),
										  sscan->rs_snapshot))
				continue;
			scan->rs_vistuples[ntup++] = off;
		}
	}

	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
	scan->rs_ntuples = ntup;
	scan->rs_cindex = 0;
	if (tbmres.lossy)
		(*lossy_pages)++;
	else
		(*exact_pages)++;
	return true;
}

static bool
np_catalog_scan_bitmap_next_tuple(TableScanDesc sscan, TupleTableSlot *slot,
								  bool *recheck, uint64 *lossy_pages,
								  uint64 *exact_pages)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;
	OffsetNumber off;
	ItemPointerData tid;

	while (scan->rs_cindex >= scan->rs_ntuples)
	{
		if (!np_catalog_bitmap_next_block(sscan, recheck, lossy_pages, exact_pages))
			return false;
	}

	off = scan->rs_vistuples[scan->rs_cindex++];
	ItemPointerSet(&tid, scan->rs_cblock, off);
	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
	np_catalog_store_heap_tuple(BufferGetPage(scan->rs_cbuf), off, &tid,
								RelationGetRelid(sscan->rs_rd), slot);
	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
	return true;
}

static bool
np_catalog_scan_sample_next_block(TableScanDesc sscan,
								  struct SampleScanState *scanstate)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	if (scan->rs_nblocks == 0)
		return false;

	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	if (tsm->NextSampleBlock)
		blockno = tsm->NextSampleBlock(scanstate, scan->rs_nblocks);
	else if (!scan->rs_inited)
		blockno = scan->rs_startblock;
	else
	{
		blockno = scan->rs_cblock + 1;
		if (blockno >= scan->rs_nblocks)
			blockno = 0;
		if (blockno == scan->rs_startblock)
			blockno = InvalidBlockNumber;
	}

	scan->rs_cblock = blockno;
	if (!BlockNumberIsValid(blockno))
	{
		scan->rs_inited = false;
		return false;
	}

	CHECK_FOR_INTERRUPTS();
	scan->rs_cbuf = ReadBufferExtended(sscan->rs_rd, MAIN_FORKNUM, blockno,
									   RBM_NORMAL, scan->rs_strategy);
	scan->rs_inited = true;
	return true;
}

static bool
np_catalog_scan_sample_next_tuple(TableScanDesc sscan,
								  struct SampleScanState *scanstate,
								  TupleTableSlot *slot)
{
	NPCatalogScanDesc scan = (NPCatalogScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	Page		page;
	OffsetNumber maxoffset;

	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(scan->rs_cbuf);
	maxoffset = PageGetMaxOffsetNumber(page);

	for (;;)
	{
		OffsetNumber tupoffset;
		ItemPointerData tid;

		CHECK_FOR_INTERRUPTS();
		tupoffset = tsm->NextSampleTuple(scanstate, scan->rs_cblock, maxoffset);
		if (!OffsetNumberIsValid(tupoffset))
		{
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			ExecClearTuple(slot);
			return false;
		}
		if (!np_catalog_item_is_fetchable(page, tupoffset))
			continue;
		if (!np_catalog_tuple_visible(np_catalog_item_header(page, tupoffset),
									  sscan->rs_snapshot))
			continue;
		ItemPointerSet(&tid, scan->rs_cblock, tupoffset);
		np_catalog_store_heap_tuple(page, tupoffset, &tid,
									RelationGetRelid(sscan->rs_rd), slot);
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
		return true;
	}
}

static uint64
np_catalog_relation_size(Relation rel, ForkNumber forkNumber)
{
	return table_block_relation_size(rel, forkNumber);
}

#define NP_CATALOG_OVERHEAD_BYTES_PER_TUPLE \
	(MAXALIGN(SizeOfNPCatalogItemHeader + SizeofHeapTupleHeader) + sizeof(ItemIdData))
#define NP_CATALOG_USABLE_BYTES_PER_PAGE \
	(BLCKSZ - SizeOfPageHeaderData)

static void
np_catalog_estimate_rel_size(Relation rel, int32 *attr_widths, BlockNumber *pages,
							 double *tuples, double *allvisfrac)
{
	table_block_relation_estimate_size(rel, attr_widths, pages, tuples, allvisfrac,
									   NP_CATALOG_OVERHEAD_BYTES_PER_TUPLE,
									   NP_CATALOG_USABLE_BYTES_PER_PAGE);
}

static bool
np_catalog_relation_needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		data_length = att_align_nominal(data_length, att->attalign);
		if (att->attlen > 0)
			data_length += att->attlen;
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid, att->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != TYPSTORAGE_PLAIN)
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;
	if (maxlength_unknown)
		return true;
	tuple_length = MAXALIGN(SizeOfNPCatalogItemHeader + SizeofHeapTupleHeader +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}

static Oid
np_catalog_relation_toast_am(Relation rel)
{
	(void) rel;
	return HEAP_TABLE_AM_OID;
}

static void
np_catalog_relation_fetch_toast_slice(Relation rel, Oid toastoid, int32 attrsize,
									  int32 sliceoffset, int32 slicelength,
									  struct varlena *result)
{
	heap_fetch_toast_slice(rel, toastoid, attrsize, sliceoffset, slicelength,
						   result);
}

static const TableAmRoutine np_catalog_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = np_catalog_slot_callbacks,

	.scan_begin = np_catalog_scan_begin,
	.scan_end = np_catalog_scan_end,
	.scan_rescan = np_catalog_scan_rescan,
	.scan_getnextslot = np_catalog_scan_getnextslot,
	.scan_set_tidrange = np_catalog_scan_set_tidrange,
	.scan_getnextslot_tidrange = np_catalog_scan_getnextslot_tidrange,

	.parallelscan_estimate = np_catalog_parallelscan_estimate,
	.parallelscan_initialize = np_catalog_parallelscan_initialize,
	.parallelscan_reinitialize = np_catalog_parallelscan_reinitialize,

	.scan_bitmap_next_tuple = np_catalog_scan_bitmap_next_tuple,
	.scan_sample_next_block = np_catalog_scan_sample_next_block,
	.scan_sample_next_tuple = np_catalog_scan_sample_next_tuple,

	.index_fetch_begin = np_catalog_index_fetch_begin,
	.index_fetch_reset = np_catalog_index_fetch_reset,
	.index_fetch_end = np_catalog_index_fetch_end,
	.index_fetch_tuple = np_catalog_index_fetch_tuple,
	.index_delete_tuples = np_catalog_index_delete_tuples,

	.tuple_insert = np_catalog_tuple_insert,
	.tuple_insert_speculative = np_catalog_tuple_insert_speculative,
	.tuple_complete_speculative = np_catalog_tuple_complete_speculative,
	.multi_insert = np_catalog_multi_insert,
	.tuple_delete = np_catalog_tuple_delete,
	.tuple_update = np_catalog_tuple_update,
	.tuple_lock = np_catalog_tuple_lock,

	.tuple_fetch_row_version = np_catalog_fetch_row_version,
	.tuple_get_latest_tid = np_catalog_get_latest_tid,
	.tuple_tid_valid = np_catalog_tuple_tid_valid,
	.tuple_satisfies_snapshot = np_catalog_tuple_satisfies_snapshot,

	.relation_set_new_filelocator = np_catalog_relation_set_new_filelocator,
	.relation_nontransactional_truncate = np_catalog_relation_nontransactional_truncate,
	.relation_copy_data = np_catalog_relation_copy_data,
	.relation_copy_for_cluster = np_catalog_relation_copy_for_cluster,
	.relation_vacuum = np_catalog_relation_vacuum,
	.scan_analyze_next_block = np_catalog_scan_analyze_next_block,
	.scan_analyze_next_tuple = np_catalog_scan_analyze_next_tuple,
	.index_build_range_scan = np_catalog_index_build_range_scan,
	.index_validate_scan = np_catalog_index_validate_scan,

	.relation_size = np_catalog_relation_size,
	.relation_needs_toast_table = np_catalog_relation_needs_toast_table,
	.relation_toast_am = np_catalog_relation_toast_am,
	.relation_fetch_toast_slice = np_catalog_relation_fetch_toast_slice,
	.relation_estimate_size = np_catalog_estimate_rel_size
};

PG_FUNCTION_INFO_V1(np_catalog_handler);
Datum
np_catalog_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&np_catalog_methods);
}
