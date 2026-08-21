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


-- Setup the graph
SELECT NeoPostGraph.create_graph('label_graph', 'public');

-- Standard labels
SELECT NeoPostGraph.create_vlabel('label_graph', 'person');
SELECT NeoPostGraph.create_vlabel('label_graph', 'software');

-- Creating Label Trees
SELECT NeoPostGraph.create_vlabel('label_graph', 'employee');
SELECT NeoPostGraph.merge_vlabels('label_graph', 2, 4);
SELECT NeoPostGraph.create_vlabel('label_graph', 'engineer');
SELECT NeoPostGraph.merge_vlabels('label_graph', 5, 6);
SELECT NeoPostGraph.create_vlabel('label_graph', 'contractor');
SELECT NeoPostGraph.merge_vlabels('label_graph', 2, 8);

SELECT NeoPostGraph.create_graph('edge_case_graph', 'public');

-- Testing the "_" naming logic
SELECT NeoPostGraph.create_vlabel('edge_case_graph', '_internal');
SELECT NeoPostGraph.create_vlabel('edge_case_graph', 'test_label_123');

-- Unicode/Multi-byte labels (to match your previous tests)
SELECT NeoPostGraph.create_vlabel('edge_case_graph', '인간'); 

SELECT NeoPostGraph.create_graph('match_test_graph', 'public');

SELECT NeoPostGraph.create_vlabel('match_test_graph', 'user');
SELECT NeoPostGraph.create_vlabel('match_test_graph', 'user_admin');
SELECT NeoPostGraph.create_vlabel('match_test_graph', 'user_profile');
SELECT NeoPostGraph.create_vlabel('match_test_graph', 'users');

-- Force the planner to use the gist index
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT *
FROM NeoPostGraph.np_vertex_label 
WHERE graph_id = 16 
  AND label @ 'employee & engineer';

SELECT *
FROM NeoPostGraph.np_vertex_label 
WHERE graph_id = 16 
  AND label @ 'employee & engineer';

SELECT *
FROM NeoPostGraph.np_vertex_label 
WHERE graph_id = 16 
  AND label @ 'person';

---
-- get_vlabel_ids (uses the index from the above 3 queries)
-- (Precursor to ltree @ 'label1 & label2 ...' operator and syntax when we start building language contructs)
---
-- 1. Baseline test with a single label
SELECT NeoPostGraph.get_vlabel_ids('label_graph', ARRAY['employee']::text[]);

-- 2. Test multi-label AND logic
SELECT NeoPostGraph.get_vlabel_ids('label_graph', ARRAY['person', 'engineer']::text[]);

-- 3. Test Partial match / Sub-path match
SELECT NeoPostGraph.get_vlabel_ids('label_graph', ARRAY['person']::text[]);

-- 4. Test non-existent label in existing graph
SELECT NeoPostGraph.get_vlabel_ids('label_graph', ARRAY['manager']::text[]);

-- 5. Test existing label in non-existent graph
SELECT NeoPostGraph.get_vlabel_ids('missing_graph', ARRAY['person']::text[]);

-- 6. Test empty array input
SELECT NeoPostGraph.get_vlabel_ids('label_graph', ARRAY[]::text[]);

-- 7. Test NULL label
SELECT NeoPostGraph.get_vlabel_ids('label_graph', NULL);

-- 8. Test exact match on underscores/numbers
SELECT NeoPostGraph.get_vlabel_ids('edge_case_graph', ARRAY['test_label_123']::text[]);

-- 9. Test multi-byte/Unicode resolution
SELECT NeoPostGraph.get_vlabel_ids('edge_case_graph', ARRAY['인간']::text[]);


EXPLAIN (COSTS OFF)
SELECT *
FROM NeoPostGraph.np_vertex_label 
WHERE graph_id = 16 
  AND label @ 'employee | software';

SELECT *
FROM NeoPostGraph.np_vertex_label 
WHERE graph_id = 16 
  AND label @ 'employee | software';

-- OR logic check
SELECT NeoPostGraph.get_or_vlabel_ids('label_graph', ARRAY['engineer', 'software']::text[]);

SELECT NeoPostGraph.get_or_vlabel_ids('label_graph', ARRAY['employee', 'software']::text[]);

SELECT NeoPostGraph.get_or_vlabel_ids('label_graph', ARRAY['engineer', 'contractor']::text[]);

-- OR logic check with non-existent sibling
SELECT NeoPostGraph.get_or_vlabel_ids('label_graph', ARRAY['engineer', 'janitor']::text[]);

SELECT * FROM NeoPostGraph.np_vertex_label;

---
--- get_elabel_ids / get_or_elabel_ids (GiST ltxtquery on np_edge_label_<G>)
---
SELECT NeoPostGraph.create_elabel('label_graph', 'knows');
SELECT NeoPostGraph.create_elabel('label_graph', 'likes');
SELECT NeoPostGraph.create_elabel('label_graph', 'reports_to');
SELECT NeoPostGraph.merge_elabels('label_graph', 2, 4);

SELECT NeoPostGraph.create_elabel('edge_case_graph', 'test_label_123');
SELECT NeoPostGraph.create_elabel('edge_case_graph', '인간');

SELECT NeoPostGraph.get_elabel_ids('label_graph', ARRAY['knows']::text[]);
SELECT NeoPostGraph.get_elabel_ids('label_graph', ARRAY['knows', 'reports_to']::text[]);
SELECT NeoPostGraph.get_elabel_ids('label_graph', ARRAY['likes']::text[]);
SELECT NeoPostGraph.get_elabel_ids('label_graph', ARRAY['manager']::text[]);
SELECT NeoPostGraph.get_elabel_ids('missing_graph', ARRAY['knows']::text[]);
SELECT NeoPostGraph.get_elabel_ids('label_graph', ARRAY[]::text[]);
SELECT NeoPostGraph.get_elabel_ids('label_graph', NULL);
SELECT NeoPostGraph.get_elabel_ids('edge_case_graph', ARRAY['test_label_123']::text[]);
SELECT NeoPostGraph.get_elabel_ids('edge_case_graph', ARRAY['인간']::text[]);

SELECT NeoPostGraph.get_or_elabel_ids('label_graph', ARRAY['knows', 'likes']::text[]);
SELECT NeoPostGraph.get_or_elabel_ids('label_graph', ARRAY['reports_to', 'likes']::text[]);
SELECT NeoPostGraph.get_or_elabel_ids('label_graph', ARRAY['knows', 'janitor']::text[]);

---
--- rename_vlabel: rewrite ltree tokens, leave physical tables
---
SELECT id, ltree, is_primary FROM neopostgraph.np_vertex_label_16 ORDER BY id;

SELECT NeoPostGraph.rename_vlabel('label_graph', 'employee', 'staff', 'public');
SELECT id, ltree, is_primary FROM neopostgraph.np_vertex_label_16 ORDER BY id;
SELECT label_name, label_type FROM neopostgraph.np_label_catalog_16 WHERE label_name IN ('employee', 'staff') ORDER BY label_name;

SELECT NeoPostGraph.rename_vlabel('label_graph', 'staff', 'staff', 'public');
SELECT NeoPostGraph.rename_vlabel('label_graph', 'staff', 'person', 'public');
SELECT NeoPostGraph.rename_vlabel('label_graph', 'missing_label', 'x', 'public');
SELECT NeoPostGraph.rename_vlabel('missing_graph', 'staff', 'x', 'public');
SELECT NeoPostGraph.rename_vlabel('label_graph', 'staff', 'has.dot', 'public');

-- is_primary = false partitions must be rewritten too
SELECT NeoPostGraph.merge_vlabels('match_test_graph', 2, 3);
SELECT NeoPostGraph.drop_vlabel('match_test_graph', 'user_admin');
SELECT id, ltree, is_primary FROM neopostgraph.np_vertex_label_18 ORDER BY id;

SELECT NeoPostGraph.rename_vlabel('match_test_graph', 'user', 'account', 'public');
SELECT id, ltree, is_primary FROM neopostgraph.np_vertex_label_18 ORDER BY id;

---
--- rename_elabel: rewrite ltree tokens, including is_primary = false
---
SELECT id, ltree, is_primary FROM neopostgraph.np_edge_label_16 ORDER BY id;

SELECT NeoPostGraph.rename_elabel('label_graph', 'likes', 'appreciates', 'public');
SELECT id, ltree, is_primary FROM neopostgraph.np_edge_label_16 ORDER BY id;

SELECT NeoPostGraph.rename_elabel('label_graph', 'appreciates', 'appreciates', 'public');
SELECT NeoPostGraph.rename_elabel('label_graph', 'appreciates', 'knows', 'public');
SELECT NeoPostGraph.rename_elabel('label_graph', 'missing_label', 'x', 'public');
SELECT NeoPostGraph.rename_elabel('missing_graph', 'knows', 'x', 'public');
SELECT NeoPostGraph.rename_elabel('label_graph', 'knows', 'has.dot', 'public');

SELECT NeoPostGraph.drop_elabel('label_graph', 'reports_to');
SELECT id, ltree, is_primary FROM neopostgraph.np_edge_label_16 ORDER BY id;

SELECT NeoPostGraph.rename_elabel('label_graph', 'knows', 'acquainted', 'public');
SELECT id, ltree, is_primary FROM neopostgraph.np_edge_label_16 ORDER BY id;