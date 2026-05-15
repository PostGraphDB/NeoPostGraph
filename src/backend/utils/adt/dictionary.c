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

#include "access/genam.h"
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "utils/gtype.h"
#include "utils/dictionary.h"

#include "../../../../../postgres/include/server/fmgr.h"
#include "../../../include/utils/gtype.h"

PG_FUNCTION_INFO_V1(dictionary_in);
Datum dictionary_in(PG_FUNCTION_ARGS) {
    char *str = PG_GETARG_CSTRING(0);
    gtype_value *val = gtype_value_from_cstring(str, strlen(str));


    if (val->type != GTV_ARRAY)
        ereport(ERROR, errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("invalid format for dictionary, expects and array"));

    gtype *gt = gtype_value_to_gtype(val);

    dictionary *dict = palloc(sizeof(dictionary) + VARSIZE(gt) - sizeof(uint32));
    dict->dictionary_id = 0;
    memcpy(&dict->array, &gt->root, VARSIZE(gt) - sizeof(uint32));

    SET_VARSIZE(dict, VARSIZE(gt) + VARHDRSZ + sizeof(uint64));

    NP_RETURN_DICTIONARY(dict);
}

PG_FUNCTION_INFO_V1(dictionary_build);
Datum dictionary_build(PG_FUNCTION_ARGS) {
    gtype *gt = NP_GET_ARG_GTYPE_P(1);
    dictionary *dict = palloc(sizeof(dictionary) + VARSIZE(gt) - sizeof(uint32));
    dict->dictionary_id = PG_GETARG_INT16(0);

    memcpy(&dict->array, &gt->root, VARSIZE(gt) - sizeof(uint32));

    SET_VARSIZE(dict, VARSIZE(gt) + VARHDRSZ + sizeof(uint64));

    NP_RETURN_DICTIONARY(dict);
}

PG_FUNCTION_INFO_V1(dictionary_out);
Datum dictionary_out(PG_FUNCTION_ARGS) {
    dictionary *dict = NP_GET_ARG_DICTIONARY(0);

    StringInfo out = makeStringInfo();
    enlargeStringInfo(out, 64);
    appendStringInfoString(out, "dict{");
    appendStringInfoString(out,
        DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(dict->dictionary_id))));
    appendStringInfoString(out, "}");

    PG_RETURN_CSTRING(gtype_to_cstring(out, &dict->array, 0));
}