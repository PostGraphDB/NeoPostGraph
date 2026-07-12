/*
 * Copyright (C) 2026 PostGraphDB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
LOAD 'neopostgraph';

SET search_path TO  public, pg_catalog, NeoPostGraph;

SELECT FROM vertex_in('{"name": "Alice", "age": 30}');


SELECT vertex_in('{"name": "Alice", "age": 30}');
SELECT * FROM vertex_in('{"name": "Alice", "age": 30}');

--
-- Vertex Input Routine
-- NOTE: Never going to be used
--
SELECT vertex_in('{}');
SELECT vertex_in('{"name": "Alice", "age": 30}');
SELECT vertex_in('{"nested": {"a": 1, "b": [2, 3]}}');
SELECT vertex_in('{"bool": true, "nullval": null, "float": 3.14}');
SELECT vertex_in('{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}');


--
-- Vertex basic constructor
--
SELECT vertex_build(0::int8, 0, 0, 0::smallint, '{}'::gtype);
SELECT vertex_build(0::int8, 0, 0, 0::smallint,'{"name": "Alice", "age": 30}'::gtype);
SELECT vertex_build(0::int8, 0, 0, 0::smallint,'{"nested": {"a": 1, "b": [2, 3]}}'::gtype);
SELECT vertex_build(0::int8, 0, 0, 0::smallint,'{"bool": true, "nullval": null, "float": 3.14}'::gtype);

SELECT vertex_build(0::int8, 0, 0, 0::smallint, '{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}'::gtype);

--
-- Vertex LTree Label Logic
--
SELECT create_graph('vertex_graph', 'public');
SELECT create_vlabel('vertex_graph', 'person');

SELECT * FROM np_graph graph WHERE graph.name = 'vertex_graph';
select * FROM np_vertex_label_21;

select * from np_vertex_21_2_linked_list_meta;

\d+ np_vertex_21_2
\d+ np_vertex_21_2_phys_map
\d+ np_vertex_21_2_linked_list_meta
\d+ np_vertex_21_2_1_linked_list
\d+ np_vertex_21_2_arraylist
 
SELECT vertex_build(0::int8, graph.id, label.id, 0::smallint,'{"name": "Alice", "age": 30}'::gtype)
FROM np_vertex_label_21 label, np_graph graph
WHERE graph.name = 'vertex_graph'
  AND label.ltree @ 'person';

SELECT create_vlabel('vertex_graph', 'person.employee.engineer');

SELECT vertex_build(1::int8, graph.id, label.id, 0::smallint,'{"name": "Alice", "age": 30}'::gtype)
FROM np_vertex_label_21 label, np_graph graph
WHERE graph.name = 'vertex_graph'
  AND label.ltree @ 'person';

--
-- Vertex with a Dictionary
--
-- 1. Default
--

SELECT dictionary_log(21, 1, '["age", "name"]'::dictionary);

SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Alice", "age": 30}'), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Alice", "age": 30, "weight": 9001}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Bob"}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"x": 1, "y": 2}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30, "extra": {"nested": true}, "flag": true}'::gtype), 1);

--
-- Vertex with a Dictionary
--
-- 2. With show_dictionary_nulls GUC ENABLED
--
SET neopostgraph.show_dictionary_nulls = true;

SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Alice", "age": 30}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Alice", "age": 30, "weight": 9001}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Bob"}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"x": 1, "y": 2}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Alice", "age": 30, "extra": {"nested": true}, "flag": true}'::gtype), 1);

--
-- Vertex with a Dictionary
--
-- 3. With show_dictionary_keys GUC DISABLED
--
SET neopostgraph.show_dictionary_keys = false;

SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Alice", "age": 30}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Alice", "age": 30, "weight": 9001}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"name": "Bob"}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint, '{"x": 1, "y": 2}'::gtype), 1);
SELECT vertex_set_dictionary(vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30, "extra": {"nested": true}, "flag": true}'::gtype), 1);

\dt public

select insert_vertex(vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype));
select insert_vertex(vertex_build(2, 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype));

select insert_edge(
  vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
  vertex_build(2, 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
  edge_build(
      0::int8, 21, 1, 0::smallint, 
      vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
      vertex_build(2, 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
      '{}'::gtype)
  );

\d+ np_edge_21_1

SELECT * FROM public.np_edge_21_1;


SELECT * FROM public.np_vertex_21_1;



RESET neopostgraph.show_dictionary_keys;