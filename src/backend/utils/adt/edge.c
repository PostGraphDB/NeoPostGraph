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

#include <string.h>
#include <assert.h>

#include "access/genam.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "utils/np_cache.h"
#include "catalog/np_label.h"
#include "utils/gtype.h"
#include "utils/dictionary.h"
#include "utils/edge.h"
#include "utils/vertex.h"

#include "access/np_entity_store.h"

PG_FUNCTION_INFO_V1(edge_in);
Datum edge_in(PG_FUNCTION_ARGS) {
    char *str = PG_GETARG_CSTRING(0);
    gtype_value *val = gtype_value_from_cstring(str, strlen(str));

    if (val->type != GTV_OBJECT)
        ereport(ERROR, errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("invalid format for properties, expects object"));

    edge *e = palloc0(sizeof(edge));
    e->id = 0;
    e->dictionary_id = 0;
    e->label_id = 0;
    e->graph_id = 0;

    e->start_id = 0;
    e->start_label = 0;
    e->end_id = 0;
    e->end_label = 0;
    SET_VARSIZE(e, sizeof(edge));

    NP_RETURN_EDGE(e);
}

PG_FUNCTION_INFO_V1(edge_build);
Datum edge_build(PG_FUNCTION_ARGS) {
    vertex *start_vertex = NP_GET_ARG_VERTEX(4);
    vertex *end_vertex = NP_GET_ARG_VERTEX(5);

    edge *e = palloc0(sizeof(edge));

    e->id = PG_GETARG_INT64(0);
    e->graph_id = PG_GETARG_INT32(1);
    e->label_id = PG_GETARG_INT32(2);
    e->dictionary_id = PG_GETARG_INT16(3);

    e->start_id = start_vertex->id;
    e->start_label = start_vertex->label_id;
    e->end_id = end_vertex->id;
    e->end_label = end_vertex->label_id;
    SET_VARSIZE(e, sizeof(edge));

    NP_RETURN_EDGE(e);
}


PG_FUNCTION_INFO_V1(edge_out);
Datum edge_out(PG_FUNCTION_ARGS) {
    edge *e = NP_GET_ARG_EDGE(0);
    StringInfoData *buffer = makeStringInfo();

    // id
    appendStringInfoString(buffer, "{\"id\": ");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->id))));

    // start id
    appendStringInfoString(buffer, ", \"start_vertex\": (");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int4out, Int32GetDatum(e->start_label))));
    appendStringInfoString(buffer, ",");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->start_id))));

    // end id
    appendStringInfoString(buffer, "), \"edge_vertex\": (");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int4out, Int32GetDatum(e->end_label))));
    appendStringInfoString(buffer, ",");
    appendStringInfoString(buffer, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(e->end_id))));

    // label
    appendStringInfoString(buffer, "), \"label\": \"");
    if (e->graph_id != 0 && e->label_id != 0) {
        label_cache_data *cache = search_edge_label_graph_id_label_id_cache(e->graph_id, e->label_id);
        if (cache) {
            bool has_labels = false;
            
            if (cache->label_string && cache->label_string[0] != '\0') {
                appendStringInfoString(buffer, cache->label_string);
                has_labels = true;
            } else {
                char *ltree_str = DatumGetCString(DirectFunctionCall1(ltree_out, PointerGetDatum(cache->label)));
                if (strncmp(ltree_str, "_.", 2) == 0) {
                    appendStringInfoString(buffer, ltree_str + 2);
                } else {
                    appendStringInfoString(buffer, ltree_str);
                }
                has_labels = true;
            }

            if (OidIsValid(cache->annotations_tbl)) {
                uint32 pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoEdgePhysMapRecord) + sizeof(ItemIdData));
                ItemPointerData phys_map_tid;
                np_id_to_tid(e->id, pmap_tuples_per_page, &phys_map_tid);

                Relation pmap_rel = table_open(cache->phys_map, AccessShareLock);
                ItemPointerData a_itemptr;
                ItemPointerSetInvalid(&a_itemptr);

                /* DEFENSE 1: Check edge physical map bounds */
                if (ItemPointerGetBlockNumber(&phys_map_tid) < RelationGetNumberOfBlocks(pmap_rel)) {
                    Buffer pmap_buf = ReadBuffer(pmap_rel, ItemPointerGetBlockNumber(&phys_map_tid));
                    LockBuffer(pmap_buf, BUFFER_LOCK_SHARE);
                    
                    Page pmap_page = BufferGetPage(pmap_buf);
                    ItemId pmap_lp = PageGetItemId(pmap_page, ItemPointerGetOffsetNumber(&phys_map_tid));
                    
                    if (ItemIdIsNormal(pmap_lp)) {
                        NeoEdgePhysMapRecord *disk_pmap_rec = (NeoEdgePhysMapRecord *) PageGetItem(pmap_page, pmap_lp);
                        a_itemptr = disk_pmap_rec->a_itemptr;
                    }
                    UnlockReleaseBuffer(pmap_buf);
                }
                table_close(pmap_rel, AccessShareLock);

                if (ItemPointerIsValid(&a_itemptr)) {
                    Relation annot_rel = table_open(cache->annotations_tbl, AccessShareLock);
                    
                    /* DEFENSE 2: Prevent garbage a_itemptr from crashing on empty/small annotation table */
                    if (ItemPointerGetBlockNumber(&a_itemptr) < RelationGetNumberOfBlocks(annot_rel)) {
                        Buffer annot_buf = ReadBuffer(annot_rel, ItemPointerGetBlockNumber(&a_itemptr));
                        LockBuffer(annot_buf, BUFFER_LOCK_SHARE);
                        
                        Page annot_page = BufferGetPage(annot_buf);
                        ItemId annot_lp = PageGetItemId(annot_page, ItemPointerGetOffsetNumber(&a_itemptr));
                        
                        if (ItemIdIsNormal(annot_lp)) {
                            NPEntityTupleHeader annot_hdr = (NPEntityTupleHeader) PageGetItem(annot_page, annot_lp);
                            
                            if (np_entity_tuple_is_live(annot_hdr)) {
                                uint64 bitset_xmin = U64FromFullTransactionId(annot_hdr->xmin);
                                CommandId bitset_cmin = annot_hdr->cmin;

                                bytea *bitset = (bytea *) annot_hdr->serialized_entity;
                                char *bits = VARDATA(bitset);

                                const graph_cache_data *graph_cache = search_graph_id_cache(e->graph_id);
                                if (graph_cache && OidIsValid(graph_cache->annot_schema_phys_map) && OidIsValid(graph_cache->annot_schema_tbl)) {
                                    uint32 schema_pmap_tuples_per_page = (BLCKSZ - SizeOfPageHeaderData) / (sizeof(NeoPhysMapRecord) + sizeof(ItemIdData));
                                    Relation schema_pmap_rel = table_open(graph_cache->annot_schema_phys_map, AccessShareLock);
                                    
                                    ItemPointerData schema_pmap_tid;
                                    np_id_to_tid(e->label_id, schema_pmap_tuples_per_page, &schema_pmap_tid); 
                                    
                                    ItemPointerData schema_itemptr;
                                    ItemPointerSetInvalid(&schema_itemptr);

                                    /* DEFENSE 3: Prevent bounds crash on schema_pmap_rel */
                                    if (ItemPointerGetBlockNumber(&schema_pmap_tid) < RelationGetNumberOfBlocks(schema_pmap_rel)) {
                                        Buffer schema_pmap_buf = ReadBuffer(schema_pmap_rel, ItemPointerGetBlockNumber(&schema_pmap_tid));
                                        LockBuffer(schema_pmap_buf, BUFFER_LOCK_SHARE);
                                        Page schema_pmap_page = BufferGetPage(schema_pmap_buf);
                                        ItemId schema_pmap_lp = PageGetItemId(schema_pmap_page, ItemPointerGetOffsetNumber(&schema_pmap_tid));
                                        
                                        if (ItemIdIsNormal(schema_pmap_lp)) {
                                            NeoPhysMapRecord *schema_pmap_rec = (NeoPhysMapRecord *) PageGetItem(schema_pmap_page, schema_pmap_lp);
                                            schema_itemptr = schema_pmap_rec->v_itemptr;
                                        }
                                        UnlockReleaseBuffer(schema_pmap_buf);
                                    }
                                    table_close(schema_pmap_rel, AccessShareLock);
                                    
                                    bool decoded = false;

                                    if (ItemPointerIsValid(&schema_itemptr)) {
                                        Relation schema_rel = table_open(graph_cache->annot_schema_tbl, AccessShareLock);
                                        
                                        while (ItemPointerIsValid(&schema_itemptr)) {
                                            /* DEFENSE 4: Prevent bounds crash on schema_itemptr in the temporal chain */
                                            if (ItemPointerGetBlockNumber(&schema_itemptr) >= RelationGetNumberOfBlocks(schema_rel)) {
                                                break; 
                                            }

                                            Buffer schema_buf = ReadBuffer(schema_rel, ItemPointerGetBlockNumber(&schema_itemptr));
                                            LockBuffer(schema_buf, BUFFER_LOCK_SHARE);
                                            Page schema_page = BufferGetPage(schema_buf);
                                            ItemId schema_lp = PageGetItemId(schema_page, ItemPointerGetOffsetNumber(&schema_itemptr));
                                            
                                            if (ItemIdIsNormal(schema_lp)) {
                                                NPEntityTupleHeader schema_hdr = (NPEntityTupleHeader) PageGetItem(schema_page, schema_lp);
                                                uint64 schema_xmin = U64FromFullTransactionId(schema_hdr->xmin);
                                                
                                                if (schema_xmin < bitset_xmin || (schema_xmin == bitset_xmin && schema_hdr->cmin <= bitset_cmin)) {
                                                    ArrayType *map_array = DatumGetArrayTypeP(PointerGetDatum(schema_hdr->serialized_entity));
                                                    Datum *map_d; bool *map_n; int map_count;
                                                    deconstruct_array(map_array, TEXTOID, -1, false, 'i', &map_d, &map_n, &map_count);
                                                    
                                                    for (int i = 0; i < map_count; i++) {
                                                        if (map_n[i]) continue;
                                                        if ((bits[i / 8] & (1 << (i % 8))) != 0) {
                                                            if (has_labels) appendStringInfoString(buffer, ":");
                                                            appendStringInfoString(buffer, TextDatumGetCString(map_d[i]));
                                                            has_labels = true;
                                                        }
                                                    }
                                                    decoded = true;
                                                    UnlockReleaseBuffer(schema_buf);
                                                    break;
                                                } else {
                                                    schema_itemptr = schema_hdr->prev_itemptr;
                                                }
                                            } else {
                                                ItemPointerSetInvalid(&schema_itemptr);
                                            }
                                            UnlockReleaseBuffer(schema_buf);
                                        }
                                        table_close(schema_rel, AccessShareLock);
                                    }

                                    if (!decoded && cache->annotation_map != NULL) {
                                        Datum *map_d; bool *map_n; int map_count;
                                        deconstruct_array(cache->annotation_map, TEXTOID, -1, false, 'i', &map_d, &map_n, &map_count);
                                        
                                        for (int i = 0; i < map_count; i++) {
                                            if (map_n[i]) continue;
                                            if ((bits[i / 8] & (1 << (i % 8))) != 0) {
                                                if (has_labels) appendStringInfoString(buffer, ":");
                                                appendStringInfoString(buffer, TextDatumGetCString(map_d[i]));
                                                has_labels = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        UnlockReleaseBuffer(annot_buf);
                    }
                    table_close(annot_rel, AccessShareLock);
                }
            }
        }
    }

    // properties
    appendStringInfoString(buffer, "\", \"properties\": ");
    gtype *props = np_fetch_edge_properties(e);
    gtype_to_cstring(buffer, &props->root, 0);
    appendStringInfoString(buffer, "}");

    PG_RETURN_CSTRING(buffer->data);
}