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

 -- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION NeoPostGraph" to load this file. \quit

                                                             --
-- Type Initial Definitions
--
-- XXX: Create the shell type here so functions can reference it, then fill in the actual definition
-- in its dedicated section.
--
CREATE TYPE gtype;
CREATE TYPE adj_list;
CREATE TYPE dictionary;
CREATE TYPE vertex;
CREATE TYPE edge;

--
-- gtype datatype
--

--
-- I/O Routines
--
CREATE FUNCTION gtype_in(cstring) RETURNS gtype
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION gtype_out(gtype) RETURNS cstring
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE TYPE gtype (
    INPUT = gtype_in,
    OUTPUT = gtype_out,
    LIKE = jsonb,
    STORAGE = extended
);

--
-- Adj_list datatype
--

--
-- I/O Routines
--
CREATE FUNCTION adj_list_in(cstring) RETURNS adj_list
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION adj_list_out(adj_list) RETURNS cstring
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE TYPE adj_list (
    INPUT = adj_list_in,
    OUTPUT = adj_list_out,
    LIKE = jsonb,
    STORAGE = extended
);

--
-- dictionary datatype
--

--
-- I/O Routines
--
CREATE FUNCTION dictionary_in(cstring) RETURNS dictionary
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION dictionary_out(dictionary) RETURNS cstring
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE TYPE dictionary (
    INPUT = dictionary_in,
    OUTPUT = dictionary_out,
    LIKE = jsonb,
    STORAGE = extended
);

--
-- dictionary constructors
--

CREATE FUNCTION dictionary_build(smallint, gtype)
RETURNS dictionary
LANGUAGE C
IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';


--
-- vertex datatype
--

--
-- I/O Routines
--
CREATE FUNCTION vertex_in(cstring) RETURNS vertex
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION vertex_out(vertex) RETURNS cstring
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE TYPE vertex (
    INPUT = vertex_in,
    OUTPUT = vertex_out,
    LIKE = jsonb,
    STORAGE = extended
);

--
-- DDL Commands
--
CREATE FUNCTION create_vlabel(graph_name Name, label public.ltree, namespace text DEFAULT NULL)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';

--
-- DML Commands
--
CREATE FUNCTION vertex_build(int8, int, int, smallint, gtype)
RETURNS vertex
LANGUAGE C
IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION insert_vertex(vertex)
RETURNS void
LANGUAGE c
AS 'MODULE_PATHNAME';

--
-- Vertex Metadata Queries
--
CREATE FUNCTION get_vlabel_ids(graph_name name, labels text[], namespace_name text DEFAULT NULL)
RETURNS SETOF int
AS 'MODULE_PATHNAME', 'get_vlabel_ids_by_path'
LANGUAGE C STABLE;

CREATE FUNCTION get_or_vlabel_ids(graph_name name, labels text[], namespace_name text DEFAULT NULL)
RETURNS SETOF int
AS 'MODULE_PATHNAME', 'get_or_vlabel_ids_by_path'
LANGUAGE C STABLE;

--
-- Vertex Dictionary
--
CREATE FUNCTION dictionary_log(int, int, dictionary)
RETURNS void
LANGUAGE c
AS 'MODULE_PATHNAME';

CREATE FUNCTION vertex_set_dictionary(vertex, int)
RETURNS vertex
LANGUAGE c
AS 'MODULE_PATHNAME';


--
-- edge datatype
--

--
-- I/O Routines
--
CREATE FUNCTION edge_in(cstring) RETURNS edge
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION edge_out(edge) RETURNS cstring
    LANGUAGE C
    IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE TYPE edge (
    INPUT = edge_in,
    OUTPUT = edge_out,
    LIKE = jsonb,
    STORAGE = extended
);

--
-- DDL Commands
--
CREATE FUNCTION create_elabel(graph_name Name, label public.ltree, namespace text DEFAULT NULL)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';

--
-- DML Commands
--
CREATE FUNCTION edge_build(int8, int, int, smallint, vertex, vertex, gtype)
RETURNS edge
LANGUAGE C
IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
AS 'MODULE_PATHNAME';

CREATE FUNCTION insert_edge(vertex, vertex, edge)
RETURNS void
LANGUAGE c
AS 'MODULE_PATHNAME';

--
--
--

--
-- Table AM
--
-- XXX: Rename to Physical Pointer Hash
CREATE OR REPLACE FUNCTION np_physmap_mutable_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'np_physmap_mutable_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD np_mutable
TYPE TABLE 
HANDLER np_physmap_mutable_handler;

--
-- Table AM
--
CREATE OR REPLACE FUNCTION np_linked_list_mutable_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'np_linked_list_mutable_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD NPLinkedList
TYPE TABLE 
HANDLER np_linked_list_mutable_handler;


CREATE FUNCTION rotate_active_linked_list_table(graph_name Name, label_id int)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';

CREATE OR REPLACE FUNCTION compact_oldest_linked_list_table(graph_name Name, label_id int)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';


--
-- catalog tables
--

--
-- Graph Metadata
--
CREATE TABLE np_graph (
    id int NOT NULL,
    name name NOT NULL,
    namespace regnamespace NOT NULL,
    vertex_labels regclass NOT NULL,
    vertex_id_seq regclass NOT NULL,
    edge_labels regclass NOT NULL,
    edge_id_seq regclass NOT NULL
);

CREATE SEQUENCE np_graph_id_seq START WITH 1 INCREMENT BY 1 MINVALUE 1 CACHE 5;

ALTER SEQUENCE np_graph_id_seq OWNED BY np_graph.id;

ALTER TABLE ONLY np_graph ALTER COLUMN id SET DEFAULT nextval('np_graph_id_seq'::regclass);

-- Indexes for np_graph
CREATE UNIQUE INDEX np_graph_name_namespace_index ON np_graph USING btree (name, namespace);

--
-- DDL Commands
--
CREATE FUNCTION create_graph(graph_name Name, namespace text DEFAULT NULL)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';