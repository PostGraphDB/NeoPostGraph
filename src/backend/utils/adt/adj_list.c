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

#include "access/transam.h"
#include "access/genam.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "utils/adj_list.h"
/*
 * adj_list is an internal type that is meant to help NeoPostGraph store
 * edge information for a vertex. This output function is used for debugging
 * purposes and shouldn't be make visible to the user. 
 */

/*
 * adj_list_out
 * Converts the internal AdjList varlena into a human-readable C string.
 */
PG_FUNCTION_INFO_V1(adj_list_out);
Datum
adj_list_out(PG_FUNCTION_ARGS)
{
    AdjList *list = DATUM_GET_ADJ_LIST(PG_GETARG_DATUM(0));
    StringInfoData str;
    
    initStringInfo(&str);
    appendStringInfoChar(&str, '[');
    
    for (int i = 0; i < list->nitems; i++)
    {
        AdjListMember *member = &list->data[i];
        if (i > 0)
            appendStringInfoString(&str, ", ");
        
        /* 
         * U64FromFullTransactionId safely extracts the uint64 value 
         * from the FullTransactionId struct.
         */
        appendStringInfo(&str, 
            "{\"edge_id\": %ld, \"edge_lid\": %d, \"dir\": %d, "
            "\"other_id\": %ld, \"other_lid\": %d, "
            "\"xmin\": %lu, \"xmax\": %lu, \"cmin\": %u, \"cmax\": %u, \"flags\": %u}",
            member->edge_id, member->edge_lid, member->dir,
            member->other_id, member->other_lid,
            U64FromFullTransactionId(member->xmin), 
            U64FromFullTransactionId(member->xmax), 
            member->cmin, member->cmax, member->flags);
    }
    
    appendStringInfoChar(&str, ']');
    
    PG_FREE_IF_COPY(list, 0);
        

    PG_RETURN_CSTRING(str.data);
}

/*
 * adj_list_in
 * Dummy input function.
 */
PG_FUNCTION_INFO_V1(adj_list_in);
Datum
adj_list_in(PG_FUNCTION_ARGS)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("NeoPostGraph: adj_list string input is not currently supported")));
    PG_RETURN_NULL();
}