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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "storage/lockdefs.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "ltree.h"

#include "catalog/np_label.h"
#include "commands/label_commands.h"
#include "utils/dictionary.h"
#include "utils/np_cache.h"
#include "utils/edge.h"
#include "utils/vertex.h"

static void register_and_validate_labels(int graph_id, char *struct_label, ArrayType *annot_array);
static ArrayType *merge_and_dedupe_text_arrays(ArrayType *arr1, ArrayType *arr2);
static Oid execute_internal_create_table(const char *namespace_name, const char *tbl_name, List *table_elts, const char *access_method);
static void 
execute_internal_create_index(const char *namespace_name, const char *tbl_name, const char *idx_name, 
                              const char *col_name, const char *access_method, bool is_unique, bool is_primary, Node *where_clause);

PG_FUNCTION_INFO_V1(create_vlabel);
Datum 
create_vlabel(PG_FUNCTION_ARGS)
{
    /* 1. Extract Graph Name */
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph name must not be NULL")));
    }
    char *graph_name = NameStr(*PG_GETARG_NAME(0));

    /* 2. Extract Label Name */
    char *label_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
    
    /* 3. Extract Annotations Array */
    ArrayType *annot_array = NULL;
    if (!PG_ARGISNULL(2)) {
        annot_array = PG_GETARG_ARRAYTYPE_P(2);
    }
    
    /* 4. Extract Namespace */
    char *namespace_name = NULL;
    if (!PG_ARGISNULL(3)) {
        namespace_name = text_to_cstring(PG_GETARG_TEXT_PP(3));
    }

    /* Call the internal logic */
    create_vlabel_internal(graph_name, label_str, annot_array, namespace_name);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(merge_vlabels);
Datum 
merge_vlabels(PG_FUNCTION_ARGS)
{
    /* 1. Extract Graph Name */
    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph name must not be NULL")));
    }
    char *graph_name = NameStr(*PG_GETARG_NAME(0));

    /* 2. Extract Parent and Child IDs */
    int32 parent_id = PG_GETARG_INT32(1);
    int32 child_id = PG_GETARG_INT32(2);

    /* 3. Extract Namespace */
    char *namespace_name = NULL;
    if (!PG_ARGISNULL(3)) {
        namespace_name = text_to_cstring(PG_GETARG_TEXT_PP(3));
    }

    /* Call Internal Logic */
    merge_vlabels_internal(graph_name, parent_id, child_id, namespace_name);

    PG_RETURN_VOID();
}


void setup_vertex_label_catalog(int graph_id, Oid namespace, Oid *out_seq, Oid *out_meta)
{
    char *seq_name = psprintf("vertex_label_id_seq_%d", graph_id);
    char *meta_tbl_name = psprintf("np_vertex_label_%d", graph_id);

    *out_seq = create_label_sequence(seq_name, get_namespace_name(namespace));
    *out_meta = create_vertex_label_metadata_table(meta_tbl_name);
    
    create_metadata_btree_index(meta_tbl_name);
    create_metadata_gist_index(meta_tbl_name);
    
    create_default_vlabel(graph_id, *out_seq, namespace);
}

void setup_edge_label_catalog(int graph_id, Oid namespace, Oid *out_seq, Oid *out_meta)
{
    char *seq_name = psprintf("edge_label_id_seq_%d", graph_id);
    char *meta_tbl_name = psprintf("np_edge_label_%d", graph_id);

    *out_seq = create_label_sequence(seq_name, get_namespace_name(namespace));
    *out_meta = create_label_metadata_table(meta_tbl_name);
    
    create_metadata_btree_index(meta_tbl_name);
    create_metadata_gist_index(meta_tbl_name);
    
    create_default_elabel(graph_id, *out_seq, namespace);
}

Oid create_default_vlabel(int graph_id, Oid vertex_id_seq, Oid namespace)
{
    Oid label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(vertex_id_seq)));
    Datum root_ltree = DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL));

    return build_vertex_label_infrastructure(
        graph_id, label_id, namespace,
        root_ltree, 0, (Datum)0, NULL, 
        InvalidOid, InvalidOid
    );
}

Oid create_default_elabel(int graph_id, Oid edge_id_seq, Oid namespace)
{
    Oid label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(edge_id_seq)));

    Oid edge_tbl = create_edge_tables(graph_id, label_id, namespace);
    Oid phys_map = create_label_edge_physical_mapping_table(
                        psprintf("np_edge_%d_%d_phys_map", graph_id, label_id), namespace);

    /* Updated to pass InvalidOid and (Datum)0 for the 6th and 7th arguments */
    insert_label(psprintf("np_edge_label_%d", graph_id), 
                 DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)), 
                 label_id, edge_tbl, phys_map, InvalidOid, (Datum)0);
                 
    //TODO
    //Oid dict_id = create_vertex_property_dictionary(graph_id, label_id);
    //create_vertex_dictionary_metadata_btree_index(graph_id, label_id, dict_id);
    CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
    char seq_name[NAMEDATALEN];
    snprintf(seq_name, NAMEDATALEN, "np_edge_id_seq_%d_%d", graph_id, label_id);
    seq_stmt->sequence = makeRangeVar("neopostgraph", seq_name, -1);
    seq_stmt->options = NIL;
    seq_stmt->ownerId = GetUserId();
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;

    DefineSequence(NULL, seq_stmt);
    CommandCounterIncrement();

    return edge_tbl;
}
Oid build_vertex_label_infrastructure(
    int graph_id, int label_id, Oid namespace,
    Datum label_ltree, int byte_allocation_size,
    Datum annot_map_datum, ArrayType *annot_array,
    Oid annot_schema_tbl, Oid annot_schema_phys_map)
{
    /* 1. Base Data Tables */
    Oid vertex_tbl = create_vertex_tables(graph_id, label_id, namespace);
    Oid phys_map = create_label_vertex_physical_mapping_table(psprintf("np_vertex_%d_%d_phys_map", graph_id, label_id), namespace);
    Oid arraylist = create_vertex_label_arraylist_table(psprintf("np_vertex_%d_%d_arraylist", graph_id, label_id), namespace);

    /* 2. Linked List Setup */
    Oid ll_seq = create_linked_list_table_sequence(psprintf("np_vertex_%d_%d_linked_list_seq", graph_id, label_id), "neopostgraph");    
    Oid ll_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(ll_seq)));
    
    char *ll_table_name = psprintf("np_vertex_%d_%d_%d_linked_list", graph_id, label_id, ll_id);
    Oid ll_table = create_vertex_label_linked_list_table(ll_table_name, namespace);
    
    char *ll_meta_table = psprintf("np_vertex_%d_%d_linked_list_meta", graph_id, label_id);
    Oid ll_meta = create_vertex_label_linked_list_metadata_table(ll_meta_table, namespace);
    
    /* Correct parameter order: (meta_table, namespace, sequence_id, table_oid) */
    insert_vertex_ll_meta(ll_meta_table, namespace, ll_id, ll_table);

    /* 3. Annotations Setup */
    char *annot_tbl_name = psprintf("np_vertex_%d_%d_annotations", graph_id, label_id);
    Oid annotations_tbl = create_vertex_label_annotation_table(annot_tbl_name, namespace, byte_allocation_size);

    /* 4. Global Catalog Registration */
    insert_vertex_label(
        psprintf("np_vertex_label_%d", graph_id),
        label_ltree, label_id, vertex_tbl, phys_map, arraylist, 
        ll_seq, ll_meta, annotations_tbl, annot_map_datum
    );
    
    if (annot_array && OidIsValid(annot_schema_tbl)) {
        insert_annotation_schema(graph_id, label_id, annot_array, annot_schema_tbl, annot_schema_phys_map);
    }

    /* 5. Create Entity ID Sequence */
    CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
    char seq_name[NAMEDATALEN];
    snprintf(seq_name, NAMEDATALEN, "np_vertex_id_seq_%d_%d", graph_id, label_id);
    seq_stmt->sequence = makeRangeVar("neopostgraph", seq_name, -1);
    seq_stmt->options = NIL;
    seq_stmt->ownerId = GetUserId();
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;

    DefineSequence(NULL, seq_stmt);
    CommandCounterIncrement();

    /* 6. Dictionary Setup */
    Oid dict_id = create_vertex_property_dictionary(graph_id, label_id);
    create_vertex_dictionary_metadata_btree_index(graph_id, label_id, dict_id);

    return vertex_tbl;
}


Oid create_vertex_tables(int graph_id, int label_id, Oid namespace) {
    ColumnDef *id = makeColumnDef("id", INT8OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    ColumnDef *vertex = makeColumnDef("vertex", VERTEXOID, -1, InvalidOid);
    vertex->constraints = list_make1(build_not_null_constraint());

    return execute_internal_create_table(get_namespace_name(namespace),
                                         psprintf("np_vertex_%d_%d", graph_id, label_id),
                                         list_make2(id, vertex), "entity_store");
}

Oid create_edge_tables(int graph_id, int label_id, Oid namespace) {
    ColumnDef *id = makeColumnDef("id", INT8OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    ColumnDef *edge = makeColumnDef("edge", EDGEOID, -1, InvalidOid);
    edge->constraints = list_make1(build_not_null_constraint());

    return execute_internal_create_table(get_namespace_name(namespace),
                                         psprintf("np_edge_%d_%d", graph_id, label_id),
                                         list_make2(id, edge), "entity_store");
}

Oid create_label_sequence(char *seq_name, char *namespace) {
    ParseState *pstate = make_parsestate(NULL);
    pstate->p_sourcetext = "(generated CREATE SEQUENCE command)";

    CreateSeqStmt *seq_stmt = makeNode(CreateSeqStmt);
    seq_stmt->sequence = makeRangeVar(namespace, seq_name, -1);
    seq_stmt->options = NIL;
    seq_stmt->ownerId = InvalidOid;
    seq_stmt->for_identity = false;
    seq_stmt->if_not_exists = false;

    DefineSequence(pstate, seq_stmt);
    CommandCounterIncrement();

    return get_relname_relid(seq_name, get_namespace_oid(namespace, false));
}

Oid create_label_catalog_table(int graph_id) {
    char *tbl_name = psprintf("np_label_catalog_%d", graph_id);

    ColumnDef *label_name = makeColumnDef("label_name", TEXTOID, -1, InvalidOid);
    label_name->constraints = list_make1(build_not_null_constraint());
    ColumnDef *label_type = makeColumnDef("label_type", CHAROID, -1, InvalidOid);
    label_type->constraints = list_make1(build_not_null_constraint());

    Oid catalog_oid = execute_internal_create_table("neopostgraph", tbl_name, 
                                                    list_make2(label_name, label_type), NULL);

    execute_internal_create_index("neopostgraph", tbl_name, psprintf("%s_idx", tbl_name), 
                                  "label_name", "btree", true, true, NULL);
    return catalog_oid;
}

Oid create_label_metadata_table(char *meta_tbl_name) {
    ColumnDef *id = makeColumnDef("id", INT4OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    
    ColumnDef *ltree = makeColumnDef("ltree", LTREEOID, -1, InvalidOid);
    ltree->constraints = list_make1(build_not_null_constraint());
    
    ColumnDef *vertex_tbl = makeColumnDef("tbl", REGCLASSOID, -1, InvalidOid);
    vertex_tbl->constraints = list_make1(build_not_null_constraint());
    
    ColumnDef *phys_map = makeColumnDef("phys_map", REGCLASSOID, -1, InvalidOid);
    phys_map->constraints = list_make1(build_not_null_constraint());

    /* Add is_primary column with DEFAULT true */
    ColumnDef *is_primary = makeColumnDef("is_primary", BOOLOID, -1, InvalidOid);
    Constraint *def_const = makeNode(Constraint);
    def_const->contype = CONSTR_DEFAULT;
    def_const->location = -1;
    def_const->raw_expr = (Node *) makeStringConst("true", -1);
    is_primary->constraints = list_make1(def_const);

    ColumnDef *annotations_tbl = makeColumnDef("annotations_tbl", REGCLASSOID, -1, InvalidOid);
    ColumnDef *annotation_map = makeColumnDef("annotation_map", TEXTARRAYOID, -1, InvalidOid);

    List *cols = list_make4(id, ltree, vertex_tbl, phys_map);
    cols = lappend(cols, annotations_tbl);
    cols = lappend(cols, annotation_map);
    cols = lappend(cols, is_primary);


    return execute_internal_create_table("neopostgraph", meta_tbl_name, cols, NULL);
}

void create_metadata_btree_index(char *tbl_name) {
    /* THE FIX: Added NULL as the 8th argument for where_clause */
    execute_internal_create_index("neopostgraph", tbl_name, psprintf("%s_btree_idx", tbl_name), 
                                  "id", "btree", true, true, NULL);
}

void create_metadata_gist_index(char *tbl_name) {
    /* Create the AST for: WHERE is_primary */
    ColumnRef *where_cr = makeNode(ColumnRef);
    where_cr->fields = list_make1(makeString("is_primary"));
    where_cr->location = -1;

    /* Pass the where_clause to create a Partial GiST Index */
    execute_internal_create_index("neopostgraph", tbl_name, psprintf("%s_gist_idx", tbl_name), 
                                  "ltree", "gist", false, false, (Node *) where_cr);
}

Oid create_label_vertex_physical_mapping_table(char *tbl_name, Oid namespace) {
    return execute_internal_create_table(get_namespace_name(namespace), tbl_name,
                                         list_make4(makeColumnDef("v_itemptr", TIDOID, -1, InvalidOid),
                                                    makeColumnDef("e_tbl_id", REGCLASSOID, -1, InvalidOid),
                                                    makeColumnDef("e_itemptr", TIDOID, -1, InvalidOid),
                                                    makeColumnDef("a_itemptr", TIDOID, -1, InvalidOid)), 
                                         "np_mutable");
}

Oid create_annotation_schema_table(int graph_id, Oid namespace) {
    char *tbl_name = psprintf("np_annotation_schema_%d", graph_id);
    
    ColumnDef *id = makeColumnDef("id", INT4OID, -1, InvalidOid);
    id->constraints = list_make1(build_not_null_constraint());
    ColumnDef *schema = makeColumnDef("schema", TEXTARRAYOID, -1, InvalidOid);

    return execute_internal_create_table(get_namespace_name(namespace), tbl_name, 
                                         list_make2(id, schema), "entity_store");
}

Oid create_annotation_schema_phys_map_table(int graph_id, Oid namespace) {
    char *tbl_name = psprintf("np_annotation_schema_phys_map_%d", graph_id);
    return execute_internal_create_table(get_namespace_name(namespace), tbl_name,
                                         list_make4(makeColumnDef("v_itemptr", TIDOID, -1, InvalidOid),
                                                    makeColumnDef("e_tbl_id", REGCLASSOID, -1, InvalidOid),
                                                    makeColumnDef("e_itemptr", TIDOID, -1, InvalidOid),
                                                    makeColumnDef("a_itemptr", TIDOID, -1, InvalidOid)), 
                                         "np_mutable");
}

int32 
create_vlabel_internal(const char *graph_name, const char *label_str, 
                       ArrayType *annot_array, const char *namespace_name)
{
    Oid namespace;
    
    /* 1. Resolve Namespace */
    if (namespace_name == NULL) {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("create_vlabel requires a search path when namespace is not specified")));
        namespace = linitial_oid(search_path);
    } else if (!OidIsValid(namespace = get_namespace_oid(namespace_name, true))) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("namespace \"%s\" does not exist", namespace_name)));
    }

    /* 2. Validate Label */
    if (strchr(label_str, '.') != NULL) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Base labels cannot contain dots. Use merge_vlabels to build hierarchies.")));
    }

    /* 3. Fetch Cache & Register */
    graph_cache_data *entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!entry) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
                errmsg("graph \"%s\" does not exist \"%s\".", graph_name, get_namespace_name(namespace))));
    }

    register_and_validate_labels(entry->id, (char *)label_str, annot_array);

    /* 4. Prepare Metadata */
    Datum label_ltree = DirectFunctionCall2(ltree_addltree,
        DirectFunctionCall1(ltree_in, CStringGetDatum(CATALOG_LTREE_ROOT_LABEL)),
        DirectFunctionCall1(ltree_in, CStringGetDatum(label_str))
    );

    int byte_allocation_size = 0;
    Datum annot_map_datum = (Datum)0;
    if (annot_array) {
        annot_map_datum = PointerGetDatum(annot_array);
        byte_allocation_size = (ArrayGetNItems(ARR_NDIM(annot_array), ARR_DIMS(annot_array)) + 7) / 8;
    }

    Oid label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(entry->vertex_id_seq)));

    /* 5. Delegate all table and sequence creation */
    build_vertex_label_infrastructure(
        entry->id, label_id, namespace,
        label_ltree, byte_allocation_size, annot_map_datum, annot_array,
        entry->annot_schema_tbl, entry->annot_schema_phys_map
    );

    ereport(NOTICE, (errmsg("vlabel \"%s\" has been created", label_str)));
    return label_id;
}

static void
register_and_validate_labels(int graph_id, char *struct_label, ArrayType *annot_array)
{
    char *cat_name = psprintf("np_label_catalog_%d", graph_id);
    char *idx_name = psprintf("np_label_catalog_%d_idx", graph_id);

    Relation cat_rel = table_open(np_relation_id(cat_name, "table"), RowExclusiveLock);
    Relation idx_rel = index_open(np_relation_id(idx_name, "index"), RowExclusiveLock);

    /* 1. Register Structural Label */
    enforce_and_insert_label_catalog(cat_rel, idx_rel, struct_label, 's');

    /* 2. Register Annotation Labels */
    if (annot_array) {
        Datum *datums;
        bool *nulls;
        int count;
        
        deconstruct_array(annot_array, TEXTOID, -1, false, 'i', &datums, &nulls, &count);
        
        for (int i = 0; i < count; i++) {
            if (nulls[i]) continue;
            char *annot_str = TextDatumGetCString(datums[i]);
            
            /* Prevent self-collision in the exact same command */
            if (strcmp(annot_str, struct_label) == 0) {
                index_close(idx_rel, RowExclusiveLock);
                table_close(cat_rel, RowExclusiveLock);
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Label '%s' cannot be passed as both structural and annotation", struct_label)));
            }
            
            enforce_and_insert_label_catalog(cat_rel, idx_rel, annot_str, 'a');
        }
    }

    index_close(idx_rel, RowExclusiveLock);
    table_close(cat_rel, RowExclusiveLock);
}

static ArrayType *merge_and_dedupe_text_arrays(ArrayType *arr1, ArrayType *arr2)
{
    if (!arr1 && !arr2) return NULL;
    if (!arr1) return arr2;
    if (!arr2) return arr1;

    Datum *d1, *d2;
    bool *n1, *n2;
    int c1, c2;

    deconstruct_array(arr1, TEXTOID, -1, false, 'i', &d1, &n1, &c1);
    deconstruct_array(arr2, TEXTOID, -1, false, 'i', &d2, &n2, &c2);

    Datum *d_out = palloc((c1 + c2) * sizeof(Datum));
    int c_out = 0;

    for (int i = 0; i < c1; i++) {
        if (n1[i]) continue;
        d_out[c_out++] = d1[i];
    }

    for (int i = 0; i < c2; i++) {
        if (n2[i]) continue;
        char *s2 = TextDatumGetCString(d2[i]);
        bool duplicate = false;
        
        for (int j = 0; j < c_out; j++) {
            char *s1 = TextDatumGetCString(d_out[j]);
            if (strcmp(s1, s2) == 0) {
                duplicate = true;
                break;
            }
        }
        
        if (!duplicate) d_out[c_out++] = d2[i];
    }

    if (c_out == 0) return NULL;
    return construct_array(d_out, c_out, TEXTOID, -1, false, 'i');
}

int32 
merge_vlabels_internal(const char *graph_name, int32 parent_id, int32 child_id, const char *namespace_name) 
{
    Oid namespace;
    
    /* 1. Resolve Namespace */
    if (namespace_name == NULL) {
        List *search_path = fetch_search_path(false);
        if (list_length(search_path) < 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Requires a search path when namespace is not specified")));
        namespace = linitial_oid(search_path);
    } else if (!OidIsValid(namespace = get_namespace_oid(namespace_name, true))) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("namespace does not exist")));
    }

    /* 2. Fetch Graph Cache */
    graph_cache_data *entry = search_graph_name_namespace_cache(graph_name, namespace);
    if (!entry) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), errmsg("graph does not exist")));
    }

    /* 3. Fetch Parent and Child Labels via Snapshot Scan */
    PushActiveSnapshot(GetLatestSnapshot());
    
    Relation meta_rel = table_open(np_relation_id(psprintf("np_vertex_label_%d", entry->id), "table"), AccessShareLock);
    SysScanDesc scan = systable_beginscan(meta_rel, InvalidOid, false, GetActiveSnapshot(), 0, NULL);
    HeapTuple tuple;
    
    Datum parent_ltree_datum = (Datum)0, child_ltree_datum = (Datum)0;
    ArrayType *parent_annot = NULL, *child_annot = NULL;
    bool found_parent = false, found_child = false;

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isnull;
        int32 current_id = DatumGetInt32(heap_getattr(tuple, 1, RelationGetDescr(meta_rel), &isnull));
        
        if (current_id == parent_id || current_id == child_id) {
            Datum ltree_val = heap_getattr(tuple, 2, RelationGetDescr(meta_rel), &isnull);
            Datum annot_val = heap_getattr(tuple, 9, RelationGetDescr(meta_rel), &isnull); 
            
            if (current_id == parent_id) {
                /* DETOAST AND DEEP COPY to avoid Use-After-Free segfaults */
                parent_ltree_datum = PointerGetDatum(PG_DETOAST_DATUM_COPY(ltree_val));
                if (!isnull) parent_annot = DatumGetArrayTypePCopy(annot_val);
                found_parent = true;
            } else {
                /* DETOAST AND DEEP COPY */
                child_ltree_datum = PointerGetDatum(PG_DETOAST_DATUM_COPY(ltree_val));
                if (!isnull) child_annot = DatumGetArrayTypePCopy(annot_val);
                found_child = true;
            }
        }
    }
    systable_endscan(scan);
    table_close(meta_rel, AccessShareLock);
    
    PopActiveSnapshot();

    if (!found_parent || !found_child) {
        ereport(ERROR, (errmsg("Failed to find parent or child label in metadata catalog")));
    }

    /* 4. Construct Merged LTree String */
    char *parent_str = DatumGetCString(DirectFunctionCall1(ltree_out, parent_ltree_datum));
    char *child_str  = DatumGetCString(DirectFunctionCall1(ltree_out, child_ltree_datum));

    if (strncmp(parent_str, CATALOG_LTREE_ROOT_LABEL ".", strlen(CATALOG_LTREE_ROOT_LABEL) + 1) == 0) {
        parent_str += strlen(CATALOG_LTREE_ROOT_LABEL) + 1;
    } else if (strcmp(parent_str, CATALOG_LTREE_ROOT_LABEL) == 0) {
        parent_str += strlen(CATALOG_LTREE_ROOT_LABEL);
    }

    if (strncmp(child_str, CATALOG_LTREE_ROOT_LABEL ".", strlen(CATALOG_LTREE_ROOT_LABEL) + 1) == 0) {
        child_str += strlen(CATALOG_LTREE_ROOT_LABEL) + 1;
    } else if (strcmp(child_str, CATALOG_LTREE_ROOT_LABEL) == 0) {
        child_str += strlen(CATALOG_LTREE_ROOT_LABEL);
    }

    char *merged_ltree_str;
    if (parent_str[0] == '\0' && child_str[0] == '\0') merged_ltree_str = pstrdup(CATALOG_LTREE_ROOT_LABEL);
    else if (parent_str[0] == '\0') merged_ltree_str = psprintf("%s.%s", CATALOG_LTREE_ROOT_LABEL, child_str);
    else if (child_str[0] == '\0') merged_ltree_str = psprintf("%s.%s", CATALOG_LTREE_ROOT_LABEL, parent_str);
    else merged_ltree_str = psprintf("%s.%s.%s", CATALOG_LTREE_ROOT_LABEL, parent_str, child_str);

    Datum merged_ltree_datum = DirectFunctionCall1(ltree_in, CStringGetDatum(merged_ltree_str));
    
    /* 5. Handle Annotations */
    ArrayType *merged_array = merge_and_dedupe_text_arrays(parent_annot, child_annot);
    Datum merged_array_datum = (merged_array != NULL) ? PointerGetDatum(merged_array) : (Datum)0;

    int byte_allocation_size = 0;
    if (merged_array != NULL) byte_allocation_size = (ArrayGetNItems(ARR_NDIM(merged_array), ARR_DIMS(merged_array)) + 7) / 8;

    /* 6. Delegate Infrastructure Generation */
    Oid new_label_id = DatumGetObjectId(DirectFunctionCall1(nextval_oid, ObjectIdGetDatum(entry->vertex_id_seq)));
    
    build_vertex_label_infrastructure(
        entry->id, new_label_id, namespace,
        merged_ltree_datum, byte_allocation_size, merged_array_datum, merged_array,
        entry->annot_schema_tbl, entry->annot_schema_phys_map
    );

    ereport(NOTICE, (errmsg("Merged vlabel \"%s\" has been created", merged_ltree_str)));
    
    return new_label_id;
}

static Oid 
execute_internal_create_table(const char *namespace_name, const char *tbl_name, List *table_elts, const char *access_method)
{
    CreateStmt *create_stmt = makeNode(CreateStmt);
    create_stmt->relation = makeRangeVar(pstrdup(namespace_name), pstrdup(tbl_name), -1);
    create_stmt->tableElts = table_elts;
    create_stmt->accessMethod = access_method ? pstrdup(access_method) : NULL;
    create_stmt->inhRelations = NIL;
    create_stmt->partbound = NULL;
    create_stmt->ofTypename = NULL;
    create_stmt->constraints = NIL;
    create_stmt->options = NIL;
    create_stmt->oncommit = ONCOMMIT_NOOP;
    create_stmt->tablespacename = NULL;
    create_stmt->if_not_exists = false;

    PlannedStmt *wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)create_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE TABLE command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver, NULL);

    CommandCounterIncrement();
    return get_relname_relid(tbl_name, get_namespace_oid(namespace_name, false));
}

static void 
execute_internal_create_index(const char *namespace_name, const char *tbl_name, const char *idx_name, 
                              const char *col_name, const char *access_method, bool is_unique, bool is_primary, Node *where_clause)
{
    IndexStmt *idx_stmt = makeNode(IndexStmt);
    idx_stmt->idxname = pstrdup(idx_name);
    idx_stmt->relation = makeRangeVar(pstrdup(namespace_name), pstrdup(tbl_name), -1);

    IndexElem *elem = makeNode(IndexElem);
    elem->name = pstrdup(col_name);

    idx_stmt->accessMethod = pstrdup(access_method);
    idx_stmt->tableSpace = NULL;
    idx_stmt->indexParams = list_make1(elem);
    idx_stmt->indexIncludingParams = NIL;
    idx_stmt->options = NIL;
    
    /* THE FIX: Inject the optional partial index condition */
    idx_stmt->whereClause = where_clause;
    
    idx_stmt->excludeOpNames = NIL;
    idx_stmt->idxcomment = NULL;
    idx_stmt->indexOid = InvalidOid;
    
    idx_stmt->unique = is_unique;
    idx_stmt->primary = is_primary;
    idx_stmt->isconstraint = is_unique;
    idx_stmt->concurrent = false;
    idx_stmt->deferrable = false;
    idx_stmt->initdeferred = false;
    idx_stmt->if_not_exists = false;
    idx_stmt->reset_default_tblspc = false;

    PlannedStmt *wrapper = makeNode(PlannedStmt);
    wrapper->commandType = CMD_UTILITY;
    wrapper->canSetTag = false;
    wrapper->utilityStmt = (Node *)idx_stmt;
    wrapper->stmt_location = -1;
    wrapper->stmt_len = 0;

    ProcessUtility(wrapper, "(generated CREATE INDEX command)", false,
                   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver, NULL);

    CommandCounterIncrement();
}
