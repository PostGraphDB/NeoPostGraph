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
 * Declarations for gtype parser.
 */

#ifndef NP_GTYPE_PARSER_H
#define NP_GTYPE_PARSER_H

#include "lib/stringinfo.h"

typedef enum
{
    GTYPE_TOKEN_INVALID,
    GTYPE_TOKEN_STRING,
    GTYPE_TOKEN_INTEGER,
    GTYPE_TOKEN_FLOAT,
    GTYPE_TOKEN_NUMERIC,
    GTYPE_TOKEN_TIMESTAMP,
    GTYPE_TOKEN_TIMESTAMPTZ,
    GTYPE_TOKEN_DATE,
    GTYPE_TOKEN_TIME,
    GTYPE_TOKEN_TIMETZ,
    GTYPE_TOKEN_INTERVAL,
    GTYPE_TOKEN_INET,
    GTYPE_TOKEN_CIDR,
    GTYPE_TOKEN_MACADDR,
    GTYPE_TOKEN_MACADDR8,
    GTYPE_TOKEN_OBJECT_START,
    GTYPE_TOKEN_OBJECT_END,
    GTYPE_TOKEN_ARRAY_START,
    GTYPE_TOKEN_ARRAY_END,
    GTYPE_TOKEN_COMMA,
    GTYPE_TOKEN_COLON,
    GTYPE_TOKEN_ANNOTATION,
    GTYPE_TOKEN_IDENTIFIER,
    GTYPE_TOKEN_TRUE,
    GTYPE_TOKEN_FALSE,
    GTYPE_TOKEN_NULL,
    GTYPE_TOKEN_END
} gtype_token_type;


typedef struct gtype_lex_context
{
    char *input;
    int input_length;
    char *token_start;
    char *token_terminator;
    char *prev_token_terminator;
    gtype_token_type token_type;
    int lex_level;
    int line_number;
    char *line_start;
    StringInfo strval;
} gtype_lex_context;

typedef void (*gtype_struct_action)(void *state);
typedef void (*gtype_ofield_action)(void *state, char *fname, bool isnull);
typedef void (*gtype_aelem_action)(void *state, bool isnull);
typedef void (*gtype_scalar_action)(void *state, char *token, gtype_token_type tokentype, char *annotation);
typedef void (*gtype_annotation_actions)(void *state, char *anotation);

typedef struct gtype_sem_action
{
    void *semstate;
    gtype_struct_action object_start;
    gtype_struct_action object_end;
    gtype_struct_action array_start;
    gtype_struct_action array_end;
    gtype_ofield_action object_field_start;
    gtype_ofield_action object_field_end;
    gtype_aelem_action array_element_start;
    gtype_aelem_action array_element_end;
    gtype_scalar_action scalar;
    gtype_annotation_actions annotation;
} gtype_sem_action;

void parse_gtype(gtype_lex_context *lex, gtype_sem_action *sem);

gtype_lex_context *make_gtype_lex_context(text *t, bool need_escapes);
gtype_lex_context *make_gtype_lex_context_cstring_len(char *str, int len, bool need_escapes);


#endif
