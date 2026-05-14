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

#include "utils/gtype_ext.h"
#include "utils/gtype.h"

/* define the type and size of the agt_header */
#define GT_HEADER_TYPE uint32
#define GT_HEADER_SIZE sizeof(GT_HEADER_TYPE)

static short np_serialize_header(StringInfo buffer, uint32 type)
{
    short padlen;
    int offset;

    padlen = pad_buffer_to_int(buffer);
    offset = reserve_from_buffer(buffer, GT_HEADER_SIZE);
    *((GT_HEADER_TYPE *)(buffer->data + offset)) = type;

    return padlen;
}

/*
 * Function serializes the data into the buffer provided.
 * Returns false if the type is not defined. Otherwise, true.
 */
bool np_serialize_extended_type(StringInfo buffer, gtentry *gtentry,
                                gtype_value *scalar_val)
{
    int numlen;
    int offset;
    short padlen;

    switch (scalar_val->type)
    {
    case GTV_INTEGER:
        padlen = np_serialize_header(buffer, GT_HEADER_INTEGER);

        /* copy in the int_value data */
        numlen = sizeof(int64);
        offset = reserve_from_buffer(buffer, numlen);
        *((int64 *)(buffer->data + offset)) = scalar_val->val.int_value;

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;

    case GTV_FLOAT:
        padlen = np_serialize_header(buffer, GT_HEADER_FLOAT);

        /* copy in the float_value data */
        numlen = sizeof(scalar_val->val.float_value);
        offset = reserve_from_buffer(buffer, numlen);
        *((float8 *)(buffer->data + offset)) = scalar_val->val.float_value;

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    case GTV_TIMESTAMP:
        padlen = np_serialize_header(buffer, GT_HEADER_TIMESTAMP);

        offset = reserve_from_buffer(buffer, sizeof(int64));
        *((int64 *)(buffer->data + offset)) = scalar_val->val.int_value;

    	*gtentry = GTENTRY_IS_GTYPE | (sizeof(int64) + GT_HEADER_SIZE);
        break;
    case GTV_TIMESTAMPTZ:
        padlen = np_serialize_header(buffer, GT_HEADER_TIMESTAMPTZ);
        numlen = sizeof(int64);
        offset = reserve_from_buffer(buffer, numlen);
        *((int64 *)(buffer->data + offset)) = scalar_val->val.int_value;

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    case GTV_DATE:
        padlen = np_serialize_header(buffer, GT_HEADER_DATE);
        numlen = sizeof(int32);
        offset = reserve_from_buffer(buffer, numlen);
        *((int32 *)(buffer->data + offset)) = scalar_val->val.date;

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
	break;
    case GTV_TIME:
        padlen = np_serialize_header(buffer, GT_HEADER_TIME);
        numlen = sizeof(int64);
        offset = reserve_from_buffer(buffer, numlen);
        *((int64 *)(buffer->data + offset)) = scalar_val->val.int_value;

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    case GTV_TIMETZ:
        padlen = np_serialize_header(buffer, GT_HEADER_TIMETZ);

        /* copy in the timetz data */
        numlen = sizeof(TimeTzADT);
        offset = reserve_from_buffer(buffer, numlen);
        *((TimeADT *)(buffer->data + offset)) = scalar_val->val.timetz.time;
        *((int32 *)(buffer->data + offset + sizeof(TimeADT))) = scalar_val->val.timetz.zone;

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    case GTV_INTERVAL:
        padlen = np_serialize_header(buffer, GT_HEADER_INTERVAL);

        numlen = sizeof(TimeOffset) + (2 * sizeof(int32));
        offset = reserve_from_buffer(buffer, numlen);
        *((TimeOffset *)(buffer->data + offset)) = scalar_val->val.interval.time;

        *((int32 *)(buffer->data + offset + sizeof(TimeOffset))) = scalar_val->val.interval.day;
        *((int32 *)(buffer->data + offset + sizeof(TimeOffset) + sizeof(int32))) = scalar_val->val.interval.month;

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    case GTV_INET:
        padlen = np_serialize_header(buffer, GT_HEADER_INET);
        numlen = sizeof(char) * 22;
        offset = reserve_from_buffer(buffer, numlen);
        memcpy(buffer->data + offset, &scalar_val->val.inet, numlen);
        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
	break;
    case GTV_CIDR:
        padlen = np_serialize_header(buffer, GT_HEADER_CIDR);

        numlen = sizeof(char) * 22;
        offset = reserve_from_buffer(buffer, numlen);
        memcpy(buffer->data + offset, &scalar_val->val.inet, numlen);

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    case GTV_MAC:
        padlen = np_serialize_header(buffer, GT_HEADER_MAC);

        numlen = sizeof(char) * 6;
        offset = reserve_from_buffer(buffer, numlen);
        memcpy(buffer->data + offset, &scalar_val->val.mac, numlen);

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    case GTV_MAC8:
        padlen = np_serialize_header(buffer, GT_HEADER_MAC8);

        numlen = sizeof(char) * 8;
        offset = reserve_from_buffer(buffer, numlen);
        memcpy(buffer->data + offset, &scalar_val->val.mac, numlen);

        *gtentry = GTENTRY_IS_GTYPE | (padlen + numlen + GT_HEADER_SIZE);
        break;
    default:
        return false;
    }
    return true;
}

/*
 * Function deserializes the data from the buffer pointed to by base_addr.
 * NOTE: This function writes to the error log and exits for any UNKNOWN
 * GT_HEADER type.
 */
void np_deserialize_extended_type(char *base_addr, uint32 offset, gtype_value *result) {
    char *base = base_addr + INTALIGN(offset);
    GT_HEADER_TYPE agt_header = *((GT_HEADER_TYPE *)base);

    switch (agt_header)
    {
    case GT_HEADER_INTEGER:
        result->type = GTV_INTEGER;
        result->val.int_value = *((int64 *)(base + GT_HEADER_SIZE));
        break;

    case GT_HEADER_FLOAT:
        result->type = GTV_FLOAT;
        result->val.float_value = *((float8 *)(base + GT_HEADER_SIZE));
        break;
    case GT_HEADER_TIMESTAMP:
        result->type = GTV_TIMESTAMP;
        result->val.int_value = *((int64 *)(base + GT_HEADER_SIZE));
        break;
    case GT_HEADER_TIMESTAMPTZ:
        result->type = GTV_TIMESTAMPTZ;
        result->val.int_value = *((int64 *)(base + GT_HEADER_SIZE));
        break;
    case GT_HEADER_DATE:
        result->type = GTV_DATE;
        result->val.date = *((int32 *)(base + GT_HEADER_SIZE));
        break;
    case GT_HEADER_TIME:
        result->type = GTV_TIME;
        result->val.int_value = *((int64 *)(base + GT_HEADER_SIZE));
        break;
    case GT_HEADER_TIMETZ:
        result->type = GTV_TIMETZ;
        result->val.timetz.time = *((TimeADT*)(base + GT_HEADER_SIZE));
        result->val.timetz.zone = *((int32*)(base + GT_HEADER_SIZE + sizeof(TimeADT)));
        break;
    case GT_HEADER_INTERVAL:
        result->type = GTV_INTERVAL;
        result->val.interval.time =  *((TimeOffset *)(base + GT_HEADER_SIZE));
        result->val.interval.day =  *((int32 *)(base + GT_HEADER_SIZE + sizeof(TimeOffset)));
        result->val.interval.month =  *((int32 *)(base + GT_HEADER_SIZE + sizeof(TimeOffset) + sizeof(int32)));
        break;
    case GT_HEADER_INET:
        result->type = GTV_INET;
        memcpy(&result->val.inet, base + GT_HEADER_SIZE, sizeof(char) * 22);
        break;
    case GT_HEADER_CIDR:
        result->type = GTV_CIDR;
        memcpy(&result->val.inet, base + GT_HEADER_SIZE, sizeof(char) * 22);
        break;
    case GT_HEADER_MAC:
        result->type = GTV_MAC;
        memcpy(&result->val.mac, base + GT_HEADER_SIZE, sizeof(char) * 6);
        break;
    case GT_HEADER_MAC8:
        result->type = GTV_MAC8;
        memcpy(&result->val.mac, base + GT_HEADER_SIZE, sizeof(char) * 8);
        break;
    default:
        elog(ERROR, "Invalid AGT header value.");
    }
}
