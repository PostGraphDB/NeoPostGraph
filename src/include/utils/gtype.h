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

/*
 * Declarations for gtype data type support.
 */

#ifndef NP_GTYPE_H
#define NP_GTYPE_H

#include "access/htup_details.h"
#include "c.h"
#include "datatype/timestamp.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "tsearch/ts_type.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/geo_decls.h"
#include "utils/multirangetypes.h"
#include "utils/numeric.h"
#include "utils/rangetypes.h"
#include "utils/syscache.h"
#include "catalog/pg_type.h"


typedef enum
{
    WGT_DONE,
    WGT_KEY,
    WGT_VALUE,
    WGT_ELEM,
    WGT_BEGIN_ARRAY,
    WGT_END_ARRAY,
    WGT_BEGIN_OBJECT,
    WGT_END_OBJECT
} gtype_iterator_token;

#define DATUM_GET_GTYPE_P(d) ((gtype *)PG_DETOAST_DATUM(d))
#define GTYPE_P_GET_DATUM(p) PointerGetDatum(p)
#define NP_GET_ARG_GTYPE_P(x) DATUM_GET_GTYPE_P(PG_GETARG_DATUM(x))
#define NP_RETURN_GTYPE_P(x) PG_RETURN_POINTER(x)

typedef struct gtype_pair gtype_pair;
typedef struct gtype_value gtype_value;

typedef uint32 gtentry;

#define GTENTRY_OFFLENMASK 0x0FFFFFFF
#define GTENTRY_TYPEMASK   0x70000000
#define GTENTRY_HAS_OFF    0x80000000

/* values stored in the type bits */
#define GTENTRY_IS_STRING     0x00000000
#define GTENTRY_IS_NUMERIC    0x10000000
#define GTENTRY_IS_BOOL_FALSE 0x20000000
#define GTENTRY_IS_BOOL_TRUE  0x30000000
#define GTENTRY_IS_NULL       0x40000000
#define GTENTRY_IS_CONTAINER  0x50000000 
#define GTENTRY_IS_GTYPE     0x60000000

/* Access macros.  Note possible multiple evaluations */
#define GTE_OFFLENFLD(agte_) \
    ((agte_)&GTENTRY_OFFLENMASK)
#define GTE_HAS_OFF(agte_) \
    (((agte_)&GTENTRY_HAS_OFF) != 0)
#define GTE_IS_STRING(agte_) \
    (((agte_)&GTENTRY_TYPEMASK) == GTENTRY_IS_STRING)
#define GTE_IS_NUMERIC(agte_) \
    (((agte_)&GTENTRY_TYPEMASK) == GTENTRY_IS_NUMERIC)
#define GTE_IS_CONTAINER(agte_) \
    (((agte_)&GTENTRY_TYPEMASK) == GTENTRY_IS_CONTAINER)
#define GTE_IS_NULL(agte_) \
    (((agte_)&GTENTRY_TYPEMASK) == GTENTRY_IS_NULL)
#define GTE_IS_BOOL_TRUE(agte_) \
    (((agte_)&GTENTRY_TYPEMASK) == GTENTRY_IS_BOOL_TRUE)
#define GTE_IS_BOOL_FALSE(agte_) \
    (((agte_)&GTENTRY_TYPEMASK) == GTENTRY_IS_BOOL_FALSE)
#define GTE_IS_BOOL(agte_) \
    (GTE_IS_BOOL_TRUE(agte_) || GTE_IS_BOOL_FALSE(agte_))
#define GTE_IS_GTYPE(agte_) \
    (((agte_)&GTENTRY_TYPEMASK) == GTENTRY_IS_GTYPE)

/* Macro for advancing an offset variable to the next gtentry */
#define GTE_ADVANCE_OFFSET(offset, agte) \
    do \
    { \
        gtentry agte_ = (agte); \
        if (GTE_HAS_OFF(agte_)) \
            (offset) = GTE_OFFLENFLD(agte_); \
        else \
            (offset) += GTE_OFFLENFLD(agte_); \
    } while (0)


#define GT_OFFSET_STRIDE 32

typedef struct gtype_container
{
    uint32 header;
    gtentry children[FLEXIBLE_ARRAY_MEMBER];
} gtype_container;


// flags for the header in gtype_container
#define GT_CMASK   0x07FFFFFF
#define GT_FSCALAR 0x10000000
#define GT_FOBJECT 0x20000000
#define GT_FARRAY  0x40000000
#define GT_FBINARY 0x80000000
#define GT_EXTENDED_COMPOSITE 0x08000000

// convenience macros for accessing an gtype_container struct 
#define GTYPE_CONTAINER_SIZE(agtc)          ((agtc)->header & GT_CMASK)
#define GTYPE_CONTAINER_IS_SCALAR(agtc)     (((agtc)->header & GT_FSCALAR) != 0)
#define GTYPE_CONTAINER_IS_OBJECT(agtc)     (((agtc)->header & GT_FOBJECT) != 0)
#define GTYPE_CONTAINER_IS_ARRAY(agtc)      (((agtc)->header & GT_FARRAY)  != 0)
#define GTYPE_CONTAINER_IS_BINARY(agtc)     (((agtc)->header & GT_FBINARY) != 0)
#define GTYPE_CONTAINER_IS_COMPOSITE(agtc)  (((agtc)->header & GT_EXTENDED_COMPOSITE) != 0)

typedef struct
{
    int32 vl_len_;
    gtype_container root;
} gtype;

// convenience macros for accessing the root container in gtype 
#define GT_ROOT_COUNT(agtp_) (*(uint32 *)VARDATA(agtp_) & GT_CMASK)
#define GT_ROOT_IS_SCALAR(agtp_) \
    ((*(uint32 *)VARDATA(agtp_) & GT_FSCALAR) != 0)
#define GT_ROOT_IS_OBJECT(agtp_) \
    ((*(uint32 *)VARDATA(agtp_) & GT_FOBJECT) != 0)
#define GT_ROOT_IS_ARRAY(agtp_) \
    ((*(uint32 *)VARDATA(agtp_) & GT_FARRAY) != 0)
#define GT_ROOT_IS_BINARY(agtp_) \
    ((*(uint32 *)VARDATA(agtp_) & GT_FBINARY) != 0)
#define GT_ROOT_BINARY_FLAGS(agtp_) \
    (*(uint32 *)VARDATA(agtp_) & GT_FBINARY_MASK)

// values for the GType header field to denote the stored data type
#define GT_HEADER_INTEGER          0x00000000
#define GT_HEADER_FLOAT            0x00000001
#define GT_HEADER_TIMESTAMP        0x00000002
#define GT_HEADER_TIMESTAMPTZ      0x00000003
#define GT_HEADER_DATE             0x00000004
#define GT_HEADER_TIME             0x00000005
#define GT_HEADER_TIMETZ           0x00000006
#define GT_HEADER_INTERVAL         0x00000007
#define GT_HEADER_INET             0x00000009
#define GT_HEADER_CIDR             0x0000000A
#define GT_HEADER_MAC              0x0000000B
#define GT_HEADER_MAC8             0x0000000C


enum gtype_value_type
{
    // Scalar types
    GTV_NULL = 0x0,
    GTV_STRING,
    GTV_NUMERIC,
    GTV_INTEGER,
    GTV_FLOAT,
    GTV_BOOL,
    GTV_TIMESTAMP,
    GTV_TIMESTAMPTZ,
    GTV_DATE,
    GTV_TIME,
    GTV_TIMETZ,
    GTV_INTERVAL,
    GTV_INET,
    GTV_CIDR,
    GTV_MAC,
    GTV_MAC8,
    // Composite types
    GTV_ARRAY = 0x10,
    GTV_OBJECT,
    // Binary (i.e. struct gtype) GTV_ARRAY/GTV_OBJECT
    GTV_BINARY
};

struct gtype_value
{
    enum gtype_value_type type;
    union {
        int64 int_value;
        float8 float_value;
        Numeric numeric;
        bool boolean;
	    Interval interval;
	    DateADT date;
	    TimeTzADT timetz;
	    inet inet;
	    macaddr mac;
        macaddr8 mac8;
        struct { int len; char *val; } string;
        struct { int num_elems; gtype_value *elems; bool raw_scalar; } array;
	    struct { int num_pairs; gtype_pair *pairs; } object;
	    struct { int len; gtype_container *data; } binary;
    } val;
};

#define IS_A_GTYPE_SCALAR(gtype_val) \
    ((gtype_val)->type >= GTV_NULL && (gtype_val)->type < GTV_ARRAY)

/*
 * Key/value pair within an Object.
 *
 * Pairs with duplicate keys are de-duplicated.  We store the originally
 * observed pair ordering for the purpose of removing duplicates in a
 * well-defined way (which is "last observed wins").
 */
struct gtype_pair
{
    gtype_value key; 
    gtype_value value;
    uint32 order;
};

/* Conversion state used when parsing gtype from text, or for type coercion */
typedef struct gtype_parse_state
{
    Size size;
    struct gtype_parse_state *next;
    gtype_value *last_updated_value;
    gtype_value cont_val;
} gtype_parse_state;

/*
 * gtype_iterator holds details of the type for each iteration. It also stores
 * an gtype varlena buffer, which can be directly accessed in some contexts.
 */
typedef enum
{
    GTI_ARRAY_START,
    GTI_ARRAY_ELEM,
    GTI_OBJECT_START,
    GTI_OBJECT_KEY,
    GTI_OBJECT_VALUE
} gt_iterator_state;

typedef struct gtype_iterator
{
    gtype_container *container;
    uint32 num_elems;
    bool is_scalar;
    gtentry *children;
    char *data_proper;
    int curr_index;
    uint32 curr_data_offset;
    uint32 curr_value_offset;
    gt_iterator_state state;
    struct gtype_iterator *parent;
} gtype_iterator;

// gtype parse state
typedef struct gtype_in_state {
    gtype_parse_state *parse_state;
    gtype_value *res;
} gtype_in_state;

// Support functions
int reserve_from_buffer(StringInfo buffer, int len);
short pad_buffer_to_int(StringInfo buffer);
uint32 get_gtype_offset(const gtype_container *agtc, int index);
uint32 get_gtype_length(const gtype_container *agtc, int index);
int compare_gtype_containers_orderability(gtype_container *a, gtype_container *b);
gtype_value *push_gtype_value(gtype_parse_state **pstate, gtype_iterator_token seq, gtype_value *agtval);
gtype_iterator *gtype_iterator_init(gtype_container *container);
gtype_iterator_token gtype_iterator_next(gtype_iterator **it, gtype_value *val, bool skip_nested);
gtype *gtype_value_to_gtype(gtype_value *val);

char *gtype_to_cstring(StringInfo out, gtype_container *in, int estimated_len);


#define GTYPEOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("gtype"), ObjectIdGetDatum(postgraph_namespace_id())))

#define GTYPEARRAYOID \
    (GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("_gtype"), ObjectIdGetDatum(postgraph_namespace_id())))

#endif
