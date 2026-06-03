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
CREATE TYPE dictionary;
CREATE TYPE vertex;
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
-- Constructor Functions
--
CREATE FUNCTION vertex_build(int8, int, int, smallint, gtype)
RETURNS vertex
LANGUAGE C
IMMUTABLE
RETURNS NULL ON NULL INPUT
PARALLEL SAFE
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
    vertex_id_seq regclass NOT NULL
);

CREATE SEQUENCE np_graph_id_seq START WITH 1 INCREMENT BY 1 MINVALUE 1 CACHE 5;

ALTER SEQUENCE np_graph_id_seq OWNED BY np_graph.id;

ALTER TABLE ONLY np_graph ALTER COLUMN id SET DEFAULT nextval('np_graph_id_seq'::regclass);

-- Indexes for np_graph
CREATE UNIQUE INDEX np_graph_name_namespace_index ON np_graph USING btree (name, namespace);

CREATE FUNCTION create_graph(graph_name Name, namespace text DEFAULT NULL)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';

CREATE FUNCTION create_vlabel(graph_name Name, label public.ltree, namespace text DEFAULT NULL)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';

CREATE FUNCTION get_vlabel_ids(graph_name name, labels text[], namespace_name text DEFAULT NULL)
RETURNS SETOF int
AS 'MODULE_PATHNAME', 'get_vlabel_ids_by_path'
LANGUAGE C STABLE;

CREATE FUNCTION get_or_vlabel_ids(graph_name name, labels text[], namespace_name text DEFAULT NULL)
RETURNS SETOF int
AS 'MODULE_PATHNAME', 'get_or_vlabel_ids_by_path'
LANGUAGE C STABLE;

CREATE FUNCTION dictionary_log(int, int, dictionary)
RETURNS void
LANGUAGE c
AS 'MODULE_PATHNAME';


CREATE FUNCTION vertex_set_dictionary(vertex, int)
RETURNS vertex
LANGUAGE c
AS 'MODULE_PATHNAME';

