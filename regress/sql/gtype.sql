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

-- 1. Scalar Types
SELECT NeoPostGraph.gtype_in('42');
SELECT NeoPostGraph.gtype_in('-9876543210');
SELECT NeoPostGraph.gtype_in('3.14159');
SELECT NeoPostGraph.gtype_in('-0.0000001');
SELECT NeoPostGraph.gtype_in('1.23456789e10');
SELECT NeoPostGraph.gtype_in('true');
SELECT NeoPostGraph.gtype_in('false');
SELECT NeoPostGraph.gtype_in('null');

-- 2. Strings
SELECT NeoPostGraph.gtype_in('"hello world"');
SELECT NeoPostGraph.gtype_in('""');                    -- empty string
SELECT NeoPostGraph.gtype_in('"with \"escaped\" quotes"');
SELECT NeoPostGraph.gtype_in('"unicode: café, 日本語, 🚀"');

-- 3. Objects
SELECT NeoPostGraph.gtype_in('{}');                    -- empty map
SELECT NeoPostGraph.gtype_in('{"name": "Alice", "age": 30}');
SELECT NeoPostGraph.gtype_in('{"nested": {"a": 1, "b": [2, 3]}}');
SELECT NeoPostGraph.gtype_in('{"bool": true, "nullval": null, "float": 3.14}');

-- 4. Arrays / Lists
SELECT NeoPostGraph.gtype_in('[]');
SELECT NeoPostGraph.gtype_in('[1, 2, 3]');
SELECT NeoPostGraph.gtype_in('["a", "b", "c"]');
SELECT NeoPostGraph.gtype_in('[1, "mixed", 3.14, true, null]');
SELECT NeoPostGraph.gtype_in('[[1,2],[3,4]]');

-- 5. Network Types
SELECT NeoPostGraph.gtype_in('127.0.0.1');
SELECT NeoPostGraph.gtype_in('192.168.1.1/24');

-- 6. Edge Cases & Special Values
SELECT NeoPostGraph.gtype_in('0');
SELECT NeoPostGraph.gtype_in('-0');
SELECT NeoPostGraph.gtype_in('1e-10');
SELECT NeoPostGraph.gtype_in('NaN');
SELECT NeoPostGraph.gtype_in('Infinity');

-- 7. Type Cast Syntax (PostgreSQL style)
SELECT NeoPostGraph.gtype_in('42::int');
SELECT NeoPostGraph.gtype_in('3.14::float');
SELECT NeoPostGraph.gtype_in('"text"::text');

-- 8. Complex Mixed Structures
SELECT NeoPostGraph.gtype_in('{"name": "Bob", "tags": ["dev", "graph"], "scores": [95, 87, 92], "active": true}');

-- 9. Error cases
SELECT NeoPostGraph.gtype_in('invalid');
SELECT NeoPostGraph.gtype_in('{broken: json}');