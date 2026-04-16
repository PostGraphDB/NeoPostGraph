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

#ifndef NP_GRAPH_H
#define NP_GRAPH_H

#include "postgres.h"

#include "catalog/np_catalog.h"

#define np_graph_relation_id() np_relation_id("np_graph", "table")
#define np_graph_name_namespace_index_id() np_relation_id("np_graph_name_namespace_index", "index")

#endif