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

// graph_cache_data contains the same fields that ag_graph catalog table has
typedef struct graph_cache_data
{
    Oid id;
    NameData name;
    Oid namespace;
    Oid vertex_id_seq;
} graph_cache_data;

const graph_cache_data *search_graph_name_namespace_cache(const char *name, Oid namespace);

#endif