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

#include "access/attnum.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/tupdesc.h"
#include "catalog/pg_collation.h"
#include "fmgr.h"
#include "storage/lockdefs.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "catalog/np_graph.h"
#include "utils/np_cache.h"

typedef struct graph_name_namespace_cache_key
{
    NameData name; // np_graph.name
    Oid namespace; // np_graph.namespace
} graph_name_namespace_cache_key;


typedef struct graph_name_namespace_cache_entry
{
    graph_name_namespace_cache_key key;
    graph_cache_data data;
} graph_name_namespace_cache_entry;

// np_graph.name
static HTAB *graph_name_namespace_cache_hash = NULL;
static ScanKeyData graph_name_namespace_scan_keys[2];

// initialize all caches
static void initialize_caches(void);

// common
static void ag_cache_scan_key_init(ScanKey entry, AttrNumber attno,
                                   RegProcedure func);
static int name_hash_compare(const void *key1, const void *key2, Size keysize);

// np_graph
static void initialize_graph_caches(void);
static void create_graph_caches(void);
static void create_graph_name_namespace_cache(void);
static void invalidate_graph_caches(Datum arg, int cache_id, uint32 hash_value);
static void flush_graph_name_namespace_cache(void);
static graph_cache_data *search_graph_name_namespace_cache_miss(graph_name_namespace_cache_key *key);
static void fill_graph_cache_data(graph_cache_data *cache_data, HeapTuple tuple, TupleDesc tuple_desc);

static void initialize_caches(void)
{
    static bool initialized = false;

    if (initialized)
        return;

    if (!CacheMemoryContext)
        CreateCacheMemoryContext();

    initialize_graph_caches();

    initialized = true;
}

static void np_cache_scan_key_init(ScanKey entry, AttrNumber attno, RegProcedure func)
{
    entry->sk_flags = 0;
    entry->sk_attno = attno;
    entry->sk_strategy = BTEqualStrategyNumber;
    entry->sk_subtype = InvalidOid;
    entry->sk_collation = C_COLLATION_OID;
    fmgr_info_cxt(func, &entry->sk_func, CacheMemoryContext);
    entry->sk_argument = (Datum)0;
}

static int name_hash_compare(const void *key1, const void *key2, Size keysize)
{
    graph_name_namespace_cache_key *cache_key1 = (graph_name_namespace_cache_key *)key1;
    graph_name_namespace_cache_key *cache_key2 = (graph_name_namespace_cache_key *)key2;


    if (cache_key1->namespace < cache_key2->namespace)
        return -1;
    else if (cache_key1->namespace > cache_key2->namespace)
        return 1;

    return strncmp(NameStr(cache_key1->name), NameStr(cache_key2->name), NAMEDATALEN);
}

static void initialize_graph_caches(void)
{
    // np_graph.name
    np_cache_scan_key_init(&graph_name_namespace_scan_keys[0], 2, F_NAMEEQ);
    // np_graph.namespace
    np_cache_scan_key_init(&graph_name_namespace_scan_keys[1], 3, F_OIDEQ);

    create_graph_caches();

    /*
     * A graph is backed by the bound namespace. So, register the invalidation
     * logic of the graph caches for invalidation events of NAMESPACEOID cache.
     */
    CacheRegisterSyscacheCallback(NAMESPACEOID, invalidate_graph_caches, (Datum)0);
}

static void create_graph_caches(void)
{
    /*
     * All the hash tables are created using their dedicated memory contexts
     * which are under TopMemoryContext.
     * 
     * XXX: Setup to handle multiple caches
     */
    create_graph_name_namespace_cache();

}

static void create_graph_name_namespace_cache(void)
{
    HASHCTL hash_ctl;

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(NameData);
    hash_ctl.entrysize = sizeof(graph_name_namespace_cache_entry);
    hash_ctl.match = name_hash_compare;

    /*
     * Please see the comment of hash_create() for the nelem value 16 here.
     * HASH_BLOBS flag is set because the key for this hash is fixed-size.
     */
    graph_name_namespace_cache_hash = ShmemInitHash("np_graph (name) cache", 16, 1000, &hash_ctl,
			     HASH_ELEM | HASH_BLOBS | HASH_COMPARE);
}


static void invalidate_graph_caches(Datum arg, int cache_id, uint32 hash_value)
{
    Assert(graph_name_namespace_cache_hash);

    /*
     * Currently, all entries in the graph caches are flushed because
     * hash_value is for an entry in NAMESPACEOID cache. Since the caches
     * are not currently used in performance-critical paths, this seems OK.
     */
    flush_graph_name_namespace_cache();

}

static void flush_graph_name_namespace_cache(void)
{
    HASH_SEQ_STATUS hash_seq;

    hash_seq_init(&hash_seq, graph_name_namespace_cache_hash);
    for (;;)
    {
        graph_name_namespace_cache_entry *entry = hash_seq_search(&hash_seq);
        if (!entry)
            break;

        if (!hash_search(graph_name_namespace_cache_hash, &entry->key.name, HASH_REMOVE, NULL))
            ereport(ERROR, (errmsg_internal("graph (name) cache corrupted")));
    }
}

const graph_cache_data *search_graph_name_namespace_cache(const char *name, const Oid namespace)
{
    initialize_caches();

    graph_name_namespace_cache_key key = { .namespace = namespace };
    namestrcpy(&key.name, name);

    graph_name_namespace_cache_entry *entry;
    if (entry = hash_search(graph_name_namespace_cache_hash, &key, HASH_FIND, NULL))
        return &entry->data;

    return search_graph_name_namespace_cache_miss(&key);
}

static graph_cache_data *search_graph_name_namespace_cache_miss(graph_name_namespace_cache_key *key)
{
    // setup scan keys
    ScanKeyData scan_keys[2];
    memcpy(scan_keys, graph_name_namespace_scan_keys, sizeof(graph_name_namespace_scan_keys));
    scan_keys[0].sk_argument = NameGetDatum(&key->name);
    scan_keys[1].sk_argument = ObjectIdGetDatum(key->namespace);

    // open graph catalog
    Relation np_graph = table_open(np_graph_relation_id(), AccessShareLock);
    SysScanDesc scan_desc = systable_beginscan(np_graph, np_graph_name_namespace_index_id(), true, NULL, 2, scan_keys);

    // get catalog record
    // don't need to loop over scan_desc because np_graph_name_namespace_index is UNIQUE
    HeapTuple tuple = systable_getnext(scan_desc);
    if (!HeapTupleIsValid(tuple)) {
        // catalog does not have record
        systable_endscan(scan_desc);
        table_close(np_graph, AccessShareLock);

        return NULL;
    }

    // catalog entry exists add to cache
    graph_name_namespace_cache_entry *entry = hash_search(graph_name_namespace_cache_hash, &key, HASH_ENTER, NULL);

    // populate cache
    fill_graph_cache_data(&entry->data, tuple, RelationGetDescr(np_graph));

    // close catalog
    systable_endscan(scan_desc);
    table_close(np_graph, AccessShareLock);

    return &entry->data;
}

static void fill_graph_cache_data(graph_cache_data *cache_data, HeapTuple tuple, TupleDesc tuple_desc)
{
    bool is_null;
    // np_graph.id
    cache_data->oid = DatumGetObjectId(heap_getattr(tuple, 1, tuple_desc, &is_null));
    // np_graph.name
    namestrcpy(&cache_data->name, DatumGetName(heap_getattr(tuple, 2, tuple_desc, &is_null))->data);
    // np_graph.namespace
    cache_data->namespace = DatumGetObjectId(heap_getattr(tuple, 3, tuple_desc, &is_null));
}
