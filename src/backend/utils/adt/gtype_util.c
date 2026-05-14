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

#include "access/hash.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "utils/gtype.h"
#include "utils/gtype_ext.h"

/*
 * Maximum number of elements in an array (or key/value pairs in an object).
 * This is limited by two things: the size of the gtentry array must fit
 * in MaxAllocSize, and the number of elements (or pairs) must fit in the bits
 * reserved for that in the gtype_container.header field.
 *
 * (The total size of an array's or object's elements is also limited by
 * GTENTRY_OFFLENMASK, but we're not concerned about that here.)
 */
#define GTYPE_MAX_ELEMS (Min(MaxAllocSize / sizeof(gtype_value), GT_CMASK))
#define GTYPE_MAX_PAIRS (Min(MaxAllocSize / sizeof(gtype_pair), GT_CMASK))

static void fill_gtype_value(gtype_container *container, int index,
                              char *base_addr, uint32 offset,
                              gtype_value *result);
static gtype *convert_to_gtype(gtype_value *val);
static void convert_gtype_value(StringInfo buffer, gtentry *header, gtype_value *val, int level);
static void convert_gtype_array(StringInfo buffer, gtentry *pheader, gtype_value *val, int level);
static void convert_gtype_object(StringInfo buffer, gtentry *pheader, gtype_value *val, int level);
static void convert_gtype_scalar(StringInfo buffer, gtentry *entry, gtype_value *scalar_val);
static void append_to_buffer(StringInfo buffer, const char *data, int len);
static void copy_to_buffer(StringInfo buffer, int offset, const char *data, int len);
static gtype_iterator *iterator_from_container(gtype_container *container, gtype_iterator *parent);
static gtype_iterator *free_and_get_parent(gtype_iterator *it);
static gtype_parse_state *push_state(gtype_parse_state **pstate);
static void append_key(gtype_parse_state *pstate, gtype_value *string);
static void append_value(gtype_parse_state *pstate, gtype_value *scalar_val);
static void append_element(gtype_parse_state *pstate, gtype_value *scalar_val);
static int length_compare_gtype_string_value(const void *a, const void *b);
static int length_compare_gtype_pair(const void *a, const void *b, void *binequal);
static gtype_value *push_gtype_value_scalar(gtype_parse_state **pstate,
                                              gtype_iterator_token seq,
                                              gtype_value *scalar_val);
static void uniqueify_gtype_object(gtype_value *object);
gtype *gtype_value_to_gtype(gtype_value *val)
{
    gtype *out;

    if (IS_A_GTYPE_SCALAR(val)) {
        // Scalar value 
        gtype_parse_state *pstate = NULL;
        gtype_value *res;
        gtype_value scalar_array;

        scalar_array.type = GTV_ARRAY;
        scalar_array.val.array.raw_scalar = true;
        scalar_array.val.array.num_elems = 1;

        push_gtype_value(&pstate, WGT_BEGIN_ARRAY, &scalar_array);
        push_gtype_value(&pstate, WGT_ELEM, val);
        res = push_gtype_value(&pstate, WGT_END_ARRAY, NULL);

        out = convert_to_gtype(res);
    } else if (val->type == GTV_OBJECT || val->type == GTV_ARRAY) {
        out = convert_to_gtype(val);
    } else {
        Assert(val->type == GTV_BINARY);
        out = palloc(VARHDRSZ + val->val.binary.len);
        SET_VARSIZE(out, VARHDRSZ + val->val.binary.len);
        memcpy(VARDATA(out), val->val.binary.data, val->val.binary.len);
    }

    return out;
}

/*
 * Get the offset of the variable-length portion of an gtype node within
 * the variable-length-data part of its container.  The node is identified
 * by index within the container's gtentry array.
 */
uint32 get_gtype_offset(const gtype_container *agtc, int index)
{
    uint32 offset = 0;
    int i;

    /*
     * Start offset of this entry is equal to the end offset of the previous
     * entry.  Walk backwards to the most recent entry stored as an end
     * offset, returning that offset plus any lengths in between.
     */
    for (i = index - 1; i >= 0; i--) {
        offset += GTE_OFFLENFLD(agtc->children[i]);
        if (GTE_HAS_OFF(agtc->children[i]))
            break;
    }

    return offset;
}

/*
 * Get the length of the variable-length portion of an gtype node.
 * The node is identified by index within the container's gtentry array.
 */
uint32 get_gtype_length(const gtype_container *agtc, int index)
{
    uint32 off;
    uint32 len;

    /*
     * If the length is stored directly in the gtentry, just return it.
     * Otherwise, get the begin offset of the entry, and subtract that from
     * the stored end+1 offset.
     */
    if (GTE_HAS_OFF(agtc->children[index]))
    {
        off = get_gtype_offset(agtc, index);
        len = GTE_OFFLENFLD(agtc->children[index]) - off;
    } else {
        len = GTE_OFFLENFLD(agtc->children[index]);
    }

    return len;
}

/*
 * A helper function to fill in an gtype_value to represent an element of an
 * array, or a key or value of an object.
 *
 * The node's gtentry is at container->children[index], and its variable-length
 * data is at base_addr + offset.  We make the caller determine the offset
 * since in many cases the caller can amortize that work across multiple
 * children.  When it can't, it can just call get_gtype_offset().
 *
 * A nested array or object will be returned as GTV_BINARY, ie. it won't be
 * expanded.
 */
static void fill_gtype_value(gtype_container *container, int index,
                              char *base_addr, uint32 offset,
                              gtype_value *result)
{
    gtentry entry = container->children[index];

    if (GTE_IS_NULL(entry)) {
        result->type = GTV_NULL;
    } else if (GTE_IS_STRING(entry)) {
        char *string_val;
        int string_len;

        result->type = GTV_STRING;
        // get the position and length of the string 
        string_val = base_addr + offset;
        string_len = get_gtype_length(container, index);
        // we need to do a deep copy of the string value 
        result->val.string.val = pnstrdup(string_val, string_len);
        result->val.string.len = string_len;
        Assert(result->val.string.len >= 0);
    } else if (GTE_IS_NUMERIC(entry)) {
        Numeric numeric;
        Numeric numeric_copy;

        result->type = GTV_NUMERIC;
        // we need to do a deep copy here 
        numeric = (Numeric)(base_addr + INTALIGN(offset));
        numeric_copy = (Numeric) palloc(VARSIZE(numeric));
        memcpy(numeric_copy, numeric, VARSIZE(numeric));
        result->val.numeric = numeric_copy;
    } else if (GTE_IS_GTYPE(entry)) {
        np_deserialize_extended_type(base_addr, offset, result);
    } else if (GTE_IS_BOOL_TRUE(entry)) {
        result->type = GTV_BOOL;
        result->val.boolean = true;
    } else if (GTE_IS_BOOL_FALSE(entry)) {
        result->type = GTV_BOOL;
        result->val.boolean = false;
    } else {
        Assert(GTE_IS_CONTAINER(entry));
        result->type = GTV_BINARY;
        // Remove alignment padding from data pointer and length 
        result->val.binary.data =
            (gtype_container *)(base_addr + INTALIGN(offset));
        result->val.binary.len = get_gtype_length(container, index) -
                                 (INTALIGN(offset) - offset);
    }
}

/*
 * Push gtype_value into gtype_parse_state.
 *
 * Used when parsing gtype tokens to form gtype, or when converting an
 * in-memory gtype_value to an gtype.
 *
 * Initial state of *gtype_parse_state is NULL, since it'll be allocated here
 * originally (caller will get gtype_parse_state back by reference).
 *
 * Only sequential tokens pertaining to non-container types should pass an
 * gtype_value.  There is one exception -- WGT_BEGIN_ARRAY callers may pass a
 * "raw scalar" pseudo array to append it - the actual scalar should be passed
 * next and it will be added as the only member of the array.
 *
 * Values of type GTV_BINARY, which are rolled up arrays and objects,
 * are unpacked before being added to the result.
 */
gtype_value *push_gtype_value(gtype_parse_state **pstate,
                                gtype_iterator_token seq,
                                gtype_value *agtval)
{
    gtype_iterator *it;
    gtype_value *res = NULL;
    gtype_value v;
    gtype_iterator_token tok;

    if (!agtval || (seq != WGT_ELEM && seq != WGT_VALUE) ||
        (agtval->type != GTV_BINARY))
    {
        // drop through 
        return push_gtype_value_scalar(pstate, seq, agtval);
    }

    // iterate through the binary and add each piece to the pstate 
    it = gtype_iterator_init(agtval->val.binary.data);
    while ((tok = gtype_iterator_next(&it, &v, false)) != WGT_DONE)
        res = push_gtype_value_scalar(pstate, tok, tok < WGT_BEGIN_ARRAY ? &v : NULL);
    return res;
}

/*
 * Do the actual pushing, with only scalar or pseudo-scalar-array values
 * accepted.
 */
static gtype_value *push_gtype_value_scalar(gtype_parse_state **pstate,
                                              gtype_iterator_token seq,
                                              gtype_value *scalar_val)
{
    gtype_value *result = NULL;

    switch (seq)
    {
    case WGT_BEGIN_ARRAY:
        Assert(!scalar_val || scalar_val->val.array.raw_scalar);
        *pstate = push_state(pstate);
        result = &(*pstate)->cont_val;
        (*pstate)->cont_val.type = GTV_ARRAY;
        (*pstate)->cont_val.val.array.num_elems = 0;
        (*pstate)->cont_val.val.array.raw_scalar =
            (scalar_val && scalar_val->val.array.raw_scalar);
        if (scalar_val && scalar_val->val.array.num_elems > 0)
        {
            // Assume that this array is still really a scalar 
            Assert(scalar_val->type == GTV_ARRAY);
            (*pstate)->size = scalar_val->val.array.num_elems;
        }
        else
        {
            (*pstate)->size = 4;
        }
        (*pstate)->cont_val.val.array.elems =
            palloc(sizeof(gtype_value) * (*pstate)->size);
        (*pstate)->last_updated_value = NULL;
        break;
    case WGT_BEGIN_OBJECT:
        Assert(!scalar_val);
        *pstate = push_state(pstate);
        result = &(*pstate)->cont_val;
        (*pstate)->cont_val.type = GTV_OBJECT;
        (*pstate)->cont_val.val.object.num_pairs = 0;
        (*pstate)->size = 4;
        (*pstate)->cont_val.val.object.pairs =
            palloc(sizeof(gtype_pair) * (*pstate)->size);
        (*pstate)->last_updated_value = NULL;
        break;
    case WGT_KEY:
        Assert(scalar_val->type == GTV_STRING);
        append_key(*pstate, scalar_val);
        break;
    case WGT_VALUE:
        Assert(IS_A_GTYPE_SCALAR(scalar_val));
        append_value(*pstate, scalar_val);
        break;
    case WGT_ELEM:
        Assert(IS_A_GTYPE_SCALAR(scalar_val));
        append_element(*pstate, scalar_val);
        break;
    case WGT_END_OBJECT:
        uniqueify_gtype_object(&(*pstate)->cont_val);
        // fall through
    case WGT_END_ARRAY:
        Assert(!scalar_val);
        result = &(*pstate)->cont_val;

        /*
         * Pop queue and push current array/object as value in parent
         * array/object
         */
        if (*pstate = (*pstate)->next)
        {
            switch ((*pstate)->cont_val.type)
            {
            case GTV_ARRAY:
                append_element(*pstate, result);
                break;
            case GTV_OBJECT:
                append_value(*pstate, result);
                break;
            default:
                ereport(ERROR, (errmsg("invalid gtype container type %d",
                                       (*pstate)->cont_val.type)));
            }
        }
        break;
    default:
        ereport(ERROR,
                (errmsg("unrecognized gtype sequential processing token")));
    }

    return result;
}


static gtype_parse_state *push_state(gtype_parse_state **pstate)
{
    gtype_parse_state *ns = palloc(sizeof(gtype_parse_state));

    ns->next = *pstate;
    return ns;
}

static void append_key(gtype_parse_state *pstate, gtype_value *string)
{
    gtype_value *object = &pstate->cont_val;

    Assert(object->type == GTV_OBJECT);
    Assert(string->type == GTV_STRING);

    if (object->val.object.num_pairs >= GTYPE_MAX_PAIRS)
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("number of gtype object pairs exceeds the maximum allowed (%zu)", GTYPE_MAX_PAIRS)));

    if (object->val.object.num_pairs >= pstate->size)
    {
        pstate->size *= 2;
        object->val.object.pairs = repalloc(
            object->val.object.pairs, sizeof(gtype_pair) * pstate->size);
    }

    object->val.object.pairs[object->val.object.num_pairs].key = *string;
    object->val.object.pairs[object->val.object.num_pairs].order =
        object->val.object.num_pairs;
}

static void append_value(gtype_parse_state *pstate, gtype_value *scalar_val)
{
    gtype_value *object = &pstate->cont_val;

    Assert(object->type == GTV_OBJECT);

    object->val.object.pairs[object->val.object.num_pairs].value = *scalar_val;

    pstate->last_updated_value =
        &object->val.object.pairs[object->val.object.num_pairs++].value;
}

static void append_element(gtype_parse_state *pstate,
                           gtype_value *scalar_val)
{
    gtype_value *array = &pstate->cont_val;

    Assert(array->type == GTV_ARRAY);

    if (array->val.array.num_elems >= GTYPE_MAX_ELEMS)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("number of gtype array elements exceeds the maximum allowed (%zu)", GTYPE_MAX_ELEMS)));

    if (array->val.array.num_elems >= pstate->size)
    {
        pstate->size *= 2;
        array->val.array.elems = repalloc(array->val.array.elems,
                                          sizeof(gtype_value) * pstate->size);
    }

    array->val.array.elems[array->val.array.num_elems] = *scalar_val;
    pstate->last_updated_value =
        &array->val.array.elems[array->val.array.num_elems++];
}

/*
 * Given an gtype_container, expand to gtype_iterator to iterate over items
 * fully expanded to in-memory representation for manipulation.
 *
 * See gtype_iterator_next() for notes on memory management.
 */
gtype_iterator *gtype_iterator_init(gtype_container *container)
{
    return iterator_from_container(container, NULL);
}

/*
 * Get next gtype_value while iterating
 *
 * Caller should initially pass their own, original iterator.  They may get
 * back a child iterator palloc()'d here instead.  The function can be relied
 * on to free those child iterators, lest the memory allocated for highly
 * nested objects become unreasonable, but only if callers don't end iteration
 * early (by breaking upon having found something in a search, for example).
 *
 * Callers in such a scenario, that are particularly sensitive to leaking
 * memory in a long-lived context may walk the ancestral tree from the final
 * iterator we left them with to its oldest ancestor, pfree()ing as they go.
 * They do not have to free any other memory previously allocated for iterators
 * but not accessible as direct ancestors of the iterator they're last passed
 * back.
 *
 * Returns "gtype sequential processing" token value.  Iterator "state"
 * reflects the current stage of the process in a less granular fashion, and is
 * mostly used here to track things internally with respect to particular
 * iterators.
 *
 * Clients of this function should not have to handle any GTV_BINARY values
 * (since recursive calls will deal with this), provided skip_nested is false.
 * It is our job to expand the GTV_BINARY representation without bothering
 * them with it.  However, clients should not take it upon themselves to touch
 * array or Object element/pair buffers, since their element/pair pointers are
 * garbage.  Also, *val will not be set when returning WGT_END_ARRAY or
 * WGT_END_OBJECT, on the assumption that it's only useful to access values
 * when recursing in.
 */
gtype_iterator_token gtype_iterator_next(gtype_iterator **it,
                                           gtype_value *val, bool skip_nested)
{
    if (*it == NULL)
        return WGT_DONE;

    /*
     * When stepping into a nested container, we jump back here to start
     * processing the child. We will not recurse further in one call, because
     * processing the child will always begin in GTI_ARRAY_START or
     * GTI_OBJECT_START state.
     */
recurse:
    switch ((*it)->state)
    {
    case GTI_ARRAY_START:
        // Set v to array on first array call 
        val->type = GTV_ARRAY;
        val->val.array.num_elems = (*it)->num_elems;

        /*
         * v->val.array.elems is not actually set, because we aren't doing
         * a full conversion
         */
        val->val.array.raw_scalar = (*it)->is_scalar;
        (*it)->curr_index = 0;
        (*it)->curr_data_offset = 0;
        (*it)->curr_value_offset = 0; // not actually used 
        // Set state for next call 
        (*it)->state = GTI_ARRAY_ELEM;
        return WGT_BEGIN_ARRAY;

    case GTI_ARRAY_ELEM:
        if ((*it)->curr_index >= (*it)->num_elems)
        {
            /*
             * All elements within array already processed.  Report this
             * to caller, and give it back original parent iterator (which
             * independently tracks iteration progress at its level of
             * nesting).
             */
            *it = free_and_get_parent(*it);
            return WGT_END_ARRAY;
        }

        fill_gtype_value((*it)->container, (*it)->curr_index,
                          (*it)->data_proper, (*it)->curr_data_offset, val);

        GTE_ADVANCE_OFFSET((*it)->curr_data_offset,
                            (*it)->children[(*it)->curr_index]);
        (*it)->curr_index++;

        if (!IS_A_GTYPE_SCALAR(val) && !skip_nested)
        {
            // Recurse into container. 
            *it = iterator_from_container(val->val.binary.data, *it);
            goto recurse;
        }
        else
        {
            /*
             * Scalar item in array, or a container and caller didn't want
             * us to recurse into it.
             */
            return WGT_ELEM;
        }

    case GTI_OBJECT_START:
        // Set v to object on first object call 
        val->type = GTV_OBJECT;
        val->val.object.num_pairs = (*it)->num_elems;

        /*
         * v->val.object.pairs is not actually set, because we aren't
         * doing a full conversion
         */
        (*it)->curr_index = 0;
        (*it)->curr_data_offset = 0;
        (*it)->curr_value_offset = get_gtype_offset((*it)->container,
                                                     (*it)->num_elems);
        // Set state for next call 
        (*it)->state = GTI_OBJECT_KEY;
        return WGT_BEGIN_OBJECT;

    case GTI_OBJECT_KEY:
        if ((*it)->curr_index >= (*it)->num_elems)
        {
            /*
             * All pairs within object already processed.  Report this to
             * caller, and give it back original containing iterator
             * (which independently tracks iteration progress at its level
             * of nesting).
             */
            *it = free_and_get_parent(*it);
            return WGT_END_OBJECT;
        }
        else
        {
            // Return key of a key/value pair.  
            fill_gtype_value((*it)->container, (*it)->curr_index,
                              (*it)->data_proper, (*it)->curr_data_offset,
                              val);
            if (val->type != GTV_STRING)
                ereport(ERROR,
                        (errmsg("unexpected gtype type as object key %d",
                                val->type)));

            // Set state for next call 
            (*it)->state = GTI_OBJECT_VALUE;
            return WGT_KEY;
        }

    case GTI_OBJECT_VALUE:
        // Set state for next call 
        (*it)->state = GTI_OBJECT_KEY;

        fill_gtype_value((*it)->container,
                          (*it)->curr_index + (*it)->num_elems,
                          (*it)->data_proper, (*it)->curr_value_offset, val);

        GTE_ADVANCE_OFFSET((*it)->curr_data_offset,
                            (*it)->children[(*it)->curr_index]);
        GTE_ADVANCE_OFFSET(
            (*it)->curr_value_offset,
            (*it)->children[(*it)->curr_index + (*it)->num_elems]);
        (*it)->curr_index++;

        /*
         * Value may be a container, in which case we recurse with new,
         * child iterator (unless the caller asked not to, by passing
         * skip_nested).
         */
        if (!IS_A_GTYPE_SCALAR(val) && !skip_nested)
        {
            *it = iterator_from_container(val->val.binary.data, *it);
            goto recurse;
        }
        else
        {
            return WGT_VALUE;
        }
    }

    ereport(ERROR, (errmsg("invalid iterator state %d", (*it)->state)));
    return -1;
}

/*
 * Initialize an iterator for iterating all elements in a container.
 */
static gtype_iterator *iterator_from_container(gtype_container *container,
                                                gtype_iterator *parent)
{
    gtype_iterator *it;

    it = palloc0(sizeof(gtype_iterator));
    it->container = container;
    it->parent = parent;
    it->num_elems = GTYPE_CONTAINER_SIZE(container);

    // Array starts just after header 
    it->children = container->children;

    switch (container->header & (GT_FARRAY | GT_FOBJECT))
    {
    case GT_FARRAY:
        it->data_proper = (char *)it->children +
                          it->num_elems * sizeof(gtentry);
        it->is_scalar = GTYPE_CONTAINER_IS_SCALAR(container);
        // This is either a "raw scalar", or an array 
        Assert(!it->is_scalar || it->num_elems == 1);

        it->state = GTI_ARRAY_START;
        break;

    case GT_FOBJECT:
        it->data_proper = (char *)it->children +
                          it->num_elems * sizeof(gtentry) * 2;
        it->state = GTI_OBJECT_START;
        break;

    default:
        ereport(ERROR,
                (errmsg("unknown type of gtype container %d",
                        container->header & (GT_FARRAY | GT_FOBJECT))));
    }

    return it;
}

/*
 * gtype_iterator_next() worker: Return parent, while freeing memory for
 *                                current iterator
 */
static gtype_iterator *free_and_get_parent(gtype_iterator *it)
{
    gtype_iterator *v = it->parent;

    pfree(it);
    return v;
}


/*
 * Functions for manipulating the resizeable buffer used by convert_gtype and
 * its subroutines.
 */

/*
 * Reserve 'len' bytes, at the end of the buffer, enlarging it if necessary.
 * Returns the offset to the reserved area. The caller is expected to fill
 * the reserved area later with copy_to_buffer().
 */
int reserve_from_buffer(StringInfo buffer, int len)
{
    int offset;

    // Make more room if needed 
    enlargeStringInfo(buffer, len);

    // remember current offset 
    offset = buffer->len;

    // reserve the space 
    buffer->len += len;

    /*
     * Keep a trailing null in place, even though it's not useful for us; it
     * seems best to preserve the invariants of StringInfos.
     */
    buffer->data[buffer->len] = '\0';

    return offset;
}

/*
 * Copy 'len' bytes to a previously reserved area in buffer.
 */
static void copy_to_buffer(StringInfo buffer, int offset, const char *data, int len)
{
    memcpy(buffer->data + offset, data, len);
}

/*
 * A shorthand for reserve_from_buffer + copy_to_buffer.
 */
static void append_to_buffer(StringInfo buffer, const char *data, int len)
{
    int offset;

    offset = reserve_from_buffer(buffer, len);
    copy_to_buffer(buffer, offset, data, len);
}

/*
 * Append padding, so that the length of the StringInfo is int-aligned.
 * Returns the number of padding bytes appended.
 */
short pad_buffer_to_int(StringInfo buffer)
{
    int padlen;
    int p;
    int offset;

    padlen = INTALIGN(buffer->len) - buffer->len;

    offset = reserve_from_buffer(buffer, padlen);

    // padlen must be small, so this is probably faster than a memset 
    for (p = 0; p < padlen; p++)
        buffer->data[offset + p] = '\0';

    return padlen;
}

/*
 * Given an gtype_value, convert to gtype. The result is palloc'd.
 */
static gtype *convert_to_gtype(gtype_value *val)
{
    StringInfoData buffer;
    gtentry aentry;
    gtype *res;

    // Should not already have binary representation 
    Assert(val->type != GTV_BINARY);

    // Allocate an output buffer. It will be enlarged as needed 
    initStringInfo(&buffer);

    // Make room for the varlena header 
    reserve_from_buffer(&buffer, VARHDRSZ);

    convert_gtype_value(&buffer, &aentry, val, 0);

    /*
     * Note: the gtentry of the root is discarded. Therefore the root
     * gtype_container struct must contain enough information to tell what
     * kind of value it is.
     */

    res = (gtype *)buffer.data;

    SET_VARSIZE(res, buffer.len);

    return res;
}


/*
 * Subroutine of convert_gtype: serialize a single gtype_value into buffer.
 *
 * The gtentry header for this node is returned in *header.  It is filled in
 * with the length of this value and appropriate type bits.  If we wish to
 * store an end offset rather than a length, it is the caller's responsibility
 * to adjust for that.
 *
 * If the value is an array or an object, this recurses. 'level' is only used
 * for debugging purposes.
 */
static void convert_gtype_value(StringInfo buffer, gtentry *header,
                                 gtype_value *val, int level)
{
    check_stack_depth();

    if (!val)
        return;

    /*
     * An gtype_value passed as val should never have a type of GTV_BINARY,
     * and neither should any of its sub-components. Those values will be
     * produced by convert_gtype_array and convert_gtype_object, the results
     * of which will not be passed back to this function as an argument.
     */

    if (IS_A_GTYPE_SCALAR(val))
        convert_gtype_scalar(buffer, header, val);
    else if (val->type == GTV_ARRAY)
        convert_gtype_array(buffer, header, val, level);
    else if (val->type == GTV_OBJECT)
        convert_gtype_object(buffer, header, val, level);
    else
        ereport(ERROR,
                (errmsg("unknown gtype type %d to convert", val->type)));
}

// define the type and size of the agt_header 
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



static void convert_gtype_array(StringInfo buffer, gtentry *pheader,
                                 gtype_value *val, int level)
{
    int base_offset;
    int gtentry_offset;
    int i;
    int totallen;
    uint32 header;
    int num_elems = val->val.array.num_elems;

    // Remember where in the buffer this array starts. 
    base_offset = buffer->len;

    // Align to 4-byte boundary (any padding counts as part of my data) 
    pad_buffer_to_int(buffer);

    /*
     * Construct the header gtentry and store it in the beginning of the
     * variable-length payload.
     */
    header = num_elems | GT_FARRAY;
    if (val->val.array.raw_scalar)
    {
        Assert(num_elems == 1);
        Assert(level == 0);
        header |= GT_FSCALAR;
    }

    append_to_buffer(buffer, (char *)&header, sizeof(uint32));

    // Reserve space for the gtentrys of the elements. 
    gtentry_offset = reserve_from_buffer(buffer, sizeof(gtentry) * num_elems);

    totallen = 0;
    for (i = 0; i < num_elems; i++)
    {
        gtype_value *elem = &val->val.array.elems[i];
        int len;
        gtentry meta;

        /*
         * Convert element, producing a gtentry and appending its
         * variable-length data to buffer
         */
        convert_gtype_value(buffer, &meta, elem, level + 1);

        len = GTE_OFFLENFLD(meta);
        totallen += len;

        /*
         * Bail out if total variable-length data exceeds what will fit in a
         * gtentry length field.  We check this in each iteration, not just
         * once at the end, to forestall possible integer overflow.
         */
        if (totallen > GTENTRY_OFFLENMASK)
        {
            ereport(
                ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg(
                     "total size of gtype array elements exceeds the maximum of %u bytes",
                     GTENTRY_OFFLENMASK)));
        }

        /*
         * Convert each GT_OFFSET_STRIDE'th length to an offset.
         */
        if ((i % GT_OFFSET_STRIDE) == 0)
            meta = (meta & GTENTRY_TYPEMASK) | totallen | GTENTRY_HAS_OFF;

        copy_to_buffer(buffer, gtentry_offset, (char *)&meta,
                       sizeof(gtentry));
        gtentry_offset += sizeof(gtentry);
    }

    // Total data size is everything we've appended to buffer 
    totallen = buffer->len - base_offset;

    // Check length again, since we didn't include the metadata above 
    if (totallen > GTENTRY_OFFLENMASK)
    {
        ereport(
            ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg(
                 "total size of gtype array elements exceeds the maximum of %u bytes",
                 GTENTRY_OFFLENMASK)));
    }

    // Initialize the header of this node in the container's gtentry array 
    *pheader = GTENTRY_IS_CONTAINER | totallen;
}

static void convert_gtype_object(StringInfo buffer, gtentry *pheader,
                                  gtype_value *val, int level)
{
    int base_offset;
    int gtentry_offset;
    int i;
    int totallen;
    uint32 header;
    int num_pairs = val->val.object.num_pairs;

    // Remember where in the buffer this object starts. 
    base_offset = buffer->len;

    // Align to 4-byte boundary (any padding counts as part of my data) 
    pad_buffer_to_int(buffer);

    /*
     * Construct the header gtentry and store it in the beginning of the
     * variable-length payload.
     */
    header = num_pairs | GT_FOBJECT;
    append_to_buffer(buffer, (char *)&header, sizeof(uint32));

    // Reserve space for the gtentrys of the keys and values. 
    gtentry_offset = reserve_from_buffer(buffer,
                                          sizeof(gtentry) * num_pairs * 2);

    /*
     * Iterate over the keys, then over the values, since that is the ordering
     * we want in the on-disk representation.
     */
    totallen = 0;
    for (i = 0; i < num_pairs; i++)
    {
        gtype_pair *pair = &val->val.object.pairs[i];
        int len;
        gtentry meta;

        /*
         * Convert key, producing an gtentry and appending its variable-length
         * data to buffer
         */
        convert_gtype_scalar(buffer, &meta, &pair->key);

        len = GTE_OFFLENFLD(meta);
        totallen += len;

        /*
         * Bail out if total variable-length data exceeds what will fit in a
         * gtentry length field.  We check this in each iteration, not just
         * once at the end, to forestall possible integer overflow.
         */
        if (totallen > GTENTRY_OFFLENMASK)
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), 
                    errmsg("total size of gtype object elements exceeds the maximum of %u bytes", GTENTRY_OFFLENMASK)));

        /*
         * Convert each GT_OFFSET_STRIDE'th length to an offset.
         */
        if ((i % GT_OFFSET_STRIDE) == 0)
            meta = (meta & GTENTRY_TYPEMASK) | totallen | GTENTRY_HAS_OFF;

        copy_to_buffer(buffer, gtentry_offset, (char *)&meta,
                       sizeof(gtentry));
        gtentry_offset += sizeof(gtentry);
    }
    for (i = 0; i < num_pairs; i++)
    {
        gtype_pair *pair = &val->val.object.pairs[i];
        int len;
        gtentry meta;

        /*
         * Convert value, producing an gtentry and appending its
         * variable-length data to buffer
         */
        convert_gtype_value(buffer, &meta, &pair->value, level + 1);

        len = GTE_OFFLENFLD(meta);
        totallen += len;

        /*
         * Bail out if total variable-length data exceeds what will fit in a
         * gtentry length field.  We check this in each iteration, not just
         * once at the end, to forestall possible integer overflow.
         */
        if (totallen > GTENTRY_OFFLENMASK)
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("total size of gtype object elements exceeds the maximum of %u bytes",
                     GTENTRY_OFFLENMASK)));

        /*
         * Convert each GT_OFFSET_STRIDE'th length to an offset.
         */
        if (((i + num_pairs) % GT_OFFSET_STRIDE) == 0)
            meta = (meta & GTENTRY_TYPEMASK) | totallen | GTENTRY_HAS_OFF;

        copy_to_buffer(buffer, gtentry_offset, (char *)&meta,
                       sizeof(gtentry));
        gtentry_offset += sizeof(gtentry);
    }

    // Total data size is everything we've appended to buffer 
    totallen = buffer->len - base_offset;

    // Check length again, since we didn't include the metadata above 
    if (totallen > GTENTRY_OFFLENMASK)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("total size of gtype object elements exceeds the maximum of %u bytes", GTENTRY_OFFLENMASK)));

    // Initialize the header of this node in the container's gtentry array 
    *pheader = GTENTRY_IS_CONTAINER | totallen;
}

static void convert_gtype_scalar(StringInfo buffer, gtentry *entry, gtype_value *scalar_val)
{
    int numlen;
    short padlen;
    bool status;

    switch (scalar_val->type)
    {
    case GTV_NULL:
        *entry = GTENTRY_IS_NULL;
        break;

    case GTV_STRING:
        append_to_buffer(buffer, scalar_val->val.string.val, scalar_val->val.string.len);

        *entry = scalar_val->val.string.len;
        break;

    case GTV_NUMERIC:
        numlen = VARSIZE_ANY(scalar_val->val.numeric);
        padlen = pad_buffer_to_int(buffer);

        append_to_buffer(buffer, (char *)scalar_val->val.numeric, numlen);

        *entry = GTENTRY_IS_NUMERIC | (padlen + numlen);
        break;

    case GTV_BOOL:
        *entry = (scalar_val->val.boolean) ? GTENTRY_IS_BOOL_TRUE : GTENTRY_IS_BOOL_FALSE;
        break;

    default:
        // returns true if there was a valid extended type processed 
        status = np_serialize_extended_type(buffer, entry, scalar_val);
        // if nothing was found, error log out 
        if (!status)
            ereport(ERROR, (errmsg("invalid gtype scalar type %d to convert",
                                   scalar_val->type)));
    }
}

/*
 * Compare two GTV_STRING gtype_value values, a and b.
 *
 * This is a special qsort() comparator used to sort strings in certain
 * internal contexts where it is sufficient to have a well-defined sort order.
 * In particular, object pair keys are sorted according to this criteria to
 * facilitate cheap binary searches where we don't care about lexical sort
 * order.
 *
 * a and b are first sorted based on their length.  If a tie-breaker is
 * required, only then do we consider string binary equality.
 */
static int length_compare_gtype_string_value(const void *a, const void *b)
{
    const gtype_value *va = (const gtype_value *)a;
    const gtype_value *vb = (const gtype_value *)b;
    int res;

    Assert(va->type == GTV_STRING);
    Assert(vb->type == GTV_STRING);

    if (va->val.string.len == vb->val.string.len)
    {
        res = memcmp(va->val.string.val, vb->val.string.val,
                     va->val.string.len);
    }
    else
    {
        res = (va->val.string.len > vb->val.string.len) ? 1 : -1;
    }

    return res;
}

/*
 * qsort_arg() comparator to compare gtype_pair values.
 *
 * Third argument 'binequal' may point to a bool. If it's set, *binequal is set
 * to true iff a and b have full binary equality, since some callers have an
 * interest in whether the two values are equal or merely equivalent.
 *
 * N.B: String comparisons here are "length-wise"
 *
 * Pairs with equals keys are ordered such that the order field is respected.
 */
static int length_compare_gtype_pair(const void *a, const void *b,
                                      void *binequal)
{
    const gtype_pair *pa = (const gtype_pair *)a;
    const gtype_pair *pb = (const gtype_pair *)b;
    int res;

    res = length_compare_gtype_string_value(&pa->key, &pb->key);
    if (res == 0 && binequal)
        *((bool *)binequal) = true;

    /*
     * Guarantee keeping order of equal pair.  Unique algorithm will prefer
     * first element as value.
     */
    if (res == 0)
        res = (pa->order > pb->order) ? -1 : 1;

    return res;
}

/*
 * Sort and unique-ify pairs in gtype_value object
 */
static void uniqueify_gtype_object(gtype_value *object)
{
    bool has_non_uniq = false;

    Assert(object->type == GTV_OBJECT);

    if (object->val.object.num_pairs > 1)
        qsort_arg(object->val.object.pairs, object->val.object.num_pairs,
                  sizeof(gtype_pair), length_compare_gtype_pair,
                  &has_non_uniq);

    if (has_non_uniq)
    {
        gtype_pair *ptr = object->val.object.pairs + 1;
        gtype_pair *res = object->val.object.pairs;

        while (ptr - object->val.object.pairs < object->val.object.num_pairs)
        {
            // Avoid copying over duplicate 
            if (length_compare_gtype_string_value(ptr, res) != 0)
            {
                res++;
                if (ptr != res)
                    memcpy(res, ptr, sizeof(gtype_pair));
            }
            ptr++;
        }

        object->val.object.num_pairs = res + 1 - object->val.object.pairs;
    }
}
