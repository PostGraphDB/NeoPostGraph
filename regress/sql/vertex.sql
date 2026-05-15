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

SELECT FROM NeoPostGraph.vertex_in('{"name": "Alice", "age": 30}');


SELECT NeoPostGraph.vertex_in('{"name": "Alice", "age": 30}');
SELECT * FROM NeoPostGraph.vertex_in('{"name": "Alice", "age": 30}');


SELECT NeoPostGraph.vertex_in('{}');
SELECT NeoPostGraph.vertex_in('{"name": "Alice", "age": 30}');
SELECT NeoPostGraph.vertex_in('{"nested": {"a": 1, "b": [2, 3]}}');
SELECT NeoPostGraph.vertex_in('{"bool": true, "nullval": null, "float": 3.14}');
SELECT NeoPostGraph.vertex_in('{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}');

SELECT NeoPostGraph.vertex_build(0::int8, 0, 0, 0::smallint, '{}'::NeoPostGraph.gtype);

SELECT NeoPostGraph.vertex_build(0::int8, 0, 0, 0::smallint,'{"name": "Alice", "age": 30}'::NeoPostGraph.gtype);
SELECT NeoPostGraph.vertex_build(0::int8, 0, 0, 0::smallint,'{"nested": {"a": 1, "b": [2, 3]}}'::NeoPostGraph.gtype);
SELECT NeoPostGraph.vertex_build(0::int8, 0, 0, 0::smallint,'{"bool": true, "nullval": null, "float": 3.14}'::NeoPostGraph.gtype);

SELECT NeoPostGraph.vertex_build(0::int8, 0, 0, 0::smallint, '{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}'::NeoPostGraph.gtype);

SELECT NeoPostGraph.create_graph('vertex_graph', 'public');
SELECT NeoPostGraph.create_vlabel('vertex_graph', 'person');

SELECT NeoPostGraph.vertex_build(0::int8, graph.id, label.id, 0::smallint,'{"name": "Alice", "age": 30}'::NeoPostGraph.gtype)
FROM NeoPostGraph.np_vertex_label label
JOIN NeoPostGraph.np_graph graph
ON label.graph_id = graph.id
WHERE graph.name = 'vertex_graph'
  AND label @ 'person';

SELECT NeoPostGraph.create_vlabel('vertex_graph', 'person.employee.engineer');

SELECT NeoPostGraph.vertex_build(1::int8, graph.id, label.id, 0::smallint,'{"name": "Alice", "age": 30}'::NeoPostGraph.gtype)
FROM NeoPostGraph.np_vertex_label label
         JOIN NeoPostGraph.np_graph graph
              ON label.graph_id = graph.id
WHERE graph.name = 'vertex_graph'
  AND label @ 'person';