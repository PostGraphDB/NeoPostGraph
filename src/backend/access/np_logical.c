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

#include "fmgr.h"
#include "replication/logical.h"
#include "replication/output_plugin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

/* Function declarations for the callbacks */
extern void _PG_init(void);
extern void _PG_output_plugin_init(OutputPluginCallbacks *cb);

static void np_startup_cb(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init);
static void np_begin_cb(LogicalDecodingContext *ctx, ReorderBufferTXN *txn);
static void np_commit_cb(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void np_message_cb(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr message_lsn,
                          bool transactional, const char *prefix, Size message_size, const char *message);

/*
 * _PG_output_plugin_init
 * This is the magic hook Postgres looks for when loading a logical decoder.
 */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
    AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

    cb->startup_cb = np_startup_cb;
    cb->begin_cb = np_begin_cb;
    cb->commit_cb = np_commit_cb;
    cb->message_cb = np_message_cb;
    
    /* We intentionally leave change_cb NULL because we ignore standard heap changes */
}

static void
np_startup_cb(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
    /* Tell Postgres this plugin outputs raw text (our JSON strings) */
    opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
}

static void
np_begin_cb(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
    OutputPluginPrepareWrite(ctx, true);
    appendStringInfo(ctx->out, "{\"op\": \"BEGIN\", \"xid\": %u}", txn->xid);
    OutputPluginWrite(ctx, true);
}

static void
np_commit_cb(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr commit_lsn)
{
    OutputPluginPrepareWrite(ctx, true);
    appendStringInfo(ctx->out, "{\"op\": \"COMMIT\", \"xid\": %u}", txn->xid);
    OutputPluginWrite(ctx, true);
}

/*
 * np_message_cb
 * The workhorse. This intercepts WAL messages, filters for our prefix, 
 * and unpacks the binary payloads into JSON.
 */
static void
np_message_cb(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr message_lsn,
              bool transactional, const char *prefix, Size message_size, const char *message)
{
    /* Ignore anything that isn't from our graph engine */
    if (strcmp(prefix, "neopostgraph") != 0)
        return;

    char opcode = message[0];
    
    OutputPluginPrepareWrite(ctx, true);

    if (opcode == 'V') 
    {
        Size v_len = message_size - 1;
        
        /* 
         * Safely copy the varlena into aligned memory. 
         * By treating it as a bytea, we can use PG's built-in hex encoder.
         */
        bytea *b = (bytea *) palloc(v_len);
        memcpy(b, message + 1, v_len);
        char *hex = DatumGetCString(DirectFunctionCall1(byteaout, PointerGetDatum(b)));
        
        appendStringInfo(ctx->out, "{\"op\": \"insert_vertex\", \"data\": \"%s\"}", hex);
        pfree(b);
    } 
    else if (opcode == 'E') 
    {
        uint64 start_id;
        uint64 end_id;
        
        memcpy(&start_id, message + 1, sizeof(uint64));
        memcpy(&end_id, message + 9, sizeof(uint64));
        
        Size e_len = message_size - 17;
        bytea *b = (bytea *) palloc(e_len);
        memcpy(b, message + 17, e_len);
        char *hex = DatumGetCString(DirectFunctionCall1(byteaout, PointerGetDatum(b)));
        
        appendStringInfo(ctx->out, 
                         "{\"op\": \"insert_edge\", \"start_id\": " UINT64_FORMAT ", \"end_id\": " UINT64_FORMAT ", \"data\": \"%s\"}", 
                         start_id, end_id, hex);
        pfree(b);
    }

    OutputPluginWrite(ctx, true);
}