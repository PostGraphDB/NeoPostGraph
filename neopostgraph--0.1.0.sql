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
-- catalog tables
--

--
-- Graph Metadata
--
CREATE TABLE np_graph (id int NOT NULL, name name NOT NULL, namespace regnamespace NOT NULL);

CREATE SEQUENCE np_graph_id_seq START WITH 1 INCREMENT BY 1 MINVALUE 1 CACHE 5;

ALTER SEQUENCE np_graph_id_seq OWNED BY np_graph.id;

ALTER TABLE ONLY np_graph ALTER COLUMN id SET DEFAULT nextval('np_graph_id_seq'::regclass);

-- Indexes for np_graph
CREATE UNIQUE INDEX np_graph_name_namespace_index ON np_graph USING btree (name, namespace);

CREATE FUNCTION create_graph(graph_name Name, namespace text DEFAULT NULL)
RETURNS void 
LANGUAGE c 
AS 'MODULE_PATHNAME';