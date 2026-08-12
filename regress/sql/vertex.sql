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
SELECT create_vlabel('vertex_graph', 'person', ARRAY['EMPLOYEED', 'FREE']);

SELECT * FROM np_graph graph WHERE graph.name = 'vertex_graph';
select * FROM np_vertex_label_21;

select * from np_vertex_21_2_linked_list_meta;

\d+ np_vertex_21_2
\d+ np_vertex_21_2_phys_map
\d+ np_vertex_21_2_linked_list_meta
\d+ np_vertex_21_2_1_linked_list
\d+ np_vertex_21_2_arraylist
\d+ np_vertex_21_2_annotations
 
 /*
SELECT vertex_build(0::int8, graph.id, label.id, 0::smallint,'{"name": "Alice", "age": 30}'::gtype)
FROM np_vertex_label_21 label, np_graph graph
WHERE graph.name = 'vertex_graph'
  AND label.ltree @ 'person';
*/
SELECT create_vlabel('vertex_graph', 'person.employee.engineer');
SELECT create_vlabel('vertex_graph', 'employee');
SELECT create_vlabel('vertex_graph', 'engineer', ARRAY['EMPLOYEED', 'ON_PTO']);
SELECT merge_vlabels('vertex_graph', 2, 3);
SELECT merge_vlabels('vertex_graph', 5, 4);

SELECT create_vlabel('vertex_graph', 'ON_PTO');
SELECT create_vlabel('vertex_graph', 'new_label', ARRAY['person']);

/*
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
RESET neopostgraph.show_dictionary_keys;
*/
select insert_vertex(vertex_build(nextval('np_vertex_id_seq_21_1'), 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype));
select insert_vertex(vertex_build(nextval('np_vertex_id_seq_21_1'), 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype));

select insert_edge(
  vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
  vertex_build(2, 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
  edge_build(
      nextval('np_edge_id_seq_21_1'), 21, 1, 0::smallint, 
      vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
      vertex_build(2, 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
      '{}'::gtype)
  );



\d+ np_edge_21_1

SELECT * FROM public.np_edge_21_1;


select insert_edge(
  vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
  vertex_build(2, 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
  edge_build(
      nextval('np_edge_id_seq_21_1'), 21, 1, 0::smallint, 
      vertex_build(1, 21, 1, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
      vertex_build(2, 21, 1, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
      '{}'::gtype)
  );

select * from np_vertex_21_2_linked_list_meta;

\d+ np_vertex_21_2_1_linked_list
\d+ np_vertex_21_2_2_linked_list
 
select * From np_vertex_21_2_1_linked_list;
select * From np_vertex_21_2_2_linked_list;

select * From np_vertex_21_1_1_linked_list;
select * From np_vertex_21_1_2_linked_list;

select * from np_vertex_21_1_phys_map;

select * From np_vertex_1_1_1_linked_list;
SELECT * FROM public.np_edge_21_1;

SELECT insert_vertex(
  vertex_build(nextval('np_vertex_id_seq_21_2'), 21, 2, 0::smallint, '{"name": "Alice", "age": 30}'::gtype),
   ARRAY['EMPLOYEED']
);

SELECT insert_vertex(
  vertex_build(nextval('np_vertex_id_seq_21_2'), 21, 2, 0::smallint, '{"name": "Bob", "age": 33}'::gtype),
   ARRAY['FREE']
);
select * from np_vertex_21_2;
select * from np_vertex_21_2_annotations;



SELECT insert_vertex(
  vertex_build(nextval('np_vertex_id_seq_21_6'), 21, 6, 0::smallint, '{"name": "Charlie", "age": 36}'::gtype),
   ARRAY['FREE']
);

select * from np_vertex_21_6;
select * from np_vertex_21_6_annotations;



select * from np_vertex_21_1_phys_map;
select insert_edge(
  vertex_build(1, 21, 2, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
  vertex_build(2, 21, 2, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
  edge_build(
      nextval('np_edge_id_seq_21_1'), 21, 1, 0::smallint, 
      vertex_build(1, 21, 2, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
      vertex_build(2, 21, 2, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
      '{}'::gtype)
  );
select * from np_vertex_21_1_phys_map;
select rotate_active_linked_list_table('vertex_graph', 2);
select * from np_vertex_21_1_phys_map;

select insert_edge(
  vertex_build(1, 21, 2, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
  vertex_build(2, 21, 2, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
  edge_build(
      nextval('np_edge_id_seq_21_1'), 21, 1, 0::smallint, 
      vertex_build(1, 21, 2, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
      vertex_build(2, 21, 2, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
      '{}'::gtype)
  );


select * from np_vertex_21_1_phys_map;
select * From np_vertex_21_2_1_linked_list;
select * From np_vertex_21_2_2_linked_list;

select compact_oldest_linked_list_table('vertex_graph', 2);
select * from np_vertex_21_1_phys_map;
select * From np_vertex_21_2_1_linked_list;
select * From np_vertex_21_2_2_linked_list;
SELECT * FROM np_vertex_21_2_arraylist;

select rotate_active_linked_list_table('vertex_graph', 2);
select compact_oldest_linked_list_table('vertex_graph', 2);
select * from np_vertex_21_1_phys_map;
select * From np_vertex_21_2_1_linked_list;
select * From np_vertex_21_2_2_linked_list;
select * From np_vertex_21_2_3_linked_list;
select * from np_vertex_21_2_linked_list_meta;
SELECT * FROM np_vertex_21_2_arraylist;


select compact_oldest_linked_list_table('vertex_graph', 2);
select compact_oldest_linked_list_table('vertex_graph', 2);
select compact_oldest_linked_list_table('vertex_graph', 2);

select ctid, * from np_vertex_21_2_phys_map;
select ctid, * from np_vertex_21_2;

select update_vertex(1::int8, 2::int4, 21::int4, '{"age": 30, "name": "Alex"}'::gtype);

select ctid, * from np_vertex_21_2_phys_map;
select ctid, * from np_vertex_21_2;


SELECT create_vlabel('vertex_graph', 'thirdLabel');
SELECT insert_vertex(
  vertex_build(nextval('np_vertex_id_seq_21_3'), 21, 3, 0::smallint, '{"name": "Alice", "age": 30}'::gtype)
);

SELECT insert_vertex(
  vertex_build(nextval('np_vertex_id_seq_21_3'), 21, 3, 0::smallint, '{"name": "Bob", "age": 33}'::gtype)
);
select * from np_vertex_21_1_phys_map;

select insert_edge(
  vertex_build(1, 21, 3, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
  vertex_build(2, 21, 3, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
  edge_build(
      nextval('np_edge_id_seq_21_1'), 21, 1, 0::smallint, 
      vertex_build(1, 21, 3, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
      vertex_build(2, 21, 3, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
      '{}'::gtype)
  );

select insert_edge(
  vertex_build(1, 21, 3, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
  vertex_build(2, 21, 3, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
  edge_build(
      nextval('np_edge_id_seq_21_1'), 21, 1, 0::smallint, 
      vertex_build(1, 21, 3, 0::smallint,'{"name": "Alice", "age": 30}'::gtype),
      vertex_build(2, 21, 3, 0::smallint,'{"name": "Bob", "age": 33}'::gtype),
      '{}'::gtype)
  );

select * from np_vertex_21_3_phys_map;
select * From np_vertex_21_3_1_linked_list;
select rotate_active_linked_list_table('vertex_graph', 3);
select compact_oldest_linked_list_table('vertex_graph', 3);


select * from np_vertex_21_1_phys_map;
select * From np_vertex_21_3_1_linked_list;
select * From np_vertex_21_3_2_linked_list;
SELECT * FROM np_vertex_21_3_arraylist;

SELECT * FROM public.np_edge_21_1;

select update_edge(1::int8, 1::int4, 21::int4, '{"known_since": "12/31/1971"::date}'::gtype);

SELECT * FROM public.np_edge_21_1;
select * from np_vertex_21_1_1_linked_list;

select delete_edge(2::int8, 1::int4, 21::int4);
SELECT * FROM public.np_edge_21_1;
select * from np_vertex_21_1_1_linked_list;


SELECT * FROM public.np_vertex_21_1;

select delete_vertex(1::int8, 1::int4, 21::int4, false);

SELECT * FROM public.np_vertex_21_1;
select * from np_vertex_21_1_phys_map;
select * From np_vertex_21_1_1_linked_list;
select * From np_vertex_21_1_2_linked_list;
SELECT * FROM np_vertex_21_1_arraylist;

select delete_vertex(1::int8, 1::int4, 21::int4, true);

SELECT * FROM public.np_vertex_21_1;
select * from np_vertex_21_1_phys_map;
select * From np_vertex_21_1_1_linked_list;
select * From np_vertex_21_1_2_linked_list;
SELECT * FROM np_vertex_21_1_arraylist;



select * FROM np_vertex_label_21;
SELECT create_vlabel('vertex_graph', 'person', ARRAY['EMPLOYEED', 'FREE']);
select * FROM np_vertex_label_21;
select * from np_vertex_21_5_annotations;



select * FROM np_vertex_label_21;
SELECT add_annotation_label('vertex_graph', 'person', 'Active');

SELECT insert_vertex(
  vertex_build(nextval('np_vertex_id_seq_21_2'), 21, 2, 0::smallint, '{"name": "David", "age": 27}'::gtype),
   ARRAY['Active']
);

select * FROM np_vertex_label_21;
select * from np_vertex_21_2;
select * from np_vertex_21_2_annotations;
select * from np_vertex_21_4_annotations;
select * from np_vertex_21_5_annotations;
select * from np_vertex_21_6_annotations;



select add_vertex_annotation_label(1, 2, 21, 'Active');
select * from np_vertex_21_2;
select * from np_vertex_21_2_annotations;


select remove_vertex_annotation_label(2, 2, 21, 'FREE');
select * from np_vertex_21_2;
select * from np_vertex_21_2_annotations;


select drop_annotation_label('vertex_graph', 'person', 'Active');
select * FROM np_vertex_label_21;
select * from np_vertex_21_2;
select * from np_vertex_21_2_annotations;
select * from np_vertex_21_4_annotations;
select * from np_vertex_21_5_annotations;
select * from np_vertex_21_6_annotations;

SELECT * FROM public.np_vertex_21_1;
select * from np_vertex_21_2_phys_map;
select * From np_vertex_21_2_1_linked_list;
select * From np_vertex_21_2_2_linked_list;
SELECT * FROM np_vertex_21_2_arraylist;

select set_vertex_label(2, 2, 21, 'missing_label');
select * FROM np_vertex_21_10;
select * FROM np_vertex_21_10_phys_map;
select * FROM np_vertex_21_10_linked_list_meta;
select * from np_vertex_21_10_1_linked_list;
select * FROM np_vertex_21_10_linked_list_seq;
select * FROM np_vertex_21_10_arraylist;
select * FROM np_vertex_21_10_annotations;


select * FROM np_vertex_label_21;



-- =====================================================================
-- TEST: remove_vertex_label (Structural Label Reduction & Dynamic DDL)
-- =====================================================================

-- 1. Error on invalid syntax (dots are not allowed)
select remove_vertex_label(1::int8, 10::int4, 21::int4, 'missing.label');

-- 2. Remove label to an EXISTING target path (Bob: _.person.missing_label -> _.person)
select remove_vertex_label(1::int8, 10::int4, 21::int4, 'missing_label');
select * FROM np_vertex_21_10;
select id, vertex FROM np_vertex_21_2 WHERE id = 4;

-- 3. Remove label to a NON-EXISTENT target path (Charlie: _.person.employee.engineer -> _.person.engineer)
select remove_vertex_label(1::int8, 6::int4, 21::int4, 'employee');
select id, ltree, tbl FROM np_vertex_label_21 WHERE ltree = '_.person.engineer';
select * FROM np_vertex_21_11;
select * FROM np_vertex_21_6;

-- =====================================================================
-- TEST: Validate Phys Map Synchronization after Compaction
-- =====================================================================
-- Expose the actual physical locations of the compacted arraylist tuples
SELECT ctid AS actual_arraylist_ctid, id AS owner_id 
FROM np_vertex_21_2_arraylist;

-- Expose the pointers in the physical map
SELECT v_itemptr, e_tbl_id, e_itemptr
FROM np_vertex_21_2_phys_map;

-- (Strict Validation) This must return 0 rows. 
-- Filters out InvalidTid '(4294967295,0)' to only catch legitimately broken live pointers.
SELECT p.v_itemptr, p.e_itemptr AS broken_pointer 
FROM np_vertex_21_2_phys_map p
LEFT JOIN np_vertex_21_2_arraylist a ON p.e_itemptr = a.ctid
WHERE a.ctid IS NULL 
  AND p.e_itemptr::text != '(4294967295,0)';

-- 4. Remove the ONLY remaining label (Alex: _.person -> _)
select remove_vertex_label(1::int8, 2::int4, 21::int4, 'person');
select id, vertex FROM np_vertex_21_1 WHERE id = 3;