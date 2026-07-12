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

SELECT FROM edge_in('{"name": "Alice", "age": 30}');


SELECT edge_in('{"name": "Alice", "age": 30}');
SELECT * FROM edge_in('{"name": "Alice", "age": 30}');

--
-- Edge Input Routine
-- NOTE: Never going to be used
--
SELECT edge_in('{}');
SELECT edge_in('{"name": "Alice", "age": 30}');
SELECT edge_in('{"nested": {"a": 1, "b": [2, 3]}}');
SELECT edge_in('{"bool": true, "nullval": null, "float": 3.14}');
SELECT edge_in('{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}');


--
-- Edge basic constructor
--
SELECT edge_build(0::int8, 0, 0, 0::smallint, vertex_in('{}'), vertex_in('{}'),'{}'::gtype);
SELECT edge_build(0::int8, 0, 0, 0::smallint, vertex_in('{}'), vertex_in('{}'),'{"name": "Alice", "age": 30}'::gtype);
SELECT edge_build(0::int8, 0, 0, 0::smallint, vertex_in('{}'), vertex_in('{}'),'{"nested": {"a": 1, "b": [2, 3]}}'::gtype);
SELECT edge_build(0::int8, 0, 0, 0::smallint, vertex_in('{}'), vertex_in('{}'),'{"bool": true, "nullval": null, "float": 3.14}'::gtype);
SELECT edge_build(0::int8, 0, 0, 0::smallint, vertex_in('{}'), vertex_in('{}'),'{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}'::gtype);

--
-- Vertex LTree Label Logic
--
SELECT create_graph('edge_graph', 'public');
SELECT create_elabel('edge_graph', 'person');

SELECT * FROM np_graph graph WHERE graph.name = 'edge_graph';
select * FROM np_edge_label_26;

\d+ np_edge_label_26
\d+ np_edge_26_2

SELECT edge_build(0::int8, graph.id, label.id, 0::smallint, vertex_in('{}'), vertex_in('{}'),'{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}'::gtype)
FROM np_edge_label_26 label, np_graph graph
WHERE graph.name = 'edge_graph'
  AND label.ltree @ 'person';

SELECT create_elabel('edge_graph', 'person.employee.engineer');

SELECT edge_build(1::int8, graph.id, label.id, 0::smallint,vertex_in('{}'), vertex_in('{}'),'{"name": "Alice", "age": 30}'::gtype)
FROM np_edge_label_26 label, np_graph graph
WHERE graph.name = 'edge_graph'
  AND label.ltree @ 'person';

