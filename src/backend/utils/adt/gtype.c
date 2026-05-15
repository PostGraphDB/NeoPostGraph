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
#include "executor/nodeAgg.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/timestamp.h"

#include "utils/gtype.h"
#include "utils/gtype_parser.h"

static size_t check_string_length(size_t len);
static void gtype_in_object_start(void *pstate);
static void gtype_in_object_end(void *pstate);
static void gtype_in_array_start(void *pstate);
static void gtype_in_array_end(void *pstate);
static void gtype_in_object_field_start(void *pstate, char *fname, bool isnull);
static void escape_gtype(StringInfo buf, const char *str);
static void gtype_in_scalar(void *pstate, char *token, gtype_token_type tokentype, char *annotation);
static char *gtype_to_cstring_worker(StringInfo out, gtype_container *in, int estimated_len, bool indent);
static void add_indent(StringInfo out, bool indent, int level);
static Datum gtype_from_cstring(char *str, int len);
static void gtype_put_escaped_value(StringInfo out, gtype_value *scalar_val);

PG_FUNCTION_INFO_V1(gtype_in);
Datum gtype_in(PG_FUNCTION_ARGS) {
    char *str = PG_GETARG_CSTRING(0);

    PG_RETURN_DATUM(gtype_from_cstring(str, strlen(str)));
}

PG_FUNCTION_INFO_V1(gtype_out);
Datum gtype_out(PG_FUNCTION_ARGS) {
    gtype *gt = NP_GET_ARG_GTYPE_P(0);

    PG_RETURN_CSTRING(gtype_to_cstring(NULL, &gt->root, VARSIZE(gt)));
}

/*
 * gtype_from_cstring
 *
 * Turns gtype string into an gtype Datum.
 *
 * Uses the gtype parser (with hooks) to construct an gtype.
 */
static Datum gtype_from_cstring(char *str, int len)
{
    gtype_lex_context *lex = make_gtype_lex_context_cstring_len(str, len, true);

    gtype_in_state state;
    gtype_sem_action sem;
    memset(&state, 0, sizeof(state));
    memset(&sem, 0, sizeof(sem));

    sem.semstate = (void *)&state;
    sem.object_start = gtype_in_object_start;
    sem.array_start = gtype_in_array_start;
    sem.object_end = gtype_in_object_end;
    sem.array_end = gtype_in_array_end;
    sem.scalar = gtype_in_scalar;
    sem.object_field_start = gtype_in_object_field_start;

    parse_gtype(lex, &sem);

    return GTYPE_P_GET_DATUM(gtype_value_to_gtype(state.res));
}

gtype_value *gtype_value_from_cstring(char *str, int len)
{
    gtype_lex_context *lex = make_gtype_lex_context_cstring_len(str, len, true);

    gtype_in_state state;
    gtype_sem_action sem;
    memset(&state, 0, sizeof(state));
    memset(&sem, 0, sizeof(sem));

    sem.semstate = (void *)&state;
    sem.object_start = gtype_in_object_start;
    sem.array_start = gtype_in_array_start;
    sem.object_end = gtype_in_object_end;
    sem.array_end = gtype_in_array_end;
    sem.scalar = gtype_in_scalar;
    sem.object_field_start = gtype_in_object_field_start;

    parse_gtype(lex, &sem);

    return state.res;
}

static size_t check_string_length(const size_t len) {
    if (len > GTENTRY_OFFLENMASK)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("string too long to represent as gtype string"),
                 errdetail("Due to an implementation restriction, gtype strings cannot exceed %d bytes.", GTENTRY_OFFLENMASK)));

    return len;
}

static void gtype_in_object_start(void *pstate) {
    gtype_in_state *_state = (gtype_in_state *)pstate;

    _state->res = push_gtype_value(&_state->parse_state, WGT_BEGIN_OBJECT, NULL);
}

static void gtype_in_object_end(void *pstate) {
    gtype_in_state *_state = (gtype_in_state *)pstate;

    _state->res = push_gtype_value(&_state->parse_state, WGT_END_OBJECT, NULL);
}

static void gtype_in_array_start(void *pstate) {
    gtype_in_state *_state = (gtype_in_state *)pstate;

    _state->res = push_gtype_value(&_state->parse_state, WGT_BEGIN_ARRAY, NULL);
}

static void gtype_in_array_end(void *pstate) {
    gtype_in_state *_state = (gtype_in_state *)pstate;

    _state->res = push_gtype_value(&_state->parse_state, WGT_END_ARRAY, NULL);
}

static void gtype_in_object_field_start(void *pstate, char *fname, bool isnull) {
    gtype_in_state *_state = (gtype_in_state *)pstate;
    gtype_value v;

    Assert(fname != NULL);
    v.type = GTV_STRING;
    v.val.string.len = check_string_length(strlen(fname));
    v.val.string.val = fname;

    _state->res = push_gtype_value(&_state->parse_state, WGT_KEY, &v);
}

static void gtype_put_escaped_value(StringInfo out, gtype_value *scalar_val)
{
    switch (scalar_val->type)
    {
    case GTV_NULL:
        appendBinaryStringInfo(out, "null", 4);
        break;
    case GTV_STRING:
        escape_gtype(out, pnstrdup(scalar_val->val.string.val, scalar_val->val.string.len));
        break;
    case GTV_NUMERIC:
        appendStringInfoString(
            out, DatumGetCString(DirectFunctionCall1(numeric_out, PointerGetDatum(scalar_val->val.numeric))));
        break;
    case GTV_INTEGER:
        appendStringInfoString(
            out, DatumGetCString(DirectFunctionCall1(int8out, Int64GetDatum(scalar_val->val.int_value))));
        break;
    case GTV_FLOAT:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(float8out, Float8GetDatum(scalar_val->val.float_value))));
        break;
    case GTV_TIMESTAMP:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(timestamp_out, TimestampGetDatum(scalar_val->val.int_value))));
        break;
    case GTV_TIMESTAMPTZ:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(timestamptz_out, TimestampGetDatum(scalar_val->val.int_value))));
        break;
    case GTV_DATE:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(date_out, DateADTGetDatum(scalar_val->val.date))));
        break;
    case GTV_TIME:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(time_out, TimeADTGetDatum(scalar_val->val.int_value))));
        break;
    case GTV_TIMETZ:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(timetz_out, TimeTzADTPGetDatum(&scalar_val->val.timetz))));
        break;
    case GTV_INTERVAL:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(interval_out, IntervalPGetDatum(&scalar_val->val.interval))));
        break;
    case GTV_INET:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(inet_out, InetPGetDatum(&scalar_val->val.inet))));
        break;
    case GTV_CIDR:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(cidr_out, InetPGetDatum(&scalar_val->val.inet))));
        break;
    case GTV_MAC:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(macaddr_out, MacaddrPGetDatum(&scalar_val->val.mac))));
        break;
    case GTV_MAC8:
        appendStringInfoString(out, 
            DatumGetCString(DirectFunctionCall1(macaddr8_out, MacaddrPGetDatum(&scalar_val->val.mac))));
        break;  
    case GTV_BOOL:
        if (scalar_val->val.boolean)
            appendBinaryStringInfo(out, "true", 4);
        else
            appendBinaryStringInfo(out, "false", 5);
        break;
    default:
        elog(ERROR, "unknown gtype scalar type");
    }
}

/*
 * Produce an gtype string literal, properly escaping characters in the text.
 */
static void escape_gtype(StringInfo buf, const char *str) {
    const char *p;

    appendStringInfoCharMacro(buf, '"');
    for (p = str; *p; p++)
    {
        switch (*p)
        {
        case '\b':
            appendStringInfoString(buf, "\\b");
            break;
        case '\f':
            appendStringInfoString(buf, "\\f");
            break;
        case '\n':
            appendStringInfoString(buf, "\\n");
            break;
        case '\r':
            appendStringInfoString(buf, "\\r");
            break;
        case '\t':
            appendStringInfoString(buf, "\\t");
            break;
        case '"':
            appendStringInfoString(buf, "\\\"");
            break;
        case '\\':
            appendStringInfoString(buf, "\\\\");
            break;
        default:
            if ((unsigned char)*p < ' ')
                appendStringInfo(buf, "\\u%04x", (int)*p);
            else
                appendStringInfoCharMacro(buf, *p);
            break;
        }
    }
    appendStringInfoCharMacro(buf, '"');
}

/*
 * For gtype we always want the de-escaped value - that's what's in token
 */
static void gtype_in_scalar(void *pstate, char *token, gtype_token_type tokentype, char *annotation) {
    gtype_in_state *_state = (gtype_in_state *)pstate;
    gtype_value v;
    Datum numd;

    /*
     * Process the scalar typecast annotations, if present, but not if the
     * argument is a null. Typecasting a null is a null.
     */
    if (annotation != NULL && tokentype != GTYPE_TOKEN_NULL) {
        if (pg_strcasecmp(annotation, "numeric") == 0)
            tokentype = GTYPE_TOKEN_NUMERIC;
        else if (pg_strcasecmp(annotation, "integer") == 0 || pg_strcasecmp(annotation, "int") == 0)
            tokentype = GTYPE_TOKEN_INTEGER;
        else if (pg_strcasecmp(annotation, "float") == 0)
            tokentype = GTYPE_TOKEN_FLOAT;
        else if (pg_strcasecmp(annotation, "timestamp") == 0)
            tokentype = GTYPE_TOKEN_TIMESTAMP;
        else if (pg_strcasecmp(annotation, "timestamptz") == 0)
            tokentype = GTYPE_TOKEN_TIMESTAMPTZ;
	    else if (pg_strcasecmp(annotation, "date") == 0)
            tokentype = GTYPE_TOKEN_DATE;
        else if (pg_strcasecmp(annotation, "time") == 0)
            tokentype = GTYPE_TOKEN_TIME;
        else if (pg_strcasecmp(annotation, "timetz") == 0)
            tokentype = GTYPE_TOKEN_TIMETZ;
	    else if (pg_strcasecmp(annotation, "interval") == 0)
            tokentype = GTYPE_TOKEN_INTERVAL;
        else if (pg_strcasecmp(annotation, "inet") == 0)
            tokentype = GTYPE_TOKEN_INET;
        else if (pg_strcasecmp(annotation, "cidr") == 0)
            tokentype = GTYPE_TOKEN_CIDR;
        else if (pg_strcasecmp(annotation, "macaddr") == 0)
            tokentype = GTYPE_TOKEN_MACADDR;
        else if (pg_strcasecmp(annotation, "macaddr8") == 0)
            tokentype = GTYPE_TOKEN_MACADDR8;
        else if (pg_strcasecmp(annotation, "text") == 0)
            tokentype = GTYPE_TOKEN_STRING;
	    else
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("invalid annotation value for scalar")));
    }

    switch (tokentype)
    {
    case GTYPE_TOKEN_STRING:
        Assert(token != NULL);
        v.type = GTV_STRING;
        v.val.string.len = check_string_length(strlen(token));
        v.val.string.val = token;
        break;
    case GTYPE_TOKEN_INTEGER:
        Assert(token != NULL);
        v.type = GTV_INTEGER;
        v.val.int_value = pg_strtoint64_safe(token, NULL);
        break;
    case GTYPE_TOKEN_FLOAT:
        Assert(token != NULL);
        v.type = GTV_FLOAT;
        v.val.float_value = float8in_internal(token, NULL, "double precision", token, NULL);
        break;
    case GTYPE_TOKEN_NUMERIC:
        Assert(token != NULL);
        v.type = GTV_NUMERIC;
        numd = DirectFunctionCall3(numeric_in, CStringGetDatum(token), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
        v.val.numeric = DatumGetNumeric(numd);
        break;
    case GTYPE_TOKEN_TIMESTAMP:
        Assert(token != NULL);
        v.type = GTV_TIMESTAMP;
        v.val.int_value = DatumGetInt64(DirectFunctionCall3(timestamp_in, CStringGetDatum(token), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
        break;
    case GTYPE_TOKEN_TIMESTAMPTZ:
        v.type = GTV_TIMESTAMPTZ;
        v.val.int_value = DatumGetInt64(DirectFunctionCall3(timestamptz_in, CStringGetDatum(token), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
        break;
    case GTYPE_TOKEN_DATE:
        v.type = GTV_DATE;
        v.val.date = DatumGetInt32(DirectFunctionCall1(date_in, CStringGetDatum(token)));
        break;
    case GTYPE_TOKEN_TIME:
        v.type = GTV_TIME;
        v.val.int_value = DatumGetInt64(DirectFunctionCall3(time_in, CStringGetDatum(token), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
	    break;
    case GTYPE_TOKEN_TIMETZ:
        v.type = GTV_TIMETZ;
        TimeTzADT *timetz = DatumGetTimeTzADTP(DirectFunctionCall3(timetz_in, CStringGetDatum(token), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
        v.val.timetz.time = timetz->time;
        v.val.timetz.zone = timetz->zone;
        break;
    case GTYPE_TOKEN_INTERVAL:
        {
        Interval *interval = DatumGetIntervalP(DirectFunctionCall3(interval_in, CStringGetDatum(token), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));

        v.type = GTV_INTERVAL;
        v.val.interval.time = interval->time;
        v.val.interval.day = interval->day;
        v.val.interval.month = interval->month;

        break;
        }
    case GTYPE_TOKEN_INET:
        {
        v.type = GTV_INET;
        inet *i = DatumGetInetPP(DirectFunctionCall1(inet_in, CStringGetDatum(token)));
	    memcpy(&v.val.inet, i, sizeof(char) * 22);
        break;
        }
    case GTYPE_TOKEN_CIDR:
        {
        v.type = GTV_CIDR;
        inet *i = DatumGetInetPP(DirectFunctionCall1(cidr_in, CStringGetDatum(token)));
        memcpy(&v.val.inet, i, sizeof(char) * 22);
        break;
        }
    case GTYPE_TOKEN_MACADDR:
        {
        v.type = GTV_MAC;
        macaddr *mac = DatumGetMacaddrP(DirectFunctionCall1(macaddr_in, CStringGetDatum(token)));
        memcpy(&v.val.mac, mac, sizeof(char) * 6);
        break;
        }
    case GTYPE_TOKEN_MACADDR8:
        {
        v.type = GTV_MAC8;
        macaddr8 *mac = DatumGetMacaddr8P(DirectFunctionCall1(macaddr8_in, CStringGetDatum(token)));
        memcpy(&v.val.mac, mac, sizeof(char) * 8);
        break;
        }
    case GTYPE_TOKEN_TRUE:
        v.type = GTV_BOOL;
        v.val.boolean = true;
        break;
    case GTYPE_TOKEN_FALSE:
        v.type = GTV_BOOL;
        v.val.boolean = false;
        break;
    case GTYPE_TOKEN_NULL:
        v.type = GTV_NULL;
        break;
    default:
        elog(ERROR, "invalid gtype token type");
    }

    if (_state->parse_state == NULL) {
        // single scalar 
        gtype_value va;

        va.type = GTV_ARRAY;
        va.val.array.raw_scalar = true;
        va.val.array.num_elems = 1;

        _state->res = push_gtype_value(&_state->parse_state, WGT_BEGIN_ARRAY, &va);
        _state->res = push_gtype_value(&_state->parse_state, WGT_ELEM, &v);
        _state->res = push_gtype_value(&_state->parse_state, WGT_END_ARRAY, NULL);
    } else {
        gtype_value *o = &_state->parse_state->cont_val;

        switch (o->type)
        {
        case GTV_ARRAY:
            _state->res = push_gtype_value(&_state->parse_state, WGT_ELEM, &v);
            break;
        case GTV_OBJECT:
            _state->res = push_gtype_value(&_state->parse_state, WGT_VALUE, &v);
            break;
        default:
            elog(ERROR, "unexpected parent of nested structure");
        }
    }
}

/*
 * gtype_to_cstring
 *     Converts gtype value to a C-string.
 *
 * If 'out' argument is non-null, the resulting C-string is stored inside the
 * StringBuffer.  The resulting string is always returned.
 *
 * A typical case for passing the StringInfo in rather than NULL is where the
 * caller wants access to the len attribute without having to call strlen, e.g.
 * if they are converting it to a text* object.
 */
char *gtype_to_cstring(StringInfo out, gtype_container *in, int estimated_len) {
    return gtype_to_cstring_worker(out, in, estimated_len, false);
}

/*
 * common worker for above two functions
 */
static char *gtype_to_cstring_worker(StringInfo out, gtype_container *in, int estimated_len, bool indent)
{
    bool first = true;
    gtype_iterator *it;
    gtype_value v;
    gtype_iterator_token type = WGT_DONE;
    int level = 0;
    bool redo_switch = false;

    // If we are indenting, don't add a space after a comma 
    int ispaces = indent ? 1 : 2;

    /*
     * Don't indent the very first item. This gets set to the indent flag at
     * the bottom of the loop.
     */
    bool use_indent = false;
    bool raw_scalar = false;
    bool last_was_key = false;

    if (out == NULL)
        out = makeStringInfo();

    enlargeStringInfo(out, (estimated_len >= 0) ? estimated_len : 64);

    it = gtype_iterator_init(in);

    while (redo_switch ||
           ((type = gtype_iterator_next(&it, &v, false)) != WGT_DONE))
    {
        redo_switch = false;
        switch (type)
        {
        case WGT_BEGIN_ARRAY:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);

            if (!v.val.array.raw_scalar) {
                add_indent(out, use_indent && !last_was_key, level);
                appendStringInfoCharMacro(out, '[');
            } else {
                raw_scalar = true;
            }

            first = true;
            level++;
            break;

        case WGT_BEGIN_OBJECT:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);

            add_indent(out, use_indent && !last_was_key, level);
            appendStringInfoCharMacro(out, '{');

            first = true;
            level++;
            break;
        case WGT_KEY:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);
            first = true;

            add_indent(out, use_indent, level);

            // gtype rules guarantee this is a string 
            gtype_put_escaped_value(out, &v);
            appendBinaryStringInfo(out, ": ", 2);

            type = gtype_iterator_next(&it, &v, false);
            if (type == WGT_VALUE) {
                first = false;
                gtype_put_escaped_value(out, &v);
            } else {
                Assert(type == WGT_BEGIN_OBJECT || type == WGT_BEGIN_ARRAY);

                /*
                 * We need to rerun the current switch() since we need to
                 * output the object which we just got from the iterator
                 * before calling the iterator again.
                 */
                redo_switch = true;
            }
            break;
        case WGT_ELEM:
            if (!first)
                appendBinaryStringInfo(out, ", ", ispaces);
            first = false;

            if (!raw_scalar)
                add_indent(out, use_indent, level);
            gtype_put_escaped_value(out, &v);
            break;
        case WGT_END_ARRAY:
            level--;
            if (!raw_scalar) {
                add_indent(out, use_indent, level);
                appendStringInfoCharMacro(out, ']');
            }
            first = false;
            break;
        case WGT_END_OBJECT:
            level--;
            add_indent(out, use_indent, level);
            appendStringInfoCharMacro(out, '}');
            first = false;
            break;
        default:
            elog(ERROR, "unknown gtype iterator token type");
        }
        use_indent = indent;
        last_was_key = redo_switch;
    }

    Assert(level == 0);

    return out->data;
}

static void add_indent(StringInfo out, bool indent, int level) {
    if (indent) {
        int i;

        appendStringInfoCharMacro(out, '\n');
        for (i = 0; i < level; i++)
            appendBinaryStringInfo(out, "    ", 4);
    }
}

