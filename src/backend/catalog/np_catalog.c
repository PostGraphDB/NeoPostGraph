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

#include "catalog/namespace.h"
#include "catalog/pg_namespace_d.h"
#include "utils/lsyscache.h"

#include "catalog/np_catalog.h"
#include "catalog/np_graph.h"


Oid np_namespace_id(void)
{
    return get_namespace_oid("neopostgraph", false);
}

Oid np_relation_id(const char *name, const char *kind)
{
    Oid id;

    if (!OidIsValid(id = get_relname_relid(name, np_namespace_id())))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                        errmsg("%s \"%s\" does not exist", kind, name)));

    return id;
}
