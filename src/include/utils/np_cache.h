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

#ifndef NP_CACHE_H
#define NP_CACHE_H

#include "postgres.h"

#include "ltree.h"

#include "utils/dictionary.h"

// graph_cache_data contains the same fields that ag_graph catalog table has
typedef struct graph_cache_data
{
    Oid id;
    NameData name;
    Oid namespace;
    Oid vertex_labels;
    Oid vertex_id_seq;
    Oid edge_labels;
    Oid edge_id_seq;
} graph_cache_data;

typedef struct label_cache_data
{
    int id;
    int graph_id;
    ltree *label;
    Oid vertex_tbl;
    Oid     phys_map;
    Oid     arraylist;
    Oid     linked_list_meta;
    Oid     linked_list_seq;
} label_cache_data;

typedef struct vertex_dictionary_cache_data
{
    int id;
    dictionary *dict;
} vertex_dictionary_cache_data;

const graph_cache_data *search_graph_name_namespace_cache(const char *name, Oid namespace);
const label_cache_data *search_vertex_label_graph_id_label_id_cache(int graph_id, int label_id);
const label_cache_data *search_edge_label_graph_id_label_id_cache(int graph_id, int label_id);


const vertex_dictionary_cache_data *search_vertex_dictionary_cache(int graph_id, int label_id, int dictionary_id);


#endif
