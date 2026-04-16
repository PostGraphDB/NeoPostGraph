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

-- Setup secondary schema
CREATE SCHEMA other_schema;

-- Test 1: Explicitly set search_path and create graph
SET search_path TO other_schema, public;
SELECT NeoPostGraph.create_graph('path_graph'); 

-- Verify it landed in 'other_schema'
SELECT name, namespace FROM NeoPostGraph.np_graph;

-- Test 2: Ensure it doesn't just default to 'public' if something else is first
SET search_path TO public, other_schema;
SELECT NeoPostGraph.create_graph('public_graph');
SELECT name, namespace FROM NeoPostGraph.np_graph;

-- Test 5: Auto-create a brand new schema
SELECT NeoPostGraph.create_graph('new_graph', 'auto_created_schema');

-- Verify schema exists in pg_namespace
SELECT nspname FROM pg_namespace;

---
--- Test 6: Permissions and Ownership Boundaries
---

-- 1. Setup a restricted user and a schema they own
CREATE USER graph_victim;
CREATE SCHEMA victim_schema AUTHORIZATION graph_victim;

-- 2. Setup a second user who will try to "squat" on that schema
CREATE USER graph_attacker;

-- 3. Switch to the attacker
SET ROLE graph_attacker;

-- This SHOULD FAIL because the attacker doesn't have CREATE permission on victim_schema.
SELECT NeoPostGraph.create_graph('stolen_graph', 'victim_schema');

-- 4. Switch back to superuser to check results
RESET ROLE;
SELECT name, namespace FROM NeoPostGraph.np_graph WHERE namespace = 'victim_schema'::regnamespace;

-- 5. Test Auto-Creation Permission
SET ROLE graph_attacker;

-- This SHOULD FAIL if the attacker doesn't have CREATE permission on the database.
-- Most default PG configs allow users to create schemas, but we can test the limit.
SELECT NeoPostGraph.create_graph('attacker_graph', 'forbidden_schema');

RESET ROLE;

---
--- Test 6.1: Search Path Hijacking Protection
---

-- Ensure that if an attacker puts a victim's schema first in their path, 
-- they can't accidentally (or intentionally) create a graph there.
SET ROLE graph_attacker;
SET search_path TO victim_schema, public;

-- Should fail because they don't have CREATE on victim_schema
SELECT NeoPostGraph.create_graph('path_hijack');

RESET ROLE;

SET search_path TO public;

-- Test 7: Long graph names (should throw an error)
SELECT NeoPostGraph.create_graph('a_very_long_graph_name_that_is_exactly_sixty_three_characters_long');
SELECT NeoPostGraph.create_graph('a_very_long_graph_name_that_is_exactly_sixty_three_characters_long_but_will_get_truncated_and_fail');

-- Test 8: Some Special characters in names
SELECT NeoPostGraph.create_graph('graph with spaces', 'public');
SELECT NeoPostGraph.create_graph('123_graph', 'public');
-- Test 9: Unicode characters (Multi-byte)
SELECT NeoPostGraph.create_graph('존은 루저다_그래프', 'public');
-- Test 10: Emojis (4-byte characters)
SELECT NeoPostGraph.create_graph('graph_🚀_fire', 'public');
-- Test 11: Mixed Script & Symbols
SELECT NeoPostGraph.create_graph('∑_graph_Ω', 'public');

---
--- Conflict Tests: Triggering both C-level ereport paths
---

-- Test 12: Conflict with EXPLICIT namespace
-- Should trigger HINT: "Use a different graph name or create the graph in a different namespace."
SELECT NeoPostGraph.create_graph('conflict_test', 'public');
SELECT NeoPostGraph.create_graph('conflict_test', 'public');

-- Test 13: Conflict with IMPLICIT namespace (search_path)
-- Should trigger HINT: "When namespace is not specified... Consider changing the search path..."
SET search_path TO public;
SELECT NeoPostGraph.create_graph('search_path_conflict');
SELECT NeoPostGraph.create_graph('search_path_conflict');

---
--- Verification
---
SELECT * FROM NeoPostGraph.np_graph;

-- Cleanup
DROP SCHEMA victim_schema CASCADE;
DROP USER graph_victim;
DROP USER graph_attacker;