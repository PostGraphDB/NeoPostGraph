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

#include "access/tableam.h" 
#include "fmgr.h"
#include "utils/guc.h"
#include "utils/elog.h"

#include "utils/vertex.h"
#include "access/map.h"

PG_MODULE_MAGIC;

void _PG_init(void);

void _PG_init(void)
{

    DefineCustomBoolVariable(
        "neopostgraph.show_dictionary_keys",
        "Enable/disable showing dictionary keys in vertex",
        "The Vertex Output routine will show the underlying array with the dictionary key values added when enable", // long description (can be NULL)
         &show_dictionary_keys,
        true,
        PGC_USERSET,
        0,
        NULL,
        assign_show_dictionary_keys,
        NULL
    );
    DefineCustomBoolVariable(
        "neopostgraph.show_dictionary_nulls",
        "Enable/disable showing dictionary keys in vertex",
        "The Vertex Output routine will show the underlying array with the dictionary key values added when enable", // long description (can be NULL)
         &show_dictionary_nulls,
        false,
        PGC_USERSET,
        0,
        NULL,
        assign_show_dictionary_nulls,
        NULL
    );
    MarkGUCPrefixReserved("neopostgraph");

    ereport(LOG, "PostGraph extension initialized");
}

void _PG_fini(void);

void _PG_fini(void)
{
    ereport(LOG, "PostGraph extension uninitialized");
}
